#include "LayerPassElement.hpp"

#include <cstddef>
#include <format>

#include <render/Renderer.hpp>
#include <render/OpenGL.hpp>
#include <output/Monitor.hpp>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;
using namespace Render::GL;

// ---------------------------------------------------------------- node buffer

static constexpr GLsizei NODE_STRIDE = sizeof(SGpuNode);

bool CNodeBuffer::ensure(size_t ringCapacity, std::string& error) {
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
    // Bounded: GL keeps one flag per error kind, so a few reads empty it, but
    // a driver that keeps reporting an error (e.g. a lost context) would
    // otherwise spin here forever.
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}

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

    // One per-instance attribute: a field of the node `nodeOffset` nodes
    // into the VBO. Floats through glVertexAttribPointer, the bits word
    // through glVertexAttribIPointer (GLES 3.0), which the shader reads as
    // uint without conversion.
    const auto floats = [](GLuint loc, GLint components, size_t nodeOffset, size_t fieldOffset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, components, GL_FLOAT, GL_FALSE, NODE_STRIDE, (void*)(nodeOffset * NODE_STRIDE + fieldOffset));
        glVertexAttribDivisor(loc, 1);
    };
    const auto bits = [](GLuint loc, size_t nodeOffset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribIPointer(loc, 1, GL_UNSIGNED_INT, NODE_STRIDE, (void*)(nodeOffset * NODE_STRIDE + offsetof(SGpuNode, bits)));
        glVertexAttribDivisor(loc, 1);
    };

    // prev = n(i-1), p0 = n(i), p1 = n(i+1), next = n(i+2); see header.
    floats(0, 2, 0, offsetof(SGpuNode, posPx));
    bits(1, 0);
    for (GLuint end = 0; end < 2; ++end) {
        const GLuint loc = 2 + end * 5;
        floats(loc + 0, 2, 1 + end, offsetof(SGpuNode, posPx));
        floats(loc + 1, 1, 1 + end, offsetof(SGpuNode, birthMs));
        floats(loc + 2, 2, 1 + end, offsetof(SGpuNode, velocity));
        floats(loc + 3, 1, 1 + end, offsetof(SGpuNode, distPx));
        bits(loc + 4, 1 + end);
    }
    floats(12, 2, 3, offsetof(SGpuNode, posPx));
    bits(13, 3);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_uploadedGen = UINT64_MAX;
    return true;
}

void CNodeBuffer::upload(const CTrailRing& ring) {
    if (!m_vbo || ring.empty() || ring.generation() == m_uploadedGen)
        return;

    // Rebase on the newest node: every uploaded birthMs is <= 0 and small, so
    // float keeps sub-ms precision regardless of how long the plugin has run.
    m_refMs = ring.newest().birthTimeMs;
    ring.orderedCopy(m_ordered, m_refMs);

    // Pads, see header: front = n0 as a segment start, back = newest.
    SGpuNode front = m_ordered.front();
    front.bits |= GPU_BIT_SEGMENT_START;
    m_ordered.insert(m_ordered.begin(), front);
    m_ordered.push_back(m_ordered.back());

    const size_t n = std::min(m_ordered.size(), m_vboNodes);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * NODE_STRIDE, m_ordered.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_uploadedGen = ring.generation();
}

