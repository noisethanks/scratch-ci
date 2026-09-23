#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <plugins/PluginAPI.hpp>
#include <plugins/HookSystem.hpp>
#include <render/Renderer.hpp>
#include <pointer/PointerManager.hpp>
#include <managers/SessionLockManager.hpp>
#include <event/EventBus.hpp>
#include <state/MonitorState.hpp>
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

typedef void (*origRenderSoftwareCursorsFor)(void*, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool, bool);

// Which render each monitor is in, and which render the cursor hook already
// ran the lifecycle for. Keyed to a per-monitor serial bumped at RENDER_BEGIN
// rather than a boolean, so a missed clear can never make a later render skip
// its lifecycle: the fallback runs unless the hook ran for *this* render.
struct SMonitorFrame {
    uint64_t renderSerial  = 0;
    uint64_t handledSerial = UINT64_MAX; // never equal before the first hook run
};

static uint64_t                                              s_frames = 0;
static Time::steady_tp                                       s_epoch;
static UP<STrailInstance>                                    s_motionTrail;
static CFunctionHook*                                        s_cursorHook = nullptr;
static CHyprSignalListener                                   s_renderStageListener;
static CHyprSignalListener                                   s_mouseMoveListener;
static std::unordered_map<Monitor::CMonitor*, SMonitorFrame> s_monFrame;

static double                                                msSinceEpoch(const Time::steady_tp& tp) {
    return std::chrono::duration<double, std::milli>(tp - s_epoch).count();
}

static bool sessionLocked() {
    return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
}

// Damage one logical, monitor-local box on this monitor: into the current
// frame's render damage and into the monitor's damage ring. Only valid inside
// a render of pMonitor.
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

// Per-render trail lifecycle for one monitor: sample/insert, damage prev ∪
// cur, add the pass element. Runs exactly once per render of pMonitor, from
// the cursor hook when the cursor is drawn, else from RENDER_LAST_MOMENT.
static void runTrailLifecycle(const PHLMONITOR& pMonitor) {
    auto&        inst  = *s_motionTrail;
    const double nowMs = msSinceEpoch(Time::steadyNow());

    // Core keeps drawing the cursor over the lock screen: the cursor is
    // rendered after renderLockscreen with no lock check (Renderer.cpp:2176
    // vs :2212-2216). This deliberately diverges from core: no trail while
    // locked.
    const bool locked = sessionLocked();

    // Sample once per frame, insert only on real movement, and only while the
    // cursor is visible: touch/tablet can move a hidden pointer. An existing
    // trail keeps fading after a hide since the fallback still runs.
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

// Draw order (SPEC §7): renderMonitor adds the cursor texture inside
// renderSoftwareCursorsFor (Renderer.cpp:2216), after windows, layers, lock
// screen, IME and overlays, and before the DPMS overlay and RENDER_LAST_MOMENT.
// Pass elements draw in add order (Pass.cpp:27-29), so running the lifecycle
// *before* the original puts the trail directly beneath the software cursor.
// With a hardware cursor the original draws nothing and the cursor plane is
// above the composited frame anyway.
static void hkRenderSoftwareCursorsFor(void* thisptr, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos, bool screencopy,
                                       bool forceRender) {
    // Screenshare calls this with fake damage (ScreenshareFrame.cpp:312, :355).
    // Whether the trail shows up in recordings is undecided, stay out for now.
    if (!screencopy && g_pHyprRenderer && s_motionTrail && pMonitor && !pMonitor->isMirror()) {
        auto& mf = s_monFrame[pMonitor.get()];
        runTrailLifecycle(pMonitor);
        mf.handledSerial = mf.renderSerial;
    }

    (*(origRenderSoftwareCursorsFor)s_cursorHook->m_original)(thisptr, pMonitor, now, damage, overridePos, screencopy, forceRender);
}

static void onRenderStage(eRenderStage stage) {
    if ((stage != RENDER_BEGIN && stage != RENDER_LAST_MOMENT) || !g_pHyprRenderer || !s_motionTrail)
        return;

    const auto pMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!pMonitor || pMonitor->isMirror())
        return;

    auto& mf = s_monFrame[pMonitor.get()];

    // RENDER_BEGIN (Renderer.cpp:2160) opens a new render of this monitor.
    // No return path between it, the cursor call (:2216) and
    // RENDER_LAST_MOMENT (:2227).
    if (stage == RENDER_BEGIN) {
        ++mf.renderSerial;
        return;
    }

    ++s_frames;
    if (s_frames <= 3 || s_frames % 600 == 0)
        Log::logger->log(Log::INFO, "[hyprtail-s4] render #{} monitor={} via {}", s_frames, pMonitor->m_name,
                         mf.handledSerial == mf.renderSerial ? "cursor hook" : "last-moment fallback");

    // Fallback: the cursor hook didn't run for this render, i.e. the cursor is
    // hidden (Renderer.cpp:2212). Nothing is drawn above us then, so order
    // doesn't matter (except the DPMS overlay, rare).
    if (mf.handledSerial != mf.renderSerial)
        runTrailLifecycle(pMonitor);
}

