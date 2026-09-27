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
  `hyprpm enable hyprtail`. **After `hyprpm update`, run `hyprpm reload -f`**
  (or restart Hyprland): `hyprpm update` builds the new version but does not
  reload a plugin that's already loaded, so the old build keeps running.
- **One instance only:** Hyprland only refuses loading the same *path*
  twice, so two builds from different paths (e.g. a manual `out/hyprtail.so`
  and hyprpm's copy) would both load and fight over the config keys and
  hooks. At init the plugin looks for another loaded plugin named `hyprtail`
  (or an older `hyprtail-stageN`) and, if found, refuses to load before
  touching anything: one error notification naming the other instance's
  path and how to unload it (`hyprpm disable hyprtail` for hyprpm's copy,
  else `hyprctl plugin unload <path>`).
- **Build revision:** the Makefile writes `out/rev.hpp` (git short hash,
  `-dirty` for uncommitted changes; rewritten only when it changes). It shows
  in the "loaded" notification, the log, the plugin version string and the
  `errors.log` header, so the running build is always identifiable.
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

> **Superseded in part by contract 2 (§13.2-13.6, built in phase 2).** No
> longer true below: the stock pair is now `shaders/classic/` (ribbon.* for
> the trail layer, ring.* for the idle layer), there are no raw attributes
> or fixed uniforms (the prelude declares them), padding is an expression
> and no longer adds to a stock extent, and `hyprtail/ribbon.glsl` no longer
> has `ht_startsSegment` (nodes carry `segmentStart`). Still true: separate
> `.vert`/`.frag` paths, the contract check (now against the prelude and the
> program's own params), the fallback policy, the include preprocessor and
> prefabs, and color management (now for every `color` param).

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
- **Contract enforcement:** after linking, the program's active uniforms
  and attribute locations are checked against what the plugin provides for
  that slot (trail: the uniforms and locations 0-11 in `shaders/trail.vert`;
  idle: its uniforms, no attributes). Anything else is rejected with a
  message naming it: the plugin would never set it, so it would read 0 (a
  shader written for a newer plugin version reading `colorSlow` on an older
  one draws fully transparent).
- **Fallback when a user shader fails** (compile, link, contract, or file
  error): if it's the same files as the active program (an edit with a
  mistake), the last working version of those files stays; if the files
  differ (a config change) or nothing is active, the built-in program is
  used. Keeping an old program across a config change would show something
  that no longer matches the config, possibly nothing at all.
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
- **Rotated outputs:** transform 1 (90°) confirmed on the host. The
  per-monitor projection passes `HYPRUTILS_TRANSFORM_NORMAL` explicitly so the
  box-as-affine-map trick isn't rotated inside the box, and relies on core's
  `targetProjection` for the monitor rotation. Transform 4 (flipped): the
  host froze when it was applied; a second freeze happened with no transform
  change (fullscreen game start), so the cause is not isolated. No plugin code
  path depends on the transform (NOTES "Freeze analysis").
- **Hotplug:** per-monitor state (render serials, last damage boxes, layout
  snapshot) is keyed by `CMonitor*` and dropped on `monitor.removed` and
  `monitor.destroyMon`, so a monitor allocated at a freed address starts
  clean. On `monitor.layoutChanged` the trail is cleared (and the last drawn
  boxes repainted) only if a monitor appeared, moved or changed size: trail
  points are global, so after a layout change they could draw in the wrong
  place until they fade. Config reloads that change nothing don't clear it.
  A monitor that disappears mid-fade needs nothing more: its points have
  nowhere to draw, and core warps the pointer off it, which breaks the trail
  (unless `interpolate_warps`).
- **Open, not yet tested:** buffer handling/culling for a monitor the trail
  isn't currently over, and damage propagation when the trail's bounding box
  straddles a seam between two outputs. Live multi-monitor hardware is now
  available for this, no longer blocked on the nested-only setup.

## 9. Config surface

> **Superseded in part by phase 4 of §13.16 (built).** The key table below
> and its "feed the built-in classic preset" paragraph describe contract-1
> config keys that no longer exist: `fade_ms`, `width`, `miter_limit`,
> `color_slow`, `color_fast`, `vertex_shader`, `fragment_shader`, and all
> `idle_*` keys are removed outright (§13.8), with no compatibility shim --
> Hyprland's own "unknown config key" error, not a plugin one. What
> replaces each is in §13.7/§13.8: shader identity and per-layer parameter
> defaults move into `preset.conf` (built-in `subtle`, `classic`, or a user
> preset directory); `preset = "<name>"` selects one; `layer1_vertex` ..
> `layer4_fragment` still override a layer's shader by config; the `params`
> string still overrides a layer's parameters by config. Kept as-is,
> deliberately not per that literal removal list: `interpolate_warps` --
> its named replacement (`warp`, §13.10) is phase 6, still unbuilt, and
> removing the old key now would delete the "connect the trail across
> warps" feature outright with nothing to take its place, which is a
> regression, not the rename §13.8 otherwise describes. `capacity`,
> `min_spacing` and `damage_padding` were never on the removed list and are
> unaffected. Still true below: the reporting, batching, status and
> failure-policy paragraphs (all still built as described); the API choice
> (V2 only); the sparse-by-design principle.

- **Errors surface as notifications, never only as log lines**
  (`debug:disable_logs` defaults to true). Every failure goes through one
  reporting path (`src/Diagnostics.*`): Hyprland log always; notification plus
  a full entry in `$XDG_STATE_HOME/hyprtail/errors.log` (fallback
  `~/.local/state/hyprtail/errors.log`) once per key per load. At plugin load
  the previous file is kept as `errors.log.1` (so a crashed or hung session's
  log survives the next login) and a fresh one started; every write is
  fsynced (survives a power-off); capped at 256 KiB. The
  config and shader loader report missing files and invalid values through
  the same path.
- **Batched while loading or reloading (built, phase 1 of §13):** reports
  made during plugin load, a config reload or a shader file change are
  collected, and one summary notification lists the count and the first
  few headlines, errors first, with the errors.log path. The batch ends once
  the shaders it queued have been compiled at the next render (the idle
  slot only counts while its effect can run), or after 2 s. Reports at
  other times notify one by one, as before.
- **Status:** `hyprctl hyprtail` (`-j` for JSON; built, phase 1 of §13):
  build and Hyprland hashes, hooks, trail and idle state with their shader
  files and last compile outcome, per-monitor render counts (lifecycle via
  hook vs. fallback, draws, renders without damage) and last drawn boxes,
  report counts, errors.log path.
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

  Since phase 2 of §13 these keys feed the built-in `classic` preset:
  `fade_ms`, `width`, `miter_limit`, `color_slow`, `color_fast` and the
  `vertex_shader` / `fragment_shader` paths go to its `trail` layer;
  `idle_enabled`, `idle_delay_ms` (`start_ms`), `idle_duration_ms`,
  `idle_radius` (`radius`), `idle_when_hidden` (`draw_when_cursor_hidden`),
  `color_slow` (`color`) and the idle shader paths to its `idle` layer.
  Shader files set here must follow contract 2 (§13.2-13.6).

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

- **Lifecycle/crash-safety: `make smoke`**, run at every re-pin after
  `make clear && make debug` in external/Hyprland. A hyprtester test
  (`tests/hyprtester/hyprtail_smoke.cpp`, config additions in
  `tests/hyprtester/smoke.lua`) in a headless Hyprland from the checkout:
  load, trail and idle effect, duplicate load refused, three rounds of
  adding an output, drawing on it and removing it mid-fade, unload, pointer
  motion after unload, reload (errors.log.1 kept), final unload. After every
  step the compositor must still answer IPC and errors.log must have nothing
  past its header (warnings count). Also checks `hyprctl hyprtail` in both
  formats. State-only: no pixel readback. Run from a terminal in a Wayland
  session: the test Hyprland has its own short `XDG_RUNTIME_DIR` (so
  hyprtester can't reach the running session, and its socket paths fit) but
  connects to the session's Wayland socket as a client for its GPU
  allocator (NOTES "Smoke test environment"). Uses the same GPU. Workspace
  changes are not covered.
- **Unit tests: `make test-unit`** (`tests/unit/unit.cpp`, `SANITIZE=1` for
  AddressSanitizer and UBSan). No compositor, no GL: parameter pragmas and
  values, padding expressions, shader preprocessing (contract 2 rules), the
  node ring (distance restarts, seeds, birth-time rebasing). Then the
  preprocessed built-in shaders through glslangValidator, each file and
  every vertex/fragment pairing linked. A reference compiler: drivers can
  still differ.
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
  currently draws a straight segment). Now in scope of the §13 draft.
- Cursor-warp tooling reliability for scripting the validation ladder
  (`hyprctl eval hl.dsp.movecursor` field names unconfirmed, `wlrctl`
  targeting issue unresolved), not urgent, manual drag testing has been
  sufficient so far

## 13. Customization model v2 (DRAFT, partly built)

**Status:** phases 0-2 of §13.16 are built; phase 3's checks and config
front end (`params`, `layer1_vertex`..`layer4_fragment`, `expects`, the
pre-link varying check), phase 4's presets/config-surface-v2
(`preset.conf`, `preset =`, the old-key removal), and phase 7's
screenshare exclude (moved up ahead of phase 5) are also built. Phases
2-4 and 7 are untested (not yet run in any compositor). Built parts are
marked **Built (phase N)** below, with any difference from the draft;
everything else is still proposal.
Decisions taken so far: no dynamic config keys; parameters go in one
plugin-validated `params` string, and per-layer shader overrides are static
keys indexed by layer number (§13.5, §13.8; NOTES "Phase 0 spikes", S2);
screenshare late drawing accepted with its
tradeoffs; the quad layer anchors to the pointer, not the newest node; no
compatibility with the current contract; `subtle` default, `classic`
optional; `preset.conf` is plain `key = value` lines; padding expressions are
numbers, parameter names, `+ - * /` and parentheses only. Where it
contradicts §5, §7 or §9, those sections still describe the code as it is;
this section describes the intended replacement. Citations at `efb5099`
unless noted. "Unverified" marks what needs a spike (§13.16, phase 0).
Where a built part contradicts §5, §7 or §9, this section wins (§5 carries a
pointer).

### 13.1 Model

Three stages:

- **Source** (CPU) produces nodes. Sources exist only for effects where
  points affect each other. For now there is one: `pointer`, the pointer
  history ring (today's `CTrailRing`). Backlog: a spring-chain source.
- **Geometry** is the vertex shader. It turns nodes into primitives under a
  declared topology (§13.3).
- **Shading** is the fragment shader.

**Principle:** anything expressible as a function of a node's birth position,
age and random seed lives in shaders, not in plugin settings. Nodes also
keep the velocity and cumulative path distance at birth: both are properties
of the recorded path, computed once at insert.

A **layer** is (source, geometry shader, shading shader, parameter values).
A **preset** is an ordered list of layers (first = bottom) plus defaults.
All layers of a preset share the one source instance and its VBO. One pass
element draws them in order, and its damage is the union of the layers'
extents. Effects are therefore stackable (core ribbon plus glow) without a
second ring, a second upload or a second damage lifecycle.

**Built (phase 2), difference:** each layer keeps its own per-monitor damage
lifecycle (`CLayer::damage`) inside the one pass element, so damage stays
exact per layer instead of one union box. The only preset is the hardcoded
`classic` (`src/Layer.cpp`), mapped from the current config keys (§13.8
not built yet).

### 13.2 Nodes and the shader-side contract

- **Fields:** birth position (global logical px, emit offset already applied,
  §13.9), birth time, velocity at birth (px/ms), path distance (px), seed
  (uint32), flags (segment start).
- **GPU layout:** 28 bytes. The seed and flags are packed into one integer
  attribute (`glVertexAttribIPointer`). GLES 3.0 has integer attributes, and
  the context is 3.2 with a 3.0 fallback (`OpenGL.cpp:199-220`).
- **Relative values:** birth time is uploaded relative to the newest node, so
  float precision doesn't degrade over a long session. Path distance is
  measured from the start of the node's segment and restarts at every break
  (decided: counting through a gap would show as a jump in any shader that
  uses distance for spacing, the same artifact class as the wrap-seam and
  teleport-seam fixes; restarting keeps each segment's geometry
  self-contained). It is uploaded as is.
- **Seed:** hash of a per-load random value and the insertion counter. It is
  stable for the node's life.
- **Shaders never touch attributes.** The loader injects a prelude that
  declares the attributes and exposes accessors plus topology-specific
  helpers. The contract is the prelude API, so the attribute layout can
  change without breaking user shaders. Contract 1 (§5) exposed raw
  attributes at fixed locations 0-11; contract 2 drops that.

**Built (phase 2):**
- `SGpuNode` (`src/TrailBuffer.hpp`): pos (8), birthMs (4), velocity (8),
  distPx (4), bits (4: bit 0 segment start, bits 1-31 seed) = 28 bytes.
  Seed: splitmix64 of a per-load `std::random_device` value plus the
  insertion counter (`CTrailRing::insert`).
- `CNodeBuffer` (`src/LayerPassElement.*`): same VBO layout as before (front
  pad, nodes, back pad), four bindings with divisor 1. 14 locations: prev
  0-1 (pos, bits), p0 2-6 and p1 7-11 (pos, birth, velocity, dist, bits),
  next 12-13 (pos, bits). GLES 3.0 guarantees 16. The bits attribute uses
  `glVertexAttribIPointer` and is read as `uint`.
- Prelude (`shaders/prelude/`, embedded): `common.glsl` (precision highp for
  float and int, built-in uniforms `ht_proj`, `ht_nowMs`, `ht_stillMs`,
  `ht_anchor`, `ht_extentPx`, and the reserved lifecycle uniforms `fade_ms`,
  `start_ms`, `duration_ms`), `vertex.glsl` (standard varyings as `out`,
  `ht_initVaryings()`, `ht_toClip()`, `HtNode`), `path.glsl` (attributes,
  `ht_prev()`, `ht_p0()`, `ht_p1()`, `ht_next()`, `ht_atEnd()`,
  `ht_side()`), `quad.glsl` (`ht_corner()`), `fragment.glsl` (varyings as
  `in`, `ht_fragColor`). `HtNode` = pos, age, vel, dist, seed (0..1),
  segmentStart; for prev/next only pos, seed and segmentStart are set. Every
  built-in and reserved uniform is set for every layer, whatever its
  topology.

### 13.3 Topologies

A geometry shader declares exactly one topology:
`#pragma hyprtail topology <kind> [options]`.

| Topology | Instances | Vertices per instance | Node access | For |
|---|---|---|---|---|
| `path` | visible segments | 4 | prev, p0, p1, next (today's four bindings, divisor 1) | ribbon |
| `path smooth N` | visible segments | 2(N+1), N <= 32 | same | smoothed ribbon |
| `instanced K` | visible nodes x K, K <= 64 | 4 | one node, divisor K; `ht_instance` = `gl_InstanceID % K` | particles, spray, jitter |
| `quad` | 1 | 4 | no nodes: pointer position (plus the layer's own offset) and `ht_stillMs()` | idle / presence effect |

- **Drawing only the visible range.** GLES 3.0 has no base-instance draw, so
  the attribute pointers are re-pointed at the first visible node before
  each draw. The visible nodes are always the newest suffix of the ring.
- **Mismatches are refused.** A fragment shader may declare
  `#pragma hyprtail expects <kind>[,<kind>...]`. The loader refuses a pair
  whose kinds don't match, with a plain message, e.g. "glow.frag expects
  topology path; dots.vert declares instanced 8". A geometry shader with no
  topology pragma is an error.
- **Catmull-Rom (`path smooth N`).** The prelude's `ht_curve(t)` evaluates
  a centripetal Catmull-Rom curve (alpha 0.5, no cusps or self-intersection
  within a segment) through prev, p0, p1, next. The existing four bindings
  are exactly its control points. Neighbouring segments share their end
  point and tangent, so joins need no miter. At segment breaks the padded
  duplicate end points make the curve degenerate to a straight end. Age and
  distance are interpolated linearly in t.
  - **Damage stays exact:** each segment is one cubic. The CPU computes that
    cubic's Bezier control points with the same formula as the prelude and
    adds them to the bounds; the curve lies inside their hull. Anything a
    shader does beyond the prelude curve must be covered by its padding
    declaration (§13.5).

**Built (phase 2):** `path` (without `smooth`) and `quad`. Differences: quad
stillness is the uniform `ht_stillMs`, not a function; path layers still
draw all `size() - 1` instances (the visible-range re-pointing comes with
phase 5). Not built: `path smooth N`, `instanced K` (phase 5). A geometry
shader without a topology pragma, a second one, an unknown kind, or one in
a fragment shader or include is refused.

**Built (phase 3):** `expects`, fragment stage, main file, at most once,
comma-separated kinds with no spaces (`ShaderSource.cpp`'s `RE_EXPECTS` /
`splitComma`). Checked in `ShaderSlot.cpp`'s `programInfo()` against the
paired vertex shader's actual topology; a mismatch is refused with the
plain message this section's example shows.

### 13.4 Visibility and lifecycle parameters

The fixed `fade_ms` and `idle_*` settings become reserved parameter names
(§13.5). The CPU reads them for visibility, ring retention, damage and
timers:

- **`path`, `instanced`:** `fade_ms`. A node is visible while its age is
  below `fade_ms`. The ring keeps nodes for the largest value across layers
  (not built: retention is still by `capacity` only, as before).
- **`quad`:** `start_ms` and `duration_ms` (0 = until the pointer moves).
  The quad is visible while the time since the last pointer motion is in
  [`start_ms`, `start_ms + duration_ms`).
  - **The idle effect becomes a quad layer.** Its timer generalizes: arm for
    the earliest future window start across layers.
  - **Anchored to the pointer, not the newest node.** With an emit offset the
    newest node is no longer at the pointer, and nodes lag it by up to
    `min_spacing` anyway. A quad layer has its own `offset_from` /
    `offset` pair, with the same meaning as `emit_from` / `emit_offset`
    (§13.9), default the hotspot. Stillness is tracked from pointer motion
    as today (`lastMotionMs`), not from node ages.
- **Layer on/off (proposed):** a reserved `enabled` parameter, default true,
  so `params = "glow:enabled=false"` turns a preset's layer off without a
  new config key.
- **Cursor hidden:** a per-layer `draw_when_cursor_hidden`. Default true for
  `path` and `instanced` (today: the trail is decoupled from cursor
  visibility, §7), false for `quad` (today's `idle_when_hidden = false`).

**Built (phase 2):** reserved `enabled`, `draw_when_cursor_hidden`,
`fade_ms`, `start_ms`, `duration_ms` (`shader::reservedParams()`), resolved
per layer (`CLayer::resolve`). A layer with `enabled = false` isn't
compiled. The idle effect is the classic preset's `idle` quad layer,
anchored at the pointer; the stillness timer arms for the earliest
`start_ms` across enabled quad layers. Not built: the quad layer's own
offset (phase 6).

### 13.5 Shader-declared parameters, padding, contract version

- **Declaration:** `#pragma hyprtail param <type> <name> <default> [<min>
  <max>]`, with types `float`, `int`, `bool`, `vec2`, `color`. The loader
  replaces the pragma line with `uniform <glsl type> <name>;` on the same
  line, so `#line` mapping stays intact. The uniform joins the program's
  contract (§5 contract check).
  - A layer's parameter set is the union over both stages. The same name
    with two types is an error.
  - **Colors** use Hyprland's color syntax and are converted on the CPU to
    the framebuffer's image description, exactly like today's palette
    (`getConvertedColor`), as `vec4` with alpha passed separately.
- **Setting parameters: one `params` string, validated by the plugin.**
  - **Syntax:** `params = "<layer>:<name>=<value> ..."`, whitespace-separated,
    the same `<layer>:` prefix as `preset.conf` (§13.7), e.g.
    `params = "core:width=4 glow:radius=18 core:color=rgba(ffffffa0)"`.
    Values: numbers for `float`/`int`, `true`/`false` for `bool`, `x,y` for
    `vec2`, Hyprland color syntax without spaces for `color`.
  - **Validation:** each entry is type- and range-checked against the
    layer's `param` pragma. Unknown layers, unknown or stale parameter names,
    bad values and out-of-range values are **plugin warnings** in the
    batched summary (§13.11); the entry is ignored and the preset or pragma
    default stays. Never a Hyprland config error.
  - **Precedence:** pragma default < `preset.conf` < `params`.
  - Reserved lifecycle parameters (§13.4) and the quad offset
    (`offset_from`, `offset`) are set the same way, e.g.
    `params = "idle:start_ms=800 idle:offset=0,-12"`.
  - Re-parsed on every `config.reloaded` and shader change. No extra reload.
  - **Why not typed keys:** Hyprland's value types are a fixed set
    (`fromGenericValue`, `lua/ConfigManager.cpp:1147-1164`), `IValue` has no
    parse hook a plugin could override (`IValue.hpp`), and `hl.config` only
    descends into a table for unregistered keys
    (`LuaBindingsConfigRules.cpp:979-1016`), so no table-valued setting.
    Keys registered per parameter at runtime were spiked and rejected: a
    key set in the config but no longer declared (preset switched, param
    removed) turns into Hyprland's "unknown config key" error bar after the
    next restart while staying silent in the session it went stale in, and
    a `hyprctl reload` that introduces a new name can briefly show the error
    bar (NOTES "Phase 0 spikes", S2).
  - **Rejected: `hl.plugin.hyprtail.*` Lua functions** (`addLuaFunction`,
    `PluginAPI.hpp:357`). Their table only exists once the plugin has
    registered them (`lua/ConfigManager.cpp:1166-1212`), so the config parse
    before the plugin loads would error at the call and skip the rest of the
    file: the main config is loaded as one chunk
    (`lua/ConfigManager.cpp:682`).
- **Padding expressions:** `#pragma hyprtail padding <expr>`, where the
  expression has numbers, parameter names, `+ - * /` and parentheses,
  nothing else. The largest declaration counts, as today; `damage_padding`
  stays additive. Example, the stock ribbon:
  `#pragma hyprtail padding width * 0.5 * miter_limit + 1`. This is still
  shader-declared padding (§5), not detection, so the §11 exclusion stands.
- **Contract version:** `#pragma hyprtail contract 2` is required in the
  entry file of every user shader stage. Missing or unsupported is refused,
  naming the supported range and pointing to migration notes. Before public
  release there is no compatibility layer for today's contract (v1).

**Built (phase 2)** (`src/Params.*`, `src/ShaderSource.*`,
`src/ShaderSlot.*`):
- Pragmas `contract`, `topology`, `param`, `padding`. The contract pragma
  must come first after `#version`, before any other hyprtail pragma or
  `#include`: it is replaced by the prelude, which sets the default
  precision the injected uniforms need.
- Param names: lowercase letter or `_` first, then letters, digits, `_`;
  `ht_` and `gl_` prefixes and the reserved names are refused. Colors accept
  `0xAARRGGBB`, `rgba(RRGGBBAA)`, `rgb(RRGGBB)` (not Hyprland's decimal
  `rgba(r, g, b, a)` form). `bool` and `int` uniforms are set with
  `glUniform1i`.
- Difference: a param declared in both stages (or twice through includes)
  must be declared **identically** (type, default, range), not just with the
  same type. A name declared twice in one stage is refused.
- Padding names must be float, int or bool params of the program or
  reserved names, checked before compiling. An evaluation error or a value
  outside 0..4096 is a plugin warning (the declaration is ignored or
  clamped).
- Parameter values come from the pragma defaults and, in this phase, the
  classic preset's mapping of the current config keys. The `params` string
  is phase 3.

**Built (phase 3):** the `params` config string (`Params::parseParamsString`,
`CLayer::setParamOverrides`). Applied on top of the phase-2 config mapping
(so `params` wins, matching pragma default < preset mapping < `params`).
Syntax problems and an unknown layer name are one batched warning
(`config:plugin:hyprtail:params`); an unknown parameter name, a bad value or
an out-of-range value for a known layer is reported per layer
(`params:<layer>`, `CLayer::resolve`), the entry ignored, the prior value
kept. Reserved lifecycle parameters and `enabled` are settable the same way
(no separate mechanism needed: they're already in `reservedParams()`).

### 13.6 Standard varyings

- **The fixed set** is declared by the prelude in both stages:

  | Varying | Type | Meaning |
  |---|---|---|
  | `ht_vLocal` | vec2 | path: x along the segment (0 at the newer end, 1 at the older), y across the width (-1..1); quad/instanced: quad coordinates (-1..1)² |
  | `ht_vAge` | float | ms |
  | `ht_vLife` | float | 1 to 0 over the visibility window |
  | `ht_vSpeed` | float | px/ms at birth |
  | `ht_vDist` | float | path distance from the node's segment start, px |
  | `ht_vSeed` | float | 0..1 |

- **Portability:** geometry shaders set the varyings through a prelude
  helper that default-initializes all of them, so any geometry shader pairs
  with any shading shader.
- **Custom varyings** are allowed but make the pair non-portable.
- **Plain messages instead of linker errors:** before linking, the loader
  compares the global `out` declarations of the preprocessed vertex source
  with the fragment source's `in` declarations and reports mismatches
  plainly, e.g. "fragment shader glow.frag reads `v_glow`, which geometry
  shader ribbon.vert doesn't write (standard varyings: ...)", or a type
  mismatch. The driver's link log is only shown if this check passes and
  linking still fails. Its format is driver-specific, so it isn't parsed.

**Built (phase 2):** the set and `ht_initVaryings()`. `make test-unit`
links every built-in vertex shader with every built-in fragment shader
through glslangValidator.

**Built (phase 3):** the pre-link check (`ShaderSlot.cpp`'s
`varyingCheck()`), with one deliberate change from the draft above: it is a
regex-based text scan (misses a multi-name `out vec2 a, b;` declaration or a
`layout(...)` qualifier), and it never blocks or skips the real
compile/link attempt — the compiler always runs first, unconditionally.
It's consulted only after an actual link failure, to turn the raw driver
log into the plain message; if it finds no mismatch, the raw log is shown
exactly as before. This removes any risk of a scan false positive (e.g. a
commented-out declaration matching the pattern) rejecting a shader that
would have linked fine.

### 13.7 Presets

- **Where:** built-in presets are embedded. User presets live in
  `$XDG_CONFIG_HOME/hypr/hyprtail/presets/<name>/` (fallback
  `~/.config/hypr/...`) and shadow built-ins of the same name.
- **Manifest:** `preset.conf`, plain `key = value` lines, `#` comments,
  reading like hyprlang. Layer keys are prefixed `<layer>:` as in
  `plugin:hyprtail:...`. `layers` gives the draw order (first = bottom).

  ```
  # Thin neutral trail
  contract    = 2
  description = Thin neutral trail
  layers      = core

  core:vertex   = hyprtail/ribbon.vert   # built-in, or relative to the preset dir
  core:fragment = solid.frag
  core:fade_ms  = 350                    # any other key = a parameter of the layer
  core:width    = 4
  core:color    = rgba(ffffffa0)
  ```

  An unknown parameter name is an error that lists the declared ones. So is
  a layer key for a layer not listed in `layers`.

  **Built (phase 4)** (`src/Preset.*`): `contract`/`description`/`layers`
  plus `<layer>:vertex`/`fragment`/`<name>`, `#` comments anywhere on a
  line, blank lines ignored. Difference: no `hyprtail/<name>.vert`-style
  built-in main-shader library exists yet (only `#include "hyprtail/<name>"`
  prefab *snippets* do, §5); a built-in preset's `vertex`/`fragment` values
  are checked against `shader::builtin()`'s existing names
  (`classic/ribbon.vert` etc.) instead, and a user preset's non-built-in
  value is a path relative to its own directory, exactly like today's
  `layer1_vertex` override. Structural mistakes (unknown top-level key, a
  `<layer>:` key for a layer not in `layers`, a bad/missing `contract`,
  `layers` empty/duplicated/over 4, an unrecognized built-in name in a
  *built-in* preset) are parse-time errors; an unknown *parameter* name
  needs the compiled program's declared params, so it's deferred to
  `CLayer::resolve()` (`params:<layer>`, entry ignored) same as always. Any
  failure loading the *selected* preset (not found, parse error) is
  reported (`preset:<name>`) and falls back to the embedded `subtle`
  manifest, guaranteed to parse since it ships with the plugin.
- **Selection and overrides:**
  - `preset = "<name>"` selects a preset. **Built (phase 4)**
    (`plugin:hyprtail:preset`, `Config.*`): default `subtle`.
  - Per-stage shader overrides, static keys indexed by the layer's position
    in the preset's `layers` list: `layer1_vertex`, `layer1_fragment`, up to
    `layer4_*` (layers are capped at four). `""` = the preset's shader. An
    override for a layer number the preset doesn't have is a plugin warning.

    **Built (phase 3, superseded by phase 4):** the eight keys themselves
    were built in phase 3, ahead of the manifest. What they fall back to
    when unset changed with phase 4: originally the old per-name config
    keys (`vertex_shader` etc., since removed, see §13.8), now the active
    preset's own per-layer shader (`SLayerSpec::vertPath`/`fragPath`, built
    in `hyprtail::preset::load()`). The unused-index warning is unchanged,
    now checked against whichever preset is actually active
    (`config:plugin:hyprtail:layerN`).
  - Parameters, including reserved ones: the `params` string (§13.5).
- **Stacking:** yes, up to 4 layers, one shared source, drawn in order.
- **Blending:** premultiplied "over" only, as today. A shader outputting
  alpha 0 with nonzero rgb gets additive light (glow) through the same blend
  function, so layers never change GL blend state.
- **Shipped:**
  - `subtle` (default): one narrow `path` layer, short fade, neutral low
    alpha, no idle layer. **Built (phase 4)** (`presets/subtle/`): reuses
    `classic/ribbon.*` rather than a new shader -- see NOTES "Phase 4" for
    why pinning `color_slow`/`color_fast` equal is presented here as an
    interim stand-in for a genuine single-color mode, not a hidden detail.
  - `vivid`: `path` core plus a wide soft glow layer, speed-based palette,
    `quad` idle pulse. **Not built**: needs a real glow shader and a
    speed-based palette, out of scope for a config-surface phase.
  - Optional `classic` (today's stock look), which makes migration easy.
    **Built (phase 4)** (`presets/classic/`): reproduces today's hardcoded
    defaults exactly, proving the manifest system is behavior-preserving.

### 13.8 Config surface v2

- **All keys are registered once, at init** (as today): `preset`,
  `params`, `layer1_vertex` ... `layer4_vertex`, `layer1_fragment` ...
  `layer4_fragment`, `capacity`, `min_spacing`, `emit_from`, `emit_offset`,
  `warp`, `screenshare`, `damage_padding`. No runtime registration, no
  state file, no extra reload; the first-parse caveat (§9) is unchanged.

  **Built (phase 4):** `preset`, `params`, the eight `layerN_*` keys,
  `capacity`, `min_spacing`, `damage_padding` (`Config.*`). Not built:
  `emit_from`, `emit_offset` (phase 6), `warp` (phase 6), `screenshare`
  (phase 7) -- registering them now, unbuilt, would just be dead config
  surface; they're added with their own phases instead.
- **Removed:** `fade_ms`, `width`, `miter_limit`, `interpolate_warps`,
  `vertex_shader`, `fragment_shader`, `color_slow`, `color_fast`, all
  `idle_*`. They become preset parameters or layer overrides. Old keys get
  Hyprland's own "unknown config key" error; a migration table goes in the
  README. There is no compatibility shim before public release.

  **Built (phase 4), one deliberate deviation from this list:**
  `fade_ms`/`width`/`miter_limit`/`color_slow`/`color_fast`/
  `vertex_shader`/`fragment_shader`/all `idle_*` are removed exactly as
  written above. `interpolate_warps` is **not** removed. Read literally,
  this list would delete it before its named replacement (`warp`, §13.10)
  exists, which isn't a rename -- it's deleting a working feature
  ("connect the trail across warps") with nothing to take its place until
  phase 6. Keeping it until `warp` actually ships is the correct read of
  this section's own intent, not an exception to it: nothing here is
  about removing working features early, only about retiring config keys
  once their replacement exists.

### 13.9 Emit offset

- **`emit_from`** (string): `"hotspot"` (default, today's behavior) or
  `"x y"`, a position normalized to the cursor image box (`0 0` = top left,
  `0.5 0.5` = center).
- **`emit_offset`** (vec2, logical px), added after. Lua `{x, y}` or
  `"x y"` (`LuaConfigVec2.cpp:14-40`).
- **Cursor image box:** `CPointerManager::getCursorBoxGlobal()` is the
  pointer position minus the hotspot, with size = image size / scale
  (`PointerManager.cpp:719-721`); the raw values come from
  `currentCursorImage()` (`PointerManager.hpp:76-89`). Core keeps one image
  for all outputs (TODO at `PointerManager.hpp:165`). With no cursor image
  (`hasCursor()`, `PointerManager.cpp:116-118`) it falls back to the hotspot
  plus the pixel offset.
- **Applied at insert:** the node's position is the emit point, so bounds and
  damage stay exact from nodes alone. Quad layers don't use it; they have
  their own offset (§13.4).
- **Shape changes:** a cursor shape change moves the emit point without any
  motion. `CPointerManager::m_events.cursorChanged` (`PointerManager.hpp:92-94`,
  emitted e.g. at `PointerManager.cpp:135`) sets `pendingBreak` whenever
  `emit_from` isn't the hotspot.

### 13.10 Warp interpolation

- **Setting:** `warp = "break" | "line" | "curve"` replaces
  `interpolate_warps` (false = break, true = line).
- **`curve`:** the warp hook inserts nodes along a quadratic Bezier from the
  newest node to the target.
  - The control point follows the newest node's velocity, for tangent
    continuity.
  - Node count comes from length / `min_spacing`, capped at a quarter of
    the capacity.
  - Birth times are spread between the previous node's birth and now, so
    the fade sweeps along the curve.
- **Why it works everywhere:** the nodes are inserted on the CPU, so damage
  stays exact and every topology works.
- **Same coverage gap as today:** warp sites calling
  `CPointerManager::warpTo` directly bypass the hook (§7).

### 13.11 Batched notifications

**Built (phase 1) on the current model**; see §9. With layers, the "shaders
compiled" condition becomes every layer's slot.

- **Batch window:** a batch opens at plugin init, on every `config.reloaded`
  and on every shader file change. It collects every report.
- **Closing:** it closes once every layer's pending program has been
  compiled (that happens in the next render), or after 2 s if nothing
  renders (e.g. all outputs off).
- **Summary:** closing shows one notification, "hyprtail: N errors, M
  warnings: <headline of the first error>. Details: <errors.log path>", or
  nothing if clean.
- **errors.log** gets each full entry immediately (fsynced, §9).
- **Outside a batch** (e.g. a GL failure mid-session) reports notify
  immediately, as today, deduplicated per key.

### 13.12 Screenshare

**What each capture path does with the trail today:**

| Path | Entry | What is captured | Trail |
|---|---|---|---|
| Monitor or region: wlr-screencopy (`Screencopy.cpp:36-37`), ext-image-copy-capture output source (`ImageCopyCapture.cpp:37`); portals are built on these | `CScreenshareFrame::renderMonitor` (`ScreenshareFrame.cpp:176-217`) | the monitor's mirror texture, which `end()` fills from the main framebuffer after every pass element (`OpenGL.cpp:799-806`, `saveBufferForMirror` `:2533-2559`) | **included** |
| Window: toplevel export (`ToplevelExport.cpp:36`), ext-image-copy-capture toplevel source (`ImageCopyCapture.cpp:39`) | `renderWindow` (`ScreenshareFrame.cpp:316-357`) | the window rendered alone; our lifecycle only runs inside `renderMonitor` | not included |
| Overlay cursor in a capture | `renderSoftwareCursorsFor(..., screencopy=true)` (`ScreenshareFrame.cpp:309-313`, `:354-356`) | the cursor, drawn into the capture | not added (our hook skips screencopy) |
| Cursor-only session | `ImageCopyCapture.cpp:140` | the cursor image | none |
| Mirrored output | `renderMirrored` (`Renderer.cpp:2007`) | the same mirror texture | **included** |

**Setting:** `screenshare = "exclude"` (default) or `"include"` (today's
behavior).

**Exclude:**
- **When:** it applies only while `CMonitor::needsACopyFB()` is true
  (`Monitor.cpp:2709-2711`: the monitor has mirrors or an active monitor or
  region session).
- **How:** our pass element is skipped. The trail is drawn instead from a
  hook on `CHyprOpenGLImpl::saveBufferForMirror`, after the original has
  run. The mirror copy (captures and mirrors) is taken without the trail;
  then the trail is drawn into the current framebuffer, before `end()`
  copies that to the output (`OpenGL.cpp:808-829`). No extra copies, no
  scratch buffers.
- **Side effects, only while capturing or mirroring:** the trail draws above
  a software cursor (hardware cursors are unaffected), and mirrored outputs
  don't show it.
- **Hook target verified (phase 0, S1):** `saveBufferForMirror` is exported
  from the host binary (`T` at `0x9e2160`), and the whole binary has exactly
  one call instruction to it (`0x9e3358`), matching the single call site in
  source (`OpenGL.cpp:802`, in `end()`). A hook on the exported entry
  therefore sees every mirror copy. **Runtime self-check:** a render of a
  monitor that needed a copy but reached `RENDER_POST` without the hook
  firing (e.g. a future build inlining another copy) switches that monitor
  to the fallback below and warns once.
- **Fallback when the hook is unavailable:** exclude degrades to not drawing
  on monitors that need a copy, so the trail never leaks into a capture, and
  one warning says so.
- **Never call `Screenshare::mgr()` from the plugin.** It is header-inline
  with a function-local static (`ScreenshareManager.hpp:247-254`). A plugin
  copy of that static would be null and would construct a second manager.
  General rule: use exported members, never header-inline singletons with
  local statics.

**Built (phase 7)** (`Config.*`, `src/main.cpp`): `screenshare` (`Config.*`,
default `"exclude"`, validated against exactly these two values). The table
above still describes the *baseline*, pre-this-feature behavior for
citation purposes; with this built and `screenshare` at its default, the
monitor/region-capture and mirrored-output rows flip to "excluded" (the
change the feature exists to make), the other three rows are unaffected
(they already matched the desired default with no code change needed).

- **Mechanism as built:** `runLifecycle()` (`main.cpp`) still runs
  `CMonitorDamage::update()` for every layer unconditionally (so damage
  bookkeeping never depends on where the actual draw happens), but only
  adds the normal `CLayerPassElement` to `m_renderPass` when
  `!excludeCaptures() || !pMonitor->needsACopyFB()`. Otherwise it stashes
  the collected `SLayerDraw`s on that monitor's per-render state
  (`SMonitorFrame::pendingCaptureDraws`) instead. The new hook,
  `hkSaveBufferForMirror`, calls the original first (so the copy is clean),
  then constructs a `CLayerPassElement` on the stack from the stashed draws
  and calls `.draw()` on it directly -- not through `m_renderPass` (which
  only runs before `endRender()`, too early for this) but as a plain
  virtual call, which is safe: `IPassElement` (`PassElement.hpp:23`) is a
  bare virtual-dispatch base with no side effects at construction; the pass
  system, not the object, is what manages queued elements' lifetime.
- **Damage correctness, verified by tracing the exact chain at the pin
  (not assumed from the phase-0 spike alone):** `GLRenderer.cpp:88`
  (`m_renderPass.render(m_renderData.damage)`) runs the pass with the
  render's own accumulated damage, which already includes this monitor's
  layer boxes (added by `damageInRender()`, `RenderUtil.cpp`, earlier the
  same render, before `endRender()`). `Pass.cpp:132` copies that in;
  `:163`/`:172` sets `finalDamage` from it (plus a blur expansion when
  anything needs blur) -- always a superset, never narrower.
  `GLRenderer.cpp:98` calls `end()` only after the pass has already run, and
  `OpenGL.cpp:786` sets `m_renderData.damage = finalDamage` there, before
  `saveBufferForMirror` (`:801-802`) and so before our hook fires. So the
  damage region live when the hook draws is coarser than the per-element
  region `Pass.cpp:193-194` normally computes (that one is occlusion-culled
  by `simplify()`), but always a superset of it -- scissoring to it can't
  crop the trail, only rasterize a marginally wider area no geometry
  reaches anyway. No explicit damage assignment was added in the hook; it
  wasn't needed.
- **Self-check and fallback as built:** `onRenderStageInternal` gets a
  `RENDER_POST` branch (`Renderer.cpp:2250`, after `endRender()`). If a
  monitor's `awaitingCaptureDraw` is still set there, the hook didn't fire
  this render; the plugin reports once (`hook:capture:<monitor>`) and sets
  `captureHookUnavailable`, sticky, so `runLifecycle()`'s branch above
  stops stashing draws for that monitor and simply doesn't draw while it
  needs a copy -- the damage.update() calls already ran, so the region
  just redraws with no trail, no artifacts. The same fallback applies from
  install time if `saveBufferForMirror` couldn't be hooked at all.
- **Status** (`hyprctl hyprtail`): `hooks.capture`, top-level `screenshare`,
  and per-monitor `needsCopyFB`/`captureFallback` -- lets the mechanism be
  confirmed live (a mirror or capture detected, the hook firing, no
  fallback) without needing an external capture tool.

### 13.13 Status command

**Built (phase 1) on the current model**; see §9. Layer, preset and
screenshare fields arrive with their phases.

- **Command:** `hyprctl hyprtail`, with `-j` for JSON.
  `registerHyprCtlCommand` (`PluginAPI.cpp:422-431`), `SHyprCtlCommand`
  (`SharedDefs.hpp:46-50`, receives the output format). It runs on the main
  thread.
- **Plugin:** build revision and pin check, hook availability (cursor,
  warp, screenshare).
- **Preset and layers:** the preset; per layer, the topology, shader
  origins (built-in or path), contract version, program state (active,
  kept last good, built-in fallback, failed) and current parameter values.
- **Source:** node count / capacity, generation, `pendingBreak`, emit mode,
  warp mode.
- **Per monitor:** renders, lifecycle run via the hook vs. the fallback,
  frames drawn, empty-damage skips, last damage box, state (drawing,
  clearing, idle), whether it's captured.
- **Diagnostics:** the errors.log path and the report count this load.

### 13.14 Backlog investigations (not in scope)

- **Spring-chain source.**
- **Cursor image as a texture (ghost-cursor preset):**
  - **Reachable from plugin headers:** `CPointerManager::getCurrentCursorTexture()`
    is public (`PointerManager.hpp:90`). It returns the cursor buffer's
    texture, created on demand, or the cursor surface's current texture
    (`PointerManager.cpp:950-961`). Size, hotspot and scale come from
    `currentCursorImage()`.
  - **To check:** the host-binary export; the GL texture target (surface
    textures may need a `samplerExternalOES` variant); lifetime across
    frames (surface commits replace the texture).
  - **Limit:** only the current shape is available, so every ghost shows
    it.

### 13.15 What stays as is

The damage lifecycle (§6), the draw-order hook and fallback (§4, §7), color
management of declared colors (§5), the include preprocessor and prefabs
(§5, now also carrying the prelude), hot reload and file watching (§9), and
the diagnostics path (§9). Everything on the §12 list stays open.

### 13.16 Phased implementation plan

Each phase ends buildable, with `make smoke` extended and passing, and is
tested nested first, then on the host. CPU-only parts (pragma parser,
padding expressions, preset manifest, Bezier and Catmull-Rom bounds) are
kept free of Hyprland headers and get unit tests (`make test-unit`, no
compositor).

0. **Spikes, no feature code (done).**
   - Host-binary exports: `CHyprOpenGLImpl::saveBufferForMirror`,
     `CMonitor::needsACopyFB`, `CPointerManager::getCursorBoxGlobal`,
     `currentCursorImage`, `getCurrentCursorTexture`.
   - Dynamic `addConfigValueV2` plus `reloadConfig()`: done, rejected
     (stale keys become errors after a restart), replaced by the `params`
     string.
   - Whether `saveBufferForMirror` leaves the current framebuffer bound and
     the render data intact for a draw.
1. **Diagnostics and status (built).** Batched notifications (§13.11) and
   `hyprctl hyprtail` (§13.13) on today's model. Both make every later phase
   easier to debug.
2. **Restructure and the whole contract v2 in one break** (merged with the
   contract parts of the former phase 3, decided so custom shaders break
   once). **Built, untested.**
   - One pass element draws N layers; the idle effect is a `quad` layer
     with generalized timers.
   - Contract 2: prelude, `contract` and `topology` pragmas, param pragmas,
     padding expressions, standard varyings.
   - The node grows seed and distance (28 bytes, integer attribute).
   - Today's look ships as the built-in `classic` preset; the current
     config keys map onto it, so behavior should be unchanged.
   - `make test-unit`: unit tests and glslangValidator over the built-ins.
3. **Checks and the config front end (built, untested).** The `params`
   string, the `layer1_vertex` ... `layer4_fragment` keys, `expects`, and
   the pre-link varying check with plain messages.
4. **Presets and config surface v2 (built, untested).** Manifest parser,
   user preset directory, `subtle` and `classic`. Not built: `vivid`
   (needs a real glow shader, out of scope for this phase), migration
   notes in the README (the SPEC §9 note above covers it for now).
5. **Topologies.** `instanced K` (plus a particle demo preset) and
   `path smooth N` with exact Bezier bounds.
6. **Pointer features.** Emit offset with the shape-change break, `warp =
   curve`.
7. **Screenshare exclude (built, untested; moved up ahead of phase 5).**
   The hook, the fallback, `screenshare = "exclude"|"include"`. Not done:
   capture tests with real external tools (grim for screencopy, a portal
   client for image-copy-capture, window capture) -- the nested check that
   *is* planned only confirms the mechanism doesn't corrupt or crash
   (mirror setup, viewed directly in nested) and that the trail is
   genuinely absent from a mirror output; a real screencopy/portal client
   is still needed before public release, per the original note.