void CNodeBuffer::destroy() {
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

GLuint CNodeBuffer::vao() const {
    return m_vao;
}

double CNodeBuffer::refMs() const {
    return m_refMs;
}

// ---------------------------------------------------------------- preset

float layerExtentPx(const SPreset& preset, const hyprtail::CLayer& layer) {
    return layer.res.paddingPx + preset.damagePaddingPx;
}

void presetReleaseGpu(SPreset& preset) {
    preset.gpu.destroy();
    if (preset.quadVao) {
        glDeleteVertexArrays(1, &preset.quadVao);
        preset.quadVao = 0;
    }
    for (auto& l : preset.layers)
        l->slot.release();
}

// ---------------------------------------------------------------- pass element

CLayerPassElement::CLayerPassElement(SPreset* preset, std::vector<SLayerDraw> draws, double nowMs) : m_preset(preset), m_draws(std::move(draws)), m_nowMs(nowMs) {}

std::optional<CBox> CLayerPassElement::boundingBox() {
    if (m_draws.empty())
        return std::nullopt;
    double x1 = m_draws.front().boxLocal.x, y1 = m_draws.front().boxLocal.y;
    double x2 = x1 + m_draws.front().boxLocal.w, y2 = y1 + m_draws.front().boxLocal.h;
    for (const auto& d : m_draws) {
        x1 = std::min(x1, d.boxLocal.x);
        y1 = std::min(y1, d.boxLocal.y);
        x2 = std::max(x2, d.boxLocal.x + d.boxLocal.w);
        y2 = std::max(y2, d.boxLocal.y + d.boxLocal.h);
    }
    return CBox{x1, y1, x2 - x1, y2 - y1};
}

std::vector<UP<IPassElement>> CLayerPassElement::draw() {
    // Called from the pass render inside Hyprland: nothing may escape. Each
    // layer is guarded on its own: one failing doesn't take the others down.
    for (const auto& d : m_draws) {
        const bool ok = hyprtail::diag::guard(std::format("draw-{}", d.layer->name()), [&] { drawLayer(d); });
        if (!ok) {
            d.layer->disabled = true;
            d.layer->slot.release(); // GL is current here
        }
    }
    return {};
}

void CLayerPassElement::drawLayer(const SLayerDraw& d) {
    auto&      rd      = g_pHyprRenderer->m_renderData;
    const auto monitor = rd.pMonitor.lock();
    auto&      layer   = *d.layer;
    auto&      preset  = *m_preset;
    if (!monitor || layer.disabled || !layer.resolved())
        return;

    const bool path = layer.topology() == hyprtail::shader::eTopology::PATH;

    GLuint     vao   = 0;
    GLsizei    count = 1;
    if (path) {
        if (preset.gpuFailed || preset.ring.size() < 2)
            return;
        if (std::string error; !preset.gpu.ensure(preset.ring.capacity(), error)) {
            preset.gpuFailed = true;
            hyprtail::diag::report(eSeverity::ERR, "gl:nodes", std::format("path layers disabled: GL resource creation failed: {}", error));
            return;
        }
        preset.gpu.upload(preset.ring);
        vao   = preset.gpu.vao();
        count = static_cast<GLsizei>(preset.ring.size() - 1); // one instance per segment
    } else {
        if (!preset.quadVao)
            glGenVertexArrays(1, &preset.quadVao);
        if (!preset.quadVao) {
            layer.disabled = true;
            hyprtail::diag::report(eSeverity::ERR, "gl:" + layer.name(), std::format("layer {} disabled: glGenVertexArrays returned no name", layer.name()));
            return;
        }
        vao = preset.quadVao;
    }

    auto& slot = layer.slot;

    // Through Hyprland's program cache (OpenGL.cpp:2561-2569), not raw
    // glUseProgram. Our uniforms aren't core's eShaderUniform ones, so they
    // are set raw on the program's own locations.
    g_pHyprOpenGL->useShader(slot.shader());

    const auto proj = hyprtail::globalProjection(monitor).getMatrix();
    glUniformMatrix3fv(slot.loc("ht_proj"), 1, GL_TRUE, proj.data());
    glUniform1f(slot.loc("ht_nowMs"), static_cast<float>(m_nowMs - preset.gpu.refMs()));
    glUniform1f(slot.loc("ht_stillMs"), static_cast<float>(m_nowMs - preset.lastMotionMs));
    glUniform2f(slot.loc("ht_anchor"), static_cast<float>(preset.lastPos.x), static_cast<float>(preset.lastPos.y));
    glUniform1f(slot.loc("ht_extentPx"), d.extentPx);
    glUniform1f(slot.loc("fade_ms"), static_cast<float>(layer.res.fadeMs));
    glUniform1f(slot.loc("start_ms"), static_cast<float>(layer.res.startMs));
    glUniform1f(slot.loc("duration_ms"), static_cast<float>(layer.res.durationMs));

    using hyprtail::params::eType;
    for (const auto& [decl, v] : layer.res.values) {
        const GLint loc = slot.loc(decl.name);
        if (loc < 0)
            continue;
        switch (decl.type) {
            case eType::FLOAT: glUniform1f(loc, static_cast<float>(v.x)); break;
            case eType::INT:
            case eType::BOOL: glUniform1i(loc, static_cast<GLint>(v.x)); break;
            case eType::VEC2: glUniform2f(loc, static_cast<float>(v.x), static_cast<float>(v.y)); break;
            // Color management like core's solid colors (RenderUtil).
            case eType::COLOR: hyprtail::setPaletteUniform(loc, CHyprColor{static_cast<uint64_t>(v.argb)}); break;
        }
    }

    // Premultiplied blending through Hyprland's cap-status cache
    // (OpenGL.cpp:981-989). Blend state is whatever the previous element
    // left, so set it explicitly.
    g_pHyprOpenGL->blend(true);

    glBindVertexArray(vao);

    // Clip to this element's damage, same pattern as core (OpenGL.cpp:1117-1124).
    // Scissor through Hyprland's cached state, not raw glEnable/glScissor.
    rd.damage.forEachRect([&rd, path, count](const auto& RECT) {
        g_pHyprOpenGL->scissor(&RECT, rd.transformDamage);
        if (path)
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
        else
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
}
