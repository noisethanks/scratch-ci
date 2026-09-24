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

bool trailPrepare(STrailInstance& inst) {
    if (inst.disabled)
        return false;

    // Compiles a pending program or falls back to the built-in one; a failing
    // user shader is reported there and the previous program kept.
    if (const auto error = inst.slot.prepare()) {
        trailDisable(inst, "shader:" + inst.name, std::format("{} trail disabled: {}", inst.name, *error));
        trailReleaseGpu(inst);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- GPU buffer

static constexpr GLsizei NODE_STRIDE = sizeof(SGpuNode);

bool CTrailGpu::ensure(size_t ringCapacity, std::string& error) {
    if (m_vao && m_vboNodes == ringCapacity + 2)
        return true;

    // Capacity changed (config reload): reallocate.
    if (m_vao)
        destroy();

    // Front and back pad, see header.
    m_vboNodes = ringCapacity + 2;

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

    // One per-instance attribute: `components` floats of field at byteOffset
    // within the node, `nodeOffset` nodes into the VBO.
    const auto attrib = [](GLuint loc, GLint components, size_t nodeOffset, size_t fieldOffset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, components, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(nodeOffset * NODE_STRIDE + fieldOffset));
        glVertexAttribDivisor(loc, 1);
    };

    // prev = n(i-1), p0 = n(i), p1 = n(i+1), next = n(i+2); see header.
    attrib(0, 2, 0, offsetof(SGpuNode, posPx));
    attrib(1, 1, 0, offsetof(SGpuNode, flags));
    for (GLuint end = 0; end < 2; ++end) {
        const GLuint loc = 2 + end * 4;
        attrib(loc + 0, 2, 1 + end, offsetof(SGpuNode, posPx));
        attrib(loc + 1, 1, 1 + end, offsetof(SGpuNode, birthMs));
        attrib(loc + 2, 2, 1 + end, offsetof(SGpuNode, velocity));
        attrib(loc + 3, 1, 1 + end, offsetof(SGpuNode, flags));
    }
    attrib(10, 2, 3, offsetof(SGpuNode, posPx));
    attrib(11, 1, 3, offsetof(SGpuNode, flags));

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

    // Pads, see header: front = n0 as a segment start, back = newest.
    SGpuNode front = m_ordered.front();
    front.flags    = GPU_FLAG_SEGMENT_START;
    m_ordered.insert(m_ordered.begin(), front);
    m_ordered.push_back(m_ordered.back());

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
    const float r = inst.padPx();

    return CBox{b.x1 - r - monitorPos.x, b.y1 - r - monitorPos.y, (b.x2 - b.x1) + 2.0 * r, (b.y2 - b.y1) + 2.0 * r};
}

void trailDisable(STrailInstance& inst, std::string_view key, std::string_view message) {
    inst.disabled = true;
    hyprtail::diag::report(eSeverity::ERR, key, message);
}

void trailReleaseGpu(STrailInstance& inst) {
    inst.gpu.destroy();
    inst.slot.release();
}

void trailInstanceCleanup(STrailInstance& inst) {
    trailReleaseGpu(inst);
    inst.damage.clear();
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

    if (!trailPrepare(*m_inst))
        return;

    if (std::string error; !m_inst->gpu.ensure(m_inst->ring.capacity(), error)) {
        trailDisable(*m_inst, "gl:" + m_inst->name, std::format("{} trail disabled: GL resource creation failed: {}", m_inst->name, error));
        trailReleaseGpu(*m_inst);
        return;
    }

    m_inst->gpu.upload(m_inst->ring);

    // One instance per segment.
    if (m_inst->ring.size() < 2)
        return;
    const auto count = sc<GLsizei>(m_inst->ring.size() - 1);

    auto& slot = m_inst->slot;

    // Through Hyprland's program cache (OpenGL.cpp:2561-2569), not raw glUseProgram.
    auto shader = g_pHyprOpenGL->useShader(slot.shader());
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, hyprtail::globalProjection(monitor).getMatrix());
    glUniform1f(slot.loc("nowMs"), sc<float>(m_nowMs - m_inst->gpu.refMs()));
    glUniform1f(slot.loc("fadeMs"), sc<float>(m_inst->fadeMs));
    glUniform1f(slot.loc("widthPx"), m_inst->widthPx);
    glUniform1f(slot.loc("miterLimit"), m_inst->miterLimit);
    glUniform1f(slot.loc("speedRef"), m_inst->speedRefPxPerMs);

    // Color management: the sRGB palette converted to the current
    // framebuffer's color space, like core's own solid colors (RenderUtil).
    // Colors a shader computes itself are not converted (shader contract).
    hyprtail::setPaletteUniform(slot.loc("colorSlow"), m_inst->colorSlow);
    hyprtail::setPaletteUniform(slot.loc("colorFast"), m_inst->colorFast);

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
