#include <algorithm>
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
#include <plugins/PluginSystem.hpp>
#include <config/lua/ConfigManager.hpp>
#include <render/Renderer.hpp>
#include <pointer/PointerManager.hpp>
#include <pointer/PointerController.hpp>
#include <managers/input/InputManager.hpp>
#include <managers/fullscreen/FullscreenController.hpp>
#include <managers/SessionLockManager.hpp>
#include <event/EventBus.hpp>
#include <state/MonitorState.hpp>
#include <output/Monitor.hpp>
#include <helpers/memory/Memory.hpp>
#include <helpers/time/Time.hpp>
#include <helpers/Color.hpp>
#include <debug/log/Logger.hpp>

#include "Config.hpp"
#include "rev.hpp"
#include "Diagnostics.hpp"
#include "FileWatch.hpp"
#include "IdlePassElement.hpp"
#include "RenderUtil.hpp"
#include "TrailPassElement.hpp"

#include <wayland-server-core.h>
#include <Compositor.hpp>

using hyprtail::diag::eSeverity;

static HANDLE s_handle = nullptr;


typedef void (*origRenderSoftwareCursorsFor)(void*, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool, bool);
typedef void (*origControllerWarpTo)(const void*, const Vector2D&, bool);


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
static UP<SIdleInstance>                                     s_idle;
static wl_event_source*                                      s_idleTimer = nullptr;
static CFunctionHook*                                        s_cursorHook = nullptr;
static CFunctionHook*                                        s_warpHook   = nullptr;
static CHyprSignalListener                                   s_renderStageListener;
static CHyprSignalListener                                   s_mouseMoveListener;
static CHyprSignalListener                                   s_workspaceActiveListener;
static CHyprSignalListener                                   s_specialActiveListener;
static CHyprSignalListener                                   s_workspaceMovedListener;
static CHyprSignalListener                                   s_configReloadListener;
static CHyprSignalListener                                   s_monitorRemovedListener;
static CHyprSignalListener                                   s_monitorDestroyListener;
static CHyprSignalListener                                   s_layoutChangedListener;
static hyprtail::cfg::SValues                                s_config;
static hyprtail::CFileWatch                                  s_fileWatch;
static std::unordered_map<Monitor::CMonitor*, SMonitorFrame> s_monFrame;
// Each enabled monitor's logical box at the last layout check, to tell a real
// layout change from an arrange() that changed nothing (onLayoutChanged).
static std::unordered_map<Monitor::CMonitor*, CBox>          s_layout;

static double                                                msSinceEpoch(const Time::steady_tp& tp) {
    return std::chrono::duration<double, std::milli>(tp - s_epoch).count();
}

static bool sessionLocked() {
    return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
}

static bool cursorHidden() {
    return g_pHyprRenderer && !g_pHyprRenderer->shouldRenderCursor();
}

// A pointer constraint (lock or confine) means the client owns the pointer:
// games, mostly. No trail while one is active (SPEC §7).
static bool pointerConstrained() {
    return g_pInputManager && g_pInputManager->isConstrained();
}


