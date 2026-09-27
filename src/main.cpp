#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <random>
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
#include "Layer.hpp"
#include "LayerPassElement.hpp"
#include "RenderUtil.hpp"
#include "Status.hpp"

#include <wayland-server-core.h>
#include <Compositor.hpp>

using hyprtail::diag::eSeverity;
using hyprtail::shader::eTopology;

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

    // Counters for `hyprctl hyprtail`.
    uint64_t hookRuns = 0, fallbackRuns = 0, draws = 0, emptySkips = 0;
};

static uint64_t                                              s_frames = 0;
static Time::steady_tp                                       s_epoch;
static UP<SPreset>                                           s_preset;
static wl_event_source*                                      s_idleTimer  = nullptr;
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
static SP<SHyprCtlCommand>                                   s_statusCommand;
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

// ---------------------------------------------------------------- layers

// Whether a layer may draw at all right now (visibility aside): it has a
// resolved program, is enabled, and either draws with the cursor hidden or
// the cursor is shown. Path layers default to drawing while hidden (the
// trail is decoupled from cursor visibility, SPEC §7); quad layers don't (an
// idle marker defeats a cursor hide).
static bool layerDrawable(const hyprtail::CLayer& l, bool hidden) {
    return !l.disabled && l.enabledSetting() && l.resolved() && (l.res.drawWhenHidden || !hidden);
}

// Quad layers: shown while the pointer has been still for [start, start +
// duration) (duration 0 = until it moves).
static bool quadInWindow(const hyprtail::CLayer& l, double nowMs) {
    const double still = nowMs - s_preset->lastMotionMs;
    if (still < l.res.startMs)
        return false;
    return l.res.durationMs <= 0.0 || still - l.res.startMs < l.res.durationMs;
}

static CBox quadBoxLocal(const Vector2D& anchor, float extentPx, const Vector2D& monitorPos) {
    const double e = extentPx + 1.0; // +1px antialiasing margin
    return CBox{anchor.x - e - monitorPos.x, anchor.y - e - monitorPos.y, 2.0 * e, 2.0 * e};
}

// (Re)start the wait for quad layers: the timer fires when the earliest one
// is due and makes a render happen there.
static void armIdleTimer() {
    if (!s_idleTimer || !s_preset)
        return;
    double start = std::numeric_limits<double>::infinity();
    for (const auto& l : s_preset->layers) {
        if (!l->disabled && l->enabledSetting() && l->resolved() && l->topology() == eTopology::QUAD)
            start = std::min(start, l->res.startMs);
    }
    if (!std::isfinite(start))
        return;
    wl_event_source_timer_update(s_idleTimer, std::max(1, sc<int>(std::ceil(start))));
}

// Pointer motion from any source: pointer events, warps, or a position change
// noticed during a render. Restarts the stillness wait.
static void noteMotion(const Vector2D& pos, double nowMs) {
    if (!s_preset || pos == s_preset->lastPos)
        return;
    s_preset->lastPos      = pos;
    s_preset->lastMotionMs = nowMs;
    armIdleTimer();
}

// GL is current: compile pending programs (config reload, file change) or
// fall back to the built-in ones, then resolve parameter values. A layer
// whose built-in program fails is disabled.
static void prepareLayers() {
    bool newlyResolved = false;
    for (auto& l : s_preset->layers) {
        if (l->disabled || !l->enabledSetting())
            continue;
        if (const auto error = l->slot.prepare()) {
            l->disabled = true;
            hyprtail::diag::report(eSeverity::ERR, "shader:" + l->name(), std::format("layer {} disabled: {}", l->name(), *error));
            l->slot.release();
            continue;
        }
        const auto before = l->res.generation;
        l->resolve();
        newlyResolved = newlyResolved || l->res.generation != before;
    }
    if (newlyResolved)
        armIdleTimer();
}

