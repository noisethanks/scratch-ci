#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <unordered_map>

#include <plugins/PluginAPI.hpp>
#include <plugins/HookSystem.hpp>
#include <plugins/PluginSystem.hpp>
#include <config/lua/ConfigManager.hpp>
#include <render/OpenGL.hpp>
#include <render/Renderer.hpp>
#include <pointer/PointerManager.hpp>
#include <pointer/PointerController.hpp>
#include <managers/input/InputManager.hpp>
#include <managers/fullscreen/FullscreenController.hpp>
#include <managers/SessionLockManager.hpp>
#include <event/EventBus.hpp>
#include <desktop/view/Window.hpp>
#include <desktop/state/FocusState.hpp>
#include <state/MonitorState.hpp>
#include <output/Monitor.hpp>
#include <helpers/memory/Memory.hpp>
#include <helpers/time/Time.hpp>
#include <helpers/Color.hpp>
#include <debug/log/Logger.hpp>

#include "Config.hpp"
#include "rev.hpp"
#include "CrashGuard.hpp"
#include "Diagnostics.hpp"
#include "FileWatch.hpp"
#include "Layer.hpp"
#include "LayerPassElement.hpp"
#include "Preset.hpp"
#include "RenderUtil.hpp"
#include "Status.hpp"
#include "StatePath.hpp"

#include <wayland-server-core.h>
#include <Compositor.hpp>

using hyprtail::diag::eSeverity;
using hyprtail::shader::eTopology;
using hyprtail::cfg::eWarpMode;

static HANDLE s_handle = nullptr;


typedef void (*origRenderSoftwareCursorsFor)(void*, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool, bool);
typedef void (*origControllerWarpTo)(const void*, const Vector2D&, bool);
typedef bool (*origSaveBufferForMirror)(void*, const CBox&);


// Which render each monitor is in, and which render the cursor hook already
// ran the lifecycle for. Keyed to a per-monitor serial bumped at RENDER_BEGIN
// rather than a boolean, so a missed clear can never make a later render skip
// its lifecycle: the fallback runs unless the hook ran for *this* render.
struct SMonitorFrame {
    uint64_t renderSerial  = 0;
    uint64_t handledSerial = UINT64_MAX; // never equal before the first hook run

    // Counters for `hyprctl hyprtail`.
    uint64_t hookRuns = 0, fallbackRuns = 0, draws = 0, emptySkips = 0;

    // Screenshare exclude (SPEC §13.12): runLifecycle() sets awaitingCaptureDraw
    // and stashes this render's draws here instead of adding a CLayerPassElement,
    // when this monitor needsACopyFB(). hkSaveBufferForMirror() consumes them
    // (drawing directly, after the original, so the copy is clean); the
    // RENDER_POST self-check in onRenderStageInternal catches it if the hook
    // never fires and sets captureHookUnavailable, sticky, so the fallback
    // (draw nothing while this monitor needs a copy) applies from then on.
    bool                    awaitingCaptureDraw    = false;
    bool                    captureHookUnavailable = false;
    std::vector<SLayerDraw> pendingCaptureDraws;
    double                  pendingCaptureNowMs = 0.0;
};

static uint64_t                                              s_frames = 0;
static Time::steady_tp                                       s_epoch;
static UP<SPreset>                                           s_preset;
static wl_event_source*                                      s_idleTimer  = nullptr;
// Crash-loop guard (SPEC §2): armed only after we've written our own marker
// (pluginInit()), so its existence alone tells teardown() the marker is ours
// to remove -- never someone else's (e.g. a nested instance sharing the same
// state directory) that we merely read and decided not to refuse over.
static wl_event_source*                                      s_crashGuardTimer = nullptr;
static CFunctionHook*                                        s_cursorHook  = nullptr;
static CFunctionHook*                                        s_warpHook    = nullptr;
static CFunctionHook*                                        s_captureHook = nullptr;
// hyprtail:no_trail dynamic window-rule effect (SPEC §7). WINDOW_RULE_EFFECT_NONE
// (0) doubles as "not registered": registerEffect() never returns 0 for a new
// dynamic name, so this is a safe sentinel for teardown() to guard on.
static Desktop::Rule::CWindowRuleEffectContainer::storageType s_noTrailEffectIdx = Desktop::Rule::WINDOW_RULE_EFFECT_NONE;
static CHyprSignalListener                                   s_renderStageListener;
static CHyprSignalListener                                   s_mouseMoveListener;
static CHyprSignalListener                                   s_workspaceActiveListener;
static CHyprSignalListener                                   s_specialActiveListener;
static CHyprSignalListener                                   s_workspaceMovedListener;
static CHyprSignalListener                                   s_configReloadListener;
static CHyprSignalListener                                   s_monitorRemovedListener;
static CHyprSignalListener                                   s_monitorDestroyListener;
static CHyprSignalListener                                   s_layoutChangedListener;
static CHyprSignalListener                                   s_cursorShapeListener;
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

