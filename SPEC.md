# Hyprtail — Cursor Trail Plugin for Hyprland — SPEC

Distilled from NOTES.md. Where a decision below has an open parameter (buffer
size, fade curve default, commit pin), NOTES.md has the full reasoning;
this file states the decision and marks what's still a placeholder.

## 1. Goals

- Smaller default cursor, faster/more aggressive hide-on-idle, offset by a
  vivid, motion-driven trail so the cursor stays easy to find while moving
  without being obtrusive when idle.
- Shader-first: expose as much as possible to shader authors, keep
  plugin-side configuration sparse. Prefer optional shader imports over new
  plugin-configured settings when adding capability.
- A real compiled Hyprland plugin with its own render pipeline, not a
  `decoration:screen_shader` wrapper (ruled out: core's uniform set can't
  carry custom per-plugin state, no continuous position-history uniform
  exists to build on).

## 2. Build & versioning

- Pin to a specific Hyprland commit. No ABI stability across commits
  (`PluginAPI.hpp`), so development against a moving `master` makes it
  impossible to distinguish your own breakage from upstream's. Advance the
  pin deliberately, rebuild, re-run the validation ladder (§10), then update.
  - **Pinned commit: `efb50993780079460b0cbed1363e2166a2de1d9f` (v0.56.2)**,
    matching the host's installed package (`hyprland 0.56.2-3.1`, CachyOS,
    GCC 16.2.1, LTO). The pin tracks the host package: a host upgrade
    requires a deliberate re-pin, and the Makefile `check-pin` target fails
    the build if `external/Hyprland` isn't at the pin and warns if the
    installed headers (`/usr/include/hyprland/src/version.h`) moved off it.
  - Previous pin `1b85c7aa` (main, v0.56.0+190) was newer than the host;
    v0.56.2 is a release branch off v0.56.0. NOTES.md citations marked
    "cited at pin 1b85c7aa" refer to that commit; later ones cite `efb5099`.
- The plugin is compiled against the `external/Hyprland` checkout (after its
  build generates `version.h` and protocol headers). At `efb5099` its headers
  are byte-identical to the installed ones for everything the plugin uses, so
  one `.so` serves both the nested (debug) build and the host.
- Build via `make clear && make debug` in `external/Hyprland` (not hand-rolled
  `cmake` flags, not bare `rm -rf build`, both miss things the project's own
  Makefile handles: `-DTESTS=true` for hyprtester, generated protocol headers
  outside `build/`).
- Dev config: `hyprlandd.lua` (debug-binary convention), nested launch via
  `external/Hyprland/build/Hyprland -c hyprlandd.lua`, no separate launch
  script, adds failure surface without adding value.

## 3. Data model

