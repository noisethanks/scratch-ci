#pragma once

#include <optional>
#include <unordered_map>

#include <helpers/memory/Memory.hpp>
#include <helpers/math/Math.hpp>
#include <helpers/Color.hpp>
#include <desktop/DesktopTypes.hpp>
#include <render/Shader.hpp>

namespace Monitor {
    class CMonitor;
}

// Render-side helpers shared by the trail and the idle slot.
namespace hyprtail {
    // Logical box -> pixel damage box covering every pixel the draw can touch,
    // including partially covered edge pixels: scale, then floor left/top and
    // ceil right/bottom. CBox::round() rounds x/y to nearest and derives w/h
    // from the rounded origin, so it can drop a partially covered edge
    // column/row.
    CBox outwardPixelBox(const CBox& logical, double scale);

    // Damage a logical, monitor-local box. Inside a render of pMonitor: into
    // the current frame's render damage and the damage ring (which also
    // schedules the next frame). Clipped to the monitor; empty, off-monitor
    // and non-finite boxes are ignored.
    void damageInRender(const PHLMONITOR& pMonitor, const CBox& boxLocal);

    // Outside a render: into the damage ring only, which schedules a frame.
    // Same clipping.
    void damageOutsideRender(const PHLMONITOR& pMonitor, const CBox& boxLocal);

    // Per-monitor damage lifecycle for one drawable (SPEC section 6): each
    // render, damage the previous box and the current one, then remember the
    // current one. An empty current box clears the last drawn one exactly
    // once, after which nothing is damaged and the monitor can go idle.
    //
    // Returns whether the caller may draw this frame. If the frame has no
    // render damage at all, renderMonitor skipped the workspace
    // (Renderer.cpp:2166), so nothing underneath would be repainted and an
    // element drawn now would land on stale content. Then only the damage
    // ring is damaged (scheduling a proper frame) and it returns false.
    class CMonitorDamage {
      public:
        // Inside a render of pMonitor.
        bool update(const PHLMONITOR& pMonitor, const CBox& curLocal);

        // Outside a render: damage the last drawn box again (ring only), so
        // the next render repaints it, e.g. after the drawable was cleared.
        // The box stays remembered; that render's update() handles it.
        void damagePrev(const PHLMONITOR& pMonitor);

        // Drop a monitor's entry. On monitor removal, so a new CMonitor
        // allocated at the same address starts clean.
        void forget(const Monitor::CMonitor* pMonitor);

        // Box last drawn on the monitor (logical, monitor-local); empty once
        // cleared, nullopt if never rendered there.
        std::optional<CBox> prev(const Monitor::CMonitor* pMonitor) const;

        void clear();

      private:
        std::unordered_map<Monitor::CMonitor*, CBox> m_prev; // logical, monitor-local
    };

    // Global layout px -> clip space for the monitor being rendered.
    Mat3x3 globalProjection(const PHLMONITOR& pMonitor);

    // Palette uniform: the sRGB color converted to the current framebuffer's
    // color space (getConvertedColor), alpha passed through. Inside a render.
    void setPaletteUniform(GLint loc, const CHyprColor& color);
}