// The focused window's hyprtail:no_trail rule, looked up fresh every call:
// no listener-driven cache. Desktop::Rule::windowEffects() and
// CWindowRuleApplicator::m_otherProps.props are the documented plugin-read
// path (WindowRuleApplicator.hpp:63-72 at the pin, SPEC §2), the same one
// hyprbars uses for its own dynamic effects (hyprbars barDeco.cpp:641-646).
static bool appRuleSuppressed() {
    if (s_noTrailEffectIdx == Desktop::Rule::WINDOW_RULE_EFFECT_NONE)
        return false;
    const auto w = Desktop::focusState()->window();
    if (!w || !w->m_ruleApplicator)
        return false;
    const auto& props = w->m_ruleApplicator->m_otherProps.props;
    const auto  it     = props.find(s_noTrailEffectIdx);
    if (it == props.end() || !it->second)
        return false;
    return hyprtail::params::ruleTruthy(it->second->effect);
}

// Session lock, pointer constraint, and the focused window's hyprtail:no_trail
// rule all suppress the source the same way (SPEC §7): no new points, idle-
// marker effects end, motion tracking pauses. Session lock is the only one of
// the three that also hard-gates drawing (runLifecycle's `!locked &&` check)
// -- path points from the other two just stop growing and fade out on their
// own.
static bool suppressed() {
    return sessionLocked() || pointerConstrained() || appRuleSuppressed();
}

