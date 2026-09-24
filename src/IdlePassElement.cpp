#include "IdlePassElement.hpp"

#include <format>

#include <render/Renderer.hpp>
#include <render/OpenGL.hpp>
#include <output/Monitor.hpp>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;
using namespace Render::GL;

std::optional<double> idleEffectMs(const SIdleInstance& idle, double nowMs) {
    if (!idle.enabled || idle.disabled)
        return std::nullopt;

    const double still = nowMs - idle.lastMotionMs;
    if (still < idle.delayMs)
        return std::nullopt;

    const double effectMs = still - idle.delayMs;
    // Finite by default: an endless animation would keep the compositor
    // rendering for as long as the pointer is idle (SPEC section 7).
    if (idle.durationMs > 0.0 && effectMs >= idle.durationMs)
        return std::nullopt;

    return effectMs;
}

CBox idleBoxLocal(const SIdleInstance& idle, const Vector2D& center, const Vector2D& monitorPos) {
    const double e = idle.extentPx() + 1.0; // +1px antialiasing margin
    return CBox{center.x - e - monitorPos.x, center.y - e - monitorPos.y, 2.0 * e, 2.0 * e};
}

static void idleDisable(SIdleInstance& idle, std::string_view key, std::string_view message) {
    idle.disabled = true;
    hyprtail::diag::report(eSeverity::ERR, key, message);
}

bool idlePrepare(SIdleInstance& idle) {
    if (idle.disabled)
        return false;

    if (const auto error = idle.slot.prepare()) {
        idleDisable(idle, "shader:" + idle.name, std::format("idle effect disabled: {}", *error));
        idleReleaseGpu(idle);
        return false;
    }
    return true;
}

void idleReleaseGpu(SIdleInstance& idle) {
    if (idle.vao) {
        glDeleteVertexArrays(1, &idle.vao);
        idle.vao = 0;
    }
    idle.slot.release();
}

void idleCleanup(SIdleInstance& idle) {
    idleReleaseGpu(idle);
    idle.damage.clear();
}

CIdlePassElement::CIdlePassElement(SIdleInstance* idle, const CBox& boxLocal, const Vector2D& center, double effectMs) :
    m_idle(idle), m_boxLocal(boxLocal), m_center(center), m_effectMs(effectMs) {}

std::vector<UP<IPassElement>> CIdlePassElement::draw() {
    // Called from the pass render inside Hyprland: nothing may escape.
    const bool ok = hyprtail::diag::guard("idle-draw", [this] { drawInternal(); });
    if (!ok && m_idle) {
        m_idle->disabled = true;
        idleReleaseGpu(*m_idle); // GL is current here
    }
    return {};
}

void CIdlePassElement::drawInternal() {
    auto&      rd      = g_pHyprRenderer->m_renderData;
    const auto monitor = rd.pMonitor.lock();
    if (!m_idle || m_idle->disabled || !monitor)
        return;

    if (!idlePrepare(*m_idle))
        return;

    if (!m_idle->vao) {
        glGenVertexArrays(1, &m_idle->vao);
        if (!m_idle->vao) {
            idleDisable(*m_idle, "gl:" + m_idle->name, "idle effect disabled: glGenVertexArrays returned no name");
            idleReleaseGpu(*m_idle);
            return;
        }
    }

    auto& slot = m_idle->slot;

    // Through Hyprland's program cache, not raw glUseProgram (see trail).
    auto shader = g_pHyprOpenGL->useShader(slot.shader());
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, hyprtail::globalProjection(monitor).getMatrix());
    glUniform2f(slot.loc("center"), sc<float>(m_center.x), sc<float>(m_center.y));
    glUniform1f(slot.loc("extentPx"), m_idle->extentPx());
    glUniform1f(slot.loc("radiusPx"), m_idle->radiusPx);
    glUniform1f(slot.loc("idleMs"), sc<float>(m_effectMs));
    glUniform1f(slot.loc("durationMs"), sc<float>(m_idle->durationMs));
    hyprtail::setPaletteUniform(slot.loc("colorSlow"), m_idle->colorSlow);
    hyprtail::setPaletteUniform(slot.loc("colorFast"), m_idle->colorFast);

    // Premultiplied blending through Hyprland's cap-status cache.
    g_pHyprOpenGL->blend(true);

    glBindVertexArray(m_idle->vao);

    // Clip to this element's damage, same pattern as core (OpenGL.cpp:1117-1124).
    rd.damage.forEachRect([&rd](const auto& RECT) {
        g_pHyprOpenGL->scissor(&RECT, rd.transformDamage);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
}
