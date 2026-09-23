#include "TrailPassElement.hpp"

#include <algorithm>
#include <cstddef>

#include <render/Renderer.hpp>
#include <render/OpenGL.hpp>
#include <render/Shader.hpp>
#include <output/Monitor.hpp>
#include <debug/log/Logger.hpp>

using namespace Render;
using namespace Render::GL;

// ---------------------------------------------------------------- shader

// Stage-3 harness shader. Reads only attribute set 0 (node i). Set 1
// (node i + 1) is declared so the binding layout matches what the ribbon will
// use; it's unused here, so the compiler is free to strip it.
static const std::string TRAIL_VERT_SRC = R"glsl(
#version 300 es
precision highp float;

uniform mat3  proj;        // global layout px -> clip, per monitor
uniform float radiusMin;   // logical px, oldest node
uniform float radiusMax;   // logical px, newest node
uniform float speedRef;    // px/ms mapped to full red
uniform int   nodeCount;

layout(location = 0) in vec2  a_pos;
layout(location = 1) in float a_birthMs;
layout(location = 2) in vec2  a_vel;
layout(location = 3) in vec2  a_nextPos;
layout(location = 4) in float a_nextBirthMs;
layout(location = 5) in vec2  a_nextVel;

out vec2 v_local;
out vec3 v_color;

void main() {
    // TRIANGLE_STRIP corners: 0 (0,0), 1 (1,0), 2 (0,1), 3 (1,1)
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;

    // Size by age rank in the ordered copy: oldest smallest, newest largest.
    // A broken wrap shows up as a size discontinuity mid-trail.
    float rank = nodeCount > 1 ? float(gl_InstanceID) / float(nodeCount - 1) : 1.0;
    float r    = mix(radiusMin, radiusMax, rank);

    // Color from the velocity attribute (last field in the node), so correct
    // colors also imply correct stride and birthTimeMs offset.
    float speed = clamp(length(a_vel) / speedRef, 0.0, 1.0);
    v_color     = mix(vec3(0.1, 0.4, 1.0), vec3(1.0, 0.1, 0.1), speed);
    v_local     = corner;

    gl_Position = vec4(proj * vec3(a_pos + corner * r, 1.0), 1.0);
}
)glsl";

static const std::string TRAIL_FRAG_SRC = R"glsl(
#version 300 es
precision highp float;

in vec2 v_local;
in vec3 v_color;

layout(location = 0) out vec4 fragColor;

void main() {
    if (dot(v_local, v_local) > 1.0)
        discard;
    fragColor = vec4(v_color, 1.0);
}
)glsl";

static bool ensureShader(STrailShader& s) {
    if (s.shader)
        return true;
    if (s.initFailed)
        return false;

    auto shader = makeShared<CShader>();
    if (!shader->createProgram(TRAIL_VERT_SRC, TRAIL_FRAG_SRC, /*dynamic=*/true, /*silent=*/false)) {
        LOG(Log::ERR, "[hyprtail-s3] trail shader compilation failed");
        s.initFailed = true;
        return false;
    }

    // Custom uniforms aren't in eShaderUniform, look them up directly.
    const auto prog = shader->program();
    s.locRadiusMin  = glGetUniformLocation(prog, "radiusMin");
    s.locRadiusMax  = glGetUniformLocation(prog, "radiusMax");
    s.locSpeedRef   = glGetUniformLocation(prog, "speedRef");
    s.locNodeCount  = glGetUniformLocation(prog, "nodeCount");
    s.shader        = shader;

    LOG(Log::INFO, "[hyprtail-s3] trail shader compiled ok, program id={}", prog);
    return true;
}

// ---------------------------------------------------------------- GPU buffer

static constexpr GLsizei NODE_STRIDE = sizeof(SCursorNode);

bool CTrailGpu::ensure(size_t ringCapacity) {
    if (m_vao)
        return true;

    // +1 trailing node, see header.
    m_vboNodes = ringCapacity + 1;

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    if (!m_vao || !m_vbo) {
        LOG(Log::ERR, "[hyprtail-s3] failed to create trail VAO/VBO");
        destroy();
        return false;
    }

    glBindVertexArray(m_vao);
    // Through Hyprland's cache, a raw glBindBuffer would desync it.
    g_pHyprOpenGL->bindArrayBuffer(m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_vboNodes * NODE_STRIDE, nullptr, GL_DYNAMIC_DRAW);

    // Set 0 = node i at offset 0, set 1 = node i + 1 at offset one stride.
    for (GLuint set = 0; set < 2; ++set) {
        const size_t base = set * NODE_STRIDE;
        const GLuint loc  = set * 3;

        glEnableVertexAttribArray(loc + 0);
        glVertexAttribPointer(loc + 0, 2, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(base + offsetof(SCursorNode, posPx)));
        glVertexAttribDivisor(loc + 0, 1);

        glEnableVertexAttribArray(loc + 1);
        glVertexAttribPointer(loc + 1, 1, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(base + offsetof(SCursorNode, birthTimeMs)));
        glVertexAttribDivisor(loc + 1, 1);

        glEnableVertexAttribArray(loc + 2);
        glVertexAttribPointer(loc + 2, 2, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(base + offsetof(SCursorNode, velocity)));
        glVertexAttribDivisor(loc + 2, 1);
    }

    glBindVertexArray(0);
    g_pHyprOpenGL->bindArrayBuffer(0);

    m_uploadedGen = UINT64_MAX;
    return true;
}