// Hardware cursors: moving the cursor plane may not trigger a render at all
// (Aquamarine's Wayland backend moveCursor is a no-op; DRM schedules one).
// Damage a small box at the new point on each monitor it touches so a render
// happens, where the normal lifecycle then samples and damages the trail.
// Position is already updated when this fires (InputManager.cpp:155, :269).
static void onMouseMove() {
    if (!s_motionTrail || sessionLocked())
        return;

    const Vector2D pos = Pointer::mgr()->position();
    const double   r   = s_motionTrail->radiusPx;

    for (const auto& m : State::monitorState()->monitors()) {
        if (!m || !m->m_enabled || m->isMirror())
            continue;

        // Same check core uses to not break fullscreen VRR (Monitor.cpp:1161-1169).
        if (m->shouldSkipScheduleFrameOnMouseEvent())
            continue;

        const CBox local{pos.x - r - m->m_position.x, pos.y - r - m->m_position.y, 2.0 * r, 2.0 * r};
        if (!local.overlaps(CBox{{}, m->m_size}))
            continue;

        m->addDamage(outwardPixelBox(local, m->m_scale));
    }
}

// Extract function address from a non-virtual member function pointer.
// Uses the Itanium C++ ABI layout: {ptr, adj}; virtual bit is the LSB of ptr.
template <typename T>
static void* pmf_address(T pmf) {
    struct PMF {
        uintptr_t ptr;
        ptrdiff_t adj;
    };
    static_assert(sizeof(T) == sizeof(PMF), "unexpected PMF size");
    auto rep = std::bit_cast<PMF>(pmf);
    if (rep.ptr & 0x01)
        throw std::runtime_error("[hyprtail-s4] unexpected virtual function pointer");
    return reinterpret_cast<void*>(rep.ptr);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    s_handle = handle;

    // ABI version check — must match the compositor we were compiled against.
    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash     = __hyprland_api_get_client_hash();
    if (compositorHash != clientHash) {
        HyprlandAPI::addNotification(handle, "[hyprtail-s4] version mismatch — recompile against running Hyprland", CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 8000);
        throw std::runtime_error(std::format("[hyprtail-s4] version mismatch: built={} running={}", clientHash, compositorHash));
    }

    s_epoch       = Time::steadyNow();
    s_motionTrail = makeUnique<STrailInstance>(TRAIL_CAPACITY, TRAIL_FADE_MS);

    // Cursor hook for draw order. The host (LTO) build calls this out of line
    // from renderMonitor, so the hook fires there too.
    void* target = pmf_address(&Pointer::CPointerManager::renderSoftwareCursorsFor);
    s_cursorHook = HyprlandAPI::createFunctionHook(handle, target, reinterpret_cast<void*>(&hkRenderSoftwareCursorsFor));
    if (!s_cursorHook || !s_cursorHook->hook()) {
        HyprlandAPI::addNotification(handle, "[hyprtail-s4] hook failed", CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 5000);
        s_motionTrail.reset();
        throw std::runtime_error("[hyprtail-s4] could not hook renderSoftwareCursorsFor");
    }

    // Serial at RENDER_BEGIN, fallback lifecycle at RENDER_LAST_MOMENT. The
    // cursor hook isn't called while the cursor is hidden
    // (Renderer.cpp:2212-2216, 2976-2978), which would freeze a trail mid-fade.
    s_renderStageListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });

    s_mouseMoveListener = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { onMouseMove(); });

    Log::logger->log(Log::INFO, "[hyprtail-s4] loaded — trail beneath cursor");
    HyprlandAPI::addNotification(handle, "[hyprtail-s4] loaded — trail should draw beneath the cursor", CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 5000);

    // Damage the primary monitor to schedule an immediate first frame.
    if (g_pHyprRenderer) {
        if (auto m = g_pHyprRenderer->m_mostHzMonitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    return {"hyprtail-stage4", "Stage 4: time-based fade, trail beneath the cursor", "dev", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    Log::logger->log(Log::INFO, "[hyprtail-s4] unloading after {} renders", s_frames);
    s_frames = 0;

    // Stop callbacks first so nothing queues new elements during teardown.
    s_mouseMoveListener.reset();
    s_renderStageListener.reset();
    if (s_cursorHook) {
        s_cursorHook->unhook();
        s_cursorHook = nullptr;
    }

    // Remove any queued elements before GPU resources go away so draw()
    // can't run against a deleted VAO/program.
    if (g_pHyprRenderer)
        g_pHyprRenderer->m_renderPass.removeAllOfType("CTrailPassElement");

    if (s_motionTrail) {
        trailInstanceCleanup(*s_motionTrail);
        s_motionTrail.reset();
    }
    s_monFrame.clear();

    Log::logger->log(Log::INFO, "[hyprtail-s4] unloaded");
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}