- CPU-side circular buffer, single source of truth:
  ```
  struct SCursorNode {            // CPU layout
      SVec2f posPx;               // global layout pixel space, NOT per-monitor normalized
      double birthTimeMs;         // ms since plugin load
      SVec2f velocity;            // px/ms, precomputed once per node on write, not per-pixel in shader
  };
  struct SGpuNode {               // GPU (VBO) layout, 20 bytes, static_assert'd
      SVec2f posPx;
      float  birthMs;             // relative to a reference chosen at upload
      SVec2f velocity;
  };
  ```
  (`SVec2f` is a plain two-float struct; glm isn't a dependency.)
- **Timestamp precision:** CPU birth times are double. A float of ms since
  load stops resolving 1ms after ~4.6h (2^24 ms). The ordered copy (below) is
  where CPU layout converts to GPU layout: birth times are rebased on a
  reference (the newest node's birth time at upload) and narrowed to float, so
  uploaded values stay small. The shader gets `nowMs` relative to the same
  reference; only `age = nowMs - birthMs` is meaningful. Upload is gated on
  the buffer changing, not every frame; `nowMs` is a per-frame uniform.
- **Coordinate space: global layout pixels** (`CPointerManager::position()`,
  confirmed monitor-independent). Converted to each monitor's local space only
  at render time, inside that monitor's own pass element. Storing
  per-monitor-normalized coordinates was considered and rejected, it silently
  breaks at monitor boundaries, the same failure class that limits
  `decoration:screen_shader` to no per-monitor granularity.
- Velocity (and acceleration, if a future effect needs it) computed CPU-side
  once per buffer write, not recomputed per-pixel in the fragment shader.
  Never existed as a Hyprland uniform at any point in its history, confirmed
  via full git log search, so this is necessary regardless of era.
- **Buffer size: TBD**, not yet chosen. Reasoning in NOTES.md assumes
  "tens of points, maybe up to ~100" when evaluating costs below; revisit
  those if the real number lands far outside that range.
- **Wrap handling:** rebuild a temporally-ordered copy from the ring each
  frame before GPU upload. The ring buffer itself is never shifted, O(1)
  writes at the cursor position always. The reordered copy is a stateless
  per-frame projection, not a second source of truth, nothing to
  desynchronize since it's fully rebuilt every frame. (Rejected alternative:
  read the raw ring directly and exclude the wrap-boundary segment via the
  index buffer, cheaper per-vertex but only pays off at buffer sizes in the
  thousands, and moves the bookkeeping into draw-call state instead of
  removing it.)

## 4. Rendering pipeline

- Own `CShader` program(s), own render-pass element, registered per monitor
  (matches how `hypr-dynamic-cursors` and core's own pass elements work).
- **Per-frame driver, exactly once per render of each monitor:**
  - **Primary: hook on `CPointerManager::renderSoftwareCursorsFor`**, running
    the trail lifecycle (sample/insert, damage, add element) *before* calling
    the original, so the trail is added right before the cursor texture and
    draws directly beneath it (§7). Skipped for screencopy calls.
  - **Fallback: `Event::bus()->m_events.render.stage` at
    `RENDER_LAST_MOMENT`**, for renders where the hook didn't run (cursor
    hidden: `renderMonitor` only calls the cursor function when
    `shouldRenderCursor()`), so a trail still fades after a hide.
  - "Hook ran for this render" is keyed to a per-monitor serial bumped at
    `RENDER_BEGIN`, not a boolean, so a missed clear can't make a later
    render skip its lifecycle. No return path exists between `RENDER_BEGIN`,
    the cursor call and `RENDER_LAST_MOMENT`.
  - Mirror monitors are skipped.
- **Motion damage (hardware cursors):** `Event::bus()->m_events.input.mouse.move`
  damages a radius-sized box at the new pointer position on each monitor it
  touches, so a render happens even when moving the hardware cursor plane
  doesn't schedule one (Aquamarine's Wayland backend, i.e. nested). The render
  then runs the normal lifecycle. Monitors where
  `shouldSkipScheduleFrameOnMouseEvent()` holds are skipped, same as core, to
  not break fullscreen VRR. `cursor:no_hardware_cursors` is **not** required.
- **Direct scanout (known behavior, not a bug):** with hardware cursors, a
  fullscreen client eligible for direct scanout bypasses composition entirely
  (`renderMonitor` returns before any render stage), so no trail draws over
  it. Software cursors block direct scanout, so this only applies with
  hardware cursors. **Derived from source, not yet observed** (verification
  blocked, see §12).
- **GL state goes through Hyprland's caches** (`useShader`, `scissor`,
  `blend`; `bindArrayBuffer` where the pinned version has it), never raw `glUseProgram`/`glEnable`/`glBindBuffer`
  on cached state. Draw only inside the element's damage rects.
- **Blending is premultiplied alpha** (`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`); the
  fragment shader outputs `vec4(rgb * a, a)`.
- **VBO attribute layout (ribbon-ready):** the same VBO is bound twice, node i
  at offset 0 (locations 0-2) and node i + 1 at offset one stride (locations
  3-5), all divisor 1, so instance i is the segment node[i] -> node[i+1] and
  `gl_VertexID` picks the corner. The VBO carries one trailing copy of the
  newest node so the i + 1 set never reads past uploaded data.
- Point buffer is VBO-resident, not a uniform array, settled by the ribbon
  geometry technique below, which needs `next`/`prev` as real vertex
  attributes, not something expressible as a fixed-size uniform array.
- **Geometry (tangent, normal, width/taper) is computed in the vertex
  shader, not plugin C++.** This is the load-bearing shader-exposure
  decision: geometry logic in C++ means shader authors can only recolor a
  fixed shape; geometry in the vertex shader means they can rewrite the
  shape itself (taper curve, velocity-responsive width, a different style
  entirely) without plugin changes. Plugin C++'s job is raw point
  maintenance and upload only, same role as the JS side in the OGL
  reference this pattern is based on.
