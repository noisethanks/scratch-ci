#include "TrailPassElement.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>

#include <render/Renderer.hpp>
#include <render/OpenGL.hpp>
#include <render/Shader.hpp>
#include <output/Monitor.hpp>
#include <debug/log/Logger.hpp>

#include "Diagnostics.hpp"

using namespace Render;
using hyprtail::diag::eSeverity;
using namespace Render::GL;

// ---------------------------------------------------------------- shader

// Dot harness shader (stage 4: time-based fade). Reads only attribute set 0
// (node i). Set 1 (node i + 1) is declared so the binding layout matches what
// the ribbon will use; it's unused here, so the compiler is free to strip it.
static const std::string TRAIL_VERT_SRC = R"glsl(
#version 300 es
precision highp float;

uniform mat3  proj;      // global layout px -> clip, per monitor
uniform float radius;    // logical px
uniform float speedRef;  // px/ms mapped to full red
uniform float nowMs;     // same reference as a_birthMs, only differences matter
uniform float fadeMs;

layout(location = 0) in vec2  a_pos;
layout(location = 1) in float a_birthMs;
layout(location = 2) in vec2  a_vel;
layout(location = 3) in vec2  a_nextPos;
layout(location = 4) in float a_nextBirthMs;
layout(location = 5) in vec2  a_nextVel;

out vec2  v_local;
out vec3  v_color;
out float v_alpha;

void main() {
    // Time-based fade (SPEC §4): age from the node's own timestamp, not its
    // position along the trail.
    float age = nowMs - a_birthMs;
    if (age >= fadeMs) {
        // Fully faded: collapse all 4 corners to one point outside clip space,
        // no fragments. Same test the CPU uses for visibleBounds().
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_local     = vec2(0.0);
        v_color     = vec3(0.0);
        v_alpha     = 0.0;
        return;
    }

    // Linear placeholder curve.
    v_alpha = clamp(1.0 - age / fadeMs, 0.0, 1.0);

    // TRIANGLE_STRIP corners: 0 (0,0), 1 (1,0), 2 (0,1), 3 (1,1)
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;

    float speed = clamp(length(a_vel) / speedRef, 0.0, 1.0);
    v_color     = mix(vec3(0.1, 0.4, 1.0), vec3(1.0, 0.1, 0.1), speed);
    v_local     = corner;

    gl_Position = vec4(proj * vec3(a_pos + corner * radius, 1.0), 1.0);
}
)glsl";

static const std::string TRAIL_FRAG_SRC = R"glsl(
#version 300 es
precision highp float;

in vec2  v_local;
in vec3  v_color;
in float v_alpha;

layout(location = 0) out vec4 fragColor;

void main() {
    // ~1px antialiased edge, inside the quad so the damage box still covers it.
    float d   = length(v_local);
    float w   = fwidth(d);
    float cov = 1.0 - smoothstep(1.0 - w, 1.0, d);
    if (cov <= 0.0)
        discard;

    // Hyprland blends premultiplied: GL_ONE, GL_ONE_MINUS_SRC_ALPHA
    // (OpenGL.cpp:981-989).
    float a   = v_alpha * cov;
    fragColor = vec4(v_color * a, a);
}
)glsl";

// CShader::createProgram logs compile/link errors and discards the text
// (Shader.cpp logShaderError), so compile and link once ourselves to capture
// the GLSL log. Raw shader/program objects only, no cached GL state touched.
// Returns the error description, or nullopt if both stages compile and link.
static std::optional<std::string> glslCheck(const std::string& vert, const std::string& frag) {
    const auto infoLog = [](GLuint obj, bool program) {
        GLint len = 0;
        program ? glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len) : glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
        std::string log(std::max(len, 0), '\0');
        if (len > 0)
            program ? glGetProgramInfoLog(obj, len, &len, log.data()) : glGetShaderInfoLog(obj, len, &len, log.data());
        log.resize(std::max(len, 0));
        while (!log.empty() && (log.back() == '\0' || log.back() == '\n' || log.back() == ' '))
            log.pop_back();
        return log.empty() ? std::string{"(no log from driver)"} : log;
    };

    const auto compile = [&](GLenum type, const std::string& src, std::string& error) -> GLuint {
        const GLuint sh = glCreateShader(type);
        if (!sh) {
            error = "glCreateShader failed";
            return 0;
        }
        const char* p = src.c_str();
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = GL_FALSE;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            error = infoLog(sh, false);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };

    std::string  error;
    const GLuint vs = compile(GL_VERTEX_SHADER, vert, error);
    if (!vs)
        return std::format("vertex shader failed to compile:\n{}", error);

    const GLuint fs = compile(GL_FRAGMENT_SHADER, frag, error);
    if (!fs) {
        glDeleteShader(vs);
        return std::format("fragment shader failed to compile:\n{}", error);
    }

    std::optional<std::string> result;
    const GLuint               prog = glCreateProgram();
    if (!prog)
        result = "glCreateProgram failed";
    else {
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE)
            result = std::format("shader program failed to link:\n{}", infoLog(prog, true));
        glDetachShader(prog, vs);
        glDetachShader(prog, fs);
        glDeleteProgram(prog);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return result;
}