void CTrailGpu::upload(const CTrailRing& ring) {
    if (!m_vbo || ring.empty() || ring.generation() == m_uploadedGen)
        return;

    ring.orderedCopy(m_ordered);
    m_ordered.push_back(m_ordered.back()); // trailing node for set 1

    const size_t n = std::min(m_ordered.size(), m_vboNodes);

    g_pHyprOpenGL->bindArrayBuffer(m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * NODE_STRIDE, m_ordered.data());
    g_pHyprOpenGL->bindArrayBuffer(0);

    m_uploadedGen = ring.generation();
}

void CTrailGpu::destroy() {
    g_pHyprOpenGL->bindArrayBuffer(0);

    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    if (m_vbo)
        glDeleteBuffers(1, &m_vbo);

    m_vao         = 0;
    m_vbo         = 0;
    m_vboNodes    = 0;
    m_uploadedGen = UINT64_MAX;
    m_ordered.clear();
}

GLuint CTrailGpu::vao() const {
    return m_vao;
}

// ---------------------------------------------------------------- instance

CBox trailBoxLocal(const STrailInstance& inst, const Vector2D& monitorPos) {
    const auto  b = inst.ring.bounds();
    const float r = inst.radiusMaxPx;

    return CBox{b.x1 - r - monitorPos.x, b.y1 - r - monitorPos.y, (b.x2 - b.x1) + 2.0 * r, (b.y2 - b.y1) + 2.0 * r};
}

void trailInstanceCleanup(STrailInstance& inst) {
    inst.gpu.destroy();
    if (inst.shader.shader) {
        inst.shader.shader->destroy();
        inst.shader.shader.reset();
    }
    inst.shader.initFailed = false;
    inst.monState.clear();
}

// ---------------------------------------------------------------- pass element

CTrailPassElement::CTrailPassElement(STrailInstance* inst, const CBox& boxLocal) : m_inst(inst), m_boxLocal(boxLocal) {}

std::optional<CBox> CTrailPassElement::boundingBox() {
    return m_boxLocal;
}

std::vector<UP<IPassElement>> CTrailPassElement::draw() {
    auto&      rd      = g_pHyprRenderer->m_renderData;
    const auto monitor = rd.pMonitor.lock();
    if (!m_inst || !monitor || m_inst->ring.empty())
        return {};

    if (!ensureShader(m_inst->shader) || !m_inst->gpu.ensure(m_inst->ring.capacity()))
        return {};

    m_inst->gpu.upload(m_inst->ring);

    const auto count = sc<GLsizei>(m_inst->ring.size());

    // Global layout px -> monitor-local px -> clip. projectBoxToTarget takes a
    // pixel-space box (Renderer.cpp:1855-1860); projectBox maps p to
    // pos + size * p (hyprutils Mat3x3.cpp:68-90), so this box encodes
    // (p - monitorPos) * scale.
    const double s    = monitor->m_scale;
    const auto   proj = g_pHyprRenderer->projectBoxToTarget(CBox{-monitor->m_position.x * s, -monitor->m_position.y * s, s, s});

    // Through Hyprland's program cache (OpenGL.cpp:2398-2406), not raw glUseProgram.
    auto shader = g_pHyprOpenGL->useShader(m_inst->shader.shader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, proj.getMatrix());
    glUniform1f(m_inst->shader.locRadiusMin, m_inst->radiusMinPx);
    glUniform1f(m_inst->shader.locRadiusMax, m_inst->radiusMaxPx);
    glUniform1f(m_inst->shader.locSpeedRef, m_inst->speedRefPxPerMs);
    glUniform1i(m_inst->shader.locNodeCount, count);

    glBindVertexArray(m_inst->gpu.vao());

    // Clip to this element's damage, same pattern as core (OpenGL.cpp:1810-1815).
    // Scissor through Hyprland's cached state, not raw glEnable/glScissor.
    rd.damage.forEachRect([&rd, count](const auto& RECT) {
        g_pHyprOpenGL->scissor(&RECT, rd.transformDamage);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
    });

    g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);

    return {};
}