- Fragment shader handles color/alpha/fade. **Fade should be time-based**
  (`now - birthTimeMs`, using the buffer's stored timestamp), not
  position-along-ribbon (`uv.x`). Position-based fade makes fade duration
  vary with cursor speed (a fast flick compresses more history into less
  visible length and fades faster in wall-clock time than a slow drift),
  which doesn't match the stated "fade time" requirement. Built in stage 4
  (placeholders: 500ms, linear), pending confirmation on the validation
  ladder.

## 5. Shader extensibility contract

- Shader authors may supply custom vertex and/or fragment shaders.
- Plugin-provided per-vertex data: `position`, `next`, `prev`, `birthTimeMs`,
  `velocity`. Exact uniform set (time, per-monitor resolution, etc.) TBD,
  should stay minimal per the sparse-config goal.
- Prefab shader-utility imports, following the pattern Hyprland core already
  uses for its own shaders (`passthru.frag`/`quad.frag` as base includes,
  `blur1`→`blur2`→`blurfinish` as a chained pipeline): geometry helpers,
  fade-curve helpers, provided as example/base files shader authors can
  `#include` rather than reimplement.
- **Damage padding is config, not derived.** Automatic damage-bound detection
  via static shader analysis was considered and explicitly ruled out, not
  deferred: a blur radius or width tied to a uniform (velocity, a user
  setting) has no fixed value in the source to find, it's runtime
  data-dependent, and pattern-matching known idioms only covers shaders that
  happen to look like the ones a detector was written for. A shader-declared
  padding value, defaulting to zero / the stock geometry's known extent, is
  the actual mechanism, not a stopgap for a smarter future one. Covers both
  fragment-effect bleed (blur, glow) and vertex-shader geometry that diverges
  from the stock extent (wider strokes, pulsing thickness) under one setting.

## 6. Damage / redraw contract

- Targeted `g_pHyprRenderer->damageBox()` + `scheduleFrame()`, per monitor.
  **Not** the global `debug:damage_tracking 0` toggle, that's fine for a
  one-off manual test, real GPU cost everywhere on screen for a shipped
  plugin.
- Damage tracks the effect's lifecycle across frames, not just the current
  frame's position. Each frame's damage region is the **union of the
  previous frame's bounding box and the current frame's bounding box**
  (raw point extent + declared padding from §5). Damaging only the new
  position leaves the old box never told to repaint, producing a visible
  smear/ghost behind the moving trail. The plugin must retain the previous
  frame's box specifically to compute this union, it isn't derivable from
  the current frame alone.
- **Lifecycle, not motion, drives damage.** The trail keeps changing after the
  cursor stops (points age and fade). While any point is visible, every frame
  damages prev ∪ cur and schedules the next frame, cursor stationary or not.
  Bounds cover only still-visible points, so the box shrinks as the tail
  fades. When the last point has faded, the final box is damaged once to
  clear it, then no more damage, no scheduled frames, and no pass element:
  an idle trail must not keep the compositor rendering.
- Damage boxes are rounded **outward** in pixel space (floor left/top, ceil
  right/bottom after scaling), not `CBox::round()`, so partially covered edge
  pixels of fractional/antialiased geometry are always included. Each box goes
  into both the current frame's render damage and `CMonitor::addDamage` (the
  damage ring), so swapchain buffers with age > 1 repaint it too.

## 7. Edge cases: teleportation, hide, idle

- **Teleportation/warps:** hook `CPointerManager::warpTo`/`warpAbsolute`
  directly (`onMouseWarp` event), a real deterministic signal distinct from
  organic motion, not a distance/velocity heuristic (kept only as a backstop
  for anything reaching cursor position outside this path). Response is
  context-dependent, not one global rule: **interpolate** (sweep the trail
  between old and new position) when both endpoints are on currently-visible,
  simultaneously-rendered content (e.g. a cross-monitor focus jump, helps
  track where focus went, not misleading since both points are real and
  on-screen at once). **Break the polyline connection** (pre-teleport points
  keep aging/fading independently, post-teleport starts a fresh unconnected
  segment) when the visible content itself changed underneath the position
  (e.g. a same-monitor workspace switch, an interpolated sweep there would
  cross space that no longer shows what it showed a frame ago).
  **Unconfirmed, blocks implementation of this distinction:** whether
  `onMouseWarp`'s event payload carries enough context to tell these cases
  apart, or just that a warp happened.