static bool ensureShader(STrailInstance& inst) {
    auto& s = inst.shader;
    if (s.shader)
        return true;

    if (const auto error = glslCheck(TRAIL_VERT_SRC, TRAIL_FRAG_SRC)) {
        trailDisable(inst, "shader:" + inst.name, std::format("{} trail disabled: {}", inst.name, *error));
        return false;
    }

    // silent: failures are ours to report; core's error bar would label them
    // "Screen shader parser", which is misleading.
    auto shader = makeShared<CShader>();
    if (!shader->createProgram(TRAIL_VERT_SRC, TRAIL_FRAG_SRC, /*dynamic=*/true, /*silent=*/true)) {
        trailDisable(inst, "shader:" + inst.name, std::format("{} trail disabled: shader passed a standalone compile/link check but CShader::createProgram failed", inst.name));
        return false;
    }

    // Custom uniforms aren't in eShaderUniform, look them up directly.
    const auto prog = shader->program();
    s.locRadius     = glGetUniformLocation(prog, "radius");
    s.locSpeedRef   = glGetUniformLocation(prog, "speedRef");
    s.locNowMs      = glGetUniformLocation(prog, "nowMs");
    s.locFadeMs     = glGetUniformLocation(prog, "fadeMs");
    s.shader        = shader;

    Log::logger->log(Log::INFO, "[hyprtail] {} trail shader compiled ok, program id={}", inst.name, prog);
    return true;
}

// ---------------------------------------------------------------- GPU buffer

static constexpr GLsizei NODE_STRIDE = sizeof(SGpuNode);

bool CTrailGpu::ensure(size_t ringCapacity, std::string& error) {
    if (m_vao)
        return true;

    // +1 trailing node, see header.
    m_vboNodes = ringCapacity + 1;

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    if (!m_vao || !m_vbo) {
        error = std::format("glGenVertexArrays/glGenBuffers returned no name (vao={}, vbo={})", m_vao, m_vbo);
        destroy();
        return false;
    }

    // Drain stale errors so the check after glBufferData is ours. This hides
    // errors core left behind; only core's debug builds look at those.
    while (glGetError() != GL_NO_ERROR) {}

    glBindVertexArray(m_vao);
    // Raw bind is fine at efb5099: there is no array-buffer cache, core binds
    // raw too (OpenGL.cpp:1547, Shader.cpp:236). Later main adds
    // CHyprOpenGLImpl::bindArrayBuffer(); use that if the pin moves past it.
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_vboNodes * NODE_STRIDE, nullptr, GL_DYNAMIC_DRAW);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        error = std::format("glBufferData for {} bytes failed, GL error 0x{:x}", m_vboNodes * NODE_STRIDE, err);
        destroy();
        return false;
    }

    // Set 0 = node i at offset 0, set 1 = node i + 1 at offset one stride.
    for (GLuint set = 0; set < 2; ++set) {
        const size_t base = set * NODE_STRIDE;
        const GLuint loc  = set * 3;

        glEnableVertexAttribArray(loc + 0);
        glVertexAttribPointer(loc + 0, 2, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(base + offsetof(SGpuNode, posPx)));
        glVertexAttribDivisor(loc + 0, 1);

        glEnableVertexAttribArray(loc + 1);
        glVertexAttribPointer(loc + 1, 1, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(base + offsetof(SGpuNode, birthMs)));
        glVertexAttribDivisor(loc + 1, 1);

        glEnableVertexAttribArray(loc + 2);
        glVertexAttribPointer(loc + 2, 2, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(base + offsetof(SGpuNode, velocity)));
        glVertexAttribDivisor(loc + 2, 1);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_uploadedGen = UINT64_MAX;
    return true;
}

void CTrailGpu::upload(const CTrailRing& ring) {
    if (!m_vbo || ring.empty() || ring.generation() == m_uploadedGen)
        return;

    // Rebase on the newest node: every uploaded birthMs is <= 0 and small, so
    // float keeps sub-ms precision regardless of how long the plugin has run.
    m_refMs = ring.newest().birthTimeMs;
    ring.orderedCopy(m_ordered, m_refMs);
    m_ordered.push_back(m_ordered.back()); // trailing node for set 1

    const size_t n = std::min(m_ordered.size(), m_vboNodes);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * NODE_STRIDE, m_ordered.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_uploadedGen = ring.generation();
}

void CTrailGpu::destroy() {
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    if (m_vbo)
        glDeleteBuffers(1, &m_vbo);

    m_vao         = 0;
    m_vbo         = 0;
    m_vboNodes    = 0;
    m_uploadedGen = UINT64_MAX;
    m_refMs       = 0.0;
    m_ordered.clear();
}

