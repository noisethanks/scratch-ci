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

#include "Diagnostics.hpp"
#include "TrailPassElement.hpp"

using hyprtail::diag::eSeverity;

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

    // A disabled instance (shader/GL failure, exception) behaves like a fully
    // faded one: no inserts, last box cleared once below, then idle.
    const bool off = locked || inst.disabled;

    // Sample once per frame, insert only on real movement, and only while the
    // cursor is visible: touch/tablet can move a hidden pointer. An existing
    // trail keeps fading after a hide since the fallback still runs.
    if (!off && g_pHyprRenderer->shouldRenderCursor()) {
        const Vector2D cursorPos = Pointer::mgr()->position();
        const SVec2f   pos{sc<float>(cursorPos.x), sc<float>(cursorPos.y)};
        if (inst.ring.empty() || inst.ring.newest().posPx != pos)
            inst.ring.insert(pos, nowMs);
    }

    // Lifecycle (SPEC §6): while anything is visible, damage prev ∪ cur every
    // frame (addDamage keeps frames coming). When the last point has faded,
    // cur is empty: prev is damaged once to clear it, then nothing, and the
    // monitor goes idle.
    const auto visible = off ? std::nullopt : inst.ring.visibleBounds(nowMs, inst.fadeMs);
    const CBox cur     = visible ? trailBoxLocal(inst, *visible, pMonitor->m_position) : CBox{};

    auto&      ms = inst.monState[pMonitor.get()];
    damageLocalBox(pMonitor, ms.prevBoxLocal);
    damageLocalBox(pMonitor, cur);
    ms.prevBoxLocal = cur;

    if (cur.empty() || !cur.overlaps(CBox{{}, pMonitor->m_size}))
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CTrailPassElement>(&inst, cur, nowMs));
}

// Lifecycle behind an exception guard. Runs inside a render (GL current), so
// on failure the instance is disabled and its GPU resources released here.
static void runTrailLifecycleGuarded(const PHLMONITOR& pMonitor, std::string_view where) {
    if (!hyprtail::diag::guard(where, [&] { runTrailLifecycle(pMonitor); })) {
        s_motionTrail->disabled = true;
        trailReleaseGpu(*s_motionTrail);
    }
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
    // Only our part is guarded; the original always runs, unwrapped, so
    // Hyprland's own behavior is unchanged.
    if (!screencopy && g_pHyprRenderer && s_motionTrail && pMonitor && !pMonitor->isMirror()) {
        hyprtail::diag::guard("cursor-hook", [&] {
            // Marked handled even if the lifecycle fails below, so the fallback
            // doesn't rerun a failing lifecycle in the same render.
            auto& mf         = s_monFrame[pMonitor.get()];
            mf.handledSerial = mf.renderSerial;
            runTrailLifecycleGuarded(pMonitor, "cursor-hook-lifecycle");
        });
    }

    (*(origRenderSoftwareCursorsFor)s_cursorHook->m_original)(thisptr, pMonitor, now, damage, overridePos, screencopy, forceRender);
}

static void onRenderStageInternal(eRenderStage stage) {
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
        Log::logger->log(Log::INFO, "[hyprtail] render #{} monitor={} via {}", s_frames, pMonitor->m_name,
                         mf.handledSerial == mf.renderSerial ? "cursor hook" : "last-moment fallback");

    // Fallback: the cursor hook didn't run for this render, i.e. the cursor is
    // hidden (Renderer.cpp:2212). Nothing is drawn above us then, so order
    // doesn't matter (except the DPMS overlay, rare).
    if (mf.handledSerial != mf.renderSerial)
        runTrailLifecycleGuarded(pMonitor, "last-moment-lifecycle");
}

static void onRenderStage(eRenderStage stage) {
    hyprtail::diag::guard("render-stage", [stage] { onRenderStageInternal(stage); });
}