- **Cursor hide:** keyed on `IHyprRenderer::shouldRenderCursor()` (same
  state core uses to skip drawing the cursor). While hidden, **no new points
  are inserted** (touch/tablet can move a hidden pointer), but the per-frame
  driver keeps running, so an existing trail finishes fading normally. This
  covers hide-on-keypress mid-motion as well as the idle-timeout case.
- **Session lock:** no trail while the session is locked (no inserts, no
  element; the last box is damaged once to clear). This deliberately differs
  from core, which keeps drawing the cursor over the lock screen.
- **Draw order:** the trail draws directly beneath the cursor: above windows,
  layers, lock screen (suppressed anyway), IME and notification/error
  overlays, below the cursor and the DPMS fade overlay. Software cursors: via
  the cursor hook (§4). Hardware cursors: the cursor plane is above composited
  content, and the hook still places the trail for frames that fall back to
  software (software locks from zoom or mirroring, tearing,
  `cursor:invisible`, nvidia auto mode, hardware plane failure). Caveat: in
  fallback-driver renders (cursor hidden) the trail is added after the DPMS
  overlay, so it can show above a DPMS fade, rare and harmless.
- **Idle/presence effects (stationary but visible cursor):** structurally out
  of reach of the ribbon pipeline as designed, it needs ≥2 distinct recent
  points to compute a tangent from; a stationary cursor stops producing new
  points (see below) and existing ones correctly fade to nothing, same
  degenerate-guard behavior as the OGL reference's coincident-point case,
  just triggered by "no motion." Not a bug, a different concern the ribbon
  was never built to cover. **Fix: a second, independent pass-element/shader
  slot** for presence/idle effects, keyed on current position + time-since-
  last-movement, not point history, symmetric with the trail's own
  extensibility (a second optional shader slot, not a new plugin-configured
  feature). Depends on: **buffer insertion happens only on real movement**,
  not every frame (now made explicit as a decision, was previously implicit).
  This makes the idle-time signal free, `now - buffer.back().birthTimeMs`,
  no new plugin state needed to drive it.
  - The idle slot's content can reuse the same ribbon-geometry technique
    (tangent/normal from an ordered point sequence) fed a synthetic pattern
    (a ring around the cursor, say) instead of real motion history, richer
    than a plain quad, and this is what the shader slot's content can
    actually be, not a competing design.
  - **Load-bearing now:** synthetic idle points must live in a separate
    buffer instance from the real motion trail. Sharing one buffer would
    either evict the still-legitimately-fading tail of real motion data, or
    interleave two kinds of point whose `birthTimeMs` means different
    things, breaking fade math either way. Buffer/upload plumbing should be
    written parameterized by instance from the start (cheap now, a refactor
    later if assumed to be singular).
  - Deferrable until after motion trails work: exact pattern shape, idle-
    trigger threshold, whether the idle slot reuses the trail's exact shader
    or a variant. A literal-shader-reuse version would need a synthetic
    stand-in for velocity (e.g. an animation phase), since a stationary
    synthetic pattern has no real per-point motion to drive the same
    width-response logic the trail uses.

## 8. Multi-monitor

- Global layout position (§3) is the single source of truth; per-monitor
  local-space conversion happens only at render time, inside each monitor's
  own pass element, mirroring what core does internally for the
  `pointer_position` screen-shader uniform.
- Cross-monitor continuity verified via `hyprctl cursorpos` tracing once
  outputs were confirmed properly aligned (`hyprctl monitors -j`, not
  eyeballed): no discontinuity once alignment was exact.
- **Rotated outputs (transform != 0): untested.** The per-monitor projection
  passes `HYPRUTILS_TRANSFORM_NORMAL` explicitly so the box-as-affine-map trick
  isn't rotated inside the box, and relies on core's `targetProjection` for
  the monitor rotation. Correct on paper, never observed.
- **Open, not yet tested:** buffer handling/culling for a monitor the trail
  isn't currently over, and damage propagation when the trail's bounding box
  straddles a seam between two outputs. Live multi-monitor hardware is now
  available for this, no longer blocked on the nested-only setup.