GLuint CTrailGpu::vao() const {
    return m_vao;
}

double CTrailGpu::refMs() const {
    return m_refMs;
}

// ---------------------------------------------------------------- instance

CBox trailBoxLocal(const STrailInstance& inst, const STrailBounds& b, const Vector2D& monitorPos) {
    const float r = inst.radiusPx;

    return CBox{b.x1 - r - monitorPos.x, b.y1 - r - monitorPos.y, (b.x2 - b.x1) + 2.0 * r, (b.y2 - b.y1) + 2.0 * r};
}

void trailDisable(STrailInstance& inst, std::string_view key, std::string_view message) {
    inst.disabled = true;
    hyprtail::diag::report(eSeverity::ERR, key, message);
}

void trailReleaseGpu(STrailInstance& inst) {
    inst.gpu.destroy();
    if (inst.shader.shader) {
        inst.shader.shader->destroy();
        inst.shader.shader.reset();
    }
}

void trailInstanceCleanup(STrailInstance& inst) {
    trailReleaseGpu(inst);
    inst.monState.clear();
}

// ---------------------------------------------------------------- pass element

CTrailPassElement::CTrailPassElement(STrailInstance* inst, const CBox& boxLocal, double nowMs) : m_inst(inst), m_boxLocal(boxLocal), m_nowMs(nowMs) {}

std::optional<CBox> CTrailPassElement::boundingBox() {
    return m_boxLocal;
}

std::vector<UP<IPassElement>> CTrailPassElement::draw() {
    // Called from the pass render inside Hyprland: nothing may escape.
    const bool ok = hyprtail::diag::guard("trail-draw", [this] { drawInternal(); });
    if (!ok && m_inst) {
        m_inst->disabled = true;
        trailReleaseGpu(*m_inst); // GL is current here
    }
    return {};
}

void CTrailPassElement::drawInternal() {
    auto&      rd      = g_pHyprRenderer->m_renderData;
    const auto monitor = rd.pMonitor.lock();
    if (!m_inst || m_inst->disabled || !monitor || m_inst->ring.empty())
        return;

    if (!ensureShader(*m_inst)) {
        trailReleaseGpu(*m_inst);
        return;
    }

    if (std::string error; !m_inst->gpu.ensure(m_inst->ring.capacity(), error)) {
        trailDisable(*m_inst, "gl:" + m_inst->name, std::format("{} trail disabled: GL resource creation failed: {}", m_inst->name, error));
        trailReleaseGpu(*m_inst);
        return;
    }

    m_inst->gpu.upload(m_inst->ring);

    const auto count = sc<GLsizei>(m_inst->ring.size());

    // Global layout px -> monitor-local px -> clip. projectBoxToTarget takes a
    // pixel-space box (Renderer.cpp:1842-1846); projectBox maps p to
    // pos + size * p (hyprutils Mat3x3.cpp:68-90), so this box encodes
    // (p - monitorPos) * scale.
    // Transform is passed explicitly as NORMAL: getBoxProjection otherwise
    // defaults to the inverted monitor transform applied *inside* the box
    // (Renderer.cpp:1836-1840), which would rotate this pseudo-box. Monitor
    // rotation stays in targetProjection (Renderer.cpp:1828). Rotated outputs
    // are untested.
    const double s    = monitor->m_scale;
    const auto   proj = g_pHyprRenderer->projectBoxToTarget(CBox{-monitor->m_position.x * s, -monitor->m_position.y * s, s, s}, HYPRUTILS_TRANSFORM_NORMAL);

    // Through Hyprland's program cache (OpenGL.cpp:2561-2569), not raw glUseProgram.
    auto shader = g_pHyprOpenGL->useShader(m_inst->shader.shader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, proj.getMatrix());
    glUniform1f(m_inst->shader.locRadius, m_inst->radiusPx);
    glUniform1f(m_inst->shader.locSpeedRef, m_inst->speedRefPxPerMs);
    glUniform1f(m_inst->shader.locNowMs, sc<float>(m_nowMs - m_inst->gpu.refMs()));
    glUniform1f(m_inst->shader.locFadeMs, sc<float>(m_inst->fadeMs));

    // Premultiplied blending through Hyprland's cap-status cache (OpenGL.cpp:981-989).
    // Blend state is whatever the previous element left, so set it explicitly.
    g_pHyprOpenGL->blend(true);

    glBindVertexArray(m_inst->gpu.vao());

    // Clip to this element's damage, same pattern as core (OpenGL.cpp:1117-1124).
    // Scissor through Hyprland's cached state, not raw glEnable/glScissor.
    rd.damage.forEachRect([&rd, count](const auto& RECT) {
        g_pHyprOpenGL->scissor(&RECT, rd.transformDamage);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
    });

    g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
}
