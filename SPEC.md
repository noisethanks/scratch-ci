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
    requires a deliberate re-pin.
  - Previous pin `1b85c7aa` (main, v0.56.0+190) was newer than the host;
    v0.56.2 is a release branch off v0.56.0. NOTES.md citations marked
    "cited at pin 1b85c7aa" refer to that commit; later ones cite `efb5099`.
- **Output:** `out/hyprtail.so` (plugin name `hyprtail`).
- **Two build modes (Makefile):**
  - `make` (default, users and hyprpm): Hyprland headers from
    `pkg-config --cflags hyprland`. Under hyprpm that resolves to the headers
    hyprpm built for the running Hyprland; otherwise to the installed
    package's. Fails with a clear message if `hyprland.pc` isn't found; warns
    (doesn't fail) if the headers' commit isn't the pin. Doesn't need
    `external/`.
  - `make DEV=1` (development): headers from the `external/Hyprland`
    checkout, after its build generates `version.h` and protocol headers.
    `check-pin` fails the build if the checkout isn't at the pin or isn't
    built, and warns if the installed host headers moved off the pin. At
    `efb5099` the checkout's headers are byte-identical to the installed
    ones for everything the plugin uses, so one `.so` serves both the nested
    (debug) build and the host.
  - Both append to `CXXFLAGS` (hyprpm passes extra flags through the
    environment) and add `--no-gnu-unique` whenever the compiler is GCC.
  - Needs a C++26 compiler with `#embed` (GCC 15+; built and tested with
    GCC 16.2.1).
- **hyprpm:** `hyprpm.toml` at the repo root: repository `hyprtail`, one
  plugin `hyprtail`, `output = "out/hyprtail.so"`, `build = ["make all"]`,
  no commit pins yet (see NOTES "hyprpm"). Install: `hyprpm add <git url>`,
  `hyprpm enable hyprtail`.
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
      bool   segmentStart;        // not connected to the previous node (§7)
  };
  struct SGpuNode {               // GPU (VBO) layout, 24 bytes, static_assert'd
      SVec2f posPx;
      float  birthMs;             // relative to a reference chosen at upload
      SVec2f velocity;
      float  flags;               // bit 0: segment start
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
  then runs the normal lifecycle. `cursor:no_hardware_cursors` is **not**
  required. Skipped while locked or while the pointer is constrained.
  **VRR gate:** with the cursor shown, monitors where core's
  `shouldSkipScheduleFrameOnMouseEvent()` holds are skipped (fullscreen
  no-break case). With the cursor hidden, core's check would skip every
  adaptive-sync output (no cursor to draw), but the trail still follows the
  pointer, so only its fullscreen part is kept: skip when adaptive sync is on
  and the monitor has a fullscreen window, regardless of
  `cursor:no_break_fs_vrr`. Fullscreen VRR is never broken; a non-fullscreen
  VRR desktop gets renders while a hidden-cursor trail moves.
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
- **VBO attribute layout:** VBO = `[front pad, n0 .. n(count-1), back pad]`
  (front pad = n0 flagged as a segment start, back pad = copy of the newest).
  The same VBO is bound four times, divisor 1, at consecutive node offsets:
  `prev` = n(i-1) (pos, flags), `p0` = n(i) and `p1` = n(i+1) (pos, birth,
  velocity, flags), `next` = n(i+2) (pos, flags): 12 attribute locations of
  the 16 GLES3 guarantees. Instance i is the segment p0 -> p1, `count - 1`
  instances, `gl_VertexID` picks the corner. prev/next exist for joins.
- **Ribbon geometry (stock shader):** miter joins computed from the incoming
  and outgoing directions at each node, miter length clamped to
  `miterLimit` half-widths. Both segments meeting at a node compute its
  corners from the same inputs through the same functions, so joints are
  watertight with no double blending of translucent pixels. Half-width tapers
  with age like the alpha (tail pinches to a point as it fades).
  Degenerate cases: a zero-length neighbor direction (trail ends, pads,
  coincident nodes) falls back to the segment's own direction; a zero-length
  segment, an unconnected segment (p1 starts a segment) or a segment with
  both ends fully faded is collapsed outside clip space.
- **Minimum insert spacing** (placeholder 2 logical px): segments much
  shorter than the ribbon width make inner miters fold over neighbors
  (double blending). The newest node can lag the pointer by less than that,
  hidden under the cursor.
- **Bounds for damage** include the node just older than the oldest visible
  one when they're connected: that segment still draws, fading toward the
  older end.
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
- **Shader files:** standalone GLSL ES 3.00 `.vert`/`.frag` files, laid out
  exactly like user-supplied shaders will be. The stock pair lives in
  `shaders/trail.vert` and `shaders/trail.frag`, embedded into the plugin at
  build time with C++26 `#embed` (the Makefile lists them as dependencies).
  The config loader reads the same kind of file from a user path
  (`plugin:hyprtail:vertex_shader` / `fragment_shader`, §9). Their header
  comments are the contract; the stock pair doubles as the reference user
  shader. Keep them ASCII (GLSL ES drivers aren't reliable with UTF-8, even
  in comments).
- **Packaging: separate `.vert` and `.frag` paths**, not a single file. They
  map 1:1 to GL stages and each can be overridden alone (the other stays
  built-in). Mixing contract: a custom fragment shader with the stock vertex
  shader reads `v_side`, `v_alpha`, `v_color`; a custom vertex shader with
  the stock fragment shader must write them. A mismatch fails at link time
  and is reported.
- **Includes (loader-side preprocessor, `src/ShaderSource.*`):** GLSL ES has
  no `#include`, so the plugin resolves it before compiling.
  `#include "hyprtail/<name>"` pulls in a built-in prefab (embedded);
  any other path is relative to the including file (absolute and `~/` also
  work). Each file is included at most once, cycles are errors, depth is
  limited to 16, included files must not contain `#version`. Built-in shaders
  may only include `hyprtail/` prefabs. Every inclusion is wrapped in
  `#line <n> <source-id>`, so GLSL errors are reported as `file:line`.
  Include prefabs after the `precision` statement (fragment shaders have no
  default float precision).
- **Prefab library:** `hyprtail/ribbon.glsl` (`ht_startsSegment`,
  `ht_collapsedPosition`, `ht_dirBetween`, `ht_jointOffset`, `HT_EPS`) and
  `hyprtail/fade.glsl` (`ht_life`, `ht_faded`), and for fragment shaders
  only `hyprtail/sdf.glsl` (`ht_sdCircle`, `ht_sdRing`, `ht_coverage`; uses
  `fwidth`, so it doesn't compile in a vertex shader). Functions only, `ht_`
  prefixed, parameters instead of uniforms. The stock trail and idle shaders
  are built on them. Source in `shaders/hyprtail/`.
- Plugin-provided per-instance data (vertex contract, `shaders/trail.vert`):
  `a_prevPos/Flags`, `a_p0Pos/BirthMs/Vel/Flags`, `a_p1Pos/BirthMs/Vel/Flags`,
  `a_nextPos/Flags` at fixed locations 0-11. Uniforms: `proj`, `nowMs`,
  `fadeMs`, `widthPx`, `miterLimit`, `speedRef`, `colorSlow`, `colorFast`.
  Output is premultiplied alpha. Should stay minimal per the sparse-config
  goal.
- **Color management:** the palette (`color_slow`, `color_fast` settings,
  sRGB) is converted on the CPU at draw time to the current framebuffer's
  image description with core's `getConvertedColor` (transfer function,
  primaries, HDR luminance, tonemapping, SDR brightness), opaque, with the
  configured alpha passed separately: exactly how core handles its own solid
  colors and borders. Shaders get `vec4 colorSlow/colorFast` (rgb converted,
  a = alpha) and blend in the output's encoding, as core does. **Colors a
  shader computes or hardcodes are unmanaged**: correct on SDR sRGB outputs,
  wrong on HDR / wide-gamut color-managed ones; documented in the shader
  contract. A per-pixel conversion like core's `CM.glsl` isn't available to
  plugins: its uniforms are set by `passCMUniforms`, which is private.
- **Damage padding** around the node extent = stock extent
  (`widthPx / 2 * miterLimit + 1px`) + shader-declared padding + config
  `damage_padding`, all additive. A shader declares its extra reach with
  `#pragma hyprtail padding <px>` (0..4096) in either stage or any include;
  the largest declaration of the active program counts, so a shared shader
  file carries its own extent. The pragma line is removed before compiling;
  any other `#pragma hyprtail` is an error. (Not recognized inside block
  comments specially: a commented-out pragma line still counts.)
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
- **Shared per-monitor damage helper** (`hyprtail::CMonitorDamage`,
  `src/RenderUtil.*`), used by the trail and the idle slot: prev ∪ cur per
  monitor, clear once. **Frames without damage:** if a render has no damage
  at all, `renderMonitor` skips the workspace, so nothing underneath is
  repainted. The helper then only damages the ring (scheduling a proper
  frame) and the caller draws nothing that frame. Otherwise an element would
  land on stale content. Anything that needs a render to start (the idle
  timer) damages the ring before the render instead of only calling
  `scheduleFrame`.
- Damage boxes are rounded **outward** in pixel space (floor left/top, ceil
  right/bottom after scaling), not `CBox::round()`, so partially covered edge
  pixels of fractional/antialiased geometry are always included. Each box goes
  into both the current frame's render damage and `CMonitor::addDamage` (the
  damage ring), so swapchain buffers with age > 1 repaint it too.