// Per-render trail lifecycle for one monitor: sample/insert, damage prev ∪
// cur, add the pass element. Runs exactly once per render of pMonitor, from
// the cursor hook when the cursor is drawn, else from RENDER_LAST_MOMENT.
static void runTrailLifecycle(const PHLMONITOR& pMonitor) {
    auto&        inst  = *s_motionTrail;
    const double nowMs = msSinceEpoch(Time::steadyNow());

    // GL is current here: compile a pending shader (config reload, file
    // change) or fall back to the built-in one. May disable the instance.
    trailPrepare(inst);

    // Core keeps drawing the cursor over the lock screen: the cursor is
    // rendered after renderLockscreen with no lock check (Renderer.cpp:2176
    // vs :2212-2216). This deliberately diverges from core: no trail while
    // locked.
    const bool locked = sessionLocked();

    // A disabled instance (shader/GL failure, exception) behaves like a fully
    // faded one: no inserts, last box cleared once below, then idle.
    const bool off = locked || inst.disabled;

    // Sample once per frame, insert only on real movement (at least
    // minSpacingPx from the newest node). The trail follows the pointer
    // whether or not the cursor is shown (hidden by timeout / key press,
    // cursor:invisible): it's decoupled from cursor visibility (SPEC §7).
    //
    // Breaks (SPEC §7): lock and workspace events (onContentChanged) break
    // unconditionally; so does a pointer constraint, during which nothing is
    // inserted. Warps break unless interpolateWarps (hkControllerWarpTo).
    if (off || pointerConstrained())
        inst.pendingBreak = true;
    else {
        const Vector2D cursorPos = Pointer::mgr()->position();
        const SVec2f   pos{sc<float>(cursorPos.x), sc<float>(cursorPos.y)};

        bool           insert = inst.ring.empty();
        if (!insert) {
            const auto& newest = inst.ring.newest().posPx;
            insert             = std::hypot(pos.x - newest.x, pos.y - newest.y) >= inst.minSpacingPx;
        }

        if (insert) {
            inst.ring.insert(pos, nowMs, inst.pendingBreak);
            inst.pendingBreak = false;
        }
    }

    // Lifecycle (SPEC §6): while anything is visible, damage prev ∪ cur every
    // frame (addDamage keeps frames coming). When the last point has faded,
    // cur is empty: prev is damaged once to clear it, then nothing, and the
    // monitor goes idle.
    const auto visible = off ? std::nullopt : inst.ring.visibleBounds(nowMs, inst.fadeMs);
    const CBox cur     = visible ? trailBoxLocal(inst, *visible, pMonitor->m_position) : CBox{};

    if (!inst.damage.update(pMonitor, cur))
        return; // workspace not rendered this frame, see CMonitorDamage

    if (cur.empty() || !cur.overlaps(CBox{{}, pMonitor->m_size}))
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CTrailPassElement>(&inst, cur, nowMs));
}

// ---------------------------------------------------------------- idle slot

// (Re)start the wait for the idle effect: the timer fires delayMs after the
// last pointer motion and makes a render happen there.
static void armIdleTimer() {
    if (!s_idleTimer || !s_idle || !s_idle->enabled || s_idle->disabled)
        return;
    wl_event_source_timer_update(s_idleTimer, std::max(1, sc<int>(std::ceil(s_idle->delayMs))));
}

// Pointer motion from any source: pointer events, warps, or a position change
// noticed during a render. Restarts the idle wait.
static void noteMotion(const Vector2D& pos, double nowMs) {
    if (!s_idle || pos == s_idle->lastPos)
        return;
    s_idle->lastPos      = pos;
    s_idle->lastMotionMs = nowMs;
    armIdleTimer();
}

// Whether the idle effect may show at all right now (besides timing).
static bool idleAllowed(const SIdleInstance& idle) {
    // Marks where the cursor is: drawing around a hidden cursor defeats the
    // hide unless the user asked for it (SPEC section 7).
    return idle.enabled && !idle.disabled && !sessionLocked() && !pointerConstrained() && (idle.whenHidden || !cursorHidden());
}

// Per-render idle lifecycle for one monitor, same shape as the trail's.
static void runIdleLifecycle(const PHLMONITOR& pMonitor) {
    auto&        idle  = *s_idle;
    const double nowMs = msSinceEpoch(Time::steadyNow());

    const Vector2D pos = Pointer::mgr()->position();
    noteMotion(pos, nowMs);

    std::optional<double> effectMs;
    if (idleAllowed(idle) && idlePrepare(idle))
        effectMs = idleEffectMs(idle, nowMs);

    // While shown, damage every frame (keeps the animation rendering); when it
    // ends (duration over, motion, hide), clear once, then idle.
    const CBox cur = effectMs ? idleBoxLocal(idle, pos, pMonitor->m_position) : CBox{};
    if (!idle.damage.update(pMonitor, cur))
        return;

    if (cur.empty() || !cur.overlaps(CBox{{}, pMonitor->m_size}))
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CIdlePassElement>(&idle, cur, pos, *effectMs));
}