## 9. Config surface

- **Errors surface as notifications, never only as log lines**
  (`debug:disable_logs` defaults to true). Every failure goes through one
  reporting path (`src/Diagnostics.*`): Hyprland log always; notification plus
  a full entry in `$XDG_STATE_HOME/hyprtail/errors.log` (fallback
  `~/.local/state/hyprtail/errors.log`) once per key per load. The file is
  truncated at plugin load (current session only) and capped at 256 KiB. The
  config and shader loader report missing files and invalid values through
  the same path.
- **Failure policy:** shader compile/link failure or GL resource failure
  disables the trail (cleared once, then idle) and reports the GLSL/GL error;
  failure to install the cursor hook keeps the plugin running with degraded
  draw order (trail above the cursor) and says so; no exception escapes into
  Hyprland from any callback (hook, listeners, pass element draw, deferred
  callbacks, init/exit).

- Deliberately sparse by design (§1). Expected surface: shader path/import,
  padding value (§5), buffer size (§3). Resist adding plugin-level settings
  for anything a shader import could instead provide.
- Packaging format (single file vs. shader + separate config) not decided,
  explicitly non-blocking for implementation start.
- **Unconfirmed:** whether `addConfigValue`/`getConfigValue` (plugin config
  API) behave the same under Hyprland's Lua config provider (0.55+) as they
  did under classic hyprlang. Worth checking early, this is core plumbing,
  not a peripheral detail.

## 10. Testing

- **Lifecycle/crash-safety:** `hyprtester` (`make debug` builds it via
  `-DTESTS=true`). Load/unload cleanly, survive monitor hotplug and
  workspace changes without crashing. Existing test names in the project
  read as state/behavior assertions; **unconfirmed** whether it supports any
  pixel/frame readback, treat as state-only until checked against source.
- **Visual correctness:** staged validation ladder, a deliberately separate
  throwaway pass-element/harness, not branches in the real trail code:
  1. Dot at fixed position, no tracking, proves the pass element registers
     and `CShader::createProgram` draws anything at all.
  2. Dot at tracked position (own uniform from `g_pPointerManager->position()`),
     proves the plugin's own uniform plumbing, separate from core's
     `SHADER_POINTER` set (confirmed to be click-ripple data, not usable
     for this).
  3. Row of dots from the full buffer as vertex data, catches buffer
     layout/upload issues early.
  4. Fade over time, cursor stationary, tests targeted damage-triggered
     redraw specifically. A frozen dot means the redraw trigger is broken,
     not the fade math.
  5. Cross-monitor, exercises global-space storage + per-monitor conversion
     together, now testable live on real hardware.

## 11. Explicitly out of scope / ruled out

- Automatic damage-bound detection via shader scanning, not a future
  enhancement, ruled out for the reasons in §5.
- `decoration:screen_shader` / hyprshade-style userspace CLI wrapper as the
  primary architecture, ruled out, can't carry the custom per-plugin uniform
  state this project needs.

## 12. Open items carried into implementation (non-blocking)

- Hyprland pin chosen (`efb5099`, v0.56.2, §2); re-pin on every host package upgrade
- Buffer size default (§3)
- **Blocked: direct scanout verification (§4).** mpv fullscreen with
  `render:direct_scanout = 1` fails with a Wayland protocol error
  (`wl_surface.attach` invalid arguments) with or without the plugin loaded,
  so it's a Hyprland or mpv issue, not the plugin's. Browsers never qualify
  (not opaque, subsurfaces). "No trail over scanned-out windows" remains
  unverified until some client actually gets direct-scanned.
- `addConfigValue`/`getConfigValue` under the Lua config provider (§9)
- Config packaging format (§9)
- Fade duration and curve (§4), placeholders 500ms linear; time-based fade
  built in stage 4, pending confirmation
- Rotated outputs untested (§8)
- Color management: trail colors bypass core's `getConvertedColor`, may be
  off on HDR/color-managed outputs
- Cursor-warp tooling reliability for scripting the validation ladder
  (`hyprctl eval hl.dsp.movecursor` field names unconfirmed, `wlrctl`
  targeting issue unresolved), not urgent, manual drag testing has been
  sufficient so far