// Hardware cursors: moving the cursor plane may not trigger a render at all
// (Aquamarine's Wayland backend moveCursor is a no-op; DRM schedules one).
// Damage a small box at the new point on each monitor it touches so a render
// happens, where the normal lifecycle then samples and damages the trail.
// Position is already updated when this fires (InputManager.cpp:155, :269).
static void onMouseMoveInternal() {
    if (!s_motionTrail || s_motionTrail->disabled || sessionLocked())
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

static void onMouseMove() {
    if (!hyprtail::diag::guard("mouse-move", [] { onMouseMoveInternal(); }) && s_motionTrail)
        s_motionTrail->disabled = true; // not in a render: GPU freed at unload
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
        return nullptr; // virtual: no fixed address to hook
    return reinterpret_cast<void*>(rep.ptr);
}

// Undo everything PLUGIN_INIT set up. Also used when init fails: Hyprland then
// ejects the plugin without calling PLUGIN_EXIT (PluginSystem.cpp:147-150,
// :122-125) and dlcloses it, so listeners and deferred callbacks pointing
// into this .so must be gone before the exception leaves PLUGIN_INIT.
static void teardown() noexcept {
    hyprtail::diag::guard("teardown", [] {
        // Stop callbacks first so nothing queues new elements during teardown.
        s_mouseMoveListener.reset();
        s_renderStageListener.reset();
        if (s_cursorHook) {
            s_cursorHook->unhook();
            HyprlandAPI::removeFunctionHook(s_handle, s_cursorHook);
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
    });

    // Last: cancels any pending deferred notification into this .so.
    hyprtail::diag::shutdown();
}

// Hook for draw order. On failure the plugin keeps running in listener-only
// mode (RENDER_LAST_MOMENT runs the lifecycle every render, trail above the
// cursor) and says so, rather than refusing to load.
static void installCursorHook() {
    void* target = pmf_address(&Pointer::CPointerManager::renderSoftwareCursorsFor);
    if (target)
        s_cursorHook = HyprlandAPI::createFunctionHook(s_handle, target, reinterpret_cast<void*>(&hkRenderSoftwareCursorsFor));

    if (s_cursorHook && s_cursorHook->hook())
        return;

    if (s_cursorHook) {
        HyprlandAPI::removeFunctionHook(s_handle, s_cursorHook);
        s_cursorHook = nullptr;
    }

    hyprtail::diag::report(eSeverity::WARN, "hook",
                           "could not hook CPointerManager::renderSoftwareCursorsFor. Running degraded: the trail draws above the cursor instead of beneath it.");
}

static PLUGIN_DESCRIPTION_INFO pluginInit() {
    s_epoch       = Time::steadyNow();
    s_motionTrail = makeUnique<STrailInstance>("trail", TRAIL_CAPACITY, TRAIL_FADE_MS);

    // Cursor hook for draw order. The host (LTO) build calls the target out of
    // line from renderMonitor, so the hook fires there too.
    installCursorHook();

    // Serial at RENDER_BEGIN, fallback lifecycle at RENDER_LAST_MOMENT. The
    // cursor hook isn't called while the cursor is hidden
    // (Renderer.cpp:2212-2216, 2976-2978), which would freeze a trail mid-fade.
    s_renderStageListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });

    s_mouseMoveListener = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { onMouseMove(); });

    Log::logger->log(Log::INFO, "[hyprtail] loaded, hook {}", s_cursorHook ? "active" : "unavailable (degraded draw order)");
    HyprlandAPI::addNotification(s_handle, "[hyprtail] loaded", CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 3000);

    // Damage the primary monitor to schedule an immediate first frame.
    if (g_pHyprRenderer) {
        if (auto m = g_pHyprRenderer->m_mostHzMonitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    return {"hyprtail", "Cursor trail (stage 4 harness: faded dots beneath the cursor)", "dev", "0.1"};
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    s_handle = handle;
    hyprtail::diag::init(handle);

    // Refusing to load is done by throwing. Hyprland only catches
    // std::exception from init (PluginSystem.cpp:121), so nothing else may
    // leave, and teardown must run first. Failures here notify synchronously:
    // the deferred path would be cancelled by teardown before it runs.
    try {
        // ABI version check — must match the compositor we were compiled against.
        const std::string compositorHash = __hyprland_api_get_hash();
        const std::string clientHash     = __hyprland_api_get_client_hash();
        if (compositorHash != clientHash)
            throw std::runtime_error(std::format("version mismatch, recompile against the running Hyprland (built={} running={})", clientHash, compositorHash));

        return pluginInit();
    } catch (const std::exception& e) {
        HyprlandAPI::addNotification(handle, std::format("[hyprtail] failed to load: {}", e.what()), CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 15000);
        teardown();
        throw;
    } catch (...) {
        HyprlandAPI::addNotification(handle, "[hyprtail] failed to load: unknown exception", CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 15000);
        teardown();
        throw std::runtime_error("[hyprtail] unknown exception in init");
    }
}

APICALL EXPORT void PLUGIN_EXIT() {
    hyprtail::diag::guard("exit", [] { Log::logger->log(Log::INFO, "[hyprtail] unloading after {} renders", s_frames); });
    s_frames = 0;
    teardown();
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}