// Sample the pointer once per render into the source ring: insert only on
// real movement (at least min_spacing from the newest node). The trail
// follows the pointer whether or not the cursor is shown (SPEC §7).
// Breaks (SPEC §7): lock and workspace events break unconditionally; so
// does a pointer constraint, during which nothing is inserted. Warps break
// unless interpolateWarps (hkControllerWarpTo).
static void sampleSource(double nowMs, bool locked) {
    auto& p = *s_preset;
    if (locked || pointerConstrained()) {
        p.pendingBreak = true;
        return;
    }

    const Vector2D cursorPos = Pointer::mgr()->position();
    const SVec2f   pos{sc<float>(cursorPos.x), sc<float>(cursorPos.y)};

    bool           insert = p.ring.empty();
    if (!insert) {
        const auto& newest = p.ring.newest().posPx;
        insert             = std::hypot(pos.x - newest.x, pos.y - newest.y) >= p.minSpacingPx;
    }

    if (insert) {
        p.ring.insert(pos, nowMs, p.pendingBreak);
        p.pendingBreak = false;
    }
}

// Per-render lifecycle for one monitor: prepare layers, sample the source,
// then for every layer damage prev ∪ cur (SPEC §6) and collect what to
// draw. Runs exactly once per render of pMonitor, from the cursor hook when
// the cursor is drawn, else from RENDER_LAST_MOMENT.
static void runLifecycle(const PHLMONITOR& pMonitor) {
    auto&        p     = *s_preset;
    const double nowMs = msSinceEpoch(Time::steadyNow());

    prepareLayers();

    // Core keeps drawing the cursor over the lock screen (Renderer.cpp:2176
    // vs :2212-2216). This deliberately diverges from core: nothing while
    // locked.
    const bool locked = sessionLocked();
    sampleSource(nowMs, locked);

    const Vector2D pos = Pointer::mgr()->position();
    noteMotion(pos, nowMs);

    const bool                hidden      = cursorHidden();
    const bool                constrained = pointerConstrained();
    auto&                     mf          = s_monFrame[pMonitor.get()];
    std::vector<SLayerDraw>   draws;
    bool                      skipped = false;

    for (auto& lp : p.layers) {
        auto& l      = *lp;
        CBox  cur    = {};
        float extent = 0.F;

        if (!locked && layerDrawable(l, hidden)) {
            extent = layerExtentPx(p, l);
            if (l.topology() == eTopology::PATH) {
                if (const auto b = p.gpuFailed ? std::nullopt : p.ring.visibleBounds(nowMs, l.res.fadeMs))
                    cur = CBox{b->x1 - extent - pMonitor->m_position.x, b->y1 - extent - pMonitor->m_position.y, (b->x2 - b->x1) + 2.0 * extent,
                               (b->y2 - b->y1) + 2.0 * extent};
            } else if (!constrained && quadInWindow(l, nowMs))
                cur = quadBoxLocal(pos, extent, pMonitor->m_position);
        }

        // While visible, damage prev ∪ cur every frame (addDamage keeps frames
        // coming); once empty, prev is damaged once to clear it, then the
        // monitor can go idle.
        if (!l.damage.update(pMonitor, cur)) {
            skipped = true; // workspace not rendered this frame, see CMonitorDamage
            continue;
        }
        if (cur.empty() || !cur.overlaps(CBox{{}, pMonitor->m_size}))
            continue;
        draws.push_back({.layer = &l, .boxLocal = cur, .extentPx = extent});
    }

    if (skipped)
        ++mf.emptySkips;
    if (draws.empty())
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CLayerPassElement>(&p, std::move(draws), nowMs));
    ++mf.draws;
}

// Timer fired: a quad layer's window may have opened. Damage its square (not
// just scheduleFrame: a frame without damage skips the workspace, see
// CMonitorDamage) so a render happens and the lifecycle draws it.
static int onIdleTimer(void*) {
    hyprtail::diag::guard("idle-timer", [] {
        if (!s_preset || sessionLocked() || pointerConstrained())
            return;
        const bool hidden = cursorHidden();
        for (const auto& l : s_preset->layers) {
            if (!layerDrawable(*l, hidden) || l->topology() != eTopology::QUAD)
                continue;
            for (const auto& m : State::monitorState()->monitors()) {
                if (!m || !m->m_enabled || m->isMirror())
                    continue;
                hyprtail::damageOutsideRender(m, quadBoxLocal(s_preset->lastPos, layerExtentPx(*s_preset, *l), m->m_position));
            }
        }
    });
    return 0;
}