// Timer fired: the pointer has been still for delayMs. Damage the idle square
// (not just scheduleFrame: a frame without damage skips the workspace, see
// CMonitorDamage) so a render happens and the lifecycle starts the effect.
static int onIdleTimer(void*) {
    hyprtail::diag::guard("idle-timer", [] {
        if (!s_idle || !idleAllowed(*s_idle))
            return;
        for (const auto& m : State::monitorState()->monitors()) {
            if (!m || !m->m_enabled || m->isMirror())
                continue;
            hyprtail::damageOutsideRender(m, idleBoxLocal(*s_idle, s_idle->lastPos, m->m_position));
        }
    });
    return 0;
}

// Lifecycle behind an exception guard. Runs inside a render (GL current), so
// on failure the instance is disabled and its GPU resources released here.
static void runTrailLifecycleGuarded(const PHLMONITOR& pMonitor, std::string_view where) {
    if (!hyprtail::diag::guard(where, [&] { runTrailLifecycle(pMonitor); })) {
        s_motionTrail->disabled = true;
        trailReleaseGpu(*s_motionTrail);
    }
    // After the trail, so the idle effect draws above it (both beneath the
    // cursor). Guarded separately: one failing doesn't take the other down.
    if (s_idle && !hyprtail::diag::guard(std::format("{}-idle", where), [&] { runIdleLifecycle(pMonitor); })) {
        s_idle->disabled = true;
        idleReleaseGpu(*s_idle);
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

// Programmatic warps (dispatchers, layouts, focus changes) go through here
// (PointerController.cpp:16-29). With interpolateWarps off, a warp starts a
// new segment; on, it connects (straight sweep). Only that decision is ours;
// the original always runs, unwrapped.
// Coverage gap: warp sites that call CPointerManager::warpTo directly bypass
// this and always connect (PointerWarp.cpp:76, InputCapture.cpp:206,
// InputManager.cpp:2248, WorkspacePlacementController.cpp:356).
// Pointer constraints also warp through here (locked: back to the hint on
// every motion, InputManager.cpp:318-320; confined: clamped at the region
// edge, :306). Those are ignored: no trail while constrained anyway.
static void hkControllerWarpTo(const void* thisptr, const Vector2D& pos, bool force) {
    Vector2D from;
    hyprtail::diag::guard("warp-hook-pre", [&] { from = Pointer::mgr()->position(); });

    (*(origControllerWarpTo)s_warpHook->m_original)(thisptr, pos, force);

    hyprtail::diag::guard("warp-hook", [&] {
        // Actual result, not the target: with cursor:no_warps nothing moves.
        const Vector2D to = Pointer::mgr()->position();
        noteMotion(to, msSinceEpoch(Time::steadyNow()));

        if (!s_motionTrail || s_motionTrail->interpolateWarps || pointerConstrained())
            return;
        if (to != from)
            s_motionTrail->pendingBreak = true;
    });
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
// Whether to skip scheduling a render for pointer motion on this monitor.
// Core's check (Monitor.cpp:1161-1169) is
//   (!shouldRenderCursor || noBreak) && adaptiveSync
// With the cursor shown that's only the fullscreen no-break case, so use it
// as is. With the cursor hidden it would skip on every adaptive-sync output,
// fullscreen or not (core has no cursor to draw then), but the trail still
// follows the pointer. Keep only the fullscreen part: skip when adaptive sync
// is on and the monitor has a fullscreen window, regardless of
// cursor:no_break_fs_vrr, so fullscreen VRR is never broken.
static bool skipMotionFrame(const PHLMONITOR& m) {
    if (!cursorHidden())
        return m->shouldSkipScheduleFrameOnMouseEvent();

    return m->m_output && m->m_output->state->state().adaptiveSync && Fullscreen::controller()->getFullscreenWindow(m);
}

static void onMouseMoveInternal() {
    const Vector2D pos = Pointer::mgr()->position();

    // Restart the idle wait; the idle lifecycle ends a showing effect at the
    // next render (the trail damage below makes one happen).
    noteMotion(pos, msSinceEpoch(Time::steadyNow()));

    if (!s_motionTrail || s_motionTrail->disabled || sessionLocked() || pointerConstrained())
        return;

    const double r = s_motionTrail->padPx();

    for (const auto& m : State::monitorState()->monitors()) {
        if (!m || !m->m_enabled || m->isMirror())
            continue;

        // Don't break fullscreen VRR, see skipMotionFrame.
        if (skipMotionFrame(m))
            continue;

        const CBox local{pos.x - r - m->m_position.x, pos.y - r - m->m_position.y, 2.0 * r, 2.0 * r};
        if (!local.overlaps(CBox{{}, m->m_size}))
            continue;

        hyprtail::damageOutsideRender(m, local);
    }
}

// Visible content changed underneath the pointer (SPEC §7): workspace switch,
// special workspace toggle, workspace moved to another monitor. The pointer
// may also have been warped in the same call stack (Monitor.cpp:1497-1507),
// before any render. Don't connect the old trail to the new position: the
// next insert starts a new segment. Every other jump (e.g. a cross-monitor
// focus warp) stays connected, which draws the straight "interpolated" sweep.
static void onContentChanged() {
    hyprtail::diag::guard("content-changed", [] {
        if (s_motionTrail)
            s_motionTrail->pendingBreak = true;
    });
}

// ---------------------------------------------------------------- hotplug

// Logical box of every enabled, non-mirror monitor. allMonitors() rather than
// monitors(): the latter is fixed up by core's own layoutChanged listener
// (MonitorState.cpp:32-46), which may run after ours.
static std::unordered_map<Monitor::CMonitor*, CBox> currentLayout() {
    std::unordered_map<Monitor::CMonitor*, CBox> out;
    for (const auto& m : State::monitorState()->allMonitors()) {
        if (!m || !m->m_enabled || m->isMirror())
            continue;
        out.emplace(m.get(), CBox{m->m_position, m->m_size});
    }
    return out;
}

// Monitor disabled (removed: Monitor.cpp:392-400, also by a monitor rule,
// MonitorRuleManager.cpp:187) or destroyed (destroyMon: MonitorState.cpp:152-156,
// after which the CMonitor can be freed and its address reused). Drops
// everything keyed by it, so a new monitor at the same address starts clean.
// Nothing else refers to the monitor: GL objects are global and pass
// elements live for one render. A trail or idle effect that was on it simply
// stops drawing there; the pointer is warped off it (Monitor.cpp:478-480),
// which breaks the trail via the warp hook unless interpolateWarps.
static void onMonitorGone(const PHLMONITOR& pMonitor) {
    hyprtail::diag::guard("monitor-removed", [&] {
        if (!pMonitor)
            return;
        Monitor::CMonitor* m = pMonitor.get();
        s_monFrame.erase(m);
        s_layout.erase(m);
        if (s_motionTrail)
            s_motionTrail->damage.forget(m);
        if (s_idle)
            s_idle->damage.forget(m);
    });
}

// Monitors arranged (MonitorLayoutController.cpp:70, Monitor.cpp:1399,
// MonitorRuleManager.cpp:204). Trail points are global coordinates: if a
// monitor moved, changed size (mode, scale, transform) or appeared, old
// points could land somewhere else on screen until they fade, so the trail
// is dropped. Only then: arrange() also runs on config reloads that change
// nothing, and those must not clear the trail. A monitor only disappearing
// needs nothing: its points have no monitor to draw on.
static void onLayoutChanged() {
    hyprtail::diag::guard("layout-changed", [] {
        auto       now     = currentLayout();
        const bool changed = std::ranges::any_of(now, [](const auto& entry) {
            const auto it = s_layout.find(entry.first);
            return it == s_layout.end() || !(it->second == entry.second);
        });
        s_layout = std::move(now);

        if (!changed || !s_motionTrail)
            return;

        auto& inst = *s_motionTrail;
        inst.ring.clear();
        inst.pendingBreak = true;
        // Repaint what was drawn last: monitor-local boxes, still where the
        // trail is on screen. The next render of each monitor sees an empty
        // trail, damages that box once more and goes idle.
        for (const auto& m : State::monitorState()->allMonitors()) {
            if (m && m->m_enabled && !m->isMirror())
                inst.damage.damagePrev(m);
        }
        Log::logger->log(Log::INFO, "[hyprtail] monitor layout changed, trail cleared");
    });
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

static void removeHook(CFunctionHook*& hook) {
    if (!hook)
        return;
    hook->unhook();
    HyprlandAPI::removeFunctionHook(s_handle, hook);
    hook = nullptr;
}

// Undo everything PLUGIN_INIT set up. Also used when init fails: Hyprland then
// ejects the plugin without calling PLUGIN_EXIT (PluginSystem.cpp:147-150,
// :122-125) and dlcloses it, so listeners and deferred callbacks pointing
// into this .so must be gone before the exception leaves PLUGIN_INIT.
static void teardown() noexcept {
    hyprtail::diag::guard("teardown", [] {
        // Stop callbacks first so nothing queues new elements during teardown.
        s_configReloadListener.reset();
        s_fileWatch.shutdown();
        if (s_idleTimer) {
            wl_event_source_remove(s_idleTimer);
            s_idleTimer = nullptr;
        }
        s_workspaceActiveListener.reset();
        s_specialActiveListener.reset();
        s_workspaceMovedListener.reset();
        s_monitorRemovedListener.reset();
        s_monitorDestroyListener.reset();
        s_layoutChangedListener.reset();
        s_mouseMoveListener.reset();
        s_renderStageListener.reset();
        removeHook(s_cursorHook);
        removeHook(s_warpHook);

        // Remove any queued elements before GPU resources go away so draw()
        // can't run against a deleted VAO/program.
        if (g_pHyprRenderer) {
            g_pHyprRenderer->m_renderPass.removeAllOfType("CTrailPassElement");
            g_pHyprRenderer->m_renderPass.removeAllOfType("CIdlePassElement");
        }

        if (s_motionTrail) {
            trailInstanceCleanup(*s_motionTrail);
            s_motionTrail.reset();
        }
        if (s_idle) {
            idleCleanup(*s_idle);
            s_idle.reset();
        }
        s_monFrame.clear();
        s_layout.clear();
        hyprtail::cfg::releaseValues();
    });

    // Last: cancels any pending deferred notification into this .so.
    hyprtail::diag::shutdown();
}

// Returns an active hook, or nullptr with nothing left registered.
static CFunctionHook* installHook(void* target, void* detour) {
    CFunctionHook* hook = target ? HyprlandAPI::createFunctionHook(s_handle, target, detour) : nullptr;
    if (hook && hook->hook())
        return hook;
    if (hook)
        HyprlandAPI::removeFunctionHook(s_handle, hook);
    return nullptr;
}


// Hooks degrade instead of refusing to load: each failure is reported with
// what stops working.
static void installHooks() {
    // Draw order. Without it RENDER_LAST_MOMENT runs the lifecycle every
    // render and the trail draws above the cursor.
    s_cursorHook = installHook(pmf_address(&Pointer::CPointerManager::renderSoftwareCursorsFor), reinterpret_cast<void*>(&hkRenderSoftwareCursorsFor));
    if (!s_cursorHook)
        hyprtail::diag::report(eSeverity::WARN, "hook:cursor",
                               "could not hook CPointerManager::renderSoftwareCursorsFor. Running degraded: the trail draws above the cursor instead of beneath it.");

    // Warp detection for interpolateWarps = false. Without it every warp
    // connects (straight sweep).
    s_warpHook = installHook(pmf_address(&Pointer::CPointerController::warpTo), reinterpret_cast<void*>(&hkControllerWarpTo));
    if (!s_warpHook)
        hyprtail::diag::report(eSeverity::WARN, "hook:warp",
                               "could not hook CPointerController::warpTo. Warps will draw a connecting line instead of breaking the trail.");
}

// Make a render happen soon, so a pending shader compiles (GL is only
// current inside a render) and a changed setting shows.
static void kickRender() {
    if (!g_pHyprRenderer)
        return;
    if (auto m = g_pHyprRenderer->m_mostHzMonitor.lock())
        m->scheduleFrame();
}

// Re-read both slots' shader stages (config path or built-in), resolve
// includes, and queue them for compilation at the next render (CShaderSlot).
// Errors keep the active programs.
static void reloadShaders() {
    std::vector<std::filesystem::path> watch;
    s_motionTrail->slot.reload(s_config.vertexShader, s_config.fragmentShader, watch);
    if (s_idle)
        s_idle->slot.reload(s_config.idleVertexShader, s_config.idleFragmentShader, watch);
    s_fileWatch.setFiles(watch);
    kickRender();
}

// Config values -> instance, on load and every Hyprland config reload.
static void applyConfig() {
    auto& inst = *s_motionTrail;
    s_config   = hyprtail::cfg::read(s_config);

    inst.fadeMs           = s_config.fadeMs;
    inst.widthPx          = s_config.widthPx;
    inst.miterLimit       = s_config.miterLimit;
    inst.minSpacingPx     = s_config.minSpacingPx;
    inst.interpolateWarps = s_config.interpolateWarps;
    inst.damagePaddingPx  = s_config.damagePaddingPx;
    inst.colorSlow        = CHyprColor{s_config.colorSlow};
    inst.colorFast        = CHyprColor{s_config.colorFast};

    auto& idle           = *s_idle;
    const bool wasOn     = idle.enabled;
    idle.enabled         = s_config.idleEnabled;
    idle.delayMs         = s_config.idleDelayMs;
    idle.durationMs      = s_config.idleDurationMs;
    idle.radiusPx        = s_config.idleRadiusPx;
    idle.whenHidden      = s_config.idleWhenHidden;
    idle.damagePaddingPx = s_config.damagePaddingPx;
    idle.colorSlow       = inst.colorSlow;
    idle.colorFast       = inst.colorFast;
    if (idle.enabled && !wasOn)
        armIdleTimer(); // start waiting from now

    // Keeps the newest points; the VBO is reallocated at the next draw
    // (CTrailGpu::ensure), where GL is current.
    if (inst.ring.capacity() != s_config.capacity)
        inst.ring.resize(s_config.capacity);

    reloadShaders();
    kickRender();
}

static PLUGIN_DESCRIPTION_INFO pluginInit() {
    s_epoch       = Time::steadyNow();
    s_config      = {};
    s_motionTrail = makeUnique<STrailInstance>("trail", s_config.capacity);
    s_idle        = makeUnique<SIdleInstance>();
    s_idle->lastPos      = Pointer::mgr()->position();
    s_idle->lastMotionMs = 0.0;

    // Idle timer on Hyprland's event loop (main thread), like the file watch.
    if (g_pCompositor && g_pCompositor->m_wlEventLoop)
        s_idleTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &onIdleTimer, nullptr);
    if (!s_idleTimer)
        hyprtail::diag::report(eSeverity::WARN, "idle-timer", "could not create the idle timer; the idle effect only starts when something else renders");

    // Settings (SPEC section 9). Registered values get their configured value
    // on the reload Hyprland schedules right after loading a plugin
    // (PluginSystem.cpp:135); until then they hold the defaults.
    hyprtail::cfg::registerValues(s_handle);
    s_fileWatch.init([] { hyprtail::diag::guard("shader-file-changed", [] { reloadShaders(); }); });
    applyConfig();
    s_configReloadListener = Event::bus()->m_events.config.reloaded.listen([] { hyprtail::diag::guard("config-reload", [] { applyConfig(); }); });

    // Cursor hook for draw order. The host (LTO) build calls the target out of
    // line from renderMonitor, so the hook fires there too.
    installHooks();

    // Serial at RENDER_BEGIN, fallback lifecycle at RENDER_LAST_MOMENT. The
    // cursor hook isn't called while the cursor is hidden
    // (Renderer.cpp:2212-2216, 2976-2978), which would freeze a trail mid-fade.
    s_renderStageListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });

    s_mouseMoveListener = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { onMouseMove(); });

    // Teleport handling (SPEC §7): break the polyline on content changes.
    s_workspaceActiveListener = Event::bus()->m_events.workspace.active.listen([] { onContentChanged(); });
    s_specialActiveListener   = Event::bus()->m_events.workspace.specialActive.listen([] { onContentChanged(); });
    s_workspaceMovedListener  = Event::bus()->m_events.workspace.moveToMonitor.listen([] { onContentChanged(); });

    // Hotplug (EventBus.hpp:158-169): per-monitor state is keyed by CMonitor*.
    s_layout                 = currentLayout();
    s_monitorRemovedListener = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR m) { onMonitorGone(m); });
    s_monitorDestroyListener = Event::bus()->m_events.monitor.destroyMon.listen([](PHLMONITOR m) { onMonitorGone(m); });
    s_layoutChangedListener  = Event::bus()->m_events.monitor.layoutChanged.listen([] { onLayoutChanged(); });

    Log::logger->log(Log::INFO, "[hyprtail] {} loaded, cursor hook {}, warp hook {}", HYPRTAIL_REV, s_cursorHook ? "active" : "unavailable",
                     s_warpHook ? "active" : "unavailable");
    HyprlandAPI::addNotification(s_handle, std::format("[hyprtail] loaded ({})", HYPRTAIL_REV), CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 3000);

    // Damage the primary monitor to schedule an immediate first frame.
    if (g_pHyprRenderer) {
        if (auto m = g_pHyprRenderer->m_mostHzMonitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    return {"hyprtail", "Cursor trail (ribbon, beneath the cursor)", "dev", "0.1-" HYPRTAIL_REV};
}

// Another hyprtail already loaded (a different build from another path: the
// plugin system only refuses the same path twice, PluginSystem.cpp:72-75).
// Two copies would fight over the config keys and hooks. Loaded plugins are
// CPlugin entries (PluginSystem.hpp:14-25) listed by getAllPlugins()
// (:45, PluginSystem.cpp:260-265). Our own entry is already in the list
// during PLUGIN_INIT, with m_path/m_handle set but m_name still empty (it's
// filled from our return value afterwards, :128-131), so it's excluded by
// handle. Older builds named themselves "hyprtail-stageN".
static const CPlugin* findOtherInstance(HANDLE self) {
    if (!g_pPluginSystem)
        return nullptr;
    for (const CPlugin* p : g_pPluginSystem->getAllPlugins()) {
        if (!p || p->m_handle == self)
            continue;
        if (p->m_name == "hyprtail" || p->m_name.starts_with("hyprtail-stage"))
            return p;
    }
    return nullptr;
}

// Whether this load comes from the Lua config's hl.plugin.load list. Hyprland
// then shows its own "failed to load" notification with our exception text
// (PluginSystem.cpp:229-230); loads through hyprctl or hyprpm show nothing
// (HyprCtl.cpp:1824-1840 only returns the error; hyprpm ignores the reply,
// hyprpm PluginManager.cpp loadUnloadPlugin), so we notify ourselves there.
// The Lua manager's m_registeredPlugins is public (lua/ConfigManager.hpp:131);
// the legacy one keeps its list private, so there we can't tell and notify.
static bool loadedFromLuaConfig(HANDLE self) {
    if (!Config::mgr() || Config::mgr()->type() != Config::CONFIG_LUA || !g_pPluginSystem)
        return false;
    const CPlugin* me = g_pPluginSystem->getPluginByHandle(self);
    if (!me)
        return false;
    const auto& list = static_cast<Config::Lua::CConfigManager*>(Config::mgr().get())->m_registeredPlugins;
    return std::ranges::find(list, me->m_path) != list.end();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    s_handle = handle;

    // Checked before anything else, including diag::init, which would
    // rotate away the other instance's errors.log. Nothing is registered yet, so
    // no teardown is needed: one notification, then a std::exception, which
    // is how a plugin refuses to load (Hyprland catches it and ejects the
    // plugin, PluginSystem.cpp:113-126).
    if (const CPlugin* other = findOtherInstance(handle)) {
        const bool  viaHyprpm = other->m_path.starts_with("/var/cache/hyprpm/");
        const auto  msg       = std::format("another hyprtail ({} {}) is already loaded from {}. Unload it first ({}), then load this one.", other->m_name,
                                            other->m_version, other->m_path,
                                            viaHyprpm ? "hyprpm disable hyprtail, or hyprctl plugin unload " + other->m_path : "hyprctl plugin unload " + other->m_path);
        // Exactly one notification: Hyprland's own for config loads, ours
        // otherwise.
        if (!loadedFromLuaConfig(handle))
            HyprlandAPI::addNotification(handle, "[hyprtail] not loaded: " + msg, CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 15000);
        throw std::runtime_error("[hyprtail] " + msg);
    }

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