## 7. Edge cases: teleportation, hide, idle

- **Trail is decoupled from cursor visibility.** It follows pointer motion
  whether the cursor is shown, hidden (inactivity timeout, key press,
  touch/tablet hide) or `cursor:invisible`. `shouldRenderCursor()` no longer
  gates insertion; while it's false the cursor hook doesn't run and the
  `RENDER_LAST_MOMENT` fallback drives the lifecycle, and motion damage still
  starts renders. A client-hidden cursor (null cursor surface) looks the same
  to the plugin: core's `m_cursorHidden`/`m_cursorHasSurface` are protected,
  so the two can't be told apart.
- **Pointer constraints:** while `CInputManager::isConstrained()` (a client
  has locked or confined the pointer, i.e. owns it: games, mostly) nothing is
  inserted, motion damage is skipped, warps are ignored (constraints correct
  the pointer through the warp path), and the next insert after the
  constraint starts a new segment. This also keeps the trail off games that
  hide the cursor. An unconstrained client that hides the cursor still gets a
  trail following the pointer.
- **Teleportation/warps: break vs. connect.** **Break the polyline
  connection** (pre-jump points keep aging/fading independently, the next
  point starts a fresh unconnected segment):
  - unconditionally when the content underneath changed:
    `Event::bus()->m_events.workspace.active`, `.specialActive`,
    `.moveToMonitor`, and on session lock;
  - after a pointer constraint (above);
  - on a warp through `CPointerController::warpTo` (hooked; dispatchers,
    layouts, focus changes) when **`interpolateWarps`** is false (placeholder
    setting, default false). When true, such warps connect.

  **Every other jump connects**, drawing the straight "interpolated" sweep
  from the old to the new position. **Coverage gap:** warps through the four
  sites that call `CPointerManager::warpTo` directly (pointer-warp protocol,
  input capture, two internal ones) always connect, whatever
  `interpolateWarps` says. No distance/velocity heuristic.
  - Corrected: `onMouseWarp` is **not** a teleport signal. It handles
    absolute-motion devices (tablets, VM pointers), i.e. continuous motion.
    `CPointerManager::warpTo` carries no reason and is also called by
    ordinary relative motion; `CPointerController::warpTo` (the programmatic
    warp layer) only gets `(pos, force)`, and several warp sites bypass it.
    The workspace events are the actual "content changed" signal.
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
- **Idle/presence effects (stationary but visible cursor):** out of reach
  of the ribbon pipeline (it needs motion to produce points; a stationary
  cursor's trail correctly fades to nothing). Handled by **a second,
  independent pass element and shader slot**, keyed on current pointer
  position and time since last movement, not point history.
  - **Design: a quad around the pointer, drawn by the fragment shader**
    (decided; replaces the earlier synthetic-point idea). One instance, a
    quad covering `center +- (radius + padding)`; the fragment shader draws
    anything (rings, pulses, crosshairs) with signed-distance functions.
    Rejected: feeding a synthetic point pattern (e.g. a ring of points)
    through the ribbon pipeline. The CPU would own the shape (shader authors
    couldn't change it), the stock trail shader fades by age so it couldn't
    be reused anyway, and it needs more plumbing for less freedom. With no
    points there is no second buffer to keep separate.
  - **Trigger and lifecycle:** starts after `idle_delay_ms` without pointer
    motion; stops immediately on motion (its box is cleared once); ends after
    `idle_duration_ms` (finite by default: an endless animation would render
    every frame for as long as the pointer is idle, against "an idle trail
    must not keep the compositor rendering"; `0` = forever is an explicit
    opt-in). Suppressed while locked, while the pointer is constrained, and
    when the instance is disabled, like the trail.
  - **Cursor visibility:** `idle_when_hidden` defaults to **false**. The idle
    effect marks where the cursor is; if the cursor was hidden (inactivity
    timeout, key press, `cursor:invisible`), drawing around it defeats the
    hide. This deliberately differs from the trail, which follows motion
    regardless of visibility.
  - **Settings** (§9): `idle_enabled` (false), `idle_delay_ms` (500),
    `idle_duration_ms` (1500; 0 = until the pointer moves), `idle_radius`
    (24 px), `idle_when_hidden` (false), `idle_vertex_shader` /
    `idle_fragment_shader` (`""` = built-in). Shared with the trail:
    `damage_padding`, `color_slow`/`color_fast`, `#pragma hyprtail padding`.
  - **Shader contract** (`shaders/idle.vert` header): no vertex attributes;
    `gl_VertexID` 0-3 picks the corner of the square `center +- extentPx`.
    The built-in vertex shader outputs `v_local` (px offset from the center)
    and `v_uv` (0..1), so most users only replace the fragment shader.
    Uniforms: `proj`, `center` (global px), `extentPx` (radius + declared +
    configured padding), `radiusPx`, `idleMs` (time since the effect
    started), `durationMs`, `colorSlow`/`colorFast` (color-managed palette).
    Premultiplied output. Stock look (`shaders/idle.frag`): a single ring in
    `color_slow`, expanding from a quarter of the radius to the edge and
    fading out over the duration (looping with a 1.2 s period when the
    duration is 0).
  - **Timing:** motion (pointer events, hooked warps, or a position change
    seen during any render) restarts a timer on Hyprland's event loop
    (`wl_event_loop_add_timer`) set to `idle_delay_ms`; when it fires, the
    square is damaged so a render happens and the lifecycle starts the
    effect. While shown it damages every frame; motion, a hide (unless
    `idle_when_hidden`), lock, a constraint or the end of the duration clears
    it once.
  - **Implementation:** `SIdleInstance` / `CIdlePassElement`
    (`src/IdlePassElement.*`), drawn after the trail (so above it, both
    beneath the cursor), with the shared per-monitor damage helper (§6) and
    the shared shader-slot helper (`hyprtail::CShaderSlot`,
    `src/ShaderSlot.*`: built-in and user stages, include preprocessing,
    deferred compile, keep previous program; reports under `shader:idle`).

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
- **Failure policy:** a user shader that fails to load, preprocess, compile
  or link is reported (GLSL errors mapped to `file:line`) and the previous
  working program stays (the built-in one if there never was one); only a
  failing built-in shader or a GL resource failure disables the trail
  (cleared once, then idle) and reports the error;
  failure to install the cursor hook keeps the plugin running with degraded
  draw order (trail above the cursor) and says so; no exception escapes into
  Hyprland from any callback (hook, listeners, pass element draw, deferred
  callbacks, init/exit).

- Deliberately sparse by design (§1). Resist adding plugin-level settings for
  anything a shader import could instead provide.
- **Config API: V2 only.** `HyprlandAPI::addConfigValueV2` with
  `Config::Values::C{Float,Int,Bool,String}Value` works with both the Lua and
  the legacy hyprlang provider. V1 `addConfigValue`/`getConfigValue` only
  work with the legacy provider at the pin (settled, see NOTES).
- **Settings** (`src/Config.*`; defaults are the built-in behavior):

  | Key | Type | Default | Range |
  |---|---|---|---|
  | `fade_ms` | float | 500 | 1..60000 |
  | `width` | float, logical px | 8 | 0..512 |
  | `capacity` | int, points | 64 | 2..4096 |
  | `min_spacing` | float, logical px | 2 | 0..256 |
  | `miter_limit` | float, half-widths | 2 | 1..16 |
  | `interpolate_warps` | bool | false | |
  | `damage_padding` | float, px, additive (§5) | 0 | 0..4096 |
  | `vertex_shader` | path | `""` = built-in | |
  | `fragment_shader` | path | `""` = built-in | |
  | `color_slow` | color (ARGB, sRGB) | `0xFF1A66FF` | |
  | `color_fast` | color (ARGB, sRGB) | `0xFFFF1A1A` | |
  | `idle_enabled` | bool | false | |
  | `idle_delay_ms` | float | 500 | 0..60000 |
  | `idle_duration_ms` | float, 0 = until moved | 1500 | 0..600000 |
  | `idle_radius` | float, logical px | 24 | 1..1024 |
  | `idle_when_hidden` | bool | false | |
  | `idle_vertex_shader` | path | `""` = built-in | |
  | `idle_fragment_shader` | path | `""` = built-in | |

  Lua: `hl.config({ plugin = { hyprtail = { fade_ms = 400 } } })`;
  hyprlang: `plugin:hyprtail:fade_ms = 400`. Colors in Lua must be strings
  in Hyprland's color syntax, e.g. `color_slow = "rgba(1a66ffff)"`; numbers
  are rejected (`LuaConfigColor.cpp:25-40` at `efb5099`). Shader paths: `~` and `~/`
  expand to `$HOME`; relative paths resolve against the directory of the
  config file in use (covers `-c`), falling back to `$XDG_CONFIG_HOME/hypr`
  or `~/.config/hypr`. Out-of-range values are rejected by Hyprland's parser
  and re-checked by the plugin (report under `config:<key>`, previous value
  kept).
- **Hot reload:** values are re-read on every `config.reloaded`. Scalars
  apply immediately. A capacity change resizes the ring keeping the newest
  points; the VBO is reallocated at the next draw. Shaders are re-read and
  preprocessed on the main thread and compiled at the next render (GL
  current), which the plugin schedules; a failure is reported (key re-armed
  every attempt) and the previous working program stays, or the built-in one
  if there never was one. Shader files and their includes are also watched
  (inotify on their directories, on Hyprland's event loop), so saving a
  shader reloads it without a Hyprland reload.
- **First-parse caveat:** `hl.plugin.load` only records the path; plugins
  load after the config parse, so the very first parse sees
  `plugin.hyprtail.*` as unknown keys. Not visible at startup (the queued
  error bar is cancelled and the config re-parsed during init, before any
  frame). Visible whenever the keys are set but the plugin isn't loaded
  (manual `hyprctl plugin load` before loading it, after an unload).

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
- Default tuning of the settings (§9): capacity, fade duration, width,
  spacing; current defaults are the original placeholders
- **Blocked: direct scanout verification (§4).** mpv fullscreen with
  `render:direct_scanout = 1` fails with a Wayland protocol error
  (`wl_surface.attach` invalid arguments) with or without the plugin loaded,
  so it's a Hyprland or mpv issue, not the plugin's. Browsers never qualify
  (not opaque, subsurfaces). "No trail over scanned-out windows" remains
  unverified until some client actually gets direct-scanned.
- Fade curve (§4): linear in the stock shader (`ht_life`); duration is the
  `fade_ms` setting. Other curves are a shader/prefab matter, not a setting
- Rotated outputs untested (§8)
- Color management built (§5): palette converted like core's colors;
  verify on an HDR / color-managed output
- Performance: measure before optimizing damage (NOTES "Performance")
- **Backlog:** bezier curves for warp interpolation (`interpolateWarps`
  currently draws a straight segment)
- Cursor-warp tooling reliability for scripting the validation ladder
  (`hyprctl eval hl.dsp.movecursor` field names unconfirmed, `wlrctl`
  targeting issue unresolved), not urgent, manual drag testing has been
  sufficient so far
