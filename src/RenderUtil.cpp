#include "RenderUtil.hpp"

#include <cmath>

#include <render/Renderer.hpp>
#include <output/Monitor.hpp>

namespace hyprtail {
    CBox outwardPixelBox(const CBox& logical, double scale) {
        const double x1 = std::floor(logical.x * scale);
        const double y1 = std::floor(logical.y * scale);
        const double x2 = std::ceil((logical.x + logical.w) * scale);
        const double y2 = std::ceil((logical.y + logical.h) * scale);
        return CBox{x1, y1, x2 - x1, y2 - y1};
    }

    static bool onMonitor(const PHLMONITOR& pMonitor, const CBox& boxLocal) {
        return !boxLocal.empty() && boxLocal.overlaps(CBox{{}, pMonitor->m_size});
    }

    void damageInRender(const PHLMONITOR& pMonitor, const CBox& boxLocal) {
        if (!onMonitor(pMonitor, boxLocal))
            return;

        const CBox px = outwardPixelBox(boxLocal, pMonitor->m_scale);

        // Current frame: beginRender already read and rotated the damage ring
        // (Renderer.cpp:1782-1783), so this frame only sees damage added to
        // the render region directly.
        g_pHyprRenderer->m_renderData.damage.add(px);

        // Damage ring: lands in m_current, read by the next frame's
        // beginRender and rotated into history, so older swapchain buffers
        // (age > 1) also repaint this box. Also schedules that next frame
        // (Monitor.cpp:1157-1158; m_pendingFrame during a render,
        // :1132-1133), which keeps an animation going with the pointer still.
        pMonitor->addDamage(px);
    }

    void damageOutsideRender(const PHLMONITOR& pMonitor, const CBox& boxLocal) {
        if (!onMonitor(pMonitor, boxLocal))
            return;
        pMonitor->addDamage(outwardPixelBox(boxLocal, pMonitor->m_scale));
    }

    bool CMonitorDamage::update(const PHLMONITOR& pMonitor, const CBox& curLocal) {
        auto& prev = m_prev[pMonitor.get()];

        if (g_pHyprRenderer->m_renderData.damage.empty()) {
            // Workspace not rendered this frame: repaint next frame instead.
            // prev stays, so the next update damages it again.
            damageOutsideRender(pMonitor, prev);
            damageOutsideRender(pMonitor, curLocal);
            return false;
        }

        damageInRender(pMonitor, prev);
        damageInRender(pMonitor, curLocal);
        prev = curLocal;
        return true;
    }

    void CMonitorDamage::clear() {
        m_prev.clear();
    }

    Mat3x3 globalProjection(const PHLMONITOR& pMonitor) {
        // Global layout px -> monitor-local px -> clip. projectBoxToTarget
        // takes a pixel-space box (Renderer.cpp:1842-1846); projectBox maps p
        // to pos + size * p (hyprutils Mat3x3.cpp:68-90), so this box encodes
        // (p - monitorPos) * scale. Transform passed explicitly as NORMAL:
        // getBoxProjection otherwise defaults to the inverted monitor
        // transform applied *inside* the box (Renderer.cpp:1836-1840), which
        // would rotate this pseudo-box. Monitor rotation stays in
        // targetProjection (Renderer.cpp:1828, Monitor.cpp:1752-1757).
        const double s = pMonitor->m_scale;
        return g_pHyprRenderer->projectBoxToTarget(CBox{-pMonitor->m_position.x * s, -pMonitor->m_position.y * s, s, s}, HYPRUTILS_TRANSFORM_NORMAL);
    }

    void setPaletteUniform(GLint loc, const CHyprColor& color) {
        if (loc < 0)
            return;
        // Same as core's solid colors (e.g. borders, OpenGL.cpp:2371):
        // convert opaque into the current framebuffer's image description
        // (Renderer.cpp:3427-3447), pass alpha separately. Cached in core.
        const auto conv = g_pHyprRenderer->getConvertedColor(color.stripA());
        glUniform4f(loc, static_cast<float>(conv.r), static_cast<float>(conv.g), static_cast<float>(conv.b), static_cast<float>(color.a));
    }
}
