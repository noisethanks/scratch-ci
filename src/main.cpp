#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>

#include <plugins/PluginAPI.hpp>
#include <render/Renderer.hpp>
#include <pointer/PointerManager.hpp>
#include <managers/SessionLockManager.hpp>
#include <event/EventBus.hpp>
#include <output/Monitor.hpp>
#include <helpers/memory/Memory.hpp>
#include <helpers/time/Time.hpp>
#include <helpers/Color.hpp>
#include <debug/log/Logger.hpp>

#include "TrailPassElement.hpp"

static HANDLE s_handle = nullptr;

// Placeholders, SPEC §3 buffer size and the fade duration/curve are still TBD.
static constexpr size_t TRAIL_CAPACITY = 64;
static constexpr double TRAIL_FADE_MS  = 500.0;

// Logical box -> pixel damage box covering every pixel the draw can touch,
// including partially covered edge pixels: scale, then floor left/top and
// ceil right/bottom. CBox::round() rounds x/y to nearest and derives w/h from
// the rounded origin, so it can drop a partially covered edge column/row.
static CBox outwardPixelBox(const CBox& logical, double scale) {
    const double x1 = std::floor(logical.x * scale);
    const double y1 = std::floor(logical.y * scale);
    const double x2 = std::ceil((logical.x + logical.w) * scale);
    const double y2 = std::ceil((logical.y + logical.h) * scale);
    return CBox{x1, y1, x2 - x1, y2 - y1};
}

static uint64_t            s_frames = 0;
static Time::steady_tp     s_epoch;
static UP<STrailInstance>  s_motionTrail;
static CHyprSignalListener s_renderStageListener;

static double              msSinceEpoch(const Time::steady_tp& tp) {
    return std::chrono::duration<double, std::milli>(tp - s_epoch).count();
}

// Damage one logical, monitor-local box on this monitor: into the current
// frame's render damage and into the monitor's damage ring.
static void damageLocalBox(const PHLMONITOR& pMonitor, const CBox& boxLocal) {
    if (boxLocal.empty() || !boxLocal.overlaps(CBox{{}, pMonitor->m_size}))
        return;

    const CBox px = outwardPixelBox(boxLocal, pMonitor->m_scale);

    // Current frame: beginRender already read and rotated the damage ring
    // (Renderer.cpp:1782-1783), so this frame only sees damage added to the
    // render region directly.
    g_pHyprRenderer->m_renderData.damage.add(px);

    // Damage ring: lands in m_current, read by the next frame's beginRender
    // and rotated into history, so older swapchain buffers (age > 1) also
    // repaint this box. Also schedules that next frame
    // (Monitor.cpp:1157-1158), which is what keeps a fading trail animating
    // with the cursor stationary.
    pMonitor->addDamage(px);
}

// Runs once per renderMonitor, with or without a visible cursor
// (Renderer.cpp:2227), after the cursor and before endRender(), so pass
// elements and render damage added here still reach this frame.
static void onRenderLastMoment(const PHLMONITOR& pMonitor) {
    auto&        inst  = *s_motionTrail;
    const double nowMs = msSinceEpoch(Time::steadyNow());

    // Core keeps drawing the cursor over the lock screen: the cursor is
    // rendered after renderLockscreen with no lock check (Renderer.cpp:2176
    // vs :2212-2216). This deliberately diverges from core: no trail while
    // locked.
    const bool locked = g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();

    // Sample once per frame, insert only on real movement, and only while the
    // cursor is visible: touch/tablet can move a hidden pointer. An existing
    // trail keeps fading after a hide since this listener still runs.
    if (!locked && g_pHyprRenderer->shouldRenderCursor()) {
        const Vector2D cursorPos = Pointer::mgr()->position();
        const SVec2f   pos{sc<float>(cursorPos.x), sc<float>(cursorPos.y)};
        if (inst.ring.empty() || inst.ring.newest().posPx != pos)
            inst.ring.insert(pos, nowMs);
    }

    // Lifecycle (SPEC §6): while anything is visible, damage prev ∪ cur every
    // frame (addDamage keeps frames coming). When the last point has faded,
    // cur is empty: prev is damaged once to clear it, then nothing, and the
    // monitor goes idle.
    const auto visible = locked ? std::nullopt : inst.ring.visibleBounds(nowMs, inst.fadeMs);
    const CBox cur     = visible ? trailBoxLocal(inst, *visible, pMonitor->m_position) : CBox{};

    auto&      ms = inst.monState[pMonitor.get()];
    damageLocalBox(pMonitor, ms.prevBoxLocal);
    damageLocalBox(pMonitor, cur);
    ms.prevBoxLocal = cur;

    if (cur.empty() || !cur.overlaps(CBox{{}, pMonitor->m_size}))
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CTrailPassElement>(&inst, cur, nowMs));
}

static void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_LAST_MOMENT || !g_pHyprRenderer || !s_motionTrail)
        return;

    const auto pMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!pMonitor || pMonitor->isMirror())
        return;

    ++s_frames;
    if (s_frames <= 3 || s_frames % 600 == 0)
        Log::logger->log(Log::INFO, "[hyprtail-s4] render stage #{} monitor={}", s_frames, pMonitor->m_name);

    onRenderLastMoment(pMonitor);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    s_handle = handle;

    // ABI version check — must match the compositor we were compiled against.
    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash     = __hyprland_api_get_client_hash();
    if (compositorHash != clientHash) {
        HyprlandAPI::addNotification(handle,
            "[hyprtail-s4] version mismatch — recompile against running Hyprland",
            CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 8000);
        throw std::runtime_error(
            std::format("[hyprtail-s4] version mismatch: built={} running={}",
                        clientHash, compositorHash));
    }

    s_epoch       = Time::steadyNow();
    s_motionTrail = makeUnique<STrailInstance>(TRAIL_CAPACITY, TRAIL_FADE_MS);

    // Replaces the stage 1-3 hook on renderSoftwareCursorsFor, which isn't
    // called while the cursor is hidden (Renderer.cpp:2212-2216, 2976-2978)
    // and would freeze a trail mid-fade.
    s_renderStageListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });

    Log::logger->log(Log::INFO, "[hyprtail-s4] loaded — stage 4 time-based fade");
    HyprlandAPI::addNotification(handle,
        "[hyprtail-s4] loaded — dots should fade out over time",
        CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 5000);

    // Damage the primary monitor to schedule an immediate first frame.
    if (g_pHyprRenderer) {
        if (auto m = g_pHyprRenderer->m_mostHzMonitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    return {"hyprtail-stage4", "Stage 4: time-based fade, damage follows the trail lifecycle", "dev", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    Log::logger->log(Log::INFO, "[hyprtail-s4] unloading after {} render stages", s_frames);
    s_frames = 0;

    // Stop callbacks first so nothing queues new elements during teardown.
    s_renderStageListener.reset();

    // Remove any queued elements before GPU resources go away so draw()
    // can't run against a deleted VAO/program.
    if (g_pHyprRenderer)
        g_pHyprRenderer->m_renderPass.removeAllOfType("CTrailPassElement");

    if (s_motionTrail) {
        trailInstanceCleanup(*s_motionTrail);
        s_motionTrail.reset();
    }

    Log::logger->log(Log::INFO, "[hyprtail-s4] unloaded");
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}