// A report batch (load, config reload, shader file change) is complete once
// every shader reload it queued has been compiled or discarded, which only
// happens inside a render (CShaderSlot::prepare). Layers that are off don't
// compile; the batch timeout in diag covers anything else.
static void maybeEndBatch() {
    if (!hyprtail::diag::batchOpen() || !s_preset)
        return;
    const bool settled = std::ranges::all_of(s_preset->layers, [](const auto& l) { return l->disabled || !l->enabledSetting() || !l->slot.hasPending(); });
    if (settled)
        hyprtail::diag::endBatch();
}

// Lifecycle behind an exception guard. Runs inside a render (GL current), so
// on failure every layer is disabled and GPU resources released here.
static void runLifecycleGuarded(const PHLMONITOR& pMonitor, std::string_view where) {
    if (!hyprtail::diag::guard(where, [&] { runLifecycle(pMonitor); })) {
        for (auto& l : s_preset->layers)
            l->disabled = true;
        presetReleaseGpu(*s_preset);
    }
    maybeEndBatch();
}

// Draw order (SPEC §7): renderMonitor adds the cursor texture inside
// renderSoftwareCursorsFor (Renderer.cpp:2216), after windows, layers, lock
// screen, IME and overlays, and before the DPMS overlay and RENDER_LAST_MOMENT.
// Pass elements draw in add order (Pass.cpp:27-29), so running the lifecycle
// *before* the original puts the layers directly beneath the software cursor.
// With a hardware cursor the original draws nothing and the cursor plane is
// above the composited frame anyway.
static void hkRenderSoftwareCursorsFor(void* thisptr, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos, bool screencopy,
                                       bool forceRender) {
    // Screenshare calls this with fake damage (ScreenshareFrame.cpp:312, :355).
    // Stay out (SPEC §13.12). Only our part is guarded; the original always
    // runs, unwrapped, so Hyprland's own behavior is unchanged.
    if (!screencopy && g_pHyprRenderer && s_preset && pMonitor && !pMonitor->isMirror()) {
        hyprtail::diag::guard("cursor-hook", [&] {
            // Marked handled even if the lifecycle fails below, so the fallback
            // doesn't rerun a failing lifecycle in the same render.
            auto& mf         = s_monFrame[pMonitor.get()];
            mf.handledSerial = mf.renderSerial;
            ++mf.hookRuns;
            runLifecycleGuarded(pMonitor, "cursor-hook-lifecycle");
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

        if (!s_preset || s_preset->interpolateWarps || pointerConstrained())
            return;
        if (to != from)
            s_preset->pendingBreak = true;
    });
}

static void onRenderStageInternal(eRenderStage stage) {
    if ((stage != RENDER_BEGIN && stage != RENDER_LAST_MOMENT) || !g_pHyprRenderer || !s_preset)
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
    if (mf.handledSerial != mf.renderSerial) {
        ++mf.fallbackRuns;
        runLifecycleGuarded(pMonitor, "last-moment-lifecycle");
    }
}

static void onRenderStage(eRenderStage stage) {
    hyprtail::diag::guard("render-stage", [stage] { onRenderStageInternal(stage); });
}

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

// Hardware cursors: moving the cursor plane may not trigger a render at all
// (Aquamarine's Wayland backend moveCursor is a no-op; DRM schedules one).
// Damage a small box at the new point on each monitor it touches so a render
// happens, where the normal lifecycle then samples and damages the layers.
// Position is already updated when this fires (InputManager.cpp:155, :269).
static void onMouseMoveInternal() {
    const Vector2D pos = Pointer::mgr()->position();

    // Restart the stillness wait; quad layers end at the next render (the
    // damage below makes one happen).
    noteMotion(pos, msSinceEpoch(Time::steadyNow()));

    if (!s_preset || sessionLocked() || pointerConstrained())
        return;

    // Reach of the widest path layer (before any program is compiled, a
    // small default: the first render sizes it).
    float r = 0.F;
    for (const auto& l : s_preset->layers) {
        if (!l->disabled && l->resolved() && l->topology() == eTopology::PATH)
            r = std::max(r, layerExtentPx(*s_preset, *l));
    }
    if (r <= 0.F)
        r = 8.F + s_preset->damagePaddingPx;

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

static void onMouseMove() {
    hyprtail::diag::guard("mouse-move", [] { onMouseMoveInternal(); });
}

// Visible content changed underneath the pointer (SPEC §7): workspace switch,
// special workspace toggle, workspace moved to another monitor. The pointer
// may also have been warped in the same call stack (Monitor.cpp:1497-1507),
// before any render. Don't connect the old trail to the new position: the
// next insert starts a new segment. Every other jump (e.g. a cross-monitor
// focus warp) stays connected, which draws the straight "interpolated" sweep.
static void onContentChanged() {
    hyprtail::diag::guard("content-changed", [] {
        if (s_preset)
            s_preset->pendingBreak = true;
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
// elements live for one render. A layer that was drawing on it simply stops
// drawing there; the pointer is warped off it (Monitor.cpp:478-480), which
// breaks the trail via the warp hook unless interpolateWarps.
static void onMonitorGone(const PHLMONITOR& pMonitor) {
    hyprtail::diag::guard("monitor-removed", [&] {
        if (!pMonitor)
            return;
        Monitor::CMonitor* m = pMonitor.get();
        s_monFrame.erase(m);
        s_layout.erase(m);
        if (s_preset) {
            for (auto& l : s_preset->layers)
                l->damage.forget(m);
        }
    });
}

// Monitors arranged (MonitorLayoutController.cpp:70, Monitor.cpp:1399,
// MonitorRuleManager.cpp:204). Nodes are global coordinates: if a monitor
// moved, changed size (mode, scale, transform) or appeared, old points could
// land somewhere else on screen until they fade, so the source is dropped.
// Only then: arrange() also runs on config reloads that change nothing, and
// those must not clear the trail. A monitor only disappearing needs nothing:
// its points have no monitor to draw on.
static void onLayoutChanged() {
    hyprtail::diag::guard("layout-changed", [] {
        auto       now     = currentLayout();
        const bool changed = std::ranges::any_of(now, [](const auto& entry) {
            const auto it = s_layout.find(entry.first);
            return it == s_layout.end() || !(it->second == entry.second);
        });
        s_layout = std::move(now);

        if (!changed || !s_preset)
            return;

        s_preset->ring.clear();
        s_preset->pendingBreak = true;
        // Repaint what was drawn last: monitor-local boxes, still where the
        // layers are on screen. The next render of each monitor sees an empty
        // source, damages those boxes once more and goes idle.
        for (const auto& m : State::monitorState()->allMonitors()) {
            if (!m || !m->m_enabled || m->isMirror())
                continue;
            for (auto& l : s_preset->layers)
                l->damage.damagePrev(m);
        }
        Log::logger->log(Log::INFO, "[hyprtail] monitor layout changed, trail cleared");
    });
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
        if (s_statusCommand)
            HyprlandAPI::unregisterHyprCtlCommand(s_handle, s_statusCommand);
        s_statusCommand.reset();
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
        if (g_pHyprRenderer)
            g_pHyprRenderer->m_renderPass.removeAllOfType("CLayerPassElement");

        if (s_preset) {
            presetReleaseGpu(*s_preset);
            s_preset.reset();
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
    // render and the layers draw above the cursor.
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

// ---------------------------------------------------------------- config

// Shader paths of the classic preset's layers from the current config keys
// (until the v2 config surface, SPEC §13.8).
static std::pair<std::string, std::string> layerShaderPaths(const std::string& layer) {
    if (layer == "trail")
        return {s_config.vertexShader, s_config.fragmentShader};
    if (layer == "idle")
        return {s_config.idleVertexShader, s_config.idleFragmentShader};
    return {};
}

// Parameter values of the classic preset's layers from the current config
// keys, on top of the preset's defaults.
static std::map<std::string, std::string> layerOverrides(const hyprtail::SLayerSpec& spec) {
    using namespace hyprtail::params;
    const auto color = [](uint64_t argb) { return format(SValue{.type = eType::COLOR, .argb = static_cast<uint32_t>(argb)}); };
    const auto num   = [](double v) { return std::format("{}", v); };

    auto       out = spec.defaults;
    if (spec.name == "trail") {
        out["fade_ms"]     = num(s_config.fadeMs);
        out["width"]       = num(s_config.widthPx);
        out["miter_limit"] = num(s_config.miterLimit);
        out["color_slow"]  = color(s_config.colorSlow);
        out["color_fast"]  = color(s_config.colorFast);
    } else if (spec.name == "idle") {
        out["enabled"]                 = s_config.idleEnabled ? "true" : "false";
        out["start_ms"]                = num(s_config.idleDelayMs);
        out["duration_ms"]             = num(s_config.idleDurationMs);
        out["radius"]                  = num(s_config.idleRadiusPx);
        out["draw_when_cursor_hidden"] = s_config.idleWhenHidden ? "true" : "false";
        out["color"]                   = color(s_config.colorSlow);
    }
    return out;
}

// Re-read every layer's shader stages (config path or built-in), preprocess
// them, and queue them for compilation at the next render (CShaderSlot).
// Errors keep the active programs.
static void reloadShaders() {
    std::vector<std::filesystem::path> watch;
    for (auto& l : s_preset->layers) {
        const auto [vert, frag] = layerShaderPaths(l->name());
        l->slot.reload(vert, frag, watch);
    }
    s_fileWatch.setFiles(watch);
    kickRender();
}

// Config values -> preset, on load and every Hyprland config reload.
static void applyConfig() {
    auto& p  = *s_preset;
    s_config = hyprtail::cfg::read(s_config);

    p.minSpacingPx     = s_config.minSpacingPx;
    p.interpolateWarps = s_config.interpolateWarps;
    p.damagePaddingPx  = s_config.damagePaddingPx;

    const auto& specs = hyprtail::classicPreset();
    for (size_t i = 0; i < p.layers.size() && i < specs.size(); ++i)
        p.layers[i]->setOverrides(layerOverrides(specs[i]));

    // Keeps the newest points; the VBO is reallocated at the next draw
    // (CNodeBuffer::ensure), where GL is current.
    if (p.ring.capacity() != s_config.capacity)
        p.ring.resize(s_config.capacity);

    reloadShaders();
    armIdleTimer();
    kickRender();
}

// ---------------------------------------------------------------- status

static hyprtail::status::SSnapshot statusSnapshot() {
    hyprtail::status::SSnapshot s;
    s.rev         = HYPRTAIL_REV;
    s.builtHash   = __hyprland_api_get_client_hash();
    s.runningHash = __hyprland_api_get_hash();
    s.cursorHook  = s_cursorHook != nullptr;
    s.warpHook    = s_warpHook != nullptr;
    s.renders     = s_frames;
    s.preset      = "classic";

    const double nowMs = msSinceEpoch(Time::steadyNow());

    if (s_preset) {
        const auto& p             = *s_preset;
        s.source.nodes            = p.ring.size();
        s.source.capacity         = p.ring.capacity();
        s.source.generation       = p.ring.generation();
        s.source.pendingBreak     = p.pendingBreak;
        s.source.interpolateWarps = p.interpolateWarps;
        s.source.gpuFailed        = p.gpuFailed;
        s.source.stillMs          = nowMs - p.lastMotionMs;

        for (const auto& l : p.layers) {
            hyprtail::status::SLayer out{
                .name     = l->name(),
                .enabled  = l->enabledSetting(),
                .disabled = l->disabled,
                .resolved = l->resolved(),
                .shader   = l->slot.status(),
            };
            if (l->resolved()) {
                out.fadeMs     = l->res.fadeMs;
                out.startMs    = l->res.startMs;
                out.durationMs = l->res.durationMs;
                out.extentPx   = layerExtentPx(p, *l);
                for (const auto& [decl, v] : l->res.values)
                    out.params.emplace_back(decl.name, hyprtail::params::format(v));
            }
            s.layers.push_back(std::move(out));
        }
    }

    for (const auto& m : State::monitorState()->allMonitors()) {
        if (!m)
            continue;
        hyprtail::status::SMonitor out{.name = m->m_name};
        if (const auto it = s_monFrame.find(m.get()); it != s_monFrame.end()) {
            const auto& mf   = it->second;
            out.renders      = mf.renderSerial;
            out.hookRuns     = mf.hookRuns;
            out.fallbackRuns = mf.fallbackRuns;
            out.draws        = mf.draws;
            out.emptySkips   = mf.emptySkips;
        }
        if (s_preset) {
            for (const auto& l : s_preset->layers)
                out.layerBoxes.push_back(l->damage.prev(m.get()));
        }
        s.monitors.push_back(std::move(out));
    }

    s.diag      = hyprtail::diag::stats();
    s.errorFile = hyprtail::diag::errorFilePath();
    return s;
}

// `hyprctl hyprtail` (-j for JSON). Registered as an exact command: HyprCtl
// matches exact names first (HyprCtl.cpp:2101-2109) and treats an empty
// reply as "unknown request" (:2123-2124), so this never returns "".
static std::string statusCommand(eHyprCtlOutputFormat format, std::string) {
    std::string out;
    const bool  ok = hyprtail::diag::guard("status-command", [&] {
        const auto snap = statusSnapshot();
        out             = format == eHyprCtlOutputFormat::FORMAT_JSON ? hyprtail::status::json(snap) : hyprtail::status::text(snap);
    });
    if (!ok || out.empty())
        return format == eHyprCtlOutputFormat::FORMAT_JSON ? R"({"error": "hyprtail status failed, see errors.log"})" : "hyprtail status failed, see errors.log";
    return out;
}

// ---------------------------------------------------------------- init / exit

static PLUGIN_DESCRIPTION_INFO pluginInit() {
    // Everything reported while loading ends up in one summary notification
    // (SPEC §13.11), once the first render has compiled the shaders.
    hyprtail::diag::beginBatch("loading");

    s_epoch  = Time::steadyNow();
    s_config = {};

    // Node seeds (SPEC §13.2): per-load random base, hashed with an
    // insertion counter.
    std::random_device rd;
    const uint64_t     seedBase = (static_cast<uint64_t>(rd()) << 32) ^ rd();

    s_preset = makeUnique<SPreset>(s_config.capacity, seedBase);
    for (const auto& spec : hyprtail::classicPreset())
        s_preset->layers.push_back(makeUnique<hyprtail::CLayer>(spec));
    s_preset->lastPos      = Pointer::mgr()->position();
    s_preset->lastMotionMs = 0.0;

    // Stillness timer for quad layers on Hyprland's event loop (main thread),
    // like the file watch.
    if (g_pCompositor && g_pCompositor->m_wlEventLoop)
        s_idleTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &onIdleTimer, nullptr);
    if (!s_idleTimer)
        hyprtail::diag::report(eSeverity::WARN, "idle-timer", "could not create the idle timer; the idle effect only starts when something else renders");

    // Settings (SPEC section 9). Registered values get their configured value
    // on the reload Hyprland schedules right after loading a plugin
    // (PluginSystem.cpp:135); until then they hold the defaults.
    hyprtail::cfg::registerValues(s_handle);
    s_fileWatch.init([] {
        hyprtail::diag::guard("shader-file-changed", [] {
            hyprtail::diag::beginBatch("a shader file change");
            reloadShaders();
        });
    });
    applyConfig();
    s_configReloadListener = Event::bus()->m_events.config.reloaded.listen([] {
        hyprtail::diag::guard("config-reload", [] {
            hyprtail::diag::beginBatch("a config reload");
            applyConfig();
        });
    });

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

    s_statusCommand = HyprlandAPI::registerHyprCtlCommand(s_handle, SHyprCtlCommand{.name = "hyprtail", .exact = true, .fn = statusCommand});
    if (!s_statusCommand)
        hyprtail::diag::report(eSeverity::WARN, "hyprctl", "could not register the `hyprctl hyprtail` status command");

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
    // rotate away the other instance's errors.log. Nothing is registered yet,
    // so no teardown is needed: one notification, then a std::exception,
    // which is how a plugin refuses to load (Hyprland catches it and ejects
    // the plugin, PluginSystem.cpp:113-126).
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