// Screenshare exclude (SPEC §13.12): anything but the literal "include"
// (a bad config value already normalized to "exclude" by cfg::read()) means
// keep the trail out of monitor/region captures and mirrors.
static bool excludeCaptures() {
    return s_config.screenshare != "include";
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

// Applies whatever applyConfig() queued: a preset switch, layerN's
// unused-index check, and the `params` string. Defined in the config
// section below, alongside applyConfig() and reloadShaders(), which it
// reuses to queue newly-constructed layers' shaders for this same render.
static void applyPendingState();

// GL is current: apply any queued preset switch (a new set of layers), then
// compile pending programs (config reload, file change) or fall back to the
// built-in ones, then resolve parameter values. A layer whose built-in
// program fails is disabled.
static void prepareLayers() {
    applyPendingState();

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

// Emit point for the next inserted node (SPEC §13.9): the raw pointer
// position ("hotspot", default), or a position normalized to the cursor
// image box, plus a fixed pixel offset added after. hasCursor() is private
// (PointerManager.hpp), so the "no cursor image" fallback is inlined here
// from the public currentCursorImage() accessor instead of calling it.
static SVec2f emitPoint(const SPreset& p) {
    Vector2D base = Pointer::mgr()->position();
    if (p.emitFromNorm) {
        const auto& img = Pointer::mgr()->currentCursorImage();
        if (img.pBuffer || img.surface) {
            const CBox box = Pointer::mgr()->getCursorBoxGlobal(); // pos = pointer - hotspot, size = image/scale
            base           = Vector2D{box.x, box.y} + Vector2D{box.w, box.h} * (*p.emitFromNorm);
        }
    }
    base += p.emitOffsetPx;
    return SVec2f{sc<float>(base.x), sc<float>(base.y)};
}

// Sample the pointer once per render into the source ring: insert only on
// real movement (at least min_spacing from the newest node). The trail
// follows the pointer whether or not the cursor is shown (SPEC §7).
// Breaks (SPEC §7): lock, pointer constraint and the app rule break
// unconditionally; so do workspace events. Nothing is inserted while
// suppressed(). Warps break only when warpMode == BREAK (hkControllerWarpTo).
static void sampleSource(double nowMs) {
    auto& p = *s_preset;
    if (suppressed()) {
        p.pendingBreak = true;
        return;
    }

    const SVec2f pos = emitPoint(p);

    bool         insert = p.ring.empty();
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
    sampleSource(nowMs);

    const Vector2D pos = Pointer::mgr()->position();
    noteMotion(pos, nowMs);

    const bool                hidden      = cursorHidden();
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
            } else if (!suppressed() && quadInWindow(l, nowMs))
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

    // Screenshare exclude (SPEC §13.12): while this monitor needs a mirror/
    // capture copy, skip the normal pass element (it runs and draws before
    // end()'s copy, Renderer.cpp:2160-2229 vs OpenGL.cpp:801-806 -- too
    // early, it would be baked into the copy) and stash the draws for
    // hkSaveBufferForMirror() instead, which runs after the copy. If the
    // hook isn't installed, or the RENDER_POST self-check already caught it
    // missing a previous render, fall back to not drawing at all: the
    // damage.update() calls above already ran, so the region simply
    // redraws without a trail, no artifacts.
    if (excludeCaptures() && pMonitor->needsACopyFB()) {
        if (s_captureHook && !mf.captureHookUnavailable) {
            mf.pendingCaptureDraws = std::move(draws);
            mf.pendingCaptureNowMs = nowMs;
            mf.awaitingCaptureDraw = true;
            ++mf.draws; // deferred, not skipped: it's still drawn, just later
        }
        return;
    }

    g_pHyprRenderer->m_renderPass.add(makeUnique<CLayerPassElement>(&p, std::move(draws), nowMs));
    ++mf.draws;
}

// Timer fired: a quad layer's window may have opened. Damage its square (not
// just scheduleFrame: a frame without damage skips the workspace, see
// CMonitorDamage) so a render happens and the lifecycle draws it.
static int onIdleTimer(void*) {
    hyprtail::diag::guard("idle-timer", [] {
        if (!s_preset || suppressed())
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

// 60s after a startup that got this far, the crash-loop window is over:
// remove our marker so a later crash, or a plain `hyprctl plugin unload`,
// doesn't leave a stale refusal behind for the next load. One-shot, not
// re-armed -- unlike the idle timer, this only ever needs to fire once per
// load.
static int onCrashGuardTimer(void*) {
    hyprtail::diag::guard("crash-guard-timer", [] { hyprtail::crashguard::removeMarker(); });
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

// Bezier warp interpolation (SPEC §13.10, warp = "curve"): inserts nodes on
// the CPU along a quadratic Bezier from the trail's current end to the warp
// target, so damage stays exact and every topology works without special
// casing -- the same argument as `path smooth N`'s CPU-computed control
// points (§13.3). Nothing to curve from if the ring is empty: falls back to
// a plain connect at the next sample, same as `line`.
static void insertWarpCurve(SPreset& p, const Vector2D& to, double nowMs) {
    if (p.ring.empty())
        return;

    // Bound to the ring's backing storage, not invalidated by insert()
    // below: a fixed-capacity circular buffer only reallocates on resize(),
    // never on insert(). Stays the pre-warp state through the whole loop.
    const auto&  prev = p.ring.newest();
    const SVec2f p0   = prev.posPx;
    const SVec2f p2{sc<float>(to.x), sc<float>(to.y)};
    const float  chord = std::hypot(p2.x - p0.x, p2.y - p0.y);

    // Control point along the incoming velocity, for tangent continuity at
    // p0. Zero velocity (a fresh segment) falls back to the chord's
    // midpoint, which makes the quadratic Bezier degenerate to a straight
    // line -- no special-casing needed.
    SVec2f       p1 = {(p0.x + p2.x) / 2.F, (p0.y + p2.y) / 2.F};
    if (const float speed = std::hypot(prev.velocity.x, prev.velocity.y); speed > 1e-6F)
        p1 = {p0.x + prev.velocity.x / speed * chord * 0.5F, p0.y + prev.velocity.y / speed * chord * 0.5F};

    // Length / min_spacing, capped at a quarter of the capacity (SPEC §13.10).
    const int    n  = std::clamp(sc<int>(std::round(chord / std::max(p.minSpacingPx, 0.01F))), 1, std::max<int>(1, sc<int>(p.ring.capacity() / 4)));
    const double t0 = prev.birthTimeMs;
    for (int i = 1; i <= n; ++i) {
        const float  t = sc<float>(i) / sc<float>(n);
        const float  u = 1.F - t;
        const SVec2f curvePos{u * u * p0.x + 2.F * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2.F * u * t * p1.y + t * t * p2.y};
        // Birth times spread between the previous node's birth and now, so
        // the fade sweeps along the curve.
        p.ring.insert(curvePos, t0 + (nowMs - t0) * t, false);
    }
}

// Programmatic warps (dispatchers, layouts, focus changes) go through here
// (PointerController.cpp:16-29). warpMode decides what happens to the trail:
// break starts a new segment, line connects with a straight sweep (the next
// natural sample does that for free), curve bakes in a Bezier immediately.
// Only that decision is ours; the original always runs, unwrapped.
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
        const Vector2D to    = Pointer::mgr()->position();
        const double   nowMs = msSinceEpoch(Time::steadyNow());
        noteMotion(to, nowMs);

        if (!s_preset || to == from || pointerConstrained())
            return;

        switch (s_preset->warpMode) {
            case eWarpMode::BREAK: s_preset->pendingBreak = true; break;
            case eWarpMode::LINE: break;
            case eWarpMode::CURVE: insertWarpCurve(*s_preset, to, nowMs); break;
        }
    });
}

// Screenshare exclude (SPEC §13.12). Called from CHyprOpenGLImpl::end()
// (OpenGL.cpp:801-802) exactly when this monitor needsACopyFB(): the
// original takes the mirror/capture copy from currentFB first; we then draw
// directly into currentFB, which is current again once the original's own
// bindTempFB guard unwinds (OpenGL.cpp:2540-2541, 2556-2558). end() copies
// that same framebuffer to the real output right after (:808-829), so the
// trail still reaches the screen, just not the copy that was already taken.
//
// m_renderData.damage isn't set explicitly here -- it doesn't need to be.
// By this point it holds finalDamage (OpenGL.cpp:786), which traces back
// through GLRenderer.cpp:88 (m_renderPass.render(m_renderData.damage)) and
// Pass.cpp:132/163/172 to the same render's damage accumulator our own
// damageInRender() added this monitor's layer boxes to earlier this render,
// in runLifecycle(), well before endRender(). finalDamage is always a
// superset of that (blur-widened at most, never narrower), so scissoring to
// it can't crop our draw -- only be marginally wider than the tightest
// per-element region Pass.cpp:193-194 would normally compute. See NOTES
// "Phase 7" for the full trace.
static bool hkSaveBufferForMirror(void* thisptr, const CBox& box) {
    const bool ok = (*(origSaveBufferForMirror)s_captureHook->m_original)(thisptr, box);

    if (!g_pHyprRenderer || !s_preset)
        return ok;
    const auto pMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!pMonitor)
        return ok;

    hyprtail::diag::guard("capture-hook", [&] {
        auto& mf = s_monFrame[pMonitor.get()];
        if (!mf.awaitingCaptureDraw)
            return; // this monitor didn't defer anything this render
        mf.awaitingCaptureDraw = false;
        if (!mf.pendingCaptureDraws.empty()) {
            CLayerPassElement el(s_preset.get(), std::move(mf.pendingCaptureDraws), mf.pendingCaptureNowMs);
            el.draw();
        }
        mf.pendingCaptureDraws.clear();
    });

    return ok;
}

static void onRenderStageInternal(eRenderStage stage) {
    if ((stage != RENDER_BEGIN && stage != RENDER_LAST_MOMENT && stage != RENDER_POST) || !g_pHyprRenderer || !s_preset)
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

    // RENDER_POST (:2250), after endRender() -- and so after end() and its
    // saveBufferForMirror call -- has already run. Self-check (SPEC §13.12):
    // if runLifecycle() stashed draws for the capture hook this render but
    // they're still sitting here unconsumed, the hook never fired (a future
    // build could inline a second copy of end() our export-based hook, S1,
    // can't catch). Fall back for this monitor from now on: don't draw while
    // it needs a copy, rather than risk the trail leaking into one.
    if (stage == RENDER_POST) {
        if (mf.awaitingCaptureDraw) {
            mf.awaitingCaptureDraw    = false;
            mf.captureHookUnavailable = true;
            mf.pendingCaptureDraws.clear();
            hyprtail::diag::report(
                eSeverity::WARN, "hook:capture:" + pMonitor->m_name,
                std::format("the screenshare-exclude hook didn't fire for {} this render; falling back to not drawing there while it needs a mirror/capture copy.",
                           pMonitor->m_name));
        }
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

    if (!s_preset || suppressed())
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
// the warp hook turns into a break, a line, or a curve, per warpMode.
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
        if (s_noTrailEffectIdx != Desktop::Rule::WINDOW_RULE_EFFECT_NONE) {
            Desktop::Rule::windowEffects()->unregisterEffect(s_noTrailEffectIdx);
            s_noTrailEffectIdx = Desktop::Rule::WINDOW_RULE_EFFECT_NONE;
        }
        s_configReloadListener.reset();
        s_fileWatch.shutdown();
        if (s_idleTimer) {
            wl_event_source_remove(s_idleTimer);
            s_idleTimer = nullptr;
        }
        // Only removes the marker if it's ours (s_crashGuardTimer is only
        // ever non-null after we wrote it, see pluginInit()) -- never a
        // marker some other instance (sharing the same state directory) is
        // still relying on.
        if (s_crashGuardTimer) {
            wl_event_source_remove(s_crashGuardTimer);
            s_crashGuardTimer = nullptr;
            hyprtail::crashguard::removeMarker();
        }
        s_workspaceActiveListener.reset();
        s_specialActiveListener.reset();
        s_workspaceMovedListener.reset();
        s_monitorRemovedListener.reset();
        s_monitorDestroyListener.reset();
        s_layoutChangedListener.reset();
        s_mouseMoveListener.reset();
        s_cursorShapeListener.reset();
        s_renderStageListener.reset();
        removeHook(s_cursorHook);
        removeHook(s_warpHook);
        removeHook(s_captureHook);

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

    // Warp detection for warpMode != line. Without it every warp connects
    // (straight sweep).
    s_warpHook = installHook(pmf_address(&Pointer::CPointerController::warpTo), reinterpret_cast<void*>(&hkControllerWarpTo));
    if (!s_warpHook)
        hyprtail::diag::report(eSeverity::WARN, "hook:warp",
                               "could not hook CPointerController::warpTo. Warps will draw a connecting line instead of breaking the trail.");

    // Screenshare exclude (SPEC §13.12). Without it exclude mode degrades to
    // not drawing on any monitor that needs a mirror/capture copy, same as
    // the RENDER_POST self-check's own fallback (onRenderStageInternal).
    s_captureHook = installHook(pmf_address(&Render::GL::CHyprOpenGLImpl::saveBufferForMirror), reinterpret_cast<void*>(&hkSaveBufferForMirror));
    if (!s_captureHook)
        hyprtail::diag::report(eSeverity::WARN, "hook:capture",
                               "could not hook CHyprOpenGLImpl::saveBufferForMirror. screenshare = \"exclude\" degrades to not drawing on monitors that need a mirror/capture copy.");
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

// Shader paths of a layer at this position in the active preset: a
// `layerN_vertex`/`layerN_fragment` override (SPEC §13.7, N = 1-based) wins
// per stage if set, else the active preset's own per-layer shader (a
// built-in name, or the path it resolved against its own directory).
static std::pair<std::string, std::string> layerShaderPaths(size_t index) {
    std::pair<std::string, std::string> fromPreset;
    if (index < s_preset->activePreset.layers.size())
        fromPreset = {s_preset->activePreset.layers[index].vertPath, s_preset->activePreset.layers[index].fragPath};

    if (index >= s_config.layerVertex.size())
        return fromPreset;
    return {s_config.layerVertex[index].empty() ? fromPreset.first : s_config.layerVertex[index],
            s_config.layerFragment[index].empty() ? fromPreset.second : s_config.layerFragment[index]};
}

// Re-read every layer's shader stages (config path, preset path, or
// built-in), preprocess them, and queue them for compilation at the next
// render (CShaderSlot). Errors keep the active programs.
static void reloadShaders() {
    std::vector<std::filesystem::path> watch;
    for (size_t i = 0; i < s_preset->layers.size(); ++i) {
        auto&      l            = s_preset->layers[i];
        const auto [vert, frag] = layerShaderPaths(i);
        l->slot.reload(vert, frag, watch);
    }
    s_fileWatch.setFiles(watch);
    kickRender();
}

// GL is current (called from prepareLayers(), before anything else):
// applies whatever applyConfig() queued.
//
// A preset switch first: swaps `layers` to fresh CLayer objects built from
// `pendingPreset` only if it actually differs from `activePreset` (a
// manifest re-parse with no real change, or an unrelated config reload, is
// a no-op here), releasing the old layers' GL programs first (the shared
// preset.gpu/quadVao are per-preset-*instance*, not per-preset-*definition*,
// so they're left alone). Reconstructing here, rather than mutating an
// existing CLayer's identity, is safe because a CLayerPassElement's raw
// CLayer* pointers (SLayerDraw) are collected fresh every render, after
// this point, and never outlive the render they were collected for -- see
// NOTES "Phase 4".
//
// Then layerN's unused-index check and the `params` string, both against
// whichever layers are now active -- so they're correct even the same
// render a preset switch happens, not one render behind it.
static void applyPendingState() {
    auto& p = *s_preset;

    if (p.pendingPreset && *p.pendingPreset != p.activePreset) {
        for (auto& l : p.layers)
            l->slot.release();
        p.layers.clear();
        for (const auto& spec : p.pendingPreset->layers)
            p.layers.push_back(makeUnique<hyprtail::CLayer>(spec));
        p.activePreset = *p.pendingPreset;
        reloadShaders();
    }
    p.pendingPreset.reset();

    // layer1_vertex .. layer4_fragment (SPEC §13.7): an override for an
    // index the active preset has no layer for is a plugin warning.
    {
        std::string unused;
        for (size_t i = p.activePreset.layers.size(); i < s_config.layerVertex.size(); ++i)
            if (!s_config.layerVertex[i].empty() || !s_config.layerFragment[i].empty())
                unused += std::format(" layer{}", i + 1);
        if (!unused.empty())
            hyprtail::diag::report(eSeverity::WARN, "config:plugin:hyprtail:layerN",
                                   std::format("preset \"{}\" has {} layer(s); override(s) for{} ignored", p.activePreset.name, p.activePreset.layers.size(), unused));
        else
            hyprtail::diag::resetKey("config:plugin:hyprtail:layerN");
    }

    // `params` string (SPEC §13.5): syntax problems and entries for an
    // unknown layer are one batched warning; entries for a known layer are
    // validated per-name/type/range in CLayer::resolve().
    {
        auto        parsed = hyprtail::params::parseParamsString(s_config.params);
        std::string unknownLayers;
        std::vector<std::map<std::string, std::string>> perLayer(p.layers.size());
        for (const auto& e : parsed.entries) {
            const auto it = std::ranges::find_if(p.layers, [&](const auto& l) { return l->name() == e.layer; });
            if (it == p.layers.end()) {
                if (unknownLayers.find(std::format(" \"{}\"", e.layer)) == std::string::npos)
                    unknownLayers += std::format(" \"{}\"", e.layer);
                continue;
            }
            perLayer[std::distance(p.layers.begin(), it)][e.name] = e.value;
        }
        for (size_t i = 0; i < p.layers.size(); ++i)
            p.layers[i]->setParamOverrides(std::move(perLayer[i]));

        if (!unknownLayers.empty()) {
            std::string names;
            for (const auto& l : p.layers)
                names += (names.empty() ? "" : ", ") + l->name();
            parsed.problems.push_back(std::format("unknown layer(s):{} (this preset has: {})", unknownLayers, names));
        }
        if (!parsed.problems.empty()) {
            std::string msg;
            for (const auto& prob : parsed.problems)
                msg += "\n  " + prob;
            hyprtail::diag::report(eSeverity::WARN, "config:plugin:hyprtail:params", std::format("plugin:hyprtail:params: problems:{}", msg));
        } else
            hyprtail::diag::resetKey("config:plugin:hyprtail:params");
    }
}

// Config values -> preset, on load and every Hyprland config reload. GL
// isn't guaranteed current here (a config reload isn't necessarily inside
// a render), so this only resolves and queues; applyPendingState() (GL
// current, from prepareLayers()) does the actual layer/shader work.
static void applyConfig() {
    auto& p  = *s_preset;
    s_config = hyprtail::cfg::read(s_config);

    p.minSpacingPx    = s_config.minSpacingPx;
    p.warpMode        = s_config.warp;
    p.damagePaddingPx = s_config.damagePaddingPx;
    p.emitFromNorm    = s_config.emitFromNorm;
    p.emitOffsetPx    = s_config.emitOffsetPx;

    p.pendingPreset = hyprtail::preset::load(s_config.preset);

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
    s.captureHook = s_captureHook != nullptr;
    s.renders     = s_frames;
    s.preset      = s_preset ? s_preset->activePreset.name : "";
    s.screenshare = s_config.screenshare;

    s.suppress.locked      = sessionLocked();
    s.suppress.constrained = pointerConstrained();
    s.suppress.appRule     = appRuleSuppressed();
    if (const auto w = Desktop::focusState()->window()) {
        s.suppress.focusedClass = w->m_class;
        s.suppress.focusedTitle = w->m_title;
    }

    const double nowMs = msSinceEpoch(Time::steadyNow());

    if (s_preset) {
        const auto& p             = *s_preset;
        s.source.nodes            = p.ring.size();
        s.source.capacity         = p.ring.capacity();
        s.source.generation       = p.ring.generation();
        s.source.pendingBreak = p.pendingBreak;
        s.source.warpMode     = hyprtail::cfg::warpModeName(p.warpMode);
        s.source.gpuFailed    = p.gpuFailed;
        s.source.stillMs      = nowMs - p.lastMotionMs;

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
        hyprtail::status::SMonitor out{.name = m->m_name, .needsCopyFB = m->needsACopyFB()};
        if (const auto it = s_monFrame.find(m.get()); it != s_monFrame.end()) {
            const auto& mf     = it->second;
            out.renders        = mf.renderSerial;
            out.hookRuns       = mf.hookRuns;
            out.fallbackRuns   = mf.fallbackRuns;
            out.draws          = mf.draws;
            out.emptySkips     = mf.emptySkips;
            out.captureFallback = mf.captureHookUnavailable;
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

    // Per-app suppression (SPEC §7): a dynamic window-rule effect, read back
    // per render via m_ruleApplicator->m_otherProps (appRuleSuppressed()).
    s_noTrailEffectIdx = Desktop::Rule::windowEffects()->registerEffect("hyprtail:no_trail");

    // Node seeds (SPEC §13.2): per-load random base, hashed with an
    // insertion counter.
    std::random_device rd;
    const uint64_t     seedBase = (static_cast<uint64_t>(rd()) << 32) ^ rd();

    // `layers` starts empty: applyConfig() below queues the configured
    // preset (default "subtle"), and the first prepareLayers() (the first
    // render, GL current) builds it -- the same path a later preset switch
    // takes, see applyPendingState().
    s_preset = makeUnique<SPreset>(s_config.capacity, seedBase);
    s_preset->lastPos               = Pointer::mgr()->position();
    s_preset->lastMotionMs          = 0.0;
    s_preset->lastCursorHotspot     = Pointer::mgr()->hotspot();
    s_preset->lastCursorSizeLogical = Pointer::mgr()->cursorSizeLogical();

    // Stillness timer for quad layers on Hyprland's event loop (main thread),
    // like the file watch.
    if (g_pCompositor && g_pCompositor->m_wlEventLoop)
        s_idleTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &onIdleTimer, nullptr);
    if (!s_idleTimer)
        hyprtail::diag::report(eSeverity::WARN, "idle-timer", "could not create the idle timer; the idle effect only starts when something else renders");

    // Crash-loop guard (SPEC §2): a fresh marker for this load. The
    // duplicate-instance-shaped refusal check in PLUGIN_INIT already ruled
    // out that a prior marker for this exact build+Hyprland pair names a
    // dead pid, so it's safe to overwrite whatever was (or wasn't) there.
    // s_crashGuardTimer is armed only when the write actually succeeded, so
    // its mere existence is teardown()'s signal that the marker is ours to
    // remove.
    {
        const hyprtail::crashguard::SKey key{HYPRTAIL_REV, __hyprland_api_get_hash()};
        const char*                      sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!hyprtail::crashguard::writeMarker(key, sig ? sig : "unknown", ::getpid()))
            hyprtail::diag::report(eSeverity::WARN, "crash-guard", "could not write the crash-loop marker; a crash before the next clean unload won't be caught");
        else if (g_pCompositor && g_pCompositor->m_wlEventLoop) {
            constexpr int CRASH_GUARD_CLEAR_MS = 60000;
            s_crashGuardTimer                  = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &onCrashGuardTimer, nullptr);
            if (s_crashGuardTimer)
                wl_event_source_timer_update(s_crashGuardTimer, CRASH_GUARD_CLEAR_MS);
        }
    }

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

    // Shape change without motion moves the emit point (SPEC §13.9). Gate on
    // the box geometry (hotspot, logical size) actually differing:
    // cursorChanged also fires for same-shape re-applies and every commit of
    // an animated client cursor surface (PointerManager.cpp:135, 153, 165,
    // 187, 201, 286), most of which don't move the box at all.
    s_cursorShapeListener = Pointer::mgr()->m_events.cursorChanged.listen([] {
        hyprtail::diag::guard("cursor-shape-changed", [] {
            if (!s_preset)
                return;
            const Vector2D hotspot = Pointer::mgr()->hotspot();
            const Vector2D size    = Pointer::mgr()->cursorSizeLogical();
            if (hotspot == s_preset->lastCursorHotspot && size == s_preset->lastCursorSizeLogical)
                return;
            s_preset->lastCursorHotspot     = hotspot;
            s_preset->lastCursorSizeLogical = size;
            if (s_preset->emitFromNorm)
                s_preset->pendingBreak = true;
        });
    });

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

    // Crash-loop guard (SPEC §2), checked here for the same reason as the
    // duplicate-instance check above: before diag::init, which would
    // otherwise rotate away the crashed session's errors.log -- the one
    // file that explains what happened. A marker naming this exact build
    // and this exact running Hyprland, whose pid is no longer alive, means
    // the load it belongs to never reached teardown() (a clean unload or a
    // caught init failure both remove it there). A live pid means some
    // other instance sharing the same state directory (e.g. a nested one)
    // is still in its own run, not a crash: ignored, not refused.
    const std::string compositorHash = __hyprland_api_get_hash();
    if (const auto marker = hyprtail::crashguard::readMarker();
        marker && hyprtail::crashguard::indicatesEarlyDeath(*marker, {HYPRTAIL_REV, compositorHash})) {
        const auto errLog = hyprtail::stateDir() / "errors.log";
        const auto msg     = std::format("refusing to load: the previous session (pid {}, instance {}) of this build never reached a clean unload. See {}. Delete {} to load anyway.",
                                          marker->pid, marker->instanceSignature, errLog.string(), hyprtail::crashguard::markerPath().string());
        HyprlandAPI::addNotification(handle, "[hyprtail] " + msg, CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 15000);
        throw std::runtime_error("[hyprtail] " + msg);
    }

    hyprtail::diag::init(handle);

    // Refusing to load is done by throwing. Hyprland only catches
    // std::exception from init (PluginSystem.cpp:121), so nothing else may
    // leave, and teardown must run first. Failures here notify synchronously:
    // the deferred path would be cancelled by teardown before it runs.
    try {
        // ABI version check — must match the compositor we were compiled against.
        const std::string clientHash = __hyprland_api_get_client_hash();
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
