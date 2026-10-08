# Hyprland Cursor Trail Plugin — Research Notes

## Decisions (settled)

- **Architecture:** real compiled Hyprland plugin with its own render-pass element(s), not a `decoration:screen_shader` wrapper (à la hyprshade). Core's built-in shader uniforms (`SHADER_POINTER*`) can't carry custom per-plugin state, no array uniform for continuous position history exists or can be declared into them, so custom state needs a real plugin with its own `CShader` program and pass element. See "hyprshade" and "Shader/uniform architecture" below for the reasoning trail.
- **Coordinate space:** store trail data in **global layout pixel space** (`CPointerManager::position()` / `m_pointerPos`, confirmed monitor-independent in `PointerManager.cpp`), not per-monitor normalized 0-1. Convert to each monitor's local space only at render time, inside that monitor's own pass element, same transform core already does internally. Normalized-per-monitor storage would silently break at monitor boundaries, same failure class that limits `decoration:screen_shader` to no per-monitor granularity.
- **Velocity/acceleration:** compute once per node, CPU-side, when the buffer updates, not per-pixel in the fragment shader (wasteful, same value recomputed redundantly for every pixel the trail touches). Shader just reads the precomputed value. Velocity/acceleration has never existed as a Hyprland uniform, confirmed via full git history search, so this was always necessary regardless of era, not a wrong turn from the old attempt.
- **Versioning:** pin to a specific Hyprland commit, don't track master during development. No ABI stability across commits (`PluginAPI.hpp`), so a moving target makes it impossible to tell your own breakage from upstream's. Advance the pin as a deliberate step (`git checkout <sha>`), rebuild, re-run the validation ladder, then update the pin recorded below.
  - **Pinned commit: `efb50993780079460b0cbed1363e2166a2de1d9f` (v0.56.2, host package).** See SPEC §2 and "Pin moved to host version" below. Previous pin `1b85c7aa`.
- **Build (Hyprland checkout):** `make clear && make debug`, not hand-rolled `cmake` flags or bare `rm -rf build`. `-DTESTS=true` (from `debug` target) is required for hyprtester to build at all. `clear` also removes generated protocol headers outside `build/` that `rm -rf build` misses.
- **Damage/redraw:** targeted `g_pHyprRenderer->damageBox()` (padded to the trail's extent) + `scheduleFrame()`, per monitor, same pattern hypr-dynamic-cursors uses. Not the global `debug:damage_tracking 0` toggle, that's fine for the one-off screen-shader test but would be a bad default for a shipped plugin, real GPU cost everywhere on screen, not just near the trail.

## Shader exposure philosophy (settled, drives several decisions below)

- Goal: expose as much as possible through shaders/config, let users write their own shaders, keep plugin-side configuration sparse, prefer optional shader imports over plugin-configured functionality for new capabilities.
- **Ribbon geometry (tangent/normal/taper) belongs in the vertex shader, not plugin C++.** Corrected reading of the OGL reference: it already computes geometry in-shader, JS there only maintains raw point positions and sets up `next`/`prev` attribute buffers, same role the plugin's circular buffer already has. This isn't just a performance choice (trivial either way at realistic buffer sizes, tens of points), it's what actually delivers the shader-exposure goal: geometry logic in C++ means shader authors can only recolor a fixed shape; geometry logic in the vertex shader means they can rewrite the shape itself, taper curve, velocity-responsive width, different style entirely, without plugin changes.
- **Damage box implication:** if geometry (thickness/taper) is computed in-shader, the plugin doesn't know the ribbon's true on-screen extent from raw point positions alone. Resolved by the same mechanism as fragment-shader bleed (see Decisions): shader-declared padding value, defaulting to zero/a known built-in extent for stock geometry, overridden by custom shaders that diverge (wider strokes, pulsing thickness, blur/glow bleed). One mechanism for both "fragment effect reaches past the geometry" and "vertex shader made the geometry bigger than the raw points suggest," not two separate settings.
- Shader packaging format (single file vs. separate config) not decided, explicitly not blocking, deferred.

## Data model — NOT fully dialed in yet

What's settled: coordinate space (global layout pixel), and that derivatives are precomputed CPU-side (see Decisions above). What's still open:

- **Architectural fork, not yet decided between two genuinely different techniques:**
  1. **Discrete point-history buffer** (the plan so far): extend the old `SCursorNode { glm::vec2 posPx; float birthTimeMs; }` with precomputed velocity, store N recent samples, upload as an array uniform, shader draws/blends based on distance to each stored point. Supports velocity-driven visual variation naturally (color/size by speed), which is the actual stated goal (smaller cursor, aggressive hide time, vivid movement feedback).
  2. **Accumulation / ping-pong framebuffer**: render to an off-screen texture each frame, blend previous frame's texture at reduced opacity + new cursor draw on top, display that texture. No point history stored at all, the trail persistence comes from the accumulation itself. Found as the confirmed technique behind a real WebGL cursor trail effect (see References below: "Uses WebGL with a single ping-pong framebuffer for the trail"). Simpler in some ways (no array-uniform size limits, no per-node bookkeeping), but doesn't naturally expose per-sample velocity for driving the visual, harder to do "color the trail by speed" cleanly, and needs a managed render target per output.
  - **This needs to be decided before finalizing struct fields or buffer storage**, it changes what "the buffer" even is. Given the stated goal is explicitly velocity-driven visual variation, (1) is probably the better fit, but this hasn't been deliberately decided against (2), just defaulted into by momentum from the old codebase.
- Exact `SCursorNode` field list beyond `posPx`/`birthTimeMs`/velocity not finalized (acceleration as a stored field too, or derived in-shader from velocity deltas between adjacent nodes? per-node color/shape variation?).
- **Buffer storage: VBO-resident, not a uniform array.** Settled by the OGL polyline reference below, `next`/`prev` per-vertex attributes are a standard technique for exactly this and aren't expressible as a uniform array anyway. Sidesteps the GLSL uniform-array-size ceiling entirely, trail length becomes a buffer-size/perf question, not a shader-compile-time constraint.
- Buffer size (N samples) not decided.
- **Real risk, flagged before implementation, not after:** circular buffer + `next`/`prev` polyline adjacency have a wrap-point interaction problem. Writing is never in question, always O(1) at the cursor position, overwrite oldest, advance, wrap. The problem is entirely on the read/draw side: raw ring-buffer index order isn't temporal order once wrapped, so if the GPU reads directly from the ring's raw layout, the newest write and the oldest surviving point (adjacent in index terms, opposite ends in time) produce a spurious connecting segment every frame. Two options: (A) rebuild a temporally-ordered copy from the ring each frame before upload (extra O(N) per-frame cost, GPU always sees clean data), or (B) let the GPU read the raw ring directly and exclude the one wrap-boundary segment via the index/element buffer each frame (cheaper, but pushes bookkeeping into draw-call management and the seam is real in the data, just not drawn). **Recommendation: (A).** At a realistic trail buffer size, tens of points, maybe up to ~100, the O(N) reorder is not a measurable cost, and it's much simpler to reason about and debug than tracking a moving exclusion index. (B) only earns its complexity at buffer sizes in the thousands, not this use case. Also worth naming directly: (A) doesn't sacrifice single-source-of-truth, the ring buffer is still the only mutated state, the per-frame reordered copy is a stateless projection with no persistence, nothing to desync because it's rebuilt from scratch every frame. (B)'s complexity doesn't disappear either, it moves into index-buffer exclusion tracking, which has to stay correct relative to the cursor too, just less visibly, and fails as a subtle render glitch rather than an obviously malformed buffer.
- Fade/decay function not decided, this is where the WebGL references below are meant to help, need a concrete visual target before picking the math. **Specific decision surfaced by the OGL polyline reference:** fade by actual elapsed time (`now - birthTimeMs`, speed-independent fade duration) vs. fade by position-along-ribbon (`uv.x`, simpler, but fade rate then varies with cursor speed, a fast flick compresses more history into less visible length and fades faster in wall-clock time than a slow drift). Given the stated goal is fade *time*, time-based is probably the better fit, not yet deliberately chosen.

## Environment status (done, confirmed working)

- Build: pinned-commit workflow via `make clear && make debug`, `git submodule update --init --recursive`, no auto `git pull`. See Decisions above.
- Nested launch: `run_dev.sh` builds the plugin (`make DEV=1 all`) and starts `external/Hyprland/build/Hyprland -c dev_env/hypr/hyprlandd.lua` with `XDG_CONFIG_HOME=dev_env` (SPEC §2). An earlier version of this note said no launch script was needed; `run_dev.sh` exists and is what produced `dev_env/hyprtail-latest.log`. Its build step used to call a `plugin_rebuild.sh` that isn't in the repo (dead reference, replaced by the `make` call above). `dev_env/launch.sh` (launch plus side-by-side window placement) was removed: `run_dev.sh` covers the launch, its `W=720` no longer matched the 1280-wide nested outputs, and host-window placement doesn't matter for these tests (see the cross-monitor bullet below).
- Config: debug binary looks for `hyprlandd.lua` specifically (confirmed, real convention, not a typo, matches the `hyprlandd.log` naming already used), release looks for `hyprland.lua`. Since Hyprland 0.55, hyprlang (classic `.conf`) is deprecated in favor of Lua (confirmed, current wiki).
- Two nested outputs confirmed working: `hyprctl output create wayland second` (from inside `hl.on("hyprland.start", function() ... end)`) + `hl.monitor({ output = ..., position = ..., scale = 1 })` for **both** outputs, primary included. Primary's `hl.monitor()` call must also be inside `hyprland.start`, a top-level call did not apply, confirmed by direct test.
- `mode` is silently ignored on wayland-backend nested outputs, confirmed on two separate outputs independently. Stuck with whatever default size Hyprland gives them (`1280x720@60` in this setup). Positioning works, custom mode doesn't. Offsets must match the real width, not the requested mode.
- Cross-monitor cursor continuity: tested via manual mouse drag + `hyprctl cursorpos` polling (`cursorpos_trace.sh`, logs `t_ms,x,y`, real timestamps not nominal interval, since a bash loop's `hyprctl` spawn overhead makes nominal spacing untrustworthy). Once outputs were confirmed properly aligned (`hyprctl monitors -j`, not eyeballed), the trace showed no discontinuity, jumps in the data were diagonal (both axes moving together, consistent with fast hand movement) rather than the axis-isolated single-frame jump that flagged the earlier misalignment bug. **Physical host-window placement is irrelevant to this test**, internal layout position (`hl.monitor`) is entirely independent of where the nested windows sit on the host screen, don't waste effort aligning them physically.
- `hyprctl dispatch <name> <space-separated args>` is unreliable under the Lua config provider, same root issue as `keyword` (legacy IPC syntax), but fails worse: instead of a clean rejection it emits malformed auto-translated Lua and errors cryptically. Use `hyprctl eval 'hl.dispatch(hl.dsp.X({...}))'` instead. Also confirmed (external report): invalid Lua dispatcher calls sometimes silently return "Ok" instead of erroring, don't trust silence as success.
- `wlrctl pointer move DX DY` (real, separate tool, virtual-pointer Wayland protocol, independent of Hyprland's IPC) attempted as an alternative cursor-warp method, didn't visibly move the cursor in initial test, likely targeting the wrong `WAYLAND_DISPLAY` (nested instance's socket, not just `HYPRLAND_INSTANCE_SIGNATURE`, they're different env vars). Not chased further, the drag+trace method already gave a usable result.

## Shader / uniform architecture (read directly from source, not docs)

- **hypr-dynamic-cursors uses no shaders.** Its effects (tilt/rotate/stretch) go through `CCursorPassElement`, a texture + transformed box (`box.rot` etc.), no GLSL. It hooks two cursor paths: software (`renderSoftwareCursorsFor`, normal render pass) and hardware (`renderHWCursorBuffer`, DRM cursor-plane buffer). Hardware path is a dead end for a trail, no room to composite history.
- **Hyprland core has a real shader class**: `src/render/Shader.hpp`, `CShader::createProgram(vert, frag)`, typed `eShaderUniform` enum (not string uniforms). Enum covers motion blur, glass/acrylic, aurora, haze, ripple, water, a "fluidjar" fluid-sim effect, substantially more shader infra than a basic blur+shadow renderer, likely a chunk of the drift from a year ago.
- **`SHADER_POINTER*` uniform cluster** (`OpenGL.cpp` ~line 1068–1393): `pointer_position`, `pointer_shape`/`_previous`, `pointer_pressed_positions`/`pointer_pressed_times` (arrays), `pointer_last_active`, `pointer_switch_time`, `pointer_inactive_timeout`, `pointer_size`. **These are for click/touch ripple animation, not continuous movement history** (corrected earlier assumption). Only `pointer_position` (single current position) is live per frame, no continuous history uniform exists in core. Confirmed live via `decoration:screen_shader` test (renders correctly, tracks cursor in real time, oval-not-circle artifact expected from comparing normalized coords without aspect correction, not a bug).
  - `pointer_position` landed July 8 2025 (PR #10821). Full cluster landed October 20 2025 (PR #11986), six weeks before the Dec 2 2025 last commit on the old attempt. Commit message explicitly frames it as being for people drawing their own cursor via the screen shader.
- A user-authored `decoration:screen_shader` file is loaded **verbatim** as the fragment shader (`applyScreenShader`, `OpenGL.cpp`), no wrapping/templating, must be a complete shader. Vertex shader auto-selected based on whether the frag starts with `#version 320 es` (`TEXVERTSRC320`) vs. anything else (`TEXVERTSRC`). `pointer_position` is normalized to monitor size, same 0-1 space as `v_texcoord`.
- Damage tracking must be disabled (`debug:damage_tracking 0`) for a screen-shader uniform like `pointer_position`/time to visibly update, confirmed directly from the `uniformRequireNoDamage` check in source. This was fine for the one-off test; **not** the mechanism to use in the real plugin, see targeted damage decision above.

## Config system: Lua provider (major drift item)

- Since Hyprland 0.55, hyprlang (classic `.conf`) is deprecated in favor of Lua (confirmed, current wiki). `hyprctl keyword` is legacy-only, errors under the Lua provider with "can't work with non-legacy parsers, use eval." `hyprctl dispatch <space-separated args>` has the same underlying problem, worse failure mode (see Environment status above).
- `hyprctl eval 'hl.config({ category = { key = value } })'` is the live-patch equivalent of `keyword`. `hl.monitor({ output, mode, position, scale })` confirmed syntax for monitor config. `hl.on("hyprland.start", function() ... end)` confirmed as the exec-once equivalent, `hl.exec_cmd(cmd)` for shelling out inside it (shell semantics of `&&`/chaining confirmed working in practice for the second-monitor setup).
- `eval` patches live config but doesn't fully reset stuck/stale state (a screen-shader path pointing at a removed file wasn't cleared by re-`eval`-ing empty), needed `hyprctl reload` (full re-parse from disk). Keep `reload` as the escape hatch.
- **Settled (efb5099), see "Config and shader loader":** V1 `addConfigValue`/`getConfigValue` do not work under Lua; V2 `addConfigValueV2` does, for both providers.

## Ecosystem drift since last attempt (~1 year ago)

- Core Hyprland factored into separate libraries: `hyprutils`, `hyprlang`, `hyprcursor`, `hyprgraphics`, `aquamarine`. **Resolved:** cursor position tracking (`PointerManager.cpp`) and the render-pass system (`CCursorPassElement`, `TexPassElement`, etc.) both live squarely in core Hyprland, not in `hyprcursor`/`hyprgraphics`. That earlier open question is settled, no need to dig into those libraries for the hook points.
- Aquamarine is backend/session abstraction (KMS/DRM/libinput/Wayland windowing), not the rendering pipeline, rendering-API-agnostic, doesn't replace `CHyprOpenGLImpl`. This transition (0.42, Aug 2024) predates the old attempt by over a year, not part of the "since last year" drift, corrected an earlier speculative assumption.
- Lua config provider (0.55+) is the single biggest drift item, see section above, affects everything from how the plugin exposes config to why manual test commands keep failing.

## Prior art & references

**Hyprland plugin/tool prior art:**
- **hypr-dynamic-cursors** (VirtCode) — actively maintained, closest structural analog for hooking cursor rendering. No shaders, see Shader architecture above.
- **hyprtrails** (`hyprwm/hyprland-plugins`) — removed in PR #663 (lack of maintenance, not because rendering broke), checked out at last commit before removal in `external/hyprland-plugins`. Reference for fade-over-motion-path technique, applied to windows not cursor.
- **hyprtail.old** (own prior attempt, `/references/hyprtail.old`) — design-idea reference (shader format, trail data model thinking) only, not a build target. Whatever hooks into Hyprland's render path is almost certainly against the pre-Lua, and possibly pre-render-pass-system, API.
- **hyprshade** — userspace Python CLI wrapping `decoration:screen_shader`, not a plugin. Ruled out as the project's own architecture (see Decisions above) because it can't carry custom per-plugin uniform state, but its CLI/TOML config shape and mustache-templated gradual-transition pattern are worth a look if/when the plugin gets its own user-facing config surface. Confirmed hard limitation: `decoration:screen_shader` has no per-monitor granularity (global only), upstream limitation not a hyprshade gap, moot now that the project isn't building on `screen_shader` anyway.

- **oframe/ogl `polylines.html` example** — a third distinct technique from the two above: ribbon/polyline geometry (triangle strip, per-vertex `next`/`prev`/`uv`/`side` attributes), not fragment-shader blob blending, not accumulation-buffer. Settles the buffer-storage question, see Data model above. Vertex shader handles geometry only (thickness, taper via `uv.y`, degenerate-point guard via `smoothstep` on segment distance), doesn't address time-based fade, that's a separate concern layered on in the fragment shader. Perspective/DPR-based pixel-width math (`current.w`) assumes a 3D camera pipeline, doesn't apply to a 2D screen-space overlay, worth stripping rather than porting faithfully.
- **Kabiirk/stylized-mouse-trail-OGL** — real WebGL trail (OGL library), closest technical analog to a GPU-shader trail among these, worth reading for actual shader technique rather than just visual inspiration.
- **Framer "LED Matrix Cursor Trail"** — confirmed technique: "WebGL with a single ping-pong framebuffer for the trail," concrete real-world example of the accumulation-buffer alternative described in the Data model fork above.
- **tholman/cursor-effects** (and the `cursor-trails` npm fork) — canvas 2D particle effects, not WebGL/shader, implementation technique doesn't transfer, but useful for surveying the range of visual behaviors (rainbow, fairy dust, ghost-following) worth having an opinion on before committing to one look.

## Testing strategy

Two separate concerns, don't conflate them:

- **Behavioral / lifecycle** (loads without crashing, hooks register, clean unload, survives monitor/workspace changes): `hyprtester`, Hyprland's own integration test binary (`./build/hyprtester/hyprtester -c hyprtester/test.lua -b ./build/Hyprland -p <plugin.so>`, built via `-DTESTS=true`, part of `make debug`). Existing test names (`dwindleSplit`, `focusMasterPrevious`, etc.) read as state/behavior assertions, no indication of pixel/frame readback. **Unconfirmed** either way, hasn't been checked against actual source.
- **Visual correctness** (does the trail render/fade/follow correctly): staged validation ladder, deliberately separate throwaway pass-element/harness, not branches in the real trail code:
  1. Dot at fixed position, no tracking, proves the custom pass element registers and `CShader::createProgram` draws anything.
  2. Dot at tracked position (`g_pPointerManager->position()`, own uniform), proves the plugin's own uniform plumbing.
  3. Row of dots from the full buffer as an array uniform, catches array-size/layout issues early.
  4. Fade over time, cursor stationary, tests targeted `damageBox`+`scheduleFrame` redraw triggering. Frozen dot = redraw problem, not fade math.
  5. Cross-monitor, exercises global-space buffer storage + per-monitor conversion together. **Live multi-monitor testing now available** (multiple physical monitors on hand), so this stage can be verified directly rather than only inferred from a position trace.

Not yet reached, environment/fixture work has been the focus so far (see Environment status above).

## Teleportation / cursor warps

- Hyprland has a real, deterministic warp path distinct from organic motion: `CPointerManager::warpTo`/`warpAbsolute` (`PointerManager.cpp`), firing `g_pInputManager->onMouseWarp(event)`. Hook this directly (same `CFunctionHook` mechanism as other internals), don't infer teleports from a distance/velocity heuristic alone, that needs empirical tuning and risks misfiring on a genuinely fast real flick. Keep a distance-based backstop for anything that reaches cursor position outside this path.
- **Response to a detected teleport is context-dependent, not a single global rule:** interpolate (sweep the trail between old and new position) when both endpoints are on currently-visible, simultaneously-rendered content, e.g. a cross-monitor focus jump, genuinely useful there, helps track where focus went, not misleading since both points are real and on-screen at once. Break the polyline connection (no swept segment, pre-teleport points keep aging/fading independently, post-teleport starts a fresh segment) when the visible content itself changed underneath the position, e.g. a same-monitor workspace switch, a swept path there would cross space that no longer shows what it showed a frame ago.
- **Unconfirmed, needs checking before implementable:** whether `onMouseWarp`'s event payload carries enough context (why the warp happened, whether a workspace/content change accompanied it) to actually distinguish these two cases, or just that a warp happened.

## Cursor hide

- `pointer_hidden` (found earlier in the `SHADER_POINTER*` cluster) confirms core already tracks cursor-hidden state as first-class. Not using that uniform anymore (moved off `screen_shader`), but the underlying C++ state it's derived from is what the plugin should hook, not reinvent.
- Two cases, not one: idle-timeout hide likely needs no special handling, by the time inactivity triggers a hide, the trail has almost certainly already faded to nothing under normal time-based decay. Hide-on-keypress is the real case, it can fire mid-motion and catch a fresh, unfaded trail with no natural reason to have decayed yet. Test that one deliberately.

## Idle / presence effects (stationary but visible cursor)

- **Gap in the current design, not a bug, a structural limit:** the ribbon pipeline requires ≥2 distinct recent points to compute a tangent from. A stationary cursor (assuming insertion only happens on real movement, see below) stops producing new points, existing ones age past the fade window, and geometry correctly collapses to nothing, same `smoothstep` degenerate-guard from the OGL reference, just triggered by "no motion" rather than a coincident-point glitch. Correct for a trail specifically, but means this pipeline has nothing to say about a still cursor, it was never built to.
- **Fix: a second, independent pass-element/shader slot for a presence/idle effect**, keyed on current position + time-since-last-movement, not point history. Symmetric with the trail's own extensibility story, a second optional shader slot, not a new plugin-configured feature.
- **Refinement:** the idle slot's content doesn't have to be a plain quad, it can reuse the same ribbon-geometry technique (tangent/normal from an ordered point sequence), fed a synthetic pattern (a ring around the cursor, say) instead of real motion history, same math, richer visual than a dot. This is what the "second shader slot" can actually contain, not a competing idea.
- **Load-bearing now, not deferrable:** synthetic idle points must live in a separate buffer instance from the real motion trail, not the same one. Writing idle-pattern points into the trail's own circular buffer every frame while stationary would either evict the still-legitimately-fading tail of real motion data, or interleave two kinds of point with `birthTimeMs` values that mean different things, breaking the fade math either way.
- **Deferrable:** exact synthetic pattern shape, idle-trigger threshold, whether the idle slot reuses the trail's exact shader or a variant. None of this constrains motion-trail work, as long as the buffer/upload plumbing isn't written with an implicit "there is exactly one instance of this" assumption, parameterizing it by which buffer/shader costs nothing now; fixing that assumption later is a refactor, not an addition.
- **Deferred implementation detail:** the real trail's shader logic is driven by actual per-point velocity, meaningless for a stationary synthetic pattern. Literal shader-code reuse across both slots would need a synthetic stand-in (e.g. an animation phase) driving the same width-response logic, not needed until the idle slot is actually built.
- **Decision now made explicit** (was implicit, never actually stated): buffer insertion happens only on real movement, not every frame regardless of position change. This was probably already assumed, but it's now load-bearing for two reasons instead of one, buffer efficiency (no duplicate-position entries) and free idle-time derivation, `now - buffer.back().birthTimeMs` gives time-since-stopped directly from the trail's own buffer, no new plugin state needed to drive the idle effect's timing.

## Hardware vs. software cursor path (confirmed, load-bearing)

- `CMonitor::shouldUseSoftwareCursors()` (`Monitor.cpp`) gates which path renders the cursor. Default mode (`cursor:no_hardware_cursors` = 2, "Auto") returns `false` outside Nvidia+multiGPU/VRR, meaning **hardware cursor rendering is the default path** in ordinary setups, not software.
- (Stages 1-3; stage 4 replaced the hook with a render-stage listener, see "Stage 4" below.) The plugin hooks only `renderSoftwareCursorsFor`. With hardware cursors active by default, that hook barely fires at all, explaining a whole cluster of early stage-2 symptoms at once: invisible over bare desktop, only tracking near a window (something about window content sporadically forcing a software fallback), general jerkiness (only ever seeing the rare frames where software mode got forced).
- Matches confirmed prior art: `hypr-dynamic-cursors` hooks **both** `renderSoftwareCursorsFor` and `renderHWCursorBuffer`, not incidentally, a real cursor-effects plugin needs both paths covered.
- **Decisive test, confirmed real syntax (straight from `hyprtester/src/tests/main/solitary.cpp`, not a guess):** `hyprctl eval 'hl.config({ cursor = { no_hardware_cursors = 1 } })'`. Forces software cursors. Confirmed to fix visibility/tracking substantially.
- **Resolved (efb5099):** neither. `no_hardware_cursors` is not required: a motion listener damages the pointer area so renders happen with hardware cursors, and the cursor-function hook handles ordering for software-cursor frames. See "Draw order and hardware cursors". `renderHWCursorBuffer` isn't needed since the trail draws into the composited frame, beneath the cursor plane.

## Damage must go through the monitor damage ring (cited at pin 1b85c7aa)

- **Stage-2 sliver bug root cause:** the hook added damage only to the `CRegion& damage` param of `renderSoftwareCursorsFor`, which is `m_renderData.damage` (`Renderer.cpp:2285`). That region feeds the current frame's pass render (`GLRenderer.cpp:101`), so the buffer being drawn is correct. But the damage ring transaction was already captured in `beginRender` (`Renderer.cpp:1790-1792`, `DamageRing.cpp:83-87`), and commit rotates only that captured region into history (`DamageRing.cpp:35-41`), not `m_renderData.damage`. Swapchain buffers with age > 1 get repaint region = ring current + history (`DamageRing.cpp:104-116`, history length 3, `DamageRing.hpp:7`), which never contained the dot boxes, so stale dot pixels survived in older buffers. Slivers rather than whole dots because core's own software-cursor damage (`damageIfSoftware`, `PointerManager.cpp:826-844`, cursor box expanded 4px, goes through `monitor->addDamage` into the ring) covered most of each old dot.
- Note: header comment `CRegion& damage /* logical */` (`PointerManager.hpp:61`) is wrong in practice, that region is pixel space. Scale before adding.
- **Stage-2 fix (option A, applied):** in the hook, add prev/new boxes to both the render-damage param (current frame) and `pMonitor->addDamage()` (ring, `Monitor.cpp:1121-1130`, also schedules a frame). Costs one extra frame per movement. Pending manual confirmation.
- **Intended long-term design (option B, not built):** emit damage outside the render path, before the frame begins, via `g_pHyprRenderer->damageBox(globalLogicalBox)` (`Renderer.cpp:2874-2883`, goes through `addDamage` into the ring for every monitor it overlaps, which also covers seam-straddling for free). Render hook then only adds the pass element. Same shape as core's `damageIfSoftware`.
  - **Caveat:** from stage 3 on, the trail keeps changing after the cursor stops (points age and fade), so damage can't be driven by mouse motion alone. It has to follow the trail's own lifecycle: keep damaging prev ∪ current trail box (SPEC §6) every frame until the last point has fully faded, then stop. Mouse motion is one input to that lifecycle, not the trigger for it.
  - Stage 4 status: the lifecycle half is built (per-frame in the render-stage listener, self-sustaining via `addDamage`). Option B's other half, damaging from outside the render path, is still open: it's what would let a monitor that isn't currently rendering start its own chain (multi-monitor, stage 5) and let motion from idle start a frame without relying on software-cursor damage.

## Plugin GL state hygiene (cited at pin 1b85c7aa)

- Hyprland caches GL state; raw GL calls from a plugin desync the cache and can break later core draws:
  - current program: `useShader()` skips `glUseProgram` if it thinks the program is already bound (`OpenGL.cpp:2398-2406`). Use `g_pHyprOpenGL->useShader()`, never raw `glUseProgram` (incl. `glUseProgram(0)` afterwards).
  - scissor: `scissor()` caches last box and `GL_SCISSOR_TEST` enable via `setCapStatus` (`OpenGL.cpp:1094-1121`, `:2485`). Use `g_pHyprOpenGL->scissor(&RECT, transformDamage)` per damage rect and `scissor(nullptr)` after, same as core (`:1810-1815`). Never raw `glEnable/glDisable(GL_SCISSOR_TEST)`.
  - array buffer binding: `bindArrayBuffer()` (`:2455`). VAO binding is not cached, raw `glBindVertexArray` is fine (core does it).
- Stage 1/2 `CDotPassElement` violated all of the scissor/program rules; retired in stage 3.
- Drawing only inside damage rects also matters for correctness once anything is translucent: redrawing AA/alpha edges over pixels whose background wasn't repainted accumulates.
- `projectBoxToTarget` takes a **pixel-space** box (target size = `m_transformedSize`, `Renderer.cpp:1855-1860`). Stage 1/2 passed a logical box, only correct at scale 1. Stage 3 builds a global-layout -> clip matrix with `projectBoxToTarget(CBox{-monPos * scale, {scale, scale}})`, since `projectBox` maps p to pos + size * p (hyprutils `Mat3x3.cpp:68-90`).
- `CShader` only knows the fixed `eShaderUniform` set; plugin uniforms go through `glGetUniformLocation` on `program()` + raw `glUniform*` (per-program state, no cache to desync).

## Stage 4: lifecycle driver, blending, hide, lock (cited at pin 1b85c7aa)

- **Hook replaced by `RENDER_LAST_MOMENT` listener.** `renderMonitor` only calls `renderSoftwareCursorsFor` when `shouldRenderCursor()` (`Renderer.cpp:2282-2285`), which is `!m_cursorHidden && m_cursorHasSurface` (`Renderer.cpp:3055-3057`). Cursor hides on key press / inactivity timeout / touch / tablet (`Renderer.cpp:2995-3032`). With the hook, a hide mid-fade meant no more damage or frames: trail frozen partially faded. `Event::bus()->m_events.render.stage` emits `RENDER_LAST_MOMENT` once per `renderMonitor` regardless of cursor state (`Renderer.cpp:2296`), after the cursor and before `endRender()`, and nowhere else in `src/` (so screenshare paths never reach it, the screencopy guard went away with the hook). Mirrors emit it too, skipped explicitly. Plugin precedent: `hyprland-plugins/hyprwinwrap/main.cpp:236` (that checkout not necessarily at our pin). Listener handle must be held (`[[nodiscard]]`, unregistered when dropped, hyprutils `Signal.hpp:48`) and reset first in `PLUGIN_EXIT`.
- **Self-sustaining frames:** `addDamage` during a render calls `scheduleFrame`, which sees `m_renderingActive` and sets `m_pendingFrame` (`Monitor.cpp:1103-1104`); `renderMonitor` reschedules after commit when `m_pendingFrame` (`Renderer.cpp:2328`). So damaging from inside the listener keeps a fading trail animating with the cursor stationary, and stops the moment we stop damaging. Off-monitor boxes are clipped by the ring and don't schedule (`DamageRing.cpp:67-76`, `Monitor.cpp:1128`).
- **Blending is premultiplied:** `CHyprOpenGLImpl::blend(true)` = `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`, commented "everything is premultiplied" (`OpenGL.cpp:1080-1083`). Core premultiplies on CPU, e.g. `renderRect` (`OpenGL.cpp:1212`), and `quad.frag` passes it through (`quad.frag:24-30`). Blend state is whatever the previous element left; core elements set it themselves (e.g. `ElementRenderer.cpp:419`). Trail shader outputs `vec4(rgb * a, a)` and `draw()` calls `g_pHyprOpenGL->blend(true)`.
- **Cursor hidden:** no inserts while `!shouldRenderCursor()` (touch/tablet can move a hidden pointer), existing trail still fades.
- **Session lock, core does not hide the cursor:** `renderLockscreen` (`Renderer.cpp:2245`) runs before the cursor (`Renderer.cpp:2282-2285`) and the cursor path has no lock check; the lock client just gets pointer focus and sets its own cursor (`InputManager.cpp:374+`). So "match core" would draw the trail over the lock screen. Plugin deliberately diverges: while `g_pSessionLockManager->isSessionLocked()`, no inserts, no element, last box damaged once to clear.
- **Draw order (resolved later, see "Draw order and hardware cursors"):** at this point the trail drew above the cursor (and the DPMS fade overlay), covering the pointer tip.
- **Color management gap (resolved, see "Color management"):** core converts colors via `getConvertedColor`; the trail shader wrote raw values.

## Pin moved to host version (cited at efb5099)

- **Why:** test on the physical monitors. Host runs `hyprland 0.56.2-3.1` (CachyOS), commit `efb5099`, GCC 16.2.1, LTO. Installed headers (`pkg-config --cflags hyprland` -> `/usr/include/hyprland{,/src,/protocols}`) are byte-identical to `git show efb5099:<file>` for every header the plugin uses.
- **Direction:** host is *older* and on a different branch. v0.56.2 = v0.56.0 + 36 release-branch commits (2026-08-05); old pin `1b85c7aa` = v0.56.0 + 190 on main (2026-09-13). Neither contains the other, so anything learned at the old pin needed rechecking, not assuming.
- **Unchanged for the plugin (re-verified):** `eRenderStage` (`SharedDefs.hpp:20-31`), `Event<eRenderStage> stage` (`EventBus.hpp:137`); renderMonitor order: lockscreen `Renderer.cpp:2176` -> `shouldRenderCursor` `:2212` -> cursor `:2216` -> DPMS overlay `:2219` -> `RENDER_LAST_MOMENT` `:2227` -> `endRender` `:2229`; `m_renderingActive` `:2109`..`:2261`, `m_pendingFrame` reschedule `:2266`; `CMonitor::scheduleFrame`/`addDamage` (`Monitor.cpp:1125-1159`, pending-frame at `:1132-1133`); `useShader` (`OpenGL.cpp:2561-2569`), `scissor` (`:991-1018`), `setCapStatus` (`:2600`); `CShader` API (`Shader.hpp:97-115`); `IPassElement` virtuals we override (`PassElement.hpp:23-42`), bounding box scaled from logical (`Pass.cpp:51`), element damage set before draw (`Pass.cpp:193-194`), `drawCustom` (`ElementRenderer.cpp:626-630`); `isSessionLocked` (`SessionLockManager.cpp:143-145`); `shouldRenderCursor` (`Renderer.cpp:2976-2978`).
- **Damage ring, different mechanism, same effect:** no `CTransaction`. `beginRender` reads `getBufferDamage(age)` then `rotate()`s immediately (`Renderer.cpp:1782-1783`, `DamageRing.cpp:37-63`). Mid-render `addDamage` still lands in the next frame's damage and in history. >8 rects collapse to extents (`DamageRing.cpp:59-60`), efficiency only.
- **Blend:** still premultiplied, `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`, but `glBlendFunc` is called raw, no blend-func cache (`OpenGL.cpp:981-989`).
- **Code changes forced by the move:**
  - No `LOG` macro; `Log::logger->log(level, fmt, args...)` (`Logger.hpp:22`), reached through `compat::log` ("Compatibility with Hyprland main"). `Log::INFO` is still debug level (`:53`).
  - No `bindArrayBuffer` and no array-buffer cache; core binds raw (`OpenGL.cpp:1547`, `Shader.cpp:236`). Plugin binds raw too (superseded: see "Compatibility with Hyprland main", it goes through `compat::bindArrayBuffer`).
  - `projectBoxToTarget` uses `pMonitor->getScaleMatrix()` = `outputProjection(m_pixelSize, NORMAL)` (`Renderer.cpp:1842-1846`, `Monitor.cpp:1757`), and `getBoxProjection` defaults the box transform to the inverted monitor transform (`Renderer.cpp:1836-1840`). Plugin now passes `HYPRUTILS_TRANSFORM_NORMAL` explicitly so the pseudo-box isn't rotated; monitor rotation comes from `targetProjection` (`Renderer.cpp:1828`). Identical on transform-0 outputs; rotated outputs untested (SPEC §8).
  - `PluginAPI.hpp` no longer includes `<format>`; `main.cpp` includes it directly.
- **Color management gap** citations at this pin: `getConvertedColor` in `OpenGL.cpp:1086`, `:2371`.
- **Build:** plugin compiles against the `external/Hyprland` checkout at the pin (after `make clear && make debug` generates `version.h`/protocols). Debug build only adds `HYPRLAND_DEBUG`/`ISDEBUG` (macros, not layout); the only layout-affecting `#if`s in installed headers are `NO_XWAYLAND`, in xwayland headers the plugin doesn't use. ABI hash strips patch versions, so system aquamarine 0.15.1 vs the package's 0.15.0 still matches (`_aq_0.15`).

## Compatibility with Hyprland main (cited at main 4bb6844b, v0.56.0-209, 2026-09-27; pin efb5099)

**Status: building against Hyprland main is unsupported at launch and fails to compile by design (its render API now takes a `Render::CRenderContext&`, see "Main 579829f" below); the port is planned for 1.1.**

- **Setup:** second checkout `external/Hyprland-main` (built with `make clear && make debug`), the pin untouched. `make DEV=1 HYPRLAND_DIR=external/Hyprland-main` builds into `out/Hyprland-main/`; the pin stays in `out/`. `make test-compat` is a compile-only check of `src/compat.hpp` against the selected headers. Main is not a newer pin: v0.56.2 is a release branch, main is 209 commits past v0.56.0.
- **Compile breaks (all now behind `src/compat.hpp`):** `desktop/view/Window.hpp` moved to `desktop/view/window/Window.hpp`; `SHyprCtlCommand`/`eHyprCtlOutputFormat` replaced by `IPC::Socket1::SCommand{name, match, handler(const SRequest&)}` (`ipc/s1/S1.hpp:51-55`, `PluginAPI.cpp:351`); `CWindow::m_class`/`m_title` became `metadata().appID()`/`title()` (`WindowMetadata.hpp:16-17`); `CLogger::log` takes a location (`Logger.hpp:41,45`, `LOG` at `:10`).
- **Silent one:** `log(level, "fmt {}", runtimeString)` and `log(level, "fmt {}", "literal")` compile against main's `(level, string_view loc, string_view str)` overload and print the format string as the `[loc]`. Found at `Diagnostics.cpp` (error file write) and `main.cpp` (loaded line). `make check-log` fails the build on any `logger->log(` outside `compat.hpp`.
- **GL caches:** main shadows array buffer, active texture, blend func and framebuffers (`OpenGL.hpp:228-229`, `OpenGL.cpp:2438-2461`; `blend(true)` -> `blendFunc`, `:1078-1085`). `useShader` (`:2399`) and `scissor` (`:1094`) are unchanged. The plugin's raw `glBindBuffer(GL_ARRAY_BUFFER)` would desync the cache; it now goes through `compat::bindArrayBuffer`, which uses the cache where it exists. No raw `glBlendFunc`/`glActiveTexture`/`glBindFramebuffer` in the plugin.
- **Damage ring:** transactions again (`Renderer.cpp:1786-1789`, `DamageRing.cpp:83-87`; pin reads and rotates at once, `Renderer.cpp:1782-1783`). Mid-render `addDamage` lands in the emptied current region and is taken by the next frame's transaction: same effect as at the pin, so no code change.
- **Render order, hooks, pass elements, event bus: unchanged for the plugin.** `RENDER_LAST_MOMENT` emitted once, after the cursor and DPMS overlay (`Renderer.cpp:2292`), pass rendered in `endRender` (`GLRenderer.cpp:106-110`); `renderSoftwareCursorsFor` callers (`Renderer.cpp:2281`, `ScreenshareFrame.cpp:316,359`) and hook signatures (`PointerManager.hpp:61`, `PointerController.hpp:9`, `OpenGL.hpp:243`) identical; `IPassElement` gained `requiresFullDamage()` (default false, `PassElement.hpp:39`) and `EK_BACKDROP_SCOPE`, `drawCustom` unchanged (`ElementRenderer.cpp:839-844`); event bus only gained events. The hook detours are `reinterpret_cast`, so the compiler does not check their signatures.
- **`projectBoxToTarget` differs:** pin uses `outputProjection(m_pixelSize)` (`Renderer.cpp:1842-1846`, `Monitor.cpp:1757`); main uses `m_transformedSize` for the monitor projection type (`Renderer.cpp:1851-1857`, `RPT_MIRROR` renamed `RPT_OUTPUT`). Same unless the monitor is rotated 90/270. The plugin goes through the core function; **untested on a rotated monitor**.
- **hyprctl:** main answers "unknown request" only when nothing matched (`S1.cpp:107`), not for an empty reply; a duplicate registration returns nullptr (`S1.cpp:224-228`, handled).
- **Smaller:** `Pointer::mgr()->position()` returns the transformed position when a plugin registered a pointer transformer (`PointerManager.hpp:68-71`); `damageBox(box, skipFrameSchedule)` skips the damage when the flag is set (`Renderer.cpp:2870-2877`), the plugin doesn't call it.
- **Not read:** the `Monitor.cpp` diff beyond `addDamage`/`scheduleFrame` (`Monitor.cpp:1096-1107`, same logic as the pin), Lua config provider behavior, workspace event semantics.
- **Symbol check:** `scripts/ci/check-imports.sh` passes for the pin build against `external/Hyprland/build/Hyprland`, the main build against `external/Hyprland-main/build/Hyprland`, and a pkg-config build against the host's `/usr/bin/Hyprland` (LTO). The script needed its exec bit (`git` had mode 100644; CI ran it after a `chmod`).

### Main 579829f: render context refactor (2026-10-04)

Source: `hyprwm/Hyprland` at `579829f065b425f7b4e850915054f2dd472b27c8`, the commit the scratch-ci `main` row and `flake-check` built against (62 commits past 4bb6844b by the compare API). Read through the GitHub contents API, not from a checkout: `external/Hyprland-main` is still at 4bb6844b and gives the "before" lines. Line numbers below are at 579829f unless marked "4bb6844b". The range adds `src/render/Context.{hpp,cpp}` and `SceneResources.*`; I did not identify the commit that introduced `Context.hpp` (the latest one touching it is ca539d4f, "render: select scene resources per render session", 2026-10-04).

**The change:** per-render state moved from `g_pHyprRenderer` into a `Render::CRenderContext` (`Context.hpp:33`) that is passed down. `IHyprRenderer::context()` returns the session's one (`Renderer.hpp:132`, `Renderer.cpp:1934-1936`).

**The three compile errors in `LayerPassElement`** (CI log, `LayerPassElement.o`, the first translation unit to fail; make stopped there):

- **`IPassElement` virtuals take the context.** `draw`, `needsLiveBlur`, `needsPrecomputeBlur`, `boundingBox` and the other virtuals now take `Render::CRenderContext& ctx` (`PassElement.hpp:32,34,35,38-43`); at 4bb6844b they take nothing (`PassElement.hpp:28-39`). `passName()` and `type()` are unchanged (`:36-37`). Our no-argument overrides (`LayerPassElement.hpp:163,165,168,172`) no longer override anything, hence "marked 'override', but does not override". Core's own elements show the new shape (`RectPassElement.cpp:8,12,16`, using `ctx.m_data.pMonitor` at `:17`).
- **`IHyprRenderer::m_renderData` is gone.** It was `Renderer.hpp:150` at 4bb6844b. The data is `CRenderContext::m_data` (`Context.hpp:52`), reached through the `ctx` parameter. The fields we read are still there: `pMonitor`, `damage`, `transformDamage` (`types.hpp:93,101,118`; `transformDamage` was `types.hpp:105` at 4bb6844b). `LayerPassElement.cpp:230`.
- **`CHyprOpenGLImpl::scissor` takes the context first** (`OpenGL.hpp:222-224`; `:237-239` at 4bb6844b, definitions `OpenGL.cpp:1100,1129,1142`). `scissor(ctx, nullptr)` still means "disable `GL_SCISSOR_TEST`" (`OpenGL.cpp:1132-1135`) and it asserts that `ctx.m_data.pMonitor` is set (`:1130`). `LayerPassElement.cpp:324,331`.

**Not compile errors yet, found by reading** (make never reached these files, which sort after `LayerPassElement.cpp`; unverified until a build gets there):

- `g_pHyprRenderer->m_renderData` in `main.cpp:580,605` and `RenderUtil.cpp:44,66`.
- `g_pHyprRenderer->m_renderPass` in `main.cpp:397,869`. It is not in `Renderer.hpp` at 579829f (it was `:136` at 4bb6844b). The pass is `CRenderContext::m_pass` (`Context.hpp:53`); elements are added with `IHyprRenderer::addPassElement(ctx, element)` (`Renderer.hpp:156`, `Renderer.cpp:571-573`).
- `m_mostHzMonitor` is still there (`Renderer.hpp:135`).

**Hooked functions: two of three changed, and the compiler cannot see it.** The plugin hooks three functions (`installHooks`), each through `pmf_address(&Class::fn)` and a `reinterpret_cast<void*>` detour, called back through `m_original` with a hand-written typedef. Nothing compares those signatures with the target's, so a changed target **compiles and then misreads every argument**. At 579829f:

- `CPointerManager::renderSoftwareCursorsFor` gained a leading `Render::CRenderContext& ctx` (`PointerManager.hpp:67-68`; `:61` at 4bb6844b without it). Changed.
- `CHyprOpenGLImpl::saveBufferForMirror` is now `(CRenderContext& ctx, const CBox&)` (`OpenGL.hpp:228`; `:243` at 4bb6844b and `:231` at the pin, `(const CBox&)`). Changed.
- `CPointerController::warpTo(const Vector2D&, bool) const` is unchanged (`PointerController.hpp:9`, same at 4bb6844b and the pin). `CPointerManager::warpTo` is unchanged too (`PointerManager.hpp:52`).

**Guard:** `src/compat.hpp` (namespace `hooks`) now holds the signature each detour is written for and static-asserts that the target has exactly it, with a message naming the detour; `main.cpp` asserts each detour and its `orig*` typedef are that signature (`DetourT`). `tests/compat/compat.cpp` checks the guards against stand-in classes with the 579829f signatures (they must be rejected) and with the expected ones. Checked: `make test-compat` and `make DEV=1 test-compat` pass at the pin, and `make DEV=1 HYPRLAND_DIR=external/Hyprland-main test-compat` passes at 4bb6844b (all three signatures were still the old ones there). No checkout at 579829f exists locally, so the real failure was shown on a copy of the pin's `src/` with each header patched to the 579829f signature: each of the three patched copies stops at the matching `static_assert`, and nothing else fails.

**Fix shape, not applied (the port is planned for 1.1):** the pin and main need different signatures for the same override, so the override signatures, the hook aliases in `hooks` and the `ctx` plumbing have to sit behind `src/compat.hpp` (for example a type that is empty on the pin and `CRenderContext&` on main). Open: how the `RENDER_LAST_MOMENT` listener and the cursor hook get a `ctx` (`context()` is the candidate; the event payload at 579829f was not read), and whether `removeAllOfType` still exists on `CRenderPass` (not read). The rest of the 62-commit range (blur providers, decorations, `ElementRenderer`) was not read.

## Smoke job: the triple Hyprland build

Why the smoke job is slow, from the scratch-ci run 37207041600 log. `nix build .#legacyPackages.x86_64-linux.smoke` printed "these 44 derivations will be built". Three of them are Hyprland trees:

1. `hyprland-with-tests` (`flake.nix:101`): `hyprland.override { withTests = true; }` (`nix/overlays.nix:67` at the pin). It is the VM's `programs.hyprland.package`. A full build, plus the test clients and gtests.
2. Plain `hyprland`: what `hyprtail` is built against (`flake.nix:108`, the overlay's `hyprland`). A second full `hyprland_lib` build: `Compositor.cpp.o` is compiled twice in the log (13:59:37 and 14:05:09).
3. `hyprtester-hyprtail` (`nix/smoke.nix`, `hyprland.overrideAttrs` of the with-tests derivation): builds only `generate-protocol-headers` and `hyprtester`, so much smaller than the other two.

The test job builds only the plain `hyprland`: 9 derivations, Hyprland in about 15 minutes. In the smoke job the first two compile at the same time on one runner, which is why it was still at 84% of `hyprland_lib` after 52 minutes. Its Hyprland libraries (hyprutils, aquamarine and the rest) are also built from source: on the stable row nothing upstream built the pinned-nixpkgs combination, so Cachix has no hit (docs/CI.md "Stable row: pinned nixpkgs").

**Collapsing it (not done).** Build `hyprtail` against `hyprland-with-tests` instead of the plain `hyprland`, so tree 2 disappears. In `flake.nix`, that means a second package set whose overlay sets `hyprland` to `hyprland-with-tests` (the plugin helper `mkHyprlandPlugin` takes `hyprland` from the same pkgs, `flake.nix:42-45`), used only by `legacyPackages.smoke`. Points to check before doing it:

- The headers and `GIT_COMMIT_HASH` are identical, so the plugin's ABI hash should be too. `WITH_TESTS` adds targets but should not change the library. Confirm by comparing the built `hyprland.pc` and `version.h`, and keep `scripts/ci/check-imports.sh` against the plain build in the test job.
- The smoke VM would then test a plugin built against the with-tests variant, not against the shipped `hyprland`. That is a small loss of fidelity. The test job still builds against the plain one.
- `nix/smoke.nix` takes `hyprtail` as an argument already, so only `flake.nix` changes.

**Estimated saving: about 20-25 minutes of wall time on a cold cache, nothing on a warm one.** This is an estimate. The measured numbers: Hyprland alone is about 15 minutes (test job); two trees at once ran at roughly a third of that speed each. Removing one should bring the cold smoke run from over 80 minutes to about 55-60, near the timeout, so the warm-up run in docs/CI.md is still needed first.

## Draw order and hardware cursors (cited at efb5099)

- **No render stage between overlays and the cursor.** `renderMonitor` order: workspace (windows + top/overlay layers) `Renderer.cpp:2175`, lockscreen `:2176`, IME `:2179`, notifications/error overlay `:2181-2184`, then `renderSoftwareCursorsFor` `:2216` (only if `shouldRenderCursor()`, `:2212`), DPMS overlay `:2219-2225`, `RENDER_LAST_MOMENT` `:2227`. No `return` between `RENDER_BEGIN` `:2160` and `endRender` `:2229`.
- **Rejected alternatives:** `RENDER_POST_WINDOWS` (`:1231`) is before top/overlay layers (`:1233-1241`), and isn't emitted when there's no damage (workspace skipped, `:2166`) or on the fullscreen early return in `renderAllClientsForWorkspace`. Pass reordering: `CRenderPass::add` only appends (`Pass.cpp:27-29`), `m_passElements` is private (`Pass.hpp:33`).
- **Hook is viable on the LTO host binary:** `renderMonitor` calls `renderSoftwareCursorsFor` out of line (`call 0x77c7a0` at `0xa221ee` in `/usr/bin/Hyprland`), symbol exported. Only callers: `Renderer.cpp:2216` (`screencopy=false`) and screenshare (`ScreenshareFrame.cpp:312`, `:355`, `screencopy=true`).
- **Hardware cursor motion and renders (Aquamarine v0.15.1, cloned read-only to `external/aquamarine`):** DRM `moveCursor` schedules a frame (`AQ_SCHEDULE_CURSOR_MOVE`) unless `skipSchedule` (`src/backend/drm/impl/Atomic.cpp:762-772`, `Legacy.cpp:17-25`; `scheduleFrame` sets `needsFrame`, `DRM.cpp:2959`), and the frame scheduler goes straight to `renderMonitor` (`MonitorFrameScheduler.cpp:84-105`), which proceeds on `needsFrame` even with no damage (`Renderer.cpp:2072`). The Wayland backend's `moveCursor` is a no-op (`src/backend/Wayland.cpp:829-831`), so in **nested** a hardware cursor move never renders: that's where the old `no_hardware_cursors = 1` requirement came from. Hyprland passes `skipSchedule = shouldSkipScheduleFrameOnMouseEvent()` (`PointerManager.cpp:371`, `Monitor.cpp:1161-1169`).
- **Motion event:** `input.mouse.move` (`EventBus.hpp:108`) is emitted from `mouseMoveUnified` (`InputManager.cpp:269`) only when the floored position changes (`:239-240`), after the pointer position is updated (`:155`, warps `:174`). Also used by touch (`:1754`). The plugin damages a radius box there; insertion stays in the render lifecycle.
- **Software fallback with hardware cursors enabled:** `updateCursorBackend` falls back on software locks (mirror source `Monitor.cpp:1395`, cursor zoom `Renderer.cpp:2125`), `shouldUseSoftwareCursors()` (tearing, `cursor:invisible`, `no_hardware_cursors` 1 / nvidia auto, `Monitor.cpp:2290-2308`) or hardware plane failure (`PointerManager.cpp:323-333`). Those frames draw the cursor in software; the hook keeps the trail beneath it.
- **Direct scanout:** software cursors block DS (`DS_BLOCK_SW` via `softwareLockedFor`, `Monitor.cpp:2056-2057`, `PointerManager.cpp:98-101`). With hardware cursors an eligible fullscreen client is scanned out and `renderMonitor` returns before any stage: no trail over it. Accepted as known behavior. **Unverified:** derived from source only; verification blocked, see Open questions.
- **Test split (decided):** nested dev config keeps `cursor:no_hardware_cursors = 1` so the software path (hook ordering) stays covered; the host stays on default hardware cursors, since that's what users run. The motion listener is what makes hardware cursors work in nested too, but nested can't exercise real DRM cursor planes, so hardware-plane behavior is host-only testing.
- **Option B status:** motion half now built (motion listener damages from outside the render path). Remaining: a monitor that never renders still can't start its own trail chain if the trail reaches it without the pointer (stage 5).

## Error reporting (cited at efb5099)

- **Why:** `debug:disable_logs` defaults to true, so log-only failures are invisible to users.
- **Module:** `src/Diagnostics.{hpp,cpp}`, namespace `hyprtail::diag`. `report(severity, key, message)`: Hyprland log every time; notification + error-file entry once per key per load (`resetKey` re-arms, for config reloads / files appearing). `guard(where, fn)` runs `fn` and turns any exception into a report under `callback:<where>`. Nothing in it throws.
- **Keys** name the failing thing, not the message: `shader:<instance>`, `gl:<instance>`, `hook:<name>` (`hook:cursor`, `hook:warp`), `callback:<where>`; future `config:<value>`, `file:<path>`. ERR = feature off, WARN = degraded but running.
- **Error file:** `$XDG_STATE_HOME/hyprtail/errors.log`, fallback `~/.local/state/hyprtail/errors.log` (not `$XDG_RUNTIME_DIR`, wiped on reboot). Truncated in `diag::init` at plugin load (superseded: now rotated to `errors.log.1` and fsynced per write, see "Freeze analysis" below), header records UTC time and `HYPRLAND_INSTANCE_SIGNATURE`, capped at 256 KiB. Notifications are cut to ~8 lines / 600 chars and end with "Full message: <path>". **Caveat:** nested and host instances share the file, so loading the plugin in one truncates the other's session log; the header's instance signature tells them apart.
- **Notifications are deferred** with `g_pEventLoopManager->doLater` (`EventLoopManager.cpp:215-238`), never shown inside a render. The pending id is cancelled with `removeDoLater` (`:240-247`) in `diag::shutdown`, since a queued lambda from an unloaded `.so` would jump into unmapped code. `addNotificationV2` (icon) with fallback to `addNotification` (`PluginAPI.cpp:112-121`, `:277-322`); the V2 `std::any` casts work across the `.so` boundary through libstdc++'s typeid fallback.
- **Init failure path:** Hyprland catches only `std::exception` from `PLUGIN_INIT`, then ejects the plugin **without** calling `PLUGIN_EXIT` (`PluginSystem.cpp:113-126`, `:147-150`), removes its hooks (`:162`) and dlcloses it. So init catches everything, notifies synchronously, runs `teardown()` (listeners, hook, elements, GPU, `diag::shutdown`) and rethrows as `std::exception`. Version mismatch uses the same path.
- **Shader errors:** `CShader::createProgram` logs and discards the GLSL log, and with `silent=false` pushes it to core's error bar prefixed "Screen shader parser" (`Shader.cpp` `logShaderError`). The plugin compiles/links once itself with raw GL to capture the log (`glslCheck`), reports it, and only then calls `createProgram(dynamic, silent=true)`.
- **GL errors:** `CTrailGpu::ensure` checks for zero names and drains/reads `glGetError` around `glBufferData`. Draining hides errors core left behind; only core's debug builds check those.
- **Hook failure:** `createFunctionHook` null or `hook()` false -> `removeFunctionHook`, WARN `hook:<name>` saying what degrades, keep running. `hook:cursor`: the per-render serial already makes the `RENDER_LAST_MOMENT` fallback run every render when the hook never fires (trail above the cursor). `hook:warp`: warps can't be detected, so every warp connects regardless of `interpolateWarps`.
- **Exceptions:** guarded at every entry from Hyprland: cursor hook (our part only; the original always runs unwrapped), render-stage listener, mouse-move listener, `CTrailPassElement::draw`, deferred flush, teardown/exit. A caught exception disables the trail instance (fail closed, no per-frame spam); inside a render its GPU resources are released right away, otherwise at unload.

## Ribbon (cited at efb5099)

- **Teleport signal research:** `onMouseWarp` (`InputManager.cpp:173-176`) calls `warpAbsolute` (`PointerManager.cpp:846`) for absolute-motion devices, i.e. continuous tablet/VM motion, not teleports. No warp event on the bus. `CPointerManager::warpTo(pos)` (`PointerManager.cpp:820-831`) has no reason and is also reached from relative motion (`move()` -> `warpTo(newPos)`, `:833-843`). `CPointerController::warpTo(pos, force)` (`PointerController.cpp:16-29`, exported) is the programmatic layer but carries no reason either, and `PointerWarp.cpp:76`, `InputCapture.cpp:206`, `InputManager.cpp:2248`, `WorkspacePlacementController.cpp:356` call the manager directly.
- **Content-changed signal used instead:** `workspace.active` (`EventBus.hpp:174`, emitted `Monitor.cpp:1507` after the focus warp / `simulateMouseMovement` at `:1497-1500` in the same call stack, so before any render), `workspace.specialActive` (`Monitor.cpp:1597`, `:1700`), `workspace.moveToMonitor` (`WorkspacePlacementController.cpp:235-236`, `:379`). Each sets `pendingBreak`; session lock sets it too. Everything else connects.
- **Superseded: hidden-state tracking.** An intermediate version broke the trail after a hide only if the pointer moved while hidden (position at hide vs. last position seen while hidden, fed by hidden renders, hidden `mouse.move` events and the warp hook). Replaced by decoupling the trail from cursor visibility entirely (see "Trail vs. cursor visibility"). The listener-order finding still holds: core's unhide-on-move listener (`Renderer.cpp:154`, registered at startup) runs before plugin listeners because hyprutils emits in registration order (`Signal.cpp:21-35`).

- **Why four bindings:** with only i and i+1, a segment can't see its neighbors' directions, so joins must overlap (capsules / extended quads) and translucent overlap blends twice. Miter joins need i-1 and i+2.
- **No face culling** anywhere in Hyprland (no `GL_CULL_FACE`/`glCullFace` in `src/`), so ribbon quad winding doesn't matter, including on flipped outputs.
- **`#embed`** works with GCC 16 in `-std=c++26` mode, but produces byte values: embed into `unsigned char` arrays (non-ASCII bytes narrow into `char` and fail to compile), and keep shader files ASCII anyway.
- **Old shaders:** the stage-2-era `shaders/trail.{vert,frag}` (320 es, `gl_PointSize`, unused since stage 3) matched nothing in `reference/hyprtail.old/`; identical copies exist in `dev_env/hypr/shaders/` and git history. Moved to `reference/stage2-shaders/` (gitignored) before writing the ribbon shaders.
- **Known risks, to check visually:** inner-miter folding on very tight turns relative to width (min spacing mitigates); joint corners rely on both instances producing bit-identical results from identical expressions (typical, not guaranteed by GLSL invariance rules) — a hairline crack at joints would point here.

## Trail vs. cursor visibility (cited at efb5099)

- **Decision:** the trail follows pointer motion regardless of cursor visibility. Breaks only on workspace events, lock, pointer constraints, and (unless `interpolateWarps`) hooked warps.
- **`cursor:invisible`:** `ensureCursorRenderingMode` folds it into `HIDE = hiddenByCondition || (*PINVISIBLE != 0) || inputCapture` (`Renderer.cpp:2936`) and applies it via `setCursorHidden` (`:2953`), i.e. `m_cursorHidden`; `shouldRenderCursor()` = `!m_cursorHidden && m_cursorHasSurface` (`:2976-2978`). Core's unhide-on-move re-runs the same computation, so an invisible cursor stays hidden across motion. It also forces software cursors (`Monitor.cpp:2297-2298`). Effect on the plugin: the cursor call is skipped (`Renderer.cpp:2212-2216`), so the `RENDER_LAST_MOMENT` fallback runs the lifecycle every render; motion damage still starts renders. Works.
- **Client-hidden cursor is indistinguishable:** a null cursor surface sets `m_cursorHasSurface = false` (`Renderer.cpp:2850`); `m_cursorHidden` / `m_cursorHasSurface` are `protected` (`Renderer.hpp:288-290`, `protected:` at `:217`), so a plugin only sees the combined `shouldRenderCursor()`.
- **Pointer constraints:** `CInputManager::isConstrained()` (public, `InputManager.hpp:119`, exported) is the plugin's "client owns the pointer" signal. Constraints correct the pointer through the programmatic warp path: locked -> `pointerController()->warpTo(HINT, true)` on every motion (`InputManager.cpp:318-320`), confined -> `warpTo(CLOSEST, true)` at the region edge (`:306`). Without ignoring those, `interpolateWarps = false` would chop a confined trail along the edge. Relative motion moves `m_pointerPos` before the constraint snaps it back (`PointerManager.cpp:833-843` then `InputManager.cpp:314+`).
- **VRR gate:** `shouldSkipScheduleFrameOnMouseEvent` = `(!shouldRenderCursor || noBreak) && adaptiveSync` (`Monitor.cpp:1161-1169`), true on every adaptive-sync output while the cursor is hidden. Plugin keeps core's check with the cursor shown, and with it hidden skips only when adaptive sync is on and `Fullscreen::controller()->getFullscreenWindow(monitor)` (`FullscreenController.hpp:71`, `:113`; both exported by `/usr/bin/Hyprland`) returns a window. Host outputs currently all have `"vrr": false`, so this is latent there.
- **`interpolateWarps`** (instance placeholder, default false): hooked `CPointerController::warpTo` that actually moved the pointer (compares the real position before/after, so `cursor:no_warps` counts as no move) sets `pendingBreak`. Direct `CPointerManager::warpTo` callers always connect. Backlog: bezier curves for the interpolated case.

## Config and shader loader (cited at efb5099)

- **V1 config API is legacy-only:** `addConfigValue`, `addConfigKeyword`, `getConfigValue` return false/nullptr unless `Config::mgr()->type() == CONFIG_LEGACY` (`PluginAPI.cpp:180-181`, `:199-200`, `:215-216`); all three `[[deprecated]]` (`PluginAPI.hpp:141`, `:151`, `:163`).
- **V2:** `addConfigValueV2(handle, SP<Config::Values::IValue>)` (`PluginAPI.hpp:348`, `PluginAPI.cpp:446-463`) -> `Config::mgr()->registerPluginValue` (Lua `lua/ConfigManager.cpp:1147-1164`, legacy `legacy/ConfigManager.cpp:2153`) then `commence()`. Value classes `CFloatValue`/`CIntValue`/`CBoolValue`/`CStringValue` (`config/values/types/*.hpp`) take `const char*` name/description stored raw (literals only) and min/max options; `value()` reads live storage. Constructors, `value()`, `commence()`, vtables all exported by the host binary. Types: `FLOAT=float`, `INTEGER=int64_t`, `BOOL=bool`, `STRING=std::string` (`config/shared/Types.hpp:15-19`).
- **Lua addressing:** names `plugin:hyprtail:x` become `plugin.hyprtail.x` (`lua/ConfigManager.cpp:1136-1141`); `hl.config` walks nested tables joining keys with `.` and reports `unknown config key` for misses (`LuaBindingsConfigRules.cpp:979-1006`). So `hl.config({ plugin = { hyprtail = { ... } } })`.
- **Precedent:** `hyprland-plugins` at `eaf18d5` ("all: update for 0.55", older than the pin): `hyprfocus/main.cpp:133-139` keeps `SP<C*Value>` and registers with `addConfigValueV2`, reads `->value()` live (`:52-102`); `hyprexpo/main.cpp:176-185` calls `registerPluginValue` directly; `hyprexpo/README.md:53-54` documents the `hl.config({ plugin = { hyprexpo = ... } })` form.
- **Unknown key on first parse, not visible at startup:** `hl.plugin.load` only records the path (`LuaBindingsConfigRules.cpp:537-553`). Config init runs at `STAGE_PRIORITY` (`Compositor.cpp:683`), `handlePluginLoads` there returns early (no plugin system yet, `lua/ConfigManager.cpp:848`, `:1120-1121`); the plugin system is created and plugins loaded at `STAGE_LATE` (`Compositor.cpp:737-738`), which on change calls `ErrorOverlay::destroy()` + `reload()` (`lua/ConfigManager.cpp:1126-1128`). The first parse only queued the red bar (`:779-801`, "displayed next frame"); `destroy()` on a not-yet-created overlay clears the queue (`errorOverlay/Overlay.cpp:257-261`), and the event loop (so any frame) only starts at `startCompositor` (`Compositor.cpp:823`). Visible only when the keys are set and the plugin isn't loaded (manual load, after unload).
- **Reload signal:** `Event::bus()->m_events.config.reloaded` (`EventBus.hpp:182`), emitted at the end of every Lua reload (`lua/ConfigManager.cpp:852`) and legacy (`legacy/ConfigManager.cpp:1072`), including the one the plugin system schedules after a load (`PluginSystem.cpp:135`).
- **Lifetimes:** on unload `onPluginUnload` erases the plugin's config entries (`lua/ConfigManager.cpp:1322-1336`) and the plugin's registered values are released with the `CPlugin` before `dlclose` (`PluginSystem.cpp:178-186`); our own `SP`s are released in `teardown()`.
- **Relative shader paths** use `Config::mgr()->getMainConfigPath()` (virtual, `ConfigManager.hpp:56`) so nested `-c` configs resolve next to their own file.
- **File watch:** inotify fd added with `wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, ...)` (`Compositor.hpp:36`; `m_wayland.loop` in `CEventLoopManager` is private, `EventLoopManager.hpp:81-95`). Watches parent directories (`IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE`) because editors save by rename.
- **Compile path:** `CShader::compileShader` passes source through untouched (`Shader.cpp:51-75`), so the preprocessed text is what the driver sees. On program swap the new program is made current through `useShader` before the old one is destroyed, so Hyprland's program cache never holds a deleted id.
- **Regex pitfall:** a default raw string `R"(...)"` ends at the first `)"`, which `"([^"]+)"` contains; the preprocessor's regexes use `R"re(...)re"`.

## hyprpm (cited at efb5099)

- **Manifest** (`hyprpm/src/core/Manifest.cpp`): `hyprpm.toml` is preferred over the legacy `hyprload.toml` (`PluginManager.cpp:240-245`). `[repository]`: `name`, `authors` (or `author`), optional `commit_pins` = array of `[hyprland_commit, plugin_commit]` string pairs (`Manifest.cpp:63-83`). Every other top-level table is a plugin: `description`, `authors`, `output` (path of the built `.so`, relative to the repo root), `build` (array of shell steps), optional `since_hyprland` (int, Hyprland commit count) (`:85-118`). `output` and `build` are required, else the manifest is rejected (`:120-123`). Plugin names are `[A-Za-z0-9_=-]` (`:5-9`).
- **Build:** clones the repo `--recursive` into a temp dir (`PluginManager.cpp:215`), then runs each step as `cd <repo> && PKG_CONFIG_PATH="<headersRoot>/share/pkgconfig:$PKG_CONFIG_PATH" [CFLAGS=...] [CXXFLAGS=...] <step>` (`:326-336`, env from `getPluginBuildEnv`/`getPkgConfigPath`, `:1137-1156`), then checks `output` exists (`:342`). `headersRoot` is filled by `hyprpm update`: it clones Hyprland, checks out the **running** Hyprland's commit (`:560`), configures with cmake and runs `make installheaders` into `headersRoot` (`:588-636`). So `pkg-config hyprland` inside a hyprpm build gives headers matching the running compositor, which is why the Makefile's default mode uses pkg-config and must not need `external/` (gitignored anyway). `CFLAGS`/`CXXFLAGS` extras come from `HYPRPM_EXTRA_*` at hyprpm build time, so the Makefile appends to `CXXFLAGS` rather than assigning. hyprpm's own dependency list: cmake, cpio, pkg-config, git, g++, gcc (`:152`).
- **Commit pins:** only consulted when no explicit revision is requested; for each `[hl, plugin]` pair, if `hl` equals the running Hyprland's commit hash exactly, hyprpm `git reset --hard`s the plugin clone to `plugin` before building (`:273-297`; update path `:787-812`). They let old Hyprland versions keep building an older plugin commit after main moves on. `hyprland-plugins` keeps one pair per Hyprland release (`hyprland-plugins/hyprpm.toml`, at `eaf18d5`).
- **Do we need pins now:** no. Main is developed against the single pinned Hyprland (`efb5099`, v0.56.2), so the latest commit is the right one for it. When the pin moves to a newer Hyprland and main stops building against v0.56.2, add `["efb50993780079460b0cbed1363e2166a2de1d9f", "<last hyprtail commit that supports it>"]` (plus any other v0.56.x release hashes users run) at that point.
- **`since_hyprland`** not set: it compares Hyprland's commit count (skipped for shallow clones, `:318`, `:820`), and counts differ between release branches and main, so it's a coarse filter at best.

## Color management (cited at efb5099)

- **What core does:** `IHyprRenderer::getConvertedColor(color)` (`Renderer.cpp:3427-3447`, exported) converts an sRGB color from `DEFAULT_SRGB_IMAGE_DESCRIPTION` to the current framebuffer's image description (`m_renderData.currentFB->imageDescription()`, else the work buffer's), cached per description id. `NColorManagement::convertColor` (`ColorManagement.cpp:492-521`): un-premultiply, to linear by source TF, primaries matrix (or ICC), to nits, re-premultiply, tonemap if needed, from linear by destination TF, SDR saturation/brightness modifiers. Core applies it CPU-side per solid color: rects (`OpenGL.cpp:1086`, premultiplied input), borders/gradients (`:2371`, `:2484`, opaque `stripA()` input with alpha applied separately), rect elements (`ElementRenderer.cpp:215`), then blends in the output's encoding.
- **Why not per-pixel:** core's texture path uses `CM.glsl` with uniforms from `CHyprOpenGLImpl::passCMUniforms` (`OpenGL.hpp:344-349`), which is **private** (`private:` from `:303`). The `CM.glsl` source itself is reachable (`Render::g_pShaderLoader->includes()`, `ShaderLoader.hpp:63`, exported), and `getCMSettings` is public (`Renderer.hpp:211`), but driving it means re-implementing a private function's uniform setup. Not done.
- **Plugin:** palette settings `color_slow`/`color_fast` (`CColorValue`, ARGB int, `values/types/ColorValue.hpp`; `CHyprColor(uint64_t)` decodes ARGB, `Color.cpp:14`). In `draw()` (render, `currentFB` valid) each is converted with `getConvertedColor(c.stripA())` and passed as `vec4(converted rgb, configured alpha)`, same pattern as borders. Shaders mix the two in the output's encoding, like core's gradients. In-shader colors are unmanaged and the contract says so.
- **Without CM:** conversion still runs (from sRGB to the framebuffer's description, e.g. gamma 2.2 default), same as for core's own colors, so the trail matches borders/rects of the same configured color.

## Performance (cited at efb5099)

- **Plugin's own per-frame cost is small:** per monitor per render, an O(visible points) bounds walk, two damage boxes, one pass element; a VBO upload only when a point was inserted (<= 1/frame); one instanced draw per damage rect; per pointer event (up to 1000/s) a loop over monitors adding one box.
- **Dominant cost is the repaint we cause:** every frame while a trail is visible, Hyprland redraws everything under the damage (windows, shadows, borders, blur, which also expands damage). The whole-trail bounding box on a fast diagonal flick can be half the screen for ~fade_ms.
- **Damage ring merge (revisit after the next re-pin):** at `efb5099`, `CDamageRing::getBufferDamage` collapses any region with more than 8 rects to its extents (`DamageRing.cpp:59-60`), and a reused buffer's damage is the union of the last 2-3 frames (`:51-56`). Per-segment or grouped damage (even 8 groups) would therefore collapse back to roughly the whole-trail box on most frames; 2-3 groups might stay under the limit. On `main` (old pin `1b85c7aa`) the merge only collapses when the extents aren't much larger than the rects (`DAMAGE_RING_EXTENTS_FACTOR`, `DamageRing.cpp:118-129` there), so tighter damage pays off after a re-pin to a version with that logic. **Decision:** measure first (`debug:overlay`, `debug:damage_blink`, flicks over blurred windows, loaded vs unloaded); no grouped damage for now.

## Idle slot implementation (cited at efb5099)

- **Design B as recorded in SPEC section 7:** quad (square) around the pointer, no point buffer, fragment shader draws via SDF. Stock ring from `shaders/idle.{vert,frag}` + `hyprtail/sdf.glsl`.
- **Shared helpers extracted for it:** `hyprtail::CShaderSlot` (`src/ShaderSlot.*`: the loader/compile/keep-previous logic that used to live in `TrailPassElement.cpp`, now per slot; uniform locations cached per program by name) and `hyprtail::CMonitorDamage` + `outwardPixelBox`, `damageInRender`/`damageOutsideRender`, `globalProjection`, `setPaletteUniform` (`src/RenderUtil.*`). The trail was refactored onto both; behavior unchanged apart from the empty-frame guard below.
- **Empty-frame guard (found while adding the idle timer):** `renderMonitor` only renders the solitary client or the workspace when `finalDamage` is non-empty (`Renderer.cpp:2164-2166`); `finalDamage` = the buffer damage from the ring (`:2148-2151`). A render triggered by `scheduleFrame` alone (frame callbacks, DRM cursor moves, our `kickRender`) has no damage, so adding render damage and an element from our lifecycle would draw onto a region whose background wasn't repainted (stale, possibly an older buffer's content). `CMonitorDamage::update` now checks `m_renderData.damage.empty()` first and in that case only damages the ring and returns false (caller draws nothing). The idle timer damages the idle square via `addDamage` before the render for the same reason.
- **Idle timer:** `wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, ...)` (libwayland-server, like the file watch), re-armed with `wl_event_source_timer_update` on every observed motion; removed in `teardown()`. Re-arming on every pointer event (up to ~1000/s) is a `timerfd_settime` per event; cheap, revisit if it shows up in measurements.
- **Motion sources for the idle wait:** `input.mouse.move` (before any early return), the `CPointerController::warpTo` hook (actual resulting position), and a position change noticed during any render (covers the direct `CPointerManager::warpTo` callers too, as soon as anything renders).
- **Visibility:** `idle_when_hidden = false` by default (user decision): the effect marks where the cursor is; drawing around a hidden cursor defeats the hide. Uses `shouldRenderCursor()`, so a client-hidden cursor also counts as hidden.

## Stale build on the host after hyprpm update (cited at efb5099)

- **Symptom:** host errors.log showed built-in shaders declaring `vec3 v_color` while the repo, HEAD `ad66d29`, and both `.so` files on disk (`/var/cache/hyprpm/abhi/hyprtail/hyprtail.so`, `out/hyprtail.so`) embed `vec4`. The message prefix "`trail trail: keeping ...`" is the pre-`CShaderSlot` format (`"{} trail: {}"` in `TrailPassElement.cpp`), present only in commit `8bede2b` (which also has the `vec3` shaders).
- **Cause:** hyprpm built HEAD correctly (`/var/cache/hyprpm/abhi/hyprtail/state.toml` hash `ad66d29`, `.so` 01:27:36) but didn't reload it. `hyprpm update` calls `ensurePluginsLoadState()` without `forceReload` (`hyprpm/src/main.cpp:158`), and that skips any plugin already loaded by name (`PluginManager.cpp:1033`). hyprpm installs the new `.so` with coreutils `install` (`Sys.cpp:154-164`, `DataState.cpp:103-107`), which replaces the file (new inode), so the running process keeps executing its old mapping without crashing. Errors at 08:31 UTC came from the `8bede2b` build loaded at session start. **Fix:** `hyprpm reload -f` (force unload + load, `main.cpp:208-209`) or restart Hyprland after `hyprpm update`.
- **Not a stale object:** every object depends on every embedded shader file (`$(SHADER_FILES)` in the Makefile's object rule), so a shader change rebuilds all of them, and hyprpm builds from a fresh clone anyway. Commits `a55b5e1`/`ad66d29` contain the `vec4` shaders; the working tree was clean.
- **Why "keeping the previous shader" drew nothing (inference):** the old C++ doesn't set `colorSlow`/`colorFast`. The user's copied shaders (current, `vec4`) read them, so a both-custom program linked and drew with alpha 0. Each later one-stage change failed to link and the slot kept that invisible program. Two fixes in `CShaderSlot`: (1) contract check after link: active uniforms and attribute locations must be ones the plugin provides for the slot, else the program is rejected with a message; (2) fallback: keep the previous program only when the failing pair comes from the same files (an edit), otherwise (a config change) use the built-in program.
- **Build revision:** now stamped into the build (`out/rev.hpp`) and shown on load and in the errors.log header, so which build is running is visible directly.

## Duplicate instance detection (cited at efb5099)

- **Symptom:** an older build from another path plus hyprpm's new copy loaded at once; the new one got "could not register config value" for every setting (name collision in `registerPluginValue`, `lua/ConfigManager.cpp:1151-1152`) and failed both hooks: ~20 warnings, no clear cause.
- **How Hyprland tracks plugins:** each loaded plugin is a `CPlugin` (`m_name`, `m_path`, `m_handle`, ..., `PluginSystem.hpp:14-25`) in the private `m_loadedPlugins` (`:52`); public `getAllPlugins()` / `getPluginByHandle()` (`:44-45`, `PluginSystem.cpp:260-265`), both exported. The only duplicate check is by path (`loadPluginInternal`, `PluginSystem.cpp:72-75`), so two builds at different paths both load. Our own entry is pushed before `PLUGIN_INIT` with `m_path`/`m_handle` set (`:77-90`); `m_name` is only filled from the init return value afterwards (`:128-131`), so during init the other instance is the one named `hyprtail` with a different handle. Committed builds named themselves `hyprtail` or `hyprtail-stageN`.
- **Refusal:** first thing in `PLUGIN_INIT`, before `diag::init` (which would rotate away the other instance's `errors.log`): throw `std::runtime_error`, which Hyprland catches and ejects the plugin (`PluginSystem.cpp:113-126`). Nothing registered yet, so no teardown.
- **Exactly one notification:** a config-driven load (Lua `hl.plugin.load`) already gets Hyprland's own "failed to load" notification with our exception text (`PluginSystem.cpp:229-230`); `hyprctl plugin load` only returns the error to the caller (`HyprCtl.cpp:1824-1840`) and hyprpm's `loadUnloadPlugin` ignores the reply entirely. So the plugin notifies itself unless the Lua config's `m_registeredPlugins` (public, `lua/ConfigManager.hpp:131`) contains its path. The legacy manager's list (`m_declaredPlugins`) is private, so under hyprlang a config-driven refusal may show two notifications. Including `config/lua/ConfigManager.hpp` needs Lua headers (`<lua.h>`); Hyprland needs them to build anyway.

## Crash-loop guard (cited at efb5099)

- **Why:** a plugin that crashes Hyprland (or hangs it) during init/render
  gets reloaded on the next session start with no memory of that -- for a
  session manager or `hyprpm` autoload, that's a silent crash loop. Nothing
  in the existing lifecycle (duplicate-instance refusal, the ABI hash
  check, `errors.log` rotation) catches "the last load of this exact build
  against this exact Hyprland never made it back to `teardown()`".
- **Marker, not a counter:** one file
  (`$XDG_STATE_HOME/hyprtail/crash-guard.marker`, same directory as
  `errors.log`, `src/StatePath.hpp`), plain `key=value` lines: `rev`
  (`HYPRTAIL_REV`), `hyprland` (`__hyprland_api_get_hash()` of the *running*
  compositor, not `__hyprland_api_get_client_hash()` -- the point is "this
  same running process", which changes on every Hyprland restart even
  without a version bump), `instance` (`HYPRLAND_INSTANCE_SIGNATURE`, for
  the refusal notification only, not part of the match), `pid`
  (`::getpid()`). A counter would need the same dead-pid check to tell
  "still running, expected to still be there" from "crashed N times"
  anyway, so a single marker plus a liveness check is the whole mechanism.
- **Match rule:** `rev` and `hyprland` equal to the current load's, *and*
  `pid` no longer alive (`kill(pid, 0)` == `ESRCH`, `CrashGuard.cpp`
  `pidIsDead`). Only `ESRCH` counts as dead; a live pid or "can't tell"
  (e.g. `EPERM`) is always treated as alive, since a live-or-unknown pid
  must never be mistaken for a crash. A live pid under a matching key means
  some other instance of the *same* build+Hyprland is still in its own run
  right now (the obvious case: a nested dev Hyprland and the host sharing
  the same `$XDG_STATE_HOME`, since only the smoke-test target isolates
  that env var, SPEC §2/§10) -- ignored, not refused, and that instance's
  marker is left alone (see below).
- **Checked before `diag::init`,** in `PLUGIN_INIT`, the same reasoning as
  the duplicate-instance check just above it: `diag::init` rotates
  `errors.log` to `errors.log.1`, which would destroy the crashed session's
  own log the moment the next (refused) load ran. The refusal notification
  names the marker path, the `errors.log` path (computed directly via
  `hyprtail::stateDir()`, not through `diag`'s own state, which is empty
  before `diag::init` runs this load) and the previous pid/instance.
- **Ownership, not a blind overwrite:** `pluginInit()` writes a fresh marker
  unconditionally once the crash-loop check has passed (nothing left to
  preserve: either there was no marker, or its key/pid didn't indicate a
  crash), and arms `s_crashGuardTimer` only if that write succeeded.
  `teardown()` removes the marker only when `s_crashGuardTimer` is
  non-null -- i.e. only a marker *we* wrote this load, never one left by
  some other still-running instance that we merely read and decided not to
  refuse over. This doesn't fully solve the shared-directory race (two
  processes writing the same file with no lock), but it does stop the one
  failure mode that actually mattered: our own `teardown()` deleting
  someone else's live marker.
- **60s clearing timer:** same `wl_event_loop_add_timer` /
  `wl_event_source_timer_update` / `wl_event_source_remove` idiom as the
  idle timer (`s_idleTimer`, "Idle slot implementation" above), one-shot
  (returns 0, never re-armed). Removed in `teardown()` unconditionally
  alongside the marker-ownership check above.
- **Unit tested** (`tests/unit/unit.cpp`, `make test-unit`): `parse`/
  `format` round-trip and reject malformed/incomplete text; `pidIsDead`
  against a real reaped child (guaranteed dead) and the test process's own
  pid (guaranteed alive); `indicatesEarlyDeath` across matching/mismatched
  rev, hash and live/dead pid; a real `writeMarker`/`readMarker`/
  `removeMarker` round trip against a scratch dir via `setenv
  ("XDG_STATE_HOME", ...)`, never the real state directory.
- **Smoke-tested** (`tests/hyprtester/hyprtail_smoke.cpp`, step 7): rather
  than deriving the expected `rev`/`hyprland` strings independently, the
  test reads the *plugin's own* marker (written by the still-loaded plugin
  from an earlier step), unloads, and rewrites only the `pid` line to a
  freshly forked-and-reaped (guaranteed dead) pid before attempting a
  reload -- refused, compositor alive, absent from `/plugin list`; deleting
  the marker lets the next load through.

## Monitor hotplug (cited at efb5099)

- **Events** (`EventBus.hpp:158-169`): `monitor.removed` fires from a scope guard in `CMonitor::onDisconnect` (`Monitor.cpp:392-400`), both for a disconnect and for a monitor rule disabling it (`MonitorRuleManager.cpp:187`, the `CMonitor` survives and may `onConnect` again). `monitor.destroyMon` fires in `CMonitorStateTracker::remove` after the monitor left both lists (`MonitorState.cpp:152-156`); after that the `CMonitor` can be freed and its address reused by the next `makeShared<CMonitor>` (`:118` area, `newMon`). `monitor.layoutChanged` fires after `arrange()` (`MonitorLayoutController.cpp:70`), after a mode change (`Monitor.cpp:1399`) and after monitor rules are applied (`MonitorRuleManager.cpp:204`), including config reloads that change nothing.
- **Before the fix:** `s_monFrame` and both `CMonitorDamage::m_prev` maps kept entries for removed monitors (leak). A monitor at a reused address inherited a stale previous box (one spurious repaint, clipped to the monitor) and stale render serials (harmless: `RENDER_BEGIN` bumps the serial before any comparison). Benign by luck.
- **Fix:** `removed` and `destroyMon` erase the monitor from `s_monFrame`, both damage maps and the layout snapshot. On `layoutChanged`, compare every enabled non-mirror monitor's logical box (`allMonitors()`, not `monitors()`: core fixes the latter up in its own `layoutChanged` listener, `MonitorState.cpp:32-46`, which may run after ours) with the snapshot; if one is new or moved or resized, clear the trail ring, set `pendingBreak`, and re-damage each monitor's last drawn box (monitor-local, so still where the trail is on screen). Only a removal changes nothing: its points have no monitor to draw on.
- **Mid-fade removal:** nothing refers to the monitor beyond these maps. GL objects belong to the shared context (`onDisconnect` frees only that monitor's render resources, `Monitor.cpp:412-413`), pass elements live for one render. Core snaps the pointer to the backup monitor with `warpTo(..., true)` (`Monitor.cpp:478-480`), which our warp hook sees: new segment unless `interpolate_warps`, and the idle effect ends (counts as motion).

## Freeze analysis (cited at efb5099)

- **Report:** host froze (all outputs, no VT switch, no crash report, nothing GPU-related in the kernel log) when `transform = 4` was applied to one of four monitors; `transform = 1` was fine. A second freeze followed with no transform change, while a fullscreen Wine game started. Both involved display reconfiguration. Host config changes made shortly before: `render:direct_scanout = 2`, VRR on, debug overlay on. Not isolated yet.
- **Plugin paths checked, nothing transform-dependent:** our only contact with the transform is through core functions core itself calls for every surface: `scissor(&RECT, rd.transformDamage)` (`OpenGL.cpp:991-1031`, box transform `hyprutils Box.cpp:94-97` for FLIPPED, plain arithmetic) and `projectBoxToTarget(..., NORMAL)` with the monitor transform in `targetProjection` (`Renderer.cpp:1828, 1842`, GPU-side only). CPU loops are bounded by the ring capacity (<= 4096) or pixman rect counts; config floats are range- and finiteness-checked; box widths are max-min plus non-negative padding. Region calls are single pixman unions (`hyprutils Region.cpp:65-68`); pixman's handling of bad rects not checked (source not in the tree). Damage/render feedback is vblank-paced: `addDamage` in a render sets `m_pendingFrame` (`Monitor.cpp:1132-1133`), one more frame is scheduled after the commit (`Renderer.cpp:2266-2269`), and it stops once the trail has faded or the idle duration ends; empty frames skip the render (`Renderer.cpp:2073`). Stock shaders have no loops.
- **One unbounded loop found:** `while (glGetError() != GL_NO_ERROR)` in `CTrailGpu::ensure`, spinning forever if a driver kept returning an error (lost context; driver behavior not cited). Only runs on buffer (re)allocation, not on a transform change, so not the cause. Capped at 16 reads.
- **Hardening (not a fix):** damage boxes rejected if not finite and clipped to the monitor before reaching render damage (only the ring clipped before, `DamageRing.cpp:21-31`); errors.log rotated to errors.log.1 at load instead of truncated, every write fsynced.
- **Core has flip-specific code in the hardware-cursor path** (`PointerManager.cpp:589-592`); the host uses hardware cursors. The plugin doesn't touch that path.
- **To isolate (user):** reproduce without the plugin loaded; with SSH from another machine, `top -H -p $(pidof Hyprland)` (busy vs. blocked) and `gdb -p $(pidof Hyprland) -batch -ex 'thread apply all bt'`; Magic SysRq REISUB instead of power-off so logs reach disk (the Hyprland log is on tmpfs, `$XDG_RUNTIME_DIR/hypr/`).

## hyprtester smoke test (cited at efb5099)

- **Structure:** tests are compiled into the `hyprtester` binary from `hyprtester/src/**/*.cpp` (`GLOB_RECURSE ... CONFIGURE_DEPENDS`, `hyprtester/CMakeLists.txt:14`), registered by `TEST_CASE` (`src/shared.hpp:49-86`) into `testCases` and runnable by name (`main.cpp:230-237`). It launches `--config <file>` with `HYPRLAND_HEADLESS_ONLY=1` (`main.cpp:67-75`), waits 10 s, attaches to the newest instance under `$XDG_RUNTIME_DIR/hypr` (`hyprctlCompat.cpp:26-80`), creates `HEADLESS-2`, loads its own test plugin, runs each test after `preTestCleanup` (`/reload`, cursor to 960,540, `main.cpp:155-182`), exits nonzero on failure.
- **Isolation:** because it attaches to the *newest* instance, a test Hyprland that died early would make it talk to a running session. `make smoke` gives it a fresh `XDG_RUNTIME_DIR` and unsets `WAYLAND_DISPLAY`/`DISPLAY`/`HYPRLAND_INSTANCE_SIGNATURE`; the environment reaches Hyprland because hyprutils `CProcess` only adds variables before `execvp` (`Process.cpp:183-234`, hyprutils 0.14.2 = host).
- **Driving it:** `/plugin load` replies after the load finishes (promise, `HyprCtl.cpp:1824-1839`), `/plugin unload` is synchronous (`:1840-1849`). `/output create headless NAME` / `/output remove NAME` (`HyprCtl.cpp:1760-1813`, remove only for user-created outputs). Cursor via `/dispatch hl.dsp.cursor.move({ x, y })` (`LuaBindingsDispatchers.cpp:77-85`) -> `warpTo` + `simulateMouseMovement` (`ConfigActions.cpp:1181-1185`), so it goes through our warp hook. `getFromSocket` returns "" when it can't connect (`hyprctlCompat.cpp:82-103`): the liveness check.
- **Config:** `/eval hl.config(...)` doesn't emit `config.reloaded` (`HyprCtl.cpp:1110-1123`, `lua/ConfigManager.cpp:880-...`), and the reload after every plugin load (`PluginSystem.cpp:135`) re-runs the config file and drops eval'd rules. So plugin settings and the test output's monitor rule live in `smoke.lua`, appended to `test.lua` (same directory, so its relative requires still resolve). test.lua disables unknown outputs (`hl.monitor({ output = "", disabled = true })`, `test.lua:28`), hence the explicit rule.
- **Build:** `make smoke` rebuilds the plugin with `DEV=1` (out/ now records its DEV mode, switching modes rebuilds everything), copies the test and config into the checkout, builds the `hyprtester` target, runs `hyprtailLifecycle` only, removes the copies. Not run by the assistant.

## Phase 0 spikes for SPEC §13 (cited at efb5099; libwayland 1.26.0)

Source analysis only: nothing was loaded or run. libwayland 1.26.0 (the host's `wayland-server.pc` version) is cloned read-only into `external/wayland` so the event-loop claims can be cited.

### S1: host-binary exports (run by the user)
- `nm -DC /usr/bin/Hyprland` shows all five exported (`T`): `Render::GL::CHyprOpenGLImpl::saveBufferForMirror(CBox const&)` at `0x9e2160`, `Monitor::CMonitor::needsACopyFB()`, `Pointer::CPointerManager::{currentCursorImage, getCursorBoxGlobal, getCurrentCursorTexture}()`.
- An objdump limited to `end()` printed nothing: `end()` isn't in the dynamic symbol table of the stripped binary, so objdump labels its code with the nearest exported symbol and the awk range never matched. A whole-binary search for calls to `0x9e2160` found exactly one: `0x9e3358`.
- **Interpretation:** source has exactly one caller, `end()` (`OpenGL.cpp:802`; the only other mention is a comment, `Renderer.cpp:1994`). One call site in source and one out-of-line call in the binary means that call is `end()`'s, wherever LTO placed that code. A hook patches the function entry, so it sees that call. Not excluded by this check: LTO emitting a second copy of `end()`'s code with the body inlined, which leaves no call to find. Covered by a runtime self-check in the design (SPEC §13.12). To see the enclosing code directly: `objdump -dC --no-show-raw-insn --start-address=0x9e3300 --stop-address=0x9e3360 /usr/bin/Hyprland`.
- The lean-ctx allowlist in `~/.config/lean-ctx/config.toml` still lacked `nm`/`objdump` when the assistant retried, so the checks were run by the user.

### S2: dynamic config keys
- **Registration after init works.**
  - No init-only restriction in the API (`PluginAPI.cpp:446-460`).
  - Lua: `registerPluginValue` just inserts into `m_configValues` (`lua/ConfigManager.cpp:1147-1164`).
  - Legacy: same path as static plugin values (`legacy/ConfigManager.cpp:2153-2183`).
  - Every parse first resets all values to defaults (`lua/ConfigManager.cpp:735-738`), so deleting a setting reverts it.
- **Registration is only undone at plugin unload** (`onPluginUnload`, `lua/ConfigManager.cpp:1325-1336`); there is no removal API.
- **Stale key within a session** (preset switched, or param removed from a shader, while the config still sets it):
  - The key stays registered, so it parses without a Hyprland error.
  - The plugin can still tell it was set: `Config::mgr()->getConfigValue(name).setByUser`, a virtual call (`ConfigManager.hpp:20-25, :54`). So it can warn itself.
- **Stale key after a Hyprland restart or plugin reload: problem.**
  - The key is never registered again, so every parse reports "unknown config key" (`LuaBindingsConfigRules.cpp:1001-1002`).
  - The error bar stays until the line is removed.
  - This applies to overrides for layers of a preset no longer selected, and to params removed from a shader.
  - Result: the same config line is silent in the session where it went stale and an error after the next restart.
- **Error bar during the extra reload.**
  - **Mechanism:** parse errors only queue the bar (`lua/ConfigManager.cpp:779-801`, `Overlay.cpp:84-92`). It is built on the overlay's next draw (`Overlay.cpp:180-185`). A clean reload before that draw calls `destroy()` on a bar not yet created, which just drops the queue (`lua/ConfigManager.cpp:807-808`, `Overlay.cpp:257-262`).
  - **Deferred reloads: no flash.** This covers the reload after a plugin load (`PluginSystem.cpp:135`) and our `reloadConfig()` (`PluginAPI.cpp:107-110`); both are `doLater` idle callbacks.
    - A `doLater` issued while an idle batch runs gets a fresh idle source (`EventLoopManager.cpp:215-238`).
    - libwayland drains idle sources until none are left before waiting on fds (`event-loop.c:965-975`, called at `:1009` and `:1064`). New ones are appended to the same list (`:792`).
    - Renders only happen from backend frame events, which are fds (`Monitor.cpp:122` then `MonitorFrameScheduler.cpp:47/105/122`). The loop is `wl_display_run` (`EventLoopManager.cpp:137`, `wayland-server.c:1735-1745`).
    - So no frame renders between the two reloads, and the bar is never drawn. This covers startup and every plugin load.
  - **Synchronous reloads: small window.** `hyprctl reload` runs the reload inside the socket's fd dispatch (`HyprCtl.cpp:1276`); inotify autoreload is also an fd (`EventLoopManager.cpp:127`).
    - Our follow-up reload runs in the idle drain after all fd events of that epoll batch (`event-loop.c:1064`).
    - A frame event later in the same batch renders with the bar created. It fades in from 0 (`Overlay.cpp:99-104`) and then out again.
    - Creating the bar reserves space for it and re-arranges layers (`Overlay.cpp:157, 160-178`), which can shift tiled windows for a frame or so.
    - Only reloads that introduce a param name not yet registered are affected. Not observed at runtime.
- **Possible fix for the stale-key problem: a persistent key registry.**
  - Every dynamic key registered is recorded, with type and range, in a state file (`$XDG_STATE_HOME/hyprtail/keys`) and registered again at plugin init, before the post-load reload.
  - Stale keys then stay known across restarts. The plugin warns when one is set but unused (`setByUser`), in the batched summary.
  - Startup needs no extra reload unless a key name is genuinely new.
  - Entries are pruned when neither declared nor set for some number of loads.
  - Cost: one more state file and pruning rules.

- **Decision (user): no dynamic keys.** Parameters go in one static `params` string (`"<layer>:<name>=<value> ..."`), type- and range-checked by the plugin against the param pragmas; unknown, stale or invalid entries are plugin warnings in the batched summary, never Hyprland config errors. Per-layer shader overrides are static keys indexed by layer number (`layer1_vertex`, `layer1_fragment` ... `layer4_*`). Every key is registered once at init: no state file, no extra reload. The registry idea above is dropped. SPEC §13.5, §13.7, §13.8.

### S3: render state inside `saveBufferForMirror`
- **When it's called:** only from `end()`, and only when `m_offloadedFramebuffer && needsACopyFB() && !m_fakeFrame` (`OpenGL.cpp:785`, `:801-806`).
- **In a `renderMonitor` render all three line up:**
  - The render starts with `begin(pMonitor, damage)` and no framebuffer (`GLRenderer.cpp:79`), which sets `m_fakeFrame = false` (`OpenGL.cpp:744`) and `m_offloadedFramebuffer = true` (`:753`).
  - The copy therefore happens exactly when `needsACopyFB()` is true. That can only change from the event loop, not mid-render.
  - So the plugin can decide at the cursor hook or `RENDER_LAST_MOMENT` whether to defer the draw to the hook, without risking a frame where neither runs.
- **State after the original returns:**
  - The mirror FB is bound through `bindTempFB`, whose guard rebinds the previous framebuffer on return (`Renderer.cpp:876-880`). The main framebuffer, holding the finished scene, is current again.
  - `pMonitor` is still set; it's reset later in `end()`.
  - Render damage is `finalDamage` (set in `end()`, `OpenGL.cpp:786`).
  - `finalDamage` already contains our boxes: the pass recomputes it from the render damage when it runs (`Pass.cpp:107-172`, called with the render damage at `GLRenderer.cpp:88`).
  - Blending is left on (`OpenGL.cpp:2556`).
  - `pushMonitorTransformEnabled(true)` only changes `getBoxProjection`'s default transform; we pass NORMAL explicitly.
- **Then** `end()` copies the main framebuffer to the output (`OpenGL.cpp:808-829`). A trail drawn in the hook after the original reaches the screen and is missing from the mirror copy.
- **Open for implementation:**
  - The viewport after the rebind (the framebuffer `bind()` wasn't read).
  - Scissor state left by `renderTexture`. Ours sets and clears its own scissor anyway.

## Phase 1: batched notifications and status command (cited at efb5099)

- **Batch** (`diag::beginBatch/endBatch`): opened at the start of `pluginInit`, in the `config.reloaded` handler and on a shader file change; extending an open batch re-arms its 2 s timeout (a `wl_event_loop` timer, removed in `diag::shutdown` since its callback lives in the .so). Reports during a batch go to the log and errors.log immediately and are collected; the end queues one summary through the existing deferred notification path, so ending inside a render is fine. Ended from the render lifecycle once no shader reload is pending (`CShaderSlot::hasPending`), idle slot only while the idle effect is enabled; otherwise by the timeout (e.g. idle enabled but the cursor hidden, or no monitor renders). The "loaded" notification stays separate.
- **Command:** `registerHyprCtlCommand` with `exact = true` (`PluginAPI.cpp:422-431`); exact names are matched first (`HyprCtl.cpp:2101-2109`), `-j` arrives as `FORMAT_JSON`, an empty reply would become "unknown request" (`:2123-2124`), so failures return a message. Unregistered in `teardown` (Hyprland also removes it on unload, `PluginSystem.cpp:172-176`). Snapshot built on the main thread (`src/Status.*` formats it); per-monitor counters live in `SMonitorFrame`, erased with the monitor on hotplug.
- **Smoke test:** checks both formats after load and "unknown request" after unload.

## Smoke test in CI (precedent read at main 4bb6844b; pin efb5099 identical)

- **Upstream precedent:** `.github/workflows/nix-test.yml` runs `nix build …#checks.x86_64-linux.tests -L` on plain `ubuntu-latest` and then reads `result/exit_status`; `nix/tests/default.nix` is a `pkgs.testers.runNixOSTest` machine with `programs.hyprland` (package `hyprland-with-tests`), alice autologin, `-vga none -device virtio-gpu-pci`, 4 cores/8 GB, `XDG_RUNTIME_DIR=/tmp`, running `hyprtester -b …/Hyprland -c /etc/test.lua -p …/hyprtestplugin.so` as alice. The script always copies the logs and `exit_status` out and lets the workflow decide, so a failing test still yields artifacts. `hyprwm/actions/nix-setup` (checked at its `main`) does no KVM/udev/disk setup.
- **Why not `make smoke` as is:** it needs a parent Wayland session for a GPU allocator (Smoke test environment below); the VM has virtio-gpu, so the DRM backend works.
- **hyprtester needs the test compiled in:** it globs `hyprtester/src/**/*.cpp` and is a subdirectory of the top-level CMake project (`CMakeLists.txt:707-711`), so `nix/smoke.nix` overrides `hyprland-with-tests` (`overrideAttrs`), copies `hyprtail_smoke.cpp` in, and builds only `generate-protocol-headers hyprtester` (the POST_BUILD step builds `hyprtestplugin.so`, `hyprtester/CMakeLists.txt:20-24`, `plugin/Makefile`). Rebuilding all of `hyprland-with-tests` would bust upstream's cache. Checked outside Nix in a fresh copy of main: configure with the Nix flags (`WITH_TESTS=ON`, no Xwayland/systemd) and that two-target build exit 0, producing `build/hyprtester/hyprtester` and `hyprtester/plugin/hyprtestplugin.so`. The Nix derivation itself is unevaluated (no `nix` here).
- **Lua config location:** `test.lua` does `require(config_dir() .. "/lua-require/absolute.lua")` (`hyprtester/test.lua:14-16`, `ConfigManager.cpp:547-548`), so the concatenated `test.lua` + `smoke.lua` is installed next to `lua-require/` and passed to `-c` by its store path. (Upstream's `/etc/test.lua` is a symlink into `$out/share/hypr`.)
- **Which Hyprland:** the flake's `hyprland` input, overridden per CI row (`--override-input hyprland github:hyprwm/Hyprland?ref=<row>`, plus `--no-write-lock-file`, and for the stable row while it is v0.56.2 a pinned `hyprland/nixpkgs`: `matrix.overrides`, docs/CI.md "Stable row: pinned nixpkgs"). The plugin under test is the flake's `hyprtail`, built against the non-tests `hyprland` of the same rev; the ABI hash is the commit plus dependency versions (`PluginAPI.hpp` `__hyprland_api_get_hash`/`_client_hash`), identical for both. `legacyPackages.<system>.smoke`, not `checks`, because `nix flake check` builds every check.
- **KVM:** a udev rule (`MODE=0666`) before Nix starts, and a hard failure if `/dev/kvm` is absent; a missing KVM would otherwise only show up as a VM too slow for the job timeout.
- **Unknowns until it has run:** the derivation on real Nix; KVM and RAM on the scratch repo's runner (VM defaults 4 GB/2 cores, `memorySize`/`cores` in `nix/smoke.nix`); the `stable` row (test compiled only against the pin and main); the loader change on main (`loadPlugin(path, pidType, requesterPid)`, `PluginSystem.cpp`) with `/plugin load` over IPC; the test's fixed sleeps under a VM.

## Smoke test environment (cited at efb5099; aquamarine 0.15.1, libwayland 1.26.0)

Two `make smoke` failures reported by the user, both caused by the first recipe's isolation. Diagnosed from source only; the fixed recipe hasn't been run by the assistant.
- **"socket path too long":**
  - **Why:** the instance directory is `$XDG_RUNTIME_DIR/hypr/<signature>` (`Compositor.cpp:192, 217`). The signature is `<40-char commit>_<unix time>_<random 0..INT32_MAX>`, up to 62 characters (`:206`). Socket2 refuses paths over 107 characters (`EventManager.cpp:21-24`), i.e. a runtime dir longer than 25 characters. The hyprctl socket is silently truncated by `snprintf` instead (`HyprCtl.cpp:2331-2333`), so hyprtester can't connect.
  - **Before:** `$(mktemp -d -t hyprtail-smoke.XXXXXX)/run` was 30 characters.
  - **Fix:** `mktemp -d /tmp/hts.XXXXXX` (15 characters) used directly as the runtime dir, with a length guard in the recipe.
- **"CBackend::create() failed!":**
  - **Where it comes from:** thrown when `m_aqBackend->start()` fails (`Compositor.cpp:334-340`). `CBackend::create` itself only fails on an empty list (`aquamarine Backend.cpp:72-73`).
  - **Backends:** at this pin Hyprland always asks for headless (mandatory), DRM (if available) and Wayland (fallback). `HYPRLAND_HEADLESS_ONLY` isn't read ("TODO: headless only", `Compositor.cpp:307-331`).
  - **Why it fails:** the headless backend has no DRM fd (`Headless.cpp:133-135`). `start()` needs an allocator from some backend's DRM fd and fails without one (`Backend.cpp:163-178`). DRM can't be taken inside a running session. The old recipe unset `WAYLAND_DISPLAY` and replaced `XDG_RUNTIME_DIR`, so the Wayland backend's `wl_display_connect(nullptr)` (`Wayland.cpp:100`) couldn't reach the session either.
  - **Fix:** pass the session socket as an absolute `WAYLAND_DISPLAY`, which libwayland accepts regardless of `XDG_RUNTIME_DIR` (`wayland-client.c:1164-1185`).
  - **Side effect:** the test Hyprland is a client of the session and creates one Wayland output (`Wayland.cpp:154`), disabled by test.lua's catch-all monitor rule. Upstream hyprtester behaves the same when run in a session.
- **Xwayland** is disabled in `smoke.lua`, so the test doesn't claim a display in the shared `/tmp/.X11-unix`.

## Phase 2: layers and contract 2 (cited at efb5099)

Built, not yet run in any compositor. SPEC §13.1-13.6 and §13.16 have the what; this section has the why and the gotchas.

- **Scope merged:** the restructure and the whole contract 2 (prelude, topology, params, padding expressions, varyings) landed together so custom shaders break once. A quad layer sizes its box from a param (`radius`), so it needed params and padding expressions anyway. Phase 3 keeps the config front end (`params` string, `layerN_*` keys) and the checks (`expects`, pre-link varying check).
- **Files:**
  - New: `src/Params.*` (Hyprland-free parsing), `src/Layer.*` (spec, classic preset, parameter resolution), `src/LayerPassElement.*` (`CNodeBuffer`, `SPreset`, `CLayerPassElement`).
  - Rewritten: `src/ShaderSource.*` (contract 2 pragmas, prelude injection), `src/ShaderSlot.*` (a program's contract comes from the prelude plus its own params), `src/main.cpp` (layer lifecycle), `src/Status.*`.
  - Deleted: `src/TrailPassElement.*`, `src/IdlePassElement.*`, `shaders/{trail,idle}.{vert,frag}`.
  - Added: `shaders/prelude/*.glsl`, `shaders/classic/{ribbon,ring}.{vert,frag}`.
- **Uniforms are set raw.** Contract 2 names (`ht_proj`, params) aren't core's `eShaderUniform` slots: `CShader::setUniformMatrix3fv` takes an `eShaderUniform` (`Shader.hpp:108`). They're set with `glUniform*` on the program's own locations, which `CShaderSlot::loc` looks up and caches. The program is still made current through `useShader`, so Hyprland's program cache stays correct.
- **Precision:** the prelude sets `precision highp float; precision highp int;` in both stages. Uniforms shared between stages (params declared in both, the built-ins) need matching precision in GLSL ES, and fragment shaders have no default float precision. This is why the contract pragma must come before any param pragma or include: the prelude has to precede the injected uniform declarations. GLES 3.x requires highp support in fragment shaders; the context is 3.2 or 3.0 (`OpenGL.cpp:199-220`).
- **Attributes:** 14 locations (GLES 3.0 guarantees at least 16). prev and next only carry position and bits: all four bindings with every field would need 20.
- **Behavior differences from phase 1, intended:**
  - An enabled quad layer is compiled at the first render, even while its effect can't show. The old idle slot only compiled when allowed. Compile errors now surface at load, and the report batch ends on the first render.
  - An exception in the lifecycle disables every layer, not just the trail.
  - A mouse-move exception no longer disables anything; it's only reported.
- **Bug found by the unit tests before it shipped:** in `shader::preprocess`, `const auto& displayName = st.out.sourceNames.front()` became dangling once the prelude's source name was pushed into the same vector. That was a use-after-free on every shader load. AddressSanitizer caught it in `make test-unit SANITIZE=1`; fixed by taking a copy.
- **Test harness:** `make test-unit` builds `tests/unit/unit.cpp` against `Params`, `ShaderSource` and `TrailBuffer` (and, since the crash-loop guard, `CrashGuard`) only, runs it (85 checks, fresh `make test-unit` run on 2026-09-30), and validates the four preprocessed built-ins with glslangValidator, linking every vertex/fragment pairing. `SANITIZE=1` adds ASan and UBSan; it passes clean. Running the binary through `make` is needed because the lean-ctx allowlist blocks running it directly, and `gdb` is blocked too.
- **Git index slip:** while deleting the old shaders I ran `git rm --cached` on them, which staged the deletions. I reverted it immediately with `git reset -q HEAD -- <files>`, so the deletions are unstaged working-tree changes only. Nothing was committed.
- **Stability risk (nested test warranted, per CLAUDE.md):** new GL resource handling. A new VBO layout with an integer attribute, a second VAO for quad layers, and per-layer draws with raw uniform setting. A mistake there crashes the compositor. The unit tests and glslang can't exercise the GL calls.

## Phase 3: checks and the config front end (built, untested)

- **`params` string precedence:** `CLayer` now holds two override maps,
  `m_overrides` (the phase-2 classic-preset/config mapping, unknown names
  silently ignored — it deliberately sets names a user shader may not
  declare) and `m_paramOverrides` (the `params` string, unknown names
  reported). `resolve()` applies `m_overrides` first, then
  `m_paramOverrides` on top, so `params` wins — this stands in for
  `preset.conf` in the precedence chain (§13.5) until phase 4 gives it a
  real one.
- **`layer1_vertex`/`layer4_fragment` naming constraint:** `IValue` stores
  its name as a raw `const char*` (`Config.cpp`'s own comment on
  `SRegistered`, "literals only") — building the eight key names with
  `std::format` would leave a dangling pointer once the temporary string is
  destroyed. Used two `static constexpr std::array<const char*, 4>` of
  literals instead, indexed in a loop.
- **`expects`/varying-check ordering, changed after review:** the pre-link
  varying check (§13.6) is a regex line-scan, not real GLSL parsing, so it
  can miss a declaration (multi-name `out`, `layout(...)`) or, in
  principle, misfire on something structured like a declaration inside a
  comment. Original plan ran it *before* the real compile/link attempt and
  used it to reject early; changed so the real `glLinkProgram` always runs
  first and unconditionally, and the scan is only consulted after an actual
  link failure, to replace the raw driver log with a plain message — never
  to block or skip the attempt itself. Removes any false-positive risk of
  rejecting a shader that would have linked fine.
- **Stability risk assessment:** no new GL resource handling or hooks
  (config parsing, pragma parsing, and a link-failure-only text scan that
  runs after the real GL calls, not instead of them), so no nested test per
  CLAUDE.md's rule — host test only (`hyprpm update && hyprpm reload -f`,
  then `hyprctl hyprtail`).

## Phase 4: presets and config surface v2 (built, untested)

- **Why full layer reconstruction on a preset switch is safe, not just
  convenient:** the original plan sketched a `CShaderSlot::rebind()` that
  mutates an existing slot's identity in place, specifically to avoid
  resizing `s_preset->layers` mid-render. Re-reading `LayerPassElement.cpp`
  showed that fear was unfounded: `CLayerPassElement` holds raw `CLayer*`
  pointers (`SLayerDraw`) collected fresh every render, *after*
  `prepareLayers()` runs, and a pass element never outlives the render it
  was queued for (Hyprland draws and discards it before the next one).
  Nothing holds a `CLayer*` across a render boundary. So `applyPendingState()`
  (called at the top of `prepareLayers()`, GL current) just releases the old
  layers' `CShaderSlot`s and replaces `s_preset->layers` outright with fresh
  `CLayer` objects from the new preset's spec — no rebind API needed at all,
  and no dangling-pointer risk. This is *why* the nested test the user
  approved (switching `preset` between `subtle`/`classic`, including right
  after startup and mid-idle-timer) is still worth running despite the
  simpler mechanism: constructing/destroying whole `CLayer`/`CShaderSlot`
  objects after init is still new relative to phase 3 (which only ever
  recompiled an *existing* slot's shader), even though the reasoning above
  says it should be safe.
- **`CLayer::setOverrides()` deleted, not just changed:** phase 3 gave
  `CLayer` a second override map (`m_paramOverrides`, for the `params`
  string) alongside the original `m_overrides`, which back then was fed by
  `main.cpp`'s hardcoded per-name config mapping (`layerOverrides()`). Now
  that presets are real, `m_overrides` is seeded once at construction from
  `SLayerSpec::defaults` (the preset manifest's own per-layer keys) and
  never needs re-seeding: a manifest edit or a different preset entirely
  goes through full `CLayer` reconstruction (see above), which already
  re-seeds it via the constructor. `setOverrides()` had no remaining
  caller, so it was removed rather than left as dead API. Its old
  silently-ignore-unknown behavior is gone too: an unknown name in a
  preset's own defaults now warns (`params:<layer>`), matching §13.7's "is
  an error" language, whereas before presets were real that silence was
  deliberate (the classic per-name mapping set names a swapped-in shader
  might not declare).
- **`std::string`'s constructor from `string_view` is `explicit`:**
  `Preset.cpp`'s manifest parser builds `key`/`value` as `string_view`s
  (via a local `trim()`), and assigning one straight to a
  `std::optional<std::string>` or a `std::map<std::string,std::string>`
  value (`raw.description = value;`, `entry[...] = value;`) does not
  compile — `operator=` needs an implicit conversion, and
  `basic_string`'s converting constructor from a `string_view`-like type
  is explicit (a well-known C++17/20 gotcha). Fixed by wrapping:
  `std::string{value}`. `emplace_back(piece)` on a `vector<string>` is
  unaffected since direct-init can call an explicit constructor.
- **Built-in preset shader names vs. a safe `CShaderSlot` identity:**
  `CShaderSlot`'s constructor takes a `vertBuiltin`/`fragBuiltin` name and
  its `builtin()` throws if `shader::builtin(name)` is empty (an assumed
  plugin bug today, since `classicPreset()` used to hardcode only known-good
  names). Once preset manifests can name *anything*, `Preset.cpp`'s
  `resolveStage()` never lets an unresolved name reach `SLayerSpec`: a
  recognized `shader::builtin()` name is used as-is; anything else, in a
  *user* preset only, becomes a path override (`vertPath`/`fragPath`) laid
  on top of an always-safe fallback identity (`classic/ribbon.vert`/`.frag`)
  — exactly the same split `layer1_vertex`/`layer4_fragment` config
  overrides already use. A built-in preset naming an unrecognized shader
  has no directory to resolve a path against, so it's a structural load
  error instead, caught before any `CLayer`/`CShaderSlot` exists.
- **`subtle`'s flat color is a shader-reuse compromise, not a new
  capability:** `classic/ribbon.frag` only declares `color_slow`/
  `color_fast` (a two-color speed gradient), no single flat `color` param
  (unlike `ring.frag`, which has one). Rather than write a new shader for
  `subtle`, its manifest pins both to the same neutral low-alpha value.
  Documented in SPEC §13.7 itself, not just here, per explicit request —
  it's a real, visible design choice (subtle's palette can't currently
  differ by speed even though the mechanism exists), not an implementation
  detail to bury in a comment.
- **`interpolate_warps` removed, now that phase 6 shipped its named
  replacement:** `warp = "break"|"line"|"curve"` (§13.10). It was kept past
  §13.8's literal removal list only until this replacement existed —
  removing it earlier would have deleted "connect the trail across warps"
  outright with nothing to take its place, a regression dressed up as a
  rename. See "Phase 6" below.
- **Not compiler-checked in this session:** `src/Preset.cpp` is new,
  Hyprland-header-free, and not wired into `make test-unit` (no new test
  cases were written — out of scope, this session is host-testing only per
  CLAUDE.md). Its first real compile happens on the host build
  (`hyprpm update`). Read it carefully before trusting it blindly on a
  first failure.

## Phase 4b: shader and preset layout, two namespaces (built, untested; cited at efb5099)

Supersedes the paths and names in the Phase 2 and Phase 4 notes above
(`shaders/classic/`, `shaders/hyprtail/`, `presets/<name>/preset.conf`,
`classic/ribbon.vert`, `hyprtail/...` includes). Nothing had been published,
so no compatibility shim.

- **Layout:** `shaders/{ribbon,ring}.{vert,frag}` (stock main shaders),
  `shaders/helpers/` (include library, was `hyprtail/`), `shaders/prelude/`
  unchanged and internal (not for editing). `presets/<name>.conf`, flat.
  `shader::builtin()` keys lost the `classic/` prefix (`ribbon.vert`).
- **One rule, applied three times:** a prefix means embedded and
  immutable, anything else is the user's own file.
  - preset name: `prefab:<name>` vs `<name>`;
  - a preset's shader stage: `prefab:ribbon.vert` vs `ribbon.vert`;
  - include: `"helpers/<name>"` vs a path.
  Why not shadow-with-fallback (what a same-named user directory did
  before): a user who copies `subtle.conf` and edits their shader copy would
  get the built-in silently if a bare name still meant "built-in first". The
  shipped presets therefore say `prefab:ribbon.vert`, and "edit it" is
  "swap the prefixed form for a bare one pointing at your copy". A bare
  preset name never resolves to a built-in; when the file is missing the
  error says which path was tried (and hints `prefab:<name>` if a built-in
  of that name exists). The failure still degrades to `prefab:subtle`,
  after the error, because the plugin must keep drawing.
- **Gotcha:** a user's own copy of a helper must be included as
  `"./helpers/<name>"`, since the bare `helpers/` prefix is always the
  embedded one (same as `hyprtail/` was).
- **Relative shader paths resolve against the hyprtail config root**
  (`cfg::hyprtailRoot()`, `$XDG_CONFIG_HOME/hypr/hyprtail`), both for a
  preset's `vertex`/`fragment` and for `layerN_vertex`/`layerN_fragment`.
  Previously a preset's paths were relative to its own directory, and the
  `layerN_*` ones to the directory of the main config file
  (`Config::mgr()->getMainConfigPath()`, Config.cpp before this change).
  Hyprland's own `decoration:screen_shader` still resolves against the main
  config path (`OpenGL.cpp:919`, `absolutePath(path,
  Config::mgr()->getMainConfigPath())` at efb5099): a deliberate divergence
  for one base inside hyprtail. Side effect: `hyprland -c <file>` no longer
  moves hyprtail's directory; `run_dev.sh` sets `XDG_CONFIG_HOME`, so the
  dev instance is unaffected.
- **Prefab presets can only use `prefab:` shaders** (no path), so an
  embedded manifest can never pick up a file from the user's directory.
- **Not covered by `make test-unit`:** `Preset.cpp` links Hyprland headers
  (`Diagnostics`, `Config`), so the unit build doesn't include it. The
  resolution logic is checked by building and by the host test.

## Phase 6: pointer features (built, untested)

- **`hasCursor()` is private, contra the original SPEC §13.9 citation.**
  It's declared in `CPointerManager`'s private section
  (`PointerManager.hpp`, grouped with `recheckPointerPosition` and other
  internals), not reachable from a plugin. Its body is trivial
  (`m_currentCursorImage.pBuffer || m_currentCursorImage.surface`,
  `PointerManager.cpp:116-118`) and both fields are public on the public
  `currentCursorImage()` accessor's return value, so the fix is inlining
  the same check rather than calling the private method — same result, no
  behavior difference from the draft's intent.
- **`cursorChanged` fires far more often than "the shape actually
  changed," caught in review before writing code.** Traced every
  `m_events.cursorChanged.emit()` site (`PointerManager.cpp:135, 153, 165,
  187, 201, 286`): a same-buffer/same-surface re-apply only emits if
  hotspot or scale changed (already a real filter), but a *new*
  buffer/surface object emits unconditionally even when its hotspot and
  size are numerically identical to the old one (common: many cursor-theme
  shapes share a box), and an animated client cursor surface emits on
  every frame commit regardless of whether the box moved at all. Fix:
  track the last-seen `(hotspot, cursorSizeLogical())` pair
  (`SPreset::lastCursorHotspot`/`lastCursorSizeLogical`) and only set
  `pendingBreak` when it actually differs, unconditionally kept up to date
  on every event (not just while `emit_from` is non-default) so toggling
  `emit_from` at runtime never compares against a stale pair.
- **Quad/idle anchoring needed no code change.** SPEC §13.4 says a quad
  layer anchors to the actual pointer, not the newest node; checked that
  `SPreset::lastPos` (the idle anchor, `ht_anchor`) is fed only by
  `noteMotion(Pointer::mgr()->position(), ...)` — the raw pointer, never
  the emit-adjusted insert point — at every call site (`sampleSource`,
  `onMouseMoveInternal`, `hkControllerWarpTo`). Already decoupled from
  `emit_offset` by construction; nothing to fix.
- **§13.4's per-layer `offset_from`/`offset` for quad layers, scoped out
  of this phase, confirmed with the user first.** It's a real SPEC §13.4
  design note but isn't in phase 6's own scope line (§13.16: "Emit offset
  ... `warp = curve`" only), and building it would mean extending the
  reserved-parameter system (`shader::reservedParams()`, numeric-only
  `params::eType`) with a value that's either a literal `"hotspot"` or a
  normalized vec2 — not a clean fit without adding a new param type for a
  single niche setting. Deferred; still proposal.
- **`hyprutils::Animation::CBezierCurve` considered and rejected for the
  warp curve.** Found via `external/hyprutils/tests/animation/Bezier.cpp`:
  it's an easing-curve solver (`getYForPoint(x)`, expects `x` roughly
  monotonic, four control points), built for animation timing curves, not
  a parametric 2D position curve. Wrong tool for interpolating a pointer
  path in space. Hand-rolled the quadratic Bezier directly instead (position
  + control-point-from-velocity, ~6 lines), matching `path smooth N`'s own
  precedent of a CPU-computed formula mirroring the shader's curve
  (§13.3), rather than pulling in an unrelated library.
- **Control point construction:** placed along the pre-warp node's
  velocity direction, at half the chord length — any positive distance
  along that ray gives the same tangent at the start (a quadratic Bezier's
  tangent at t=0 is the direction `P1 - P0`), so the exact distance only
  affects how much the curve bulges, not whether it's C1-continuous.
  Zero velocity (a fresh segment start) falls back to the chord's
  midpoint, which is exactly the straight-line case for a quadratic
  Bezier — no `if` needed to special-case it.
- **Reference validity across the insert loop:** `insertWarpCurve` binds
  `const auto& prev = p.ring.newest()` once before inserting the curve's
  points one at a time. Safe because `CTrailRing` is a fixed-capacity
  circular buffer (`TrailBuffer.hpp`) that only reallocates its backing
  vector in `resize()`, never in `insert()`, so `prev` keeps pointing at
  the same slot (the pre-warp node) through the whole loop even as new
  nodes are written elsewhere in the ring.

## Phase 7: screenshare exclude (built, untested; moved up ahead of phase 5)

- **The damage-safety question, traced and cited before writing the hook,
  not after:** the user asked directly whether the manual `.draw()` call
  needed to set `m_renderData.damage` itself, matching what
  `Pass.cpp:193-194` normally does for a queued element, or whether it
  would rely on stale/wrong state with no crash to reveal it. Traced the
  exact chain at the pin rather than trusting the phase-0 spike's own
  claim on faith: `GLRenderer.cpp:88` runs the pass with the render's own
  accumulated damage (already includes our layers' boxes, added earlier
  the same render via `damageInRender()`); `Pass.cpp:132/163/172` derives
  `finalDamage` from that, always a superset (blur-widened at most, never
  narrower); `GLRenderer.cpp:98` calls `end()` only after the pass ran;
  `OpenGL.cpp:786` sets `m_renderData.damage = finalDamage` there, before
  `saveBufferForMirror`. Conclusion: what's live when the hook fires is
  coarser than the tightest per-element region, but never narrower than
  our own contribution -- scissoring to it can't crop the trail, only
  waste a little rasterization outside geometry that isn't there anyway.
  No fix needed; the trace itself is now the citation, in both `main.cpp`
  (at `hkSaveBufferForMirror`) and SPEC §13.12, not just here.
- **Why calling `CLayerPassElement::draw()` directly, outside
  `m_renderPass`, is safe:** checked `PassElement.hpp` before relying on
  it. `IPassElement` is a bare abstract base -- virtual dispatch and two
  cached bools, no registration or side effect at construction. The pass
  system (`m_renderPass.add()`/`CRenderPass::render()`) is what manages a
  queued element's lifetime and draw order; the object itself doesn't
  care who calls `draw()` or when, as long as `g_pHyprRenderer->m_renderData`
  is in a state `drawLayer()` can read from -- which it is, per the
  damage trace above and the earlier S3 spike notes on framebuffer/blend
  state after `saveBufferForMirror` returns.
- **Per-monitor vs. global fallback state:** SPEC's own wording ("switches
  *that monitor* to the fallback") was taken literally --
  `captureHookUnavailable` lives on `SMonitorFrame`, not as one global
  flag. In practice a hook that misses at all almost certainly misses for
  every monitor (it's a build characteristic, not a monitor-specific
  one), but per-monitor state costs nothing extra (the map already
  exists) and matches the spec text exactly, including for a
  hypothetical future case where only some monitors take a different
  code path to `end()`.
- **Stability risk assessment:** yes, warranted, and scoped narrowly per
  the user's own request. Genuinely new relative to everything shipped so
  far: a hook target never hooked before (`CHyprOpenGLImpl::saveBufferForMirror`),
  and a real GL draw call (shader, blend, scissor, VAO) from a new call
  site *inside* the host's own `end()`, between two host GL calls that
  each explicitly re-establish their own state afterward (`bindFB`,
  `blend(false)`) rather than assuming anything was left alone -- which
  is what makes reusing `drawLayer()`'s existing cleanup (unbinds VAO,
  clears scissor) sufficient without new cleanup code. Nested test scope,
  per the user's explicit widening: not just "doesn't crash" but a real
  functional check -- set up a mirror in the nested instance
  (`needsACopyFB()`'s own trigger, no external capture tool needed),
  confirm the trail still renders correctly on the *source* monitor via
  the hook path, and confirm it's genuinely absent on the *mirror output*
  itself, viewed directly. Everything else (the `screenshare` setting,
  `checkEnum`) is CPU-only, host-test-only like phases 3, 4 and the rest
  of this one.

## Phase 5: instanced topology (built, untested; cited at efb5099)

`instanced K`, two demo presets, `helpers/noise.glsl`. `path smooth N` (also phase 5 in SPEC §13.16) is not built. Decided with the user before any code: K is a literal or an int param name, both bounded 1..64 (§13.3); two presets, not one; the visible-range draw is for `instanced` only (§13.3 says why); the nested test is the user's.

- **Nothing here depends on Hyprland behavior beyond what earlier phases already cite.** The new GL is plain GLES 3.0 (`glVertexAttribDivisor`, `glDrawArraysInstanced`, `glVertexAttribIPointer`), through the same `compat::bindArrayBuffer` and raw VAO calls the path VAO already uses (the array-buffer binding-cache reasoning is in the comment in `CNodeBuffer::ensure` and in "Plugin GL state hygiene"). The plugin creates and binds its own VAOs and unbinds to 0 afterwards, same as path and quad layers.
- **Second VAO, not a re-pointed path VAO.** The path VAO's 14 attribute bindings stay fixed for the life of the VBO; the instanced VAO owns locations 0-4 alone. Re-pointing one VAO between two layouts would mean re-enabling and disabling arrays on every layer switch, and a stacked preset (path, quad, instanced in one pass element) switches VAOs per layer anyway. Cost: one more VAO per preset instance, created and destroyed with the node buffer (`CNodeBuffer::ensure`/`destroy`, same failure path: `gpuFailed`).
- **Why re-point every draw, and what that means for the divisor.** GLES 3.0 has no `glDrawArraysInstancedBaseInstance`, and with divisor K the attribute for instance i is `base + (i / K) * stride`, so the only way to start at the first visible node is to move `base`. `CNodeBuffer::pointInstanced` therefore sets pointer and divisor (all five attributes) before every draw, with the instanced VAO and the VBO bound (a `glVertexAttribPointer` captures the array-buffer binding current at the call, hence the `bindArrayBuffer(m_vbo)` and the reset to 0 afterwards). `gl_InstanceID` is not divided by the attribute divisor, so `ht_instance() = gl_InstanceID % ht_K` needs no attribute. Integer attribute (`bits`) uses `glVertexAttribIPointer` with a divisor like the float ones. Every draw re-points once, not once per damage rect: the rects loop only re-issues the draw.
- **Node indexing.** The VBO is [front pad, n0 .. n(count-1), back pad] (unchanged). Ring node j is VBO node j + 1. With `visible` newest nodes the first is ring node `size() - visible`, so the byte offset is `(size() - visible + 1) * stride`. The last node read is `size()` (VBO node `count`), inside the buffer, whatever K is: the divisor makes instance i read node `first + i / K`, and i runs to `visible * K - 1`.
- **Visible count is computed from the ring, at draw time, with the pass element's `nowMs`** (`CTrailRing::visibleCount`), the instant `runLifecycle` used for the damage box, so the box and the draw agree. Both walk newest to oldest and stop at the first node with age >= fade, the same test `visibleBounds` uses, so the count and the bounds can't disagree about a node. `visibleBounds` gained `includeOlderNode` (default true, path unchanged): an instanced layer doesn't draw the segment to the next older node, so its box leaves that node out. Unit tests cover both variants, the segment-start case, a wrapped ring and a resized one.
- **Damage for instanced.** `main.cpp`'s `nodeLayer()` (topology != quad) picks the box; the motion-damage radius (`onMouseMoveInternal`) counts instanced layers' extent too, so a first pointer move damages enough for a wide spray before any box has been computed. `draw_when_cursor_hidden` was already true for everything but quad (`Layer.cpp`), so instanced needed nothing there.
- **K resolution.** `SInstanceCount {literal, param}` is parsed with the topology pragma; `instanceCountProblem()` (unit tested) validates a named K against the merged params in `programInfo`; `CLayer::resolve` reads the value into `res.instances` (clamped 1..64 as a last guard), so a `params` change re-resolves without a recompile (the resolve cache keys on the overrides version). The uniform `ht_K` and the divisor both take `res.instances` at draw. K = 1 is legal and just draws one quad per node.
- **The K cap (1..64) is a joint correctness and performance bound, not just a performance guard.** Everything in phase 5 was built and checked against that worst case, so raising or removing it (for example "allow any K with a performance disclaimer") is more than a change to the range validation in `ShaderSource.cpp`. Re-check together: (1) the worst-case draw, 64 x 4096 = 262144 instances in one `glDrawArraysInstanced` (`visible * copies` is cast to `GLsizei` in `LayerPassElement.cpp`), which is the largest draw the nested test exercises and the only size ever tried on a driver; (2) the visible-range draw, which exists because an instanced layer costs K instances per node, so drawing faded nodes multiplies the waste by K (SPEC §13.3), and its cost model assumes K is small; (3) padding: the shipped shaders' reach doesn't depend on K (each copy stays within a bound set by params), but a shader whose reach grows with K would need K in its padding expression, and the cap is what keeps that bounded, next to the separate 0..4096 px clamp in `CLayer::resolve`; (4) the guards that assume 1..64: the `int` range check in `instanceCountProblem()`, the clamp in `CLayer::resolve` (`res.instances`), and the `ht_K` uniform and attribute divisor set from it at draw. Damage correctness, draw size and GPU cost all have to be re-derived for a new bound; none of them is covered by the loader check alone.
- **Refusal wording.** `topologyText()` gives `instanced 8` or `instanced <param>`; the status line is now `topology instanced 8, vertex ...` (it was `<topology> topology, vertex ...`, which read badly with the K in it).
- **Prelude contract check.** The instanced program may use attribute locations 0-4 and the `ht_K` uniform; `preludeUniforms()` lists `ht_K` for every topology (a path shader that declares its own `ht_K` is refused as a reserved-prefix name anyway). A unit test scans the injected prelude for `layout(location = N) in` and compares with `preludeAttribLocations()` for path and instanced, so the two can't drift.
- **`ht_rand(float seed, uint salt)`, not `(uint seedBits, ...)`.** The node's `seed` reaches the shader as a float (`HtNode.seed`, 0..1); rebuilding the 31 bits from it would lose the low ones. `ht_rand` hashes `floatBitsToUint(seed)`, which is stable per node and distinct enough (24 mantissa bits). The prelude's `HtNode` is unchanged, so contract 2 is unchanged.
- **Padding disciplines in the shipped shaders.** `jitter.vert`: fixed offset <= `spread`, size <= `size`; padding `spread + size + 1`. `spray.vert`: heading from the node's seed, leaned toward `-vel/|vel|` (direction only, so reach doesn't depend on pointer speed), speed <= `speed` px/s, age capped at `fade_ms` in the shader, sideways wobble <= `wobble`; padding `speed * fade_ms / 1000 + wobble + size + 1`. `fade_ms` is allowed in padding because it is a reserved name (`programInfo`'s padding check accepts reserved names). If a user sets `fade_ms` through `params`, the padding follows on the next resolve. GPU-side and CPU-side `fade_ms` can differ by float rounding at the last frame of a node's life; the +1 in the padding and the cap cover it.
- **Manifest check in the unit tests.** `preset::parse` needs Hyprland headers (Diagnostics), so it isn't in the unit build. `testPresetManifests` re-reads `hyprtail/presets/*.conf` (was `presets/*.conf` before the `hyprtail/` move) with the same line rules and checks each layer's shaders are built-ins that preprocess, `expects` fits, K is valid, and every other key is a parameter of the paired program with a value of its type and range. That is the class of mistake that otherwise only shows up as a runtime warning (`params:<layer>`).
- **What unit tests and the build prove, and what they can't.** Proven here (all run): pragma grammar (K literal, param, bounds, `path 4`, missing K), K validation, `expects` with `instanced`, the prelude/contract locations, `visibleCount`/bounds, manifest values, glslang compile of every built-in and glslang link of all 12 vertex x fragment pairs, both Makefile modes and `test-compat`. Not provable without a GL context: that the VAO state, integer attribute re-pointing and the divisor behave on a real driver, and rendering.
- **Nested test (step 4b in `hyprtail_smoke.cpp`), prepared, not run by me** (see CLAUDE.md; the user runs it). State only: liveness after every step, `errors.log` clean, and the status naming the layers compiled and neither `DISABLED` nor `NODE BUFFER FAILED`. It needs the smoke config to change plugin settings mid-run. Mechanism, verified in source at `efb5099`: `smoke.lua` reads `$XDG_STATE_HOME/hyprtail-smoke-plugin.conf` (`os.getenv`/`io.open`: the Lua config state opens the standard libraries, `config/lua/ConfigManager.cpp:520`, and the reload's `package.loaded` clearing skips `io` and `os`, `:660`); the test rewrites that file and sends `/reload`, which reloads and runs the main config file again (`ConfigManager.cpp:634`, `luaL_loadfile` at `:682`) and emits `config.reloaded` (`:852`), the event the plugin's `applyConfig` listens to. A plugin loaded over IPC stays loaded across reloads (`PluginSystem.cpp:207-215` only unloads plugins with `m_loadedWithConfig`, set at `:235` for config-listed ones). `/reload` from hyprtester: `persistent.cpp:48` in the checkout. Stacked layers use a user preset written under a scratch `XDG_CONFIG_HOME` (`cfg::hyprtailRoot()`, `Config.cpp:230`), added to the Makefile's and `nix/smoke.nix`'s environment. Not verified by running: that a headless output renders during the test (the status assertions wait up to 3 s for the layers to compile, and fail with the status text if they never do), and the smoke Lua file (no Lua syntax checker was permitted here; the C++ was syntax-checked against the hyprtester headers).
- **Stability risk, stated per CLAUDE.md:** this phase is new GL resource handling (a VAO with a divisor above 1, an integer attribute re-pointed every draw, up to 64 x 4096 instances in a draw), so a nested run is warranted and is the user's. Scope: load each instanced preset; change K live across 1..64; resize `capacity` (2..4096) under a showing layer; unplug an output while one draws; stack path, quad and instanced; switch presets mid-run.
- **Left for the host (rendering, not checkable here):** what jitter and spray look like, damage correctness of spray (a stale edge would be a padding error), cost at high K and capacity, HDR colour (the dots' colours are `color` params and go through the usual conversion; nothing hardcoded).

## Per-app suppression (cited at efb5099)

A third suppress condition, alongside session lock and pointer constraint:
a dynamic window-rule effect, `hyprtail:no_trail`. It was first checked
against the focused window; it now follows the window under the pointer, see
"Rule follows the pointer, not focus" at the end of this section. The
decisions below that mention focus, no cache, or fading are superseded there.

- **Mechanism, confirmed via `nm -D` on the installed host binary and
  direct source reads:** `Desktop::Rule::windowEffects()->registerEffect()`
  / `unregisterEffect()` (`external/Hyprland/src/desktop/rule/effect/EffectContainer.hpp:28-49`,
  wrapped by `WindowRuleEffectContainer.hpp:1-83`) are exported dynamic
  symbols (`T Desktop::Rule::windowEffects()`, `W …IEffectContainer<eWindowRuleEffect>
  ::registerEffect/unregisterEffect`), resolved by the plugin `.so` at load
  time the same way `saveBufferForMirror` is. A window's current value for
  a registered dynamic effect lives in
  `window->m_ruleApplicator->m_otherProps.props` (a
  `storageType -> SCustomPropContainer{idx, propMask, effect}` map);
  `WindowRuleApplicator.hpp:63-72`'s own comment says "Plugins may read
  this." `Desktop::focusState()->window()` (also exported) gives the
  currently focused window.
- **Precedent:** hyprbars registers three of its own dynamic effects
  (`hyprbars:no_bar`, `:bar_color`, `:title_color`) exactly this way —
  `registerEffect()` once in `PLUGIN_INIT` (`hyprland-plugins/hyprbars/main.cpp:204-206`),
  read via `m_otherProps.props.at(idx)->effect` (`barDeco.cpp:641-646`),
  `unregisterEffect()` in `PLUGIN_EXIT` (`main.cpp:276-278`). Both native
  windowrule blocks and the Lua `hl.window_rule({...})`/exec-rule tables
  resolve an unrecognized key against `windowEffects()->get(key)` before
  erroring (`LuaBindingsConfigRules.cpp:1242-1258`,
  `LuaBindingsInternal.cpp:533-545`), so no plugin-side config parsing is
  needed for either config surface.
- **Design decision: no cache, no event listener.** The original proposal
  (research turn) cached a bool, refreshed on `window.active`/
  `window.updateRules` events (`EventBus.hpp:84,91`). Dropped: the
  per-render lookup (one `focusState()` call, one hashmap lookup) is cheap
  enough to just do fresh every render, and `sampleSource`'s existing
  `pendingBreak = true` (set every call while suppressed, not just on a
  transition) already produces the right behavior on both edges without
  any explicit transition tracking.
- **Design decision: fades like pointer constraint, not hard-clear like
  lock.** Considered making the app rule hard-gate the path layer's drawn
  box the way session lock does. Rejected: it would diverge the app rule
  from pointer constraint's existing (unchanged) behavior for no strong
  reason, and touches `runLifecycle`'s per-layer draw gate, a wider blast
  radius than necessary. Session lock stays the only hard draw-gate; SPEC
  §7 states this explicitly now.
- **Own value parser** (`params::ruleTruthy`, `Params.cpp`): deliberately
  not `params::parseValue(eType::BOOL, …)`, which is strict (errors on
  anything but `true`/`false`/`1`/`0`/`yes`/`no`) and belongs to the
  shader-param contract, a different surface with different failure
  semantics (a bad shader param is a config error; a bad/missing rule
  value should just mean "not suppressed," silently).

### Rule follows the pointer, not focus (cited at efb5099)

Symptom: with `input:follow_mouse` = 2 or 3 the focused window and the one
under the pointer differ, and the rule was applied to the focused one.

- **Old read site:** `appRuleSuppressed()` in `src/main.cpp` (was `:146-158`),
  `Desktop::focusState()->window()` at `:149`, reached through `suppressed()`
  from `sampleSource`, `onMouseMoveInternal`, `onIdleTimer` and the quad branch
  of `runLifecycle`. Focus-based, so the fix applied. The status also named the
  focused window (`statusSnapshot`, was `:1121-1124`).
- **Hit test used:** `Desktop::viewState()->hitTest().windowAt(Pointer::mgr()->position(),
  Desktop::View::RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING)`
  (`ViewHitTester.hpp:22`, `ViewState.hpp:17`, `ViewStateTracker.hpp:19`; flag
  enum `Window.hpp:63-74`). This answers the backlog item below: the helper
  is not on `Compositor`, it is `CViewHitTester` at the pin.
- **Why not core's exact flags:** the pointer-focus lookup is
  `RESERVED_EXTENTS | INPUT_EXTENTS | ALLOW_FLOATING | FOLLOW_MOUSE_CHECK`
  (`InputManager.cpp:487-488`). `FOLLOW_MOUSE_CHECK` shrinks the hitbox of every
  window except `focusState()->window()` by `input:follow_mouse_shrink`
  (`ViewHitTester.cpp:42-43`, `:61-62`, `:104-105`, `:233-234`), a hysteresis for
  switching focus. Keeping it would make "under the pointer" depend on focus
  again (and off by a few pixels at window edges). Every other
  pointer-location caller omits it (`InputManager.cpp:876`, `:946`, `:1008`;
  `KeybindManager.cpp:956`; `Monitor.cpp:1491`). Floating and z-order are the hit
  test's own: pinned floating first (`ViewHitTester.cpp:52-72`), floating above
  fullscreen (`:126-131`), a fullscreen window (`:139-150`), then tiled.
- **Where it runs:** `render.pre` (`Renderer.cpp:2095`, `EventBus.hpp:136`), per
  non-mirror monitor render. It sits before `beginRender` (`:2140`), which takes
  the damage ring (`:1782-1783`), so ring damage added there is part of that
  render, and before any pass element exists. Not `RENDER_BEGIN`: that is after
  `beginRender`, where ring damage only reaches the next frame.
- **Deviation from "once per frame, shared across monitors":** there is no
  frame shared by monitors; each renders on its own vblank. Re-evaluating at
  each monitor's `render.pre` is one `windowAt` call (a loop over the window
  list) per render, and `CPointerGate` makes the effects edge-triggered, so the
  buffer clear and the damage happen once. Consumers read the stored answer
  (`pointerExcluded()`), none runs its own hit test. The status command is the
  exception: it reports the window under the pointer fresh.
- **Hard gate, superseding "fades like pointer constraint":** the earlier
  decision rejected hard-gating to avoid touching `runLifecycle`. The request
  is that nothing is drawn over an excluded window, including what is already
  fading, so the excluded window joins session lock in `runLifecycle`'s one
  `hardGated` check. Trail and idle layers both pass through it, and
  `suppressed()` (shared by sampling, the idle timer and the warp hook) includes
  the gate. Pointer constraint is unchanged.
- **Enter / exit:** on enter the source is cleared, a break is queued and
  `repaintLastDrawn()` (the helper `onLayoutChanged` already needed, extracted
  and reused) ring-damages each layer's last drawn box on every monitor; the
  render's `damage.update` with an empty box then clears it once. On exit the
  source is cleared again and a break queued, so the first new node starts a
  segment. The state logic (`CPointerGate`, `PointerGate.hpp`) has no Hyprland
  includes and is unit-tested for the pointer and spring sources
  (`testPointerGate`): no churn without a change, buffer emptied on enter and
  on exit, first node after exit unconnected.
- **Motion damage while excluded:** `onMouseMoveInternal` used `suppressed()`
  as its early return. With the gate in `suppressed()` that would skip the
  damage that schedules the render whose `render.pre` sees the pointer leave,
  leaving the gate shut with hardware cursors. It now returns only for lock
  and constraint. Cost: a small damaged box per pointer motion over an excluded
  window, as for any window without the rule.
- **Status:** `focusedClass`/`focusedTitle` became `hoveredClass`/`hoveredTitle`
  (JSON and text), now naming the window under the pointer; `appRule` is the
  gate as of the last render. Nothing else in the repo read the old names.
- **Not extended: the smoke test.** hyprtester can spawn clients
  (`Tests::spawnKitty`, `hyprtester/src/tests/shared.hpp`) and the smoke test
  already warps the pointer with `hl.dsp.cursor.move`, but `appRule` only
  changes on a render, which a headless output is not known to do (the smoke
  assertions already carry that caveat), and it needs kitty in the nested
  environment. An assertion I cannot run, that may never fire, is worse than
  none. Left for the host, see below.
- **Stability risk, stated per CLAUDE.md:** low. One new event listener
  (removed in `teardown`), no new GL resources, no hooks. Ring damage from
  `render.pre` is the pattern `onMouseMoveInternal` already uses outside a
  render. A nested run is not needed; host test below.
- **Host test steps:** `hyprpm update`, `hyprpm reload -f`. Rule on a class,
  e.g. `hl.window_rule({ match = { class = "^(mpv)$" }, ["hyprtail:no_trail"] = true })`,
  with `input:follow_mouse = 2` (or 3).
  1. Open mpv and a terminal; click the terminal (it holds focus). Move the
     pointer over mpv: no trail, and none left over where it was drawn.
     `hyprctl hyprtail` shows `suppressed: yes (app rule)` and the mpv class under
     "window under pointer".
  2. Leave mpv onto the terminal: the trail returns and its first segment starts
     at the pointer, with no line back across mpv.
  3. Same with `follow_mouse = 0` and mpv focused, pointer over the terminal:
     trail shown.
  4. Fast sweep across mpv and out again: no ghost pixels at the entry point.
  5. Stationary pointer over the terminal; move mpv under it
     (`hyprctl dispatch` a move/float, or open mpv there): trail ends. Close
     mpv: trail works again on the next motion.
  6. Floating mpv over a tiled terminal, pointer on the overlap: excluded.
     Pinned/floating window above an excluded tiled one: not excluded.
  7. Two monitors, pointer entering mpv from the other monitor: nothing left
     on either.
  8. With an idle layer (`prefab:classic`): stay still over mpv, no marker;
     over the terminal, marker appears after its `start_ms`.
- **Known limitations:**
  - The hit test ignores layer surfaces. A layer surface above an excluded
    window (a panel, launcher or overlay) still reads as that window, where
    core's pointer focus (`InputManager.cpp:475-478`) would give the layer.
    Left as is.
  - An override-redirect X11 window (menu, tooltip) that does not take focus
    makes `windowAt` return `focusState()->window()` (`ViewHitTester.cpp:107-109`),
    so over such a popup the rule is read from the focused window.
  - The gate changes only when a monitor renders. With direct scanout active
    (`Renderer.cpp:2080-2092`) `render.pre` does not run and nothing is drawn by
    us anyway; the gate catches up on the next normal render.
  - Which window is "under" the pointer at a window border follows
    `general:resize_on_border` / `extend_border_grab_area` (`ViewHitTester.cpp:40`),
    as pointer focus does.

## CI: test rows through the flake, stable-row nixpkgs pin (cited at efb5099, v0.56.1, main 4bb6844b; 2026-09-30)

Source analysis plus read-only fetches of upstream files; no `nix` binary here, so none of the Nix side has been evaluated. The first CI run decides. What `docs/CI.md` states is the result; this is the why.

- **What failed on the first full run:** `hyprland-main` at `check-headers` ("Hyprland headers not found by pkg-config"); `hyprland-stable` and `hyprland-smoke-stable` inside the Nix build with "could not find git for clone of glaze". `hyprland-smoke-main` passed.
- **Stable: a dependency version conflict, not the override.** v0.56.2's `CMakeLists.txt:133` is `find_package(glaze 7...<8 QUIET)`; when that fails it falls back to `FetchContent` of glaze v7.2.0 (`:134-144`), and the Nix sandbox has no network (and `git` is only a `buildInput`, hence "could not find git"). The tag's own `flake.lock` pins nixpkgs `e72e4f29` (2026-08-04), where glaze is 8.0.0 (nixpkgs commit `6c89db18`, "glaze: 7.9.1 -> 8.0.0", 2026-08-03), so `glaze-hyprland` (`nix/overlays.nix`, nixpkgs glaze with SSL and interop off) never satisfies the bound. Upstream's own build of that tag would hit the same thing; I could not check upstream's CI result without GitHub. Main's bound is `find_package(glaze QUIET)` (`:131`) and its lock has glaze 8.4.0, so it resolves. The `test` job never passed an override at all, and the `smoke` job builds the override from the same `matrix.ref` for every row, so nothing about how stable's override is constructed differs from main's.
- **Why a nixpkgs pin and not the alternatives:** no release newer than v0.56.2 existed (2026-09-30), so there was nothing to move to. Patching the `<8` bound out of the CMake was rejected: v0.56.2 is not known to compile against glaze 8, and a build that succeeds against an unsupported dependency is worse than an honest failure. So stable gets an older nixpkgs, for that row only.
- **Which rev:** `61b7c44c4073f0b827768aff0049561b5110ea5a`, the nixpkgs in v0.56.1's `flake.lock` (nixos-unstable, 2026-07-18): glaze 7.9.0, `enableSSL`/`enableInterop` present, `gcc16Stdenv` and `lua5_5` present. v0.56.1's `nix/default.nix` and `nix/overlays.nix` are byte-identical to v0.56.2's and its `CMakeLists.txt` has the same glaze bound, so the packaging is the combination upstream already built, only a release apart. The alternative, nixpkgs just before the glaze bump (`12d28633`, glaze 7.9.1), would have been a rev nothing upstream ever built against. Only the nixpkgs node moves: aquamarine, hyprutils, hyprgraphics, hyprwire, hyprland-guiutils and xdph stay at v0.56.2's own locks (they differ from v0.56.1's), and all of them `follow` nixpkgs, so one override moves the whole closure. Whether those inputs build on the older nixpkgs is unverified.
- **How it is applied:** `resolve` computes `matrix.overrides` once per row: `--override-input hyprland github:hyprwm/Hyprland?ref=<ref> --no-write-lock-file` (the repo commits no `flake.lock`), plus `--override-input hyprland/nixpkgs github:NixOS/nixpkgs/<rev>` when the latest stable is exactly `v0.56.2`. The path is `hyprland/nixpkgs` because `flake.nix` has `nixpkgs.follows = "hyprland/nixpkgs"`. Keyed on the tag so the next release drops the pin by itself; the `release-branch` row is not pinned (a red there is information). Cost: nothing upstream built this combination, so the stable row has no Cachix hit and rebuilds Hyprland and its inputs on a cold cache.
- **Main: three separate reasons `check-headers` could not pass.** (1) `nix build …#hyprland-with-tests` installs the default `outputsToInstall` (nixpkgs `check-meta.nix`: `bin`, else `out`, plus `man`), never `dev`, so there is no `result-dev`. (2) `hyprland.pc` is installed under `share/pkgconfig` (`CMakeLists.txt:686-687` on main); the workflow used `lib/pkgconfig`. hyprpm itself uses `share/pkgconfig` (`PluginManager.cpp:419`, `:1157`). (3) `hyprland.pc` `Requires:` aquamarine, hyprcursor, hyprgraphics, hyprlang, hyprutils, libdrm, egl, cairo, xkbcommon, libinput and wayland-server, none on a bare runner's pkg-config path, and the runner's system GCC is older than the GCC 15 that `#embed` needs. The header-build step had not failed silently: `set -euo pipefail` was on and `check-headers` belongs to the next step.
- **The restructure:** everything goes through our flake with the row's overrides. `.#legacyPackages.<system>.hyprland` is the very `hyprland` derivation hyprtail is built against, built once per row; `.#hyprtail` is `make all` under `mkHyprlandPlugin` (hyprpm's default pkg-config mode); `devShells.ci` (`inputsFrom` the plugin, `hyprland.stdenv`, `pkg-config`, `glslang`) runs `make test-unit` and `make test-compat`. The nm check reads `bin/.Hyprland-wrapped`: `nix/default.nix` wraps `bin/Hyprland` with `wrapProgram`, so `bin/Hyprland` is a shell script with no symbols to read. `hyprland-unwrapped` (`wrapRuntimeDeps = false`, `nix/overlays.nix`) would have given a plain ELF but is a different derivation, hence a second full Hyprland build per row, which is the cost that matters most on the uncached stable row; `hyprland-with-tests` stays only in the smoke job, which needs its compiled-in test binary.
- **Side effects to expect:** `glslangValidator` is now on the CI path, so `make test-unit`'s GLSL checks actually run there (locally they skip when the tool is missing). Promotion counters for the smoke rows start at zero (docs/CI.md): earlier runs predate the pin and this restructure.
- **Smoke config drift found in the same pass:** `tests/hyprtester/smoke.lua` still set the contract-1 keys (`fade_ms`, `idle_enabled`, `idle_delay_ms`, `idle_duration_ms`), removed in phase 4, so the default `prefab:subtle` preset (no idle layer) ran and the idle effect was never exercised. It now selects `prefab:classic` and sets `params = "idle:enabled=true idle:start_ms=50 idle:duration_ms=200 trail:fade_ms=500"`.
- **Stability risk:** none for the plugin (workflow, flake and test config only), so no nested-instance test was warranted for the CI work itself.

## Spring-chain stage 1: the source interface (built, untested on host)

Interface only, no spring math. Reasoning:

- **Scope:** `ISource` covers producing the trail's point buffer and nothing
  else. It is not an animation interface.
- **`isSettled(nowMs, fadeMs)`:** "fully faded, nothing to draw" is decided
  per layer today (`visibleBounds` in `runLifecycle`, `visibleCount` in
  `drawLayer`), because `fade_ms` is a layer setting. A no-argument
  `isSettled()` would have had to guess a fade. The ring's version is
  `visibleCount(nowMs, fadeMs) == 0`.
- **`insert` keeps `segmentStart`:** breaks are part of the source contract
  (SPEC §7); `sampleSource` passes `pendingBreak`.
- **Extra reads:** `generation()`, `empty()`, `newestBirthMs()` are what the
  upload gate and its reference time read, so the gate can take an `ISource`.
- **Gate:** `sourceNeedsUpload` is `!empty && (generation != uploaded ||
  needsContinuousUpload)`. For the ring the flag is constant false, so it
  equals the old `!(empty || generation == uploaded)`; `testSource` checks the
  truth table across insert, resize and clear. No GL call was added, moved or
  removed, so host testing only.
- **Not done in stage 1:** `SPreset` still held a concrete `CTrailRing`,
  `tick` was not called anywhere, and `m_refMs`/`ht_nowMs` still rebased on
  the newest node's birth time. Stage 2 (below) did all three.

## Spring-chain stage 2: the spring source (built, untested on host)

`CSpringChainSource` (`src/SpringChain.*`), the `source` preset key and
`source:<name>` settings (`src/Source.*`, `Preset.*`), `prefab:spring`. SPEC
§13.1 and §13.7 have the design; this records why.

- **Reframing checked against the solver, not assumed.** `advanceSpring`
  solves around a fixed rest of 1.0 (`displacement = value - 1`,
  `external/hyprutils/src/animation/Spring.cpp:18`), time in seconds, closed
  form in all three damping regimes (so any `dt` is stable), `dt <= 0` is a
  no-op (`:10-12`), mass/stiffness floored at 1e-4 and damping at 0
  (`:14-16`). So `value = 1 + (pos - target)` in and `pos = target + (value -
  1)` out is exact, done fresh each frame against that frame's target, and the
  velocity carried between frames is the point's own (the target is constant
  over a step). State is px/s; the node's `velocity` is px/ms, divided by 1000
  on the way out. `testSpringChain` checks the critically damped closed form
  (`d0 (1 + wt) e^-wt`) independently of the reframing. hyprutils v0.14.2 is
  both the checkout and the host (`/usr/lib/pkgconfig/hyprutils.pc`); the
  installed `Spring.hpp`/`AnimationManager.hpp` are identical to the
  checkout's and `libhyprutils.so` exports `advanceSpring`, which Hyprland's
  own process already has loaded, so the plugin resolves it at load like any
  other host symbol (`check-imports.sh` counts every library Hyprland links).
  The host library's own source isn't readable here; the implementation is
  read from the checkout.
- **Settling rule is core's** (`AnimatedVariable.cpp:129-137`: within
  epsilon of 1 and of rest, then snap), at pixel scale: 0.05 px, 2 px/s. The
  snap makes settling exact, so `needsContinuousUpload()` really turns false.
- **Why `needsContinuousUpload()` is "true while unsettled", not always
  true.** `drawLayer` uploads per layer and per monitor, so a flag that is
  always true re-uploads an unchanged buffer on every draw through the fade.
  Settled, nothing changes, so the generation gate is enough *if* the tick
  that settles the last point bumps the generation, which it does (any
  position or velocity change). `testSpringChain` runs the renderer's gate
  against the chain until it settles and compares what was "uploaded" with
  the final state.
- **`m_refMs` with uploads every frame:** still correct, unchanged. Each
  upload rebases `birthMs` on the newest node's birth and stores the same
  value as `m_refMs`, and every draw uses `ht_nowMs = m_nowMs - m_refMs`
  with `m_nowMs` the lifecycle time `tick` ran at. The head's birth is about
  now while moving, so the floats are small. Two draws in one frame (layers,
  monitors) re-upload the same state. The things that *would* go stale are a
  replaced source (generations are per object, fixed by `invalidate()`) and
  the last frame of motion (fixed by the settling tick bumping the
  generation).
- **Ages:** a chain has no real births, but the shaders fade and taper by
  age and `visibleCount` needs births that don't increase toward the tail.
  Point k is born at `activeMs - k * age_step_ms`; `activeMs` is the last
  unsettled tick or insert. Consequence worth knowing: with a layer's
  `fade_ms` F, only `F / age_step_ms` points are visible at once.
- **Break re-seeds the chain** instead of fading an old trail (there is no
  coherent severed piece of a rope).
- **`dt`:** the caller passes 0 on the first tick after the source was at
  rest (the gap is idle time) and the source clamps to 33 ms (without it the
  first motion after a stall would teleport the chain to the pointer). Two
  monitors rendering within the same ms give `dt` ~ 0, harmless.
- **Damage and scheduling:** a layer's box is computed only while
  `!isSettled(now, fadeMs)`, which for the ring is the same as
  `visibleBounds` being empty. An unsettled chain therefore damages every
  frame (the existing addDamage feedback, NOTES "Damage/render feedback is
  vblank-paced") and a settled, faded one goes idle. The one case this
  doesn't cover: a chain still moving while everything has faded (only
  `fade_ms` 0), which has nothing to show.
- **Not layer params:** all layers share one source, so `mass` etc. as layer
  parameters would be ambiguous. They are `source:<name>`, using the same
  parameter types, ranges, precedence and `params` string.
- **`ISource` grew** (`size`, `capacity`, `resize`, `clear`, `newest`,
  `visibleCount`, `visibleBounds`, `kind`, `configure`): the damage box, draw
  count, spacing gate, warp curve and status all read the source, so they
  read it through the interface. `insertWarpCurve` copies `newest()` now,
  since the chain's changes on insert.
- **Stability risk, and the test it needs (the user's, not run here):**
  continuous upload runs for real for the first time, and the source can
  change at runtime. Nested instance, scoped: load `prefab:spring`; move and
  stop, confirm `hyprctl hyprtail` shows `moving` false and renders stop
  (no more frames once faded); stack it with other layers (a user preset with
  more layers over `source = spring`); switch to a pointer preset and back
  mid-motion, and from a spring preset to another spring preset (state
  kept); change `capacity` while it is active (both directions);
  `params = "source:damping=..."` live; a lock/unlock and a workspace
  switch (re-seed).

## Preset rework slice 1: the shader kit (built, untested on host; SPEC 13.7.1)

- **No time-based palette from `ht_nowMs`.** `ht_nowMs` is uploaded as
  `nowMs - preset.gpu.refMs()` (`LayerPassElement.cpp:289`), and the buffer
  is rebased on the newest node at each upload (`LayerPassElement.cpp:127`),
  so the value jumps back whenever a point is added. A color cycle on it
  would stutter while the pointer moves. `color_by = 4` (cycle) runs on each
  point's age plus its seed instead, which is stable per point. `dots.frag`'s
  twinkle does the same. Quad looks (`pulse`, `sizzle`) use `ht_stillMs`,
  which is continuous while the pointer rests, the only time they show.
- **Calligraphy nib width per joint.** `ribbon.vert` computes the half-width
  at a joint from the mean direction of the two segments meeting there, so
  both segments sharing the joint agree and the strip doesn't step. `nib`
  and `taper` only narrow the ribbon, so the padding is unchanged.
- **`drift.vert` gravity padding.** `gravity_dir` is normalized in the shader
  (zero turns gravity off), so `gravity` alone bounds the pull:
  `gravity * fade_ms^2 / 2000000` px on top of the old speed term, with age
  capped at `fade_ms` as before. A vec2 param can't appear in a padding
  expression (`params::scalar`, `Params.cpp:223-226`, returns none for it),
  hence magnitude and direction as two params.
- **`sizzle.frag` stays inside its square.** Spark points reach at most
  `sqrt(0.8^2 + 0.12^2) * radius`, about `0.81 * radius`; anything past the
  quad (glow, thick sparks on a small radius) is clipped by the quad, which is
  exactly the damaged box, so it can't leave ghosts. Its burst decisions
  depend only on uniforms, so the early discards are uniform control flow and
  `ht_coverage`'s `fwidth` afterwards is well defined.
- **Smoke test params.** `HYPRTAIL_HEALTHY` fails on any warning, and
  `prefab:subtle`'s layer is now `thread`, so the preset-switch loops pass
  `thread:fade_ms` for subtle instead of `trail:fade_ms`.
- **Codrops "stylised mouse trails" (OGL):** used as inspiration only (eased
  point chain drawn as a thick ribbon; our spring source is the chain). No
  code taken. The article page wasn't readable from here (HTTP 403); OGL
  itself is Unlicense (github.com/oframe/ogl). The demo's own license wasn't
  found.

## Setting rename `preset` -> `trail`, and the `hyprtail/` directory (built; compiled and unit-tested only)

Two structural changes, done together before any release, so no deprecation
alias and no compatibility shim.

- **`preset` setting renamed `trail`** (`plugin:hyprtail:trail`,
  `SValues::trail`, `hyprctl hyprtail` field `trail`, text and JSON). The
  value was resolved as follows (`preset::load`; superseded by the next
  section, which replaces the bare name with a `.conf` path):
  `prefab:<name>` is an embedded manifest, a bare `<name>` (no `/`,
  `Preset.cpp:279`) is `<hyprtail root>/presets/<name>.conf`
  (`Preset.cpp:285`). Strings that name the setting changed (the
  load-failure report is now `trail "<name>": ...`, diag key `trail:<name>`;
  the smoke test's state file and `smoke.lua` use `trail=`). Left alone, the
  word `preset` still names the concept (a manifest file) or a symbol:
  namespace `hyprtail::preset`, `Preset.{hpp,cpp}`, `SPreset`,
  `SResolved`, `pendingPreset`/`activePreset`, `s_preset`,
  `presetReleaseGpu`, the `presets/` directory, `PRESET_FILES` in the
  Makefile, `testPresetManifests`, the "prefab preset" wording in errors,
  and the `idle`/`trail` layer names (a layer called `trail` is unrelated to
  the setting).
- **`presets/` and `shaders/` moved under `hyprtail/`** (`git mv`, history
  follows). The repository's `hyprtail/` is the content of the config root
  (`cfg::hyprtailRoot()`, `Config.cpp:240-248`), so installing is copying
  that one directory into `~/.config/hypr/`. Updated: the Makefile
  (`SHADER_FILES`, `PRESET_FILES`, both feed the object dependency rule),
  every `#embed` (`"../hyprtail/shaders/..."` in `ShaderSource.cpp`,
  `"../hyprtail/presets/..."` in `Preset.cpp`), and `testPresetManifests`
  (reads `hyprtail/presets` from the repo root). Not changed, because they
  never named the old paths: `.gitignore`, `hyprpm.toml`, `run_dev.sh`,
  `check-pin`, the flake and CI files.
- **No doubled path.** The request worried about `hyprtail/shaders/hyprtail/`.
  There is none: no shipped shader contains `#include "hyprtail/..."` (Phase
  4b renamed that library to `helpers/`), and `helpers/` includes never touch
  the disk: `resolveInclude` (`ShaderSource.cpp:232-237`) looks the name up
  in the embedded `prefabs()` table. The move therefore cannot change how any
  include resolves. A user file's relative include is still resolved against
  the including file (`ShaderSource.cpp:242-244`).
- **Relative paths** still resolve against the hyprtail config root, not
  the main Hyprland config directory (`resolveShaderPath`,
  `Config.cpp:223-238`; SPEC 13.7). Unchanged.
- **Embedded fallback unchanged:** a run with no files under the config root
  loads `prefab:subtle` from the embedded copy.
- **Checked:** `make all` and `make DEV=1 all` build; both `.so` files hold
  `plugin:hyprtail:trail` (no `plugin:hyprtail:preset`), the embedded
  manifests and the shader helpers; `make test-unit` passes (307 checks plus
  the glslangValidator pass). Nothing was loaded into any Hyprland instance.

## Trail paths and shader paths (built; compiled and unit-tested only, loader not run)

Follows the rename section above, before any release: no alias.

- **Shipped presets name shaders by path.** Every `<layer>:vertex`/`fragment`
  in `hyprtail/presets/*.conf` (27 references in 9 files) went from
  `prefab:<name>` to the path relative to the hyprtail root,
  `shaders/<name>`. A purely mechanical rewrite; the only other text
  changes are two comments: the `subtle.conf` header (how to copy and
  select it) and one sentence in `vivid.conf`. The manifest grammar already
  expressed this (`parse`, `Preset.cpp:47-146`: a value is everything after
  the first `=` up to `#`), so no syntax change was needed.
- **One resolver.** Disk-mode shader paths and the `trail` value both go
  through `cfg::resolveShaderPath` (`Config.cpp:223`), the function
  `layerN_vertex`/`layerN_fragment` already used. Not renamed (it now also
  resolves a `.conf` path); its comment in `Config.hpp` says so.
- **Embedded table keyed by the same string.** `shader::builtin()` keys
  were `ribbon.vert`; they are now `shaders/ribbon.vert`
  (`ShaderSource.cpp:403-410`), so a prefab preset's stage value is looked
  up as written. The slot's safe identity (`SLayerSpec::vertBuiltin`,
  `Preset.cpp:260,263`, the hardcoded fallback) uses the new keys, and
  built-in shaders now show as `<shaders/ribbon.vert>` in messages. The
  `helpers/` table (`prefabs()`, `ShaderSource.cpp:83-90`) is keyed
  relative to `shaders/` and was left alone as asked, so the two embedded
  tables use different bases. A wart, not a bug.
- **Embedded presets never read the disk** (`Preset.cpp:241-244`): a stage
  not in the table is an error naming the expected form. File presets
  resolve on disk (`Preset.cpp:247-255`).
- **`prefab:<name>` as a shader stage is kept** as shorthand for the embedded
  `shaders/<name>` (`Preset.cpp:231-236`), for file presets that want to pin
  an embedded shader. It also keeps the smoke test's scratch presets
  working with no `shaders/` folder on disk. Removing it later is one
  branch in `resolveStage`.
- **Missing shader file in a file preset: warning, keep the current
  trail.** The existing slot-level path already reports a missing file as a
  `WARN` (`shader:<layer>`) and keeps the current *shader*, but on a preset
  switch the slot is a fresh object with no program, so "keeping" meant
  drawing the built-in ribbon. To keep the previous *trail*, `resolveStage`
  collects missing files (`Preset.cpp:253-254`) and `load()` returns nothing
  for them when a trail is showing (`Preset.cpp:354-376`); `applyConfig`
  then drops the queued switch (`main.cpp:1089-1092`). Same diag path, so
  the orange notification and `errors.log` as before. Other load failures
  (no such preset file, parse error, bad bare name) are unchanged: `ERR`
  and fallback to `prefab:subtle`. `activePreset.layers.empty()` is the
  "nothing showing yet" test (it is empty until the first
  `prepareLayers()`, `LayerPassElement.hpp:97`). Also: a preset kept this
  way isn't re-checked until the next config reload, and the missing file
  isn't watched (the active preset's files are).
- **`trail` value forms.** `prefab:<name>`, or a path ending `.conf`
  (`Preset.cpp:306`). Anything else, bare names included, gets `notATrail`
  (`Preset.cpp:281`): `"subtle" isn't a trail: write "prefab:subtle" for the
  built-in, or "presets/subtle.conf" for your own file ...`. No way to name
  a bare shader file in `trail`; a custom shader goes in a preset.
- **Tests.** `testPresetManifests` now requires path-form stages and checks,
  per stage of each shipped preset, both modes: the string is a key of
  `shader::builtin()` (embedded) and `hyprtail/<string>` exists with
  byte-identical contents (disk). Checked it fails when a disk shader is
  edited. The loader itself (`Preset.cpp`) needs Hyprland headers, so it is
  not in the unit build; a scratch harness that links the real
  `preset::load` (stubbing `diag::report`) was written but the shell
  allowlist refused to run it, so the loader's own branches (`notATrail`,
  the missing-file warning, `load()`'s `nullopt`) have only been compiled.
- **Helpers on disk: no effect.** `#include "helpers/..."` is looked up in
  the embedded table before any path logic (`ShaderSource.cpp:232-237`),
  whether the including shader is embedded or a file. `SSource::files`, the
  list that feeds the file watcher, is only filled for units with a path
  (`ShaderSource.cpp:264-265`, `ShaderSlot.cpp` `reload`), so an edit under
  `hyprtail/shaders/helpers/` triggers nothing and changes nothing; the
  copy is reference material. Only `#include "./helpers/<name>"` (or any
  path not starting with the bare `helpers/`) reads the disk
  (`ShaderSource.cpp:239-255`), and `prelude/` is never a file.

## Shader contract audit and the vert/frag link matrix (report only; compiled and unit-tested, nothing run in a compositor)

Read at the working tree on top of `18b3708`. Nothing here was loaded into
any Hyprland instance, and no rendering was looked at.

### The link matrix

- **Test:** `testShaderLinkMatrix` (`tests/unit/unit.cpp:1141`). It globs
  `hyprtail/shaders/*.vert` and `*.frag` (`globShaders`, `:1102`, run from the
  repo root like `testPresetManifests`), so a new layer joins the matrix by
  existing. Each shader is assembled by `shader::preprocess(shader::builtin(name),
  name, {}, stage)`, the call `CShaderSlot::builtin()` makes for the built-in
  slots (`ShaderSlot.cpp:253-261`), so prelude, topology variant (path, quad,
  instanced: `ShaderSource.cpp:139-151`) and helper includes are the loader's
  own. Each vert x frag pair then goes to `glslangValidator -l <vert> <frag>`
  (`runCapture`, `:1054`); the dialect is the shader's own `#version 300 es`,
  no extra flags, exactly what the single-file pass in the Makefile uses. A
  failing pair prints its name and the validator's log with `<id>:<line>`
  mapped back to file:line through `shader::mapLog`; a glob hit that is not
  in `shader::builtin()` also fails (it could not be a prefab). No skip list.
  The shaders are left in `$OUT_DIR` for the Makefile's single-file pass.
- **Result: 4 x 4 = 16 of 16 pairs link.** Shown to report properly by
  temporarily breaking `dots.frag` (4 pairs failed, each with `dots.frag:39`
  and the syntax error); restored.
- **Not reachable from a unit test:** the loader's `glslCheck`/`varyingCheck`
  and `programInfo` (`ShaderSlot.cpp:33-247`) are in `ShaderSlot.cpp`, which
  includes Hyprland's GL headers and needs a current GL context. The test
  uses the closest GL-free thing: `preprocess` + glslang. It does not run the
  driver link, `contractCheck` (`ShaderSlot.cpp:33-73`), or `programInfo`'s
  `expects` / param-consistency checks.
- **Moved, not added:** the old pair loop in the Makefile (hard-coded list in
  `main()`, loop in `test-unit`) is gone; the unit binary owns it now.
- **The matrix is blind to the failure class the brief names.** A control
  pair (`canary.vert` writes nothing custom, `canary.frag` declares `in float
  ht_notWritten`) links under `glslangValidator -l`; the test prints a note
  each run rather than asserting. So a fragment reading a varying no vertex
  shader declares would pass this matrix and fail in the driver. The loader
  has a text check for it (`varyingCheck`, `ShaderSlot.cpp:83-119`) but it is
  anonymous-namespace code in the GL file and only runs after a real link
  failure. Hoisting it into `ShaderSource.cpp` (GL-free) would let the matrix
  call the loader's own check; not done, it touches loader code.
- **The matrix passing says little about pairing.** `expects` is the real
  gate: by the pragmas (`gradient.frag:3`, `dots.frag:3`, `pulse.frag:3`,
  `sizzle.frag:3`) only 6 of the 16 pairs are accepted by `expectsMismatch`
  (`ShaderSource.cpp:413-422`): ribbon+gradient, halo+dots, scatter+dots,
  drift+dots, halo+pulse, halo+sizzle. Counted from the pragmas, not run.
  Pairs such as halo.vert + gradient.frag link, and would draw nothing
  (halo writes no `ht_vLife`, so gradient's alpha is 0) if `expects` did not
  refuse them.

### Varyings

Declared once per stage by the prelude: out in `prelude/vertex.glsl:4-10`,
in in `prelude/fragment.glsl:3-8`; `ht_initVaryings()` zeroes all six
(`vertex.glsl:12-19`) and every built-in vertex shader calls it first
(`ribbon.vert:63`, `scatter.vert:35`, `drift.vert:41`, `halo.vert:21`). The
loader's list of them is also in `varyingCheck`'s message
(`ShaderSlot.cpp:101`).

| Varying | Type, range, unit | ribbon.vert | scatter.vert | drift.vert | halo.vert |
|---|---|---|---|---|---|
| `ht_vLocal` | vec2; path x 0 newer end..1 older end, y -1..1 across; else quad -1..1 | `:99` | `:55` (corner) | `:78` (corner) | `:24` (corner) |
| `ht_vAge` | float, ms | `:100` node age | `:56` node age | `:79` node age | `:25` **`ht_stillMs`**, not a node age |
| `ht_vLife` | float, 1 to 0 over fade_ms | `:101` | `:57` | `:80` | not written (0) |
| `ht_vSpeed` | float, px/ms | `:102` `length(vel)` at birth | `:58` same | `:81` the particle's own `v*0.001` | not written (0) |
| `ht_vDist` | float, px from the segment start | `:103` | `:59` | `:82` | not written (0) |
| `ht_vSeed` | float, 0..1 | `:104` per node | `:60` per **copy** | `:83` per **copy** | not written (0) |

Fragment reads (all inside the six): `gradient.frag` vLocal.y `:30-31`, the
other five through `ht_paletteT` `:36` (`helpers/palette.glsl:38-48`) and
vLife `:39`; `dots.frag` vLocal `:40`, vAge+vSeed `:44`, palette `:45`, vLife
`:48`; `pulse.frag` vLocal `:28`; `sizzle.frag` vLocal `:61`. Nothing reads a
custom varying, and none exists.

### What shaders get today

- **Uniforms:** `ht_proj` (mat3), `ht_nowMs`, `ht_stillMs`, `ht_anchor`
  (vec2), `ht_extentPx`, `fade_ms`, `start_ms`, `duration_ms`
  (`prelude/common.glsl:10-20`); `ht_K` (int, instanced,
  `prelude/instanced.glsl:13`). Both stages see all of them (`common.glsl` is
  in the fragment prelude too, `ShaderSource.cpp:139-142`). Set per layer at
  `LayerPassElement.cpp:288-297`. Declared params become `uniform float|int|
  bool|vec2|vec4` (`ShaderSource.cpp:333`, set at `LayerPassElement.cpp:
  299-312`). The allowed set is `preludeUniforms()` (`ShaderSource.cpp:
  441-444`); any other active uniform fails the program (`ShaderSlot.cpp:
  47-48`).
- **Attributes (vertex only):** path, 14 at locations 0-13: prev (pos, bits),
  p0 and p1 (pos, birth, vel, dist, bits), next (pos, bits)
  (`prelude/path.glsl:6-19`); instanced, 5 at 0-4 (`instanced.glsl:7-11`);
  quad, none (`ShaderSource.cpp:446-456`). Plus `gl_VertexID`,
  `gl_InstanceID`. Shaders do not touch them directly, only through
  `ht_prev()` etc.
- **Buffers:** one VBO of 28-byte `SGpuNode` per preset (`TrailBuffer.hpp:
  39-52`), `capacity + 2` nodes (front and back pad, `LayerPassElement.cpp:
  45`), `glBufferSubData` on a generation change (`:123-141`). Path binds the
  same VBO four times, shifted one node each, divisor 1 (`:82-92`);
  instanced re-points five attributes at the first visible node with divisor
  K (`:108-120`). **No uniform array, UBO, SSBO, texture or sampler exists**
  (grep for `glTexImage`, `glGenTextures`, `UniformBlock`, `glBindBufferBase`,
  `sampler2D`, `texelFetch` in `src/` and `hyprtail/`: no hits). Node data
  reaches the vertex stage only, as per-instance attributes. The fragment
  stage sees the six interpolated varyings, the uniforms above, and
  `gl_FragCoord` (unused today).
- **Max node count:** `capacity` 2..4096 (`Config.cpp:160-166`), default 64
  (`Config.hpp:32`); spring size is always `capacity` once seeded
  (`SPEC.md` 13.1). At 4096 the VBO is 4098 x 28 = 114,744 B. Path draws
  `size()-1` instances (`LayerPassElement.cpp:257`); instanced draws at most
  `visible x K` = 4096 x 64 = 262,144 (`SPEC.md:1076`). At most 4 layers
  (`Preset.cpp:118-119`).
- **A per-pixel loop over nodes in a fragment shader** has no data to read
  today; it needs a new texture (RGBA32F, 1-2 texels per node, `texelFetch`)
  or a UBO. GLES 3.0's guaranteed minima (spec, not in the repo; Hyprland
  asks for 3.2 with a 3.0 fallback, `SPEC.md:958` citing `OpenGL.cpp:
  199-220`) are 224 fragment uniform vectors and a 16 KB uniform block, so a
  uniform array holds on the order of 100-200 nodes (one or two vec4 each) and
  a UBO about 500-585, neither the 4096 cap; a texture holds all. Cost is arithmetic, not
  measured: iterations per fragment = visible nodes N, times fragments in the
  layer's draw (damage box x strip overdraw, since each path segment or
  instanced quad is its own strip and a pixel under k strips runs the loop k
  times). At roughly 10 ALU per iteration: N=64 (default) is about 640 ops per
  fragment; N=512, 5k; N=4096, 41k. Over a 600x600 box (0.36 Mpx): 23 M, 184 M
  and 1.5 G iterations per frame; over 1920x1080 (2.07 Mpx) at N=4096, 8.5 G
  per frame. Only the visible newest nodes need looping (`visibleCount`,
  `TrailBuffer.hpp`), so the practical N is `fade_ms` x insert rate, not
  `capacity`; but the spring source keeps all `capacity` nodes visible while
  moving. A spring chain also re-uploads every frame (`SpringChain.cpp:203`),
  so the texture would too (4096 x 32 B = 128 KB).

### Candidate varyings

A varying added to both preludes and to `ht_initVaryings()` is declared and
zeroed in every vertex shader that calls `ht_initVaryings()` (all four
built-ins), so no vertex shader is forced to change to keep linking, and the
matrix still passes. The brief's premise ("any varying a frag reads must be
written by every vert, or the matrix fails") holds only for custom varyings
outside the prelude (`SPEC.md:1238`), and per the control above this matrix
would not catch those either. What a new varying forces is semantic: a vertex
shader that does not write it leaves 0, which a fragment reading it
mistakes for a value. It is additive, so no contract bump
(`CONTRACT_VERSION`, `ShaderSource.hpp:57`). GLES 3.0 guarantees 15 varying
vectors; the six use 7 components.

| Candidate | Exists? | Cost to add / notes |
|---|---|---|
| age | Yes: `ht_vAge`, ms, written by all four vertex shaders (halo: stillMs) | Nothing. Linear between segment ends. |
| arc length along the path | Yes: `ht_vDist`, px from the segment start. Ring: accumulates `prev.dist + hypot` (`TrailBuffer.cpp:22`), restarts at a break, is not rebased when old nodes are evicted; spring: recomputed from the tail every rebuild (`SpringChain.cpp:218-231`), so it changes each frame | Nothing for px along the polyline. A 0..1 position along the whole trail needs the total length: one new uniform (`common.glsl`, `preludeUniforms`, `LayerPassElement.cpp:288-297`), no vertex change. |
| speed | Yes: `ht_vSpeed`, px/ms at birth, `|vel|` against the previous node (`TrailBuffer.cpp:17-21`), 0 at a segment start; drift writes the particle's own speed instead | Nothing. |
| across-width coordinate | Yes for path: `ht_vLocal.y`, -1..1 (`ribbon.vert:99`). In px it is not available: the half-width (`ribbon.vert:92-93`) never leaves the vertex stage | One more float (`ht_vHalfWidthPx`): prelude both stages, `ht_initVaryings`, one write in `ribbon.vert`. Others stay 0. |
| per-node hash | Partly: `ht_vSeed` is per node at the endpoints and interpolated, so across a ribbon segment it is a smooth blend of two hashes, not a constant. Instanced shaders overwrite it per copy (`scatter.vert:60`, `drift.vert:83`) | A constant per segment would be a `flat out` varying written the same on all four vertices (the `varyingCheck` regex already accepts `flat`, `ShaderSlot.cpp:84-85`). Same cost shape as above. GLES `flat` rules are from the spec, not checked in the repo. |

### ribbon.vert with coincident nodes

No `normalize()` appears anywhere under `hyprtail/` (grep). Every direction
goes through `ht_dirBetween` (`helpers/ribbon.glsl:20-24`), which divides
only when the length is above `HT_EPS` (1e-3, `:9`) and otherwise returns the
fallback.

- Coincident p0/p1 (`length < HT_EPS`): returns before any direction is
  computed, all four vertices take the same branch (it depends on per-instance
  attributes only), `gl_Position` is `ht_collapsedPosition()` (2,2,2,1,
  outside the clip volume) and the varyings are already zeroed
  (`ribbon.vert:63`, `:78-81`). Draws nothing.
- Coincident prev/p0 or p1/next, and the trail ends (prev/next are copies of
  the end node, `path.glsl:4`): `dirPrev`/`dirNext` fall back to the
  segment's own `dir` (`ribbon.vert:83-85`).
- `halfWidth` sums two unit directions that cancel on a full reversal; the
  length is then 0 and `ht_dirBetween` returns its fallback (`:54`).
  `ht_jointOffset` returns `nOut * hw` when `|m| < HT_EPS`
  (`ribbon.glsl:34-35`) and clamps the miter divisor by `1/miter_limit`
  (`:38`), `miter_limit` min 1 (`ribbon.vert:36`). `ht_life` divides by
  `fade_ms`, min 1 (`ShaderSource.cpp:471`).
- One edge: a length exactly equal to `HT_EPS` passes the `<` at `:78` but
  gets the fallback `(1,0)` from `ht_dirBetween` (`>`); still a unit vector,
  no NaN.

So a zero-length tangent cannot reach a division, and the result for a
stationary pointer is a collapsed, undrawn segment. Where coincident nodes
come from: the ring with `min_spacing` 0 (allowed, `Config.cpp:157`; the gate
is `>= min_spacing`, `main.cpp:316`, so every render inserts a node);
`min_spacing` 2 (default) avoids it. The spring chain at rest: settled points
snap onto their target (`SpringChain.cpp:100-103`), so the whole settled chain
is one point and every ribbon segment collapses. Not run on a GPU.

### Spring source against the "head spring, followers lerp" model

Model: the head springs toward the pointer with its own spring constant,
friction and offset; each later point lerps a fixed factor toward the one
before it; no timestamps, no fade.

- **Already covered:**
  - Head spring: closed-form damped spring per axis, dt-based, via
    `advanceSpring` (`SpringChain.cpp:27-33`, `:92-93`; the solve is in
    `external/hyprutils/src/animation/Spring.cpp`, whose checkout version was
    not checked here; `SPEC.md:898` cites v0.14.2). Spring constant =
    `stiffness`, friction = `damping`, plus `mass` (`Source.cpp:19-24`,
    `SPEC.md:1354-1356`), live (`SPEC.md:1359-1360`).
  - Offset: `emit_from` / `emit_offset` shift the position handed to
    `insert` (`main.cpp:287-294`), so the head target and the whole chain move
    by it. It is a plugin-wide setting (`SPEC.md` 13.9), not a spring setting.
  - Chain length is `capacity`; a ribbon, dots and glow layer can stack on the
    chain (`SPEC.md:940-943`).
- **Differs:**
  - Followers are springs, not lerps: each point chases the already-advanced
    point before it with the same curve (`SpringChain.cpp:87`, `:107`). One
    `SSpringCurve` for the whole chain (`SpringChain.hpp`, `m_curve`), so the
    head cannot have different constants from the followers.
  - Births: a point has a synthetic birth `activeMs - k * age_step_ms`
    (`SpringChain.cpp:229`), because visibility, damage and `isSettled` are
    all built on birth times and `fade_ms` (`TrailBuffer.hpp`, `visibleCountOf`).
    `age_step_ms` 0 (range 0..1000, `Source.cpp:22`) removes the taper but not
    the fade.
- **New if wanted:**
  - A lerp follower: a `follow` setting or a third source kind. The tick loop
    (`SpringChain.cpp:88-107`) swaps `advanceAxis` for `p += (target - p) * f`
    for k >= 1; a fixed per-frame factor is frame-rate dependent, so a dt-based
    `1 - exp(-dt/tau)` is the safe form. Separate head constants: a second
    curve and four settings in `Source.cpp`. Small, no shader or GL change.
  - No fade: not supported. `fade_ms` is 1..60000 (`ShaderSource.cpp:471`),
    and `m_activeMs` freezes when the chain settles (`SpringChain.cpp:110-113`),
    so the chain disappears at most 60 s after it stops; the settled chain is
    also a single point for ribbons. A node set that never expires needs a
    flag on `ISource` read by `visibleCount`, `visibleBounds` and
    `isSettled` and by `ht_life`'s callers: a cross-cutting change.
  - **Several layers with different spring parameters: no.** A preset has one
    source (`LayerPassElement.hpp:84`) and one `CNodeBuffer` (`:86`); spring
    settings are `source:<name>` keys of the preset, not layer params, by
    design (`SPEC.md:1357-1358`). Layers can differ in shape and color, not
    in motion. Per-layer motion would mean a source and a node buffer per
    layer (or per distinct parameter set): its own tick, upload generation
    (`LayerPassElement.cpp:123-141`), `needsContinuousUpload`, and a manifest
    key to bind a layer to a source. Damage is already per layer
    (`SPEC.md:945-947`), which helps. Medium-sized, touches `SPreset`,
    `drawLayer`, `main.cpp`'s tick and sample, and `Preset.cpp`.

## Pairing gate and the demo layers (compiled and unit-tested; nothing run in a compositor)

### What the loader does on an `expects` mismatch

- **It refuses the pair; it does not warn or draw it.** `expectsMismatch`
  (`ShaderSource.cpp:413-422`) is called first thing in `programInfo`
  (`ShaderSlot.cpp:211-215`); its text becomes the error of
  `compileAndActivate` (`:299-302`), before any GL call. `prepare()`
  (`:338-363`) then reports it (key `shader:<layer>`, `WARN` for a pair with
  a user file, `ERR` for a built-in pair) and either keeps the last working
  program of the same files, or destroys the active one and falls back to the
  slot's built-in pair (`:366-374`). If that built-in pair fails too,
  `prepare()` returns an error and `main.cpp:266-268` disables the layer.
- **The fallback pair is not always safe.** A stage given as a path falls back
  to `shaders/ribbon.vert` / `shaders/gradient.frag`; a stage given as
  `prefab:<name>` falls back to that prefab (`Preset.cpp:231-235`, `:260`,
  `:263`). So a user vertex shader paired with `prefab:dots.frag` falls back
  to `ribbon.vert` + `dots.frag`, which `expects` refuses again, and the
  layer is disabled with an `ERR`. Read from the code, not run.
- **Nothing is checked when the preset is read**: `resolveStage` only checks
  the files exist. The refusal appears when the layer's program is first
  prepared, in a GL context.

### What each clause protects

- `gradient.frag:3` `expects path`: reads `ht_vLocal.y` as the position
  across a strip (`:30-32`) and `ht_vLife` (`:39`). `halo.vert` writes only
  `ht_vLocal` and `ht_vAge` (`:24-25`), so with it alpha is 0 and nothing
  draws; with instanced geometry `ht_vLocal` is a quad coordinate and the
  look is a clipped band.
- `dots.frag:3` `expects quad,instanced`: `shapeDistance(ht_vLocal)`
  (`:31-41`) needs quad coordinates, -1..1 on both axes. On a strip,
  `ht_vLocal.x` is 0 or 1 and the shape is a sliver.
- `pulse.frag:3`, `sizzle.frag:3` `expects quad`: they take
  `ht_vLocal * ht_extentPx` as px from the pointer (`pulse.frag:28`,
  `sizzle.frag:61`) and time from `ht_stillMs` with `start_ms` /
  `duration_ms` (`pulse.frag:20-21`, `sizzle.frag:48`). That holds only for the
  square around `ht_anchor` (`halo.vert:8-10,23-26`, `quad.glsl`), and a quad
  layer is visible and damaged by those parameters and by the pointer, not by
  node ages and node bounds (`SPEC.md:1105-1110`). On other geometry the
  effect would be misplaced and its damage box wrong.

### Matrix test, now with the gate

`testShaderLinkMatrix` (`tests/unit/unit.cpp`) covers the shipped shaders
(embedded, `shader::builtin()`) and `demo/hyprtail/shaders/` (loaded from disk
with `shader::load`, as `CShaderSlot::reload` does), links every vertex shader
with every fragment shader, evaluates the real `expectsMismatch` for each pair
and prints the table. Link failures fail the test; an `expects` refusal is
only a table entry. `testDemoPresets` checks each demo preset: header
comment, contract, layer names (unique across the demo presets), shader
references resolved as a file preset does (`prefab:<name>` embedded, anything
else under `demo/hyprtail/`), `expectsMismatch`, and every parameter name,
type and range. Not covered by any unit test, so checked once by a scratch
script: a parameter declared in both stages with different declarations
(`ShaderSlot.cpp:217-228`) and padding names (`:234-245`); none found. The
table, 6 vertex x 7 fragment = 42 pairs: 17 ok, 25 refused, 0 link failures.

### Demo layers (`demo/hyprtail/`, not embedded, not in any Makefile list)

All path topology, all contract 2 unchanged (no new varying, uniform,
texture or prelude change). Shader mains are written from scratch; they
include only the contract's own `helpers/` (ribbon.glsl joins and the
degenerate-segment guard, fade.glsl, noise.glsl `ht_hash2`).

- **Why every new fragment shader is `expects path`:** each reads
  `ht_vLocal.y` as across-width and/or `ht_vDist` as arc length, which mean
  that only for a connected strip. Dropping `expects` would let them pair with
  quad and instanced geometry, where they draw bands. So the new shaders pair
  with the three path vertex shaders (ribbon, comet, taper) and nothing else:
  12 of 42 pairs; the rest of the table is the shipped quad and instanced
  group.
- **Head taper is a time ramp.** The contract has no distance from the head:
  `ht_vDist` counts from the other end (`TrailBuffer.cpp:22`). `taper.vert`
  ramps the width over `head_ms` of a node's age, which is sharp while moving
  and relaxes once the newest node ages.
- **Helix and lattice are drawn per pixel in strip space** from `ht_vDist`
  (path px) and `ht_vLocal.y` (-1..1 of the local half-width), so they do not
  alias with node spacing. The strand distance uses the screen-space
  derivatives of those two varyings, which are per device pixel: `strand_px`
  is in device pixels. The lattice cannot see the strip's px width, so
  `cell_px` and `rows` are set together by the preset.
- **Sideways offset** of `taper.vert` is the path shifted along the miter
  normal by `offset` (one `ht_jointOffset` call; it is linear in its width
  argument). Its padding uses the top of the range (24), since padding
  expressions have no absolute value.

### Contract requests (not made; each with what it would unlock)

1. **Strip half-width in px as a varying** (`ht_vHalfWidthPx`): regular
   lattice cells whatever the taper (`lattice.frag`), px-exact amplitude and
   rungs (`strands.frag`), and a minimum-coverage correction so a strip thinner
   than a pixel fades instead of shimmering (`demo-thread`, the rails of
   `demo-tether`).
2. **Distance from the head** (arc length from the newest node, or the trail's
   total length as a uniform): a spatial head taper in `taper.vert` and a
   comet whose length does not depend on pointer speed (`comet.vert`,
   `demo-comet`, `demo-comet-helix`).
3. **Global position of the fragment as a varying** (with `ht_anchor`, which
   fragment shaders can already read): cells or strands that brighten near the
   pointer (`lattice.frag`), a head glow (`softline.frag`).
4. **Output scale as a uniform:** hairlines of the same logical thickness on
   1x and 2x outputs (`strands.frag`, `softline.frag` edges).
5. **`path smooth N`** (`SPEC.md:1038`, not built): smooth curves between
   sparse nodes for `demo-thread`, `demo-comet`, the rails of `demo-tether`;
   today corners are miter joins.
6. **Per-layer source or per-layer spring settings** (`SPEC.md:1357-1358`
   keeps them per preset): rails that lag differently in `demo-tether`.

### Previewing

The nested instance reads its hyprtail root from `$XDG_CONFIG_HOME/hypr/hyprtail`
and `run_dev.sh` sets `XDG_CONFIG_HOME=dev_env`, so the root is
`dev_env/hypr/hyprtail`. Link `demo/hyprtail` there (untracked, remove after);
shipped shaders are named `prefab:` in the presets, so nothing else is
needed. See the report for the commands.

## One shipped default: `subtle` and `classic` folded into `ink` (built; compiled and unit-tested only)

- `hyprtail/presets/subtle.conf` and `classic.conf` are deleted. `prefab:ink`
  is the default `trail` and the failed-load fallback (`Config.hpp`,
  `FALLBACK_PRESET` in `Preset.cpp`).
- `ink.conf` gained classic's `idle` layer, values unchanged, with
  `idle:enabled = false`. The disable mechanism already existed: `enabled` is
  a reserved per-layer parameter (`ShaderSource.cpp:467`, read in
  `CLayer::resolve`, `Layer.cpp:84`). Layer cap is 4 (`Preset.cpp:118`), ink
  has 2; `idle` collides with no other shipped layer name.
- Entries above that name `subtle` or `classic` are the record of what was
  true then and are left as written.

## `spray` removed (built; compiled and unit-tested only)

- `hyprtail/presets/spray.conf` is deleted, with its `#embed` and map entry in
  `src/Preset.cpp` and its unit and smoke references. `drift.vert` and
  `dots.frag` stay: `embers.conf` uses both.
- Compared with `embers`: same shaders and pointer source, but spray was one
  layer (`trail`) with no idle layer and different values (no gravity, no
  twinkle, no `color_by`, white to orange, `fade_ms` 700). Not just the idle
  layer.
- Smoke (`hyprtail_smoke.cpp`, section 4b) lost its only second instanced
  preset: the instanced rounds, K-param loop and hotplug now run on
  `prefab:jitter` alone, so `drift.vert`'s instanced path is no longer
  exercised there. `embers` could be added back with `embers:` keys.

## `comet.vert` folded into `ribbon.vert` (built; compiled and unit-tested only)

- `demo/hyprtail/shaders/comet.vert` and `demo/hyprtail/presets/demo-comet.conf`
  are deleted. `ribbon.vert` gained `tail_power` (default 1, 0.25 to 6): the
  half-width is `0.5 * width * mix(1, life^tail_power, taper) * pen`. At the
  default it is today's ribbon, so no shipped preset changes. Padding is
  unchanged: the exponent only narrows the strip.
- The head swelling (`bulb`, `bulb_ms`) is dropped, not ported: it added 2.5 px
  of half-width at the defaults and sits under the pointer when it rests.
- `demo-comet-helix.conf` now uses `prefab:ribbon.vert` (demo presets load from
  disk, so the embedded shader needs the `prefab:` form) with
  `core:tail_power = 1`; `curve`, `bulb` and `bulb_ms` are gone, so its head is
  blunt instead of faintly swollen.
- Older entries (the demo-layer notes naming `comet.vert`, `demo-comet`) are
  left as the record.
- Naming: `tail_power` names the exponent in the width = life^p profile.
  `taper.vert` still calls the same idea `tail_curve`; the rename pass should
  settle both together with `taper`.

## `demo-lattice` shipped as `prefab:mosaic` (built; compiled and unit-tested only)

- `demo/hyprtail/shaders/lattice.frag` moved to `hyprtail/shaders/hexagons.frag`
  and `demo/hyprtail/presets/demo-lattice.conf` to `hyprtail/presets/mosaic.conf`
  (embedded in `src/ShaderSource.cpp` and `src/Preset.cpp`). Names: the shader
  names the pattern, the preset the look, the layer stays `scales`, so no two
  shipped names match.
- Only the shader's header line changed. The preset now uses
  `shaders/ribbon.vert` (shipped, embedded) instead of the demo
  `taper.vert`: `tail_curve = 0.7` became `tail_power = 0.7` and `head_ms`
  (240) is gone, so the head is blunt instead of ramping up from a point.
  Everything else is carried over.
- Unit tests: manifest minimum is 7, `hexagons.frag` joins the built-in
  fragment list, and mosaic is checked for its layer, shaders and
  `cell_px * rows == width` (regular hexagons).
- Older entries naming `lattice.frag` are left as the record.

## `softline.frag` folded into `gradient.frag` (built; compiled and unit-tested only)

- `demo/hyprtail/shaders/softline.frag` is deleted. `gradient.frag` gained
  `alpha` (default 1, 0 to 1) and `fade_curve` (default 1, 0.25 to 4); opacity
  is now `c.a * alpha * max(life, 0)^fade_curve * cov`. At the defaults that is
  the old `c.a * life * cov`, so no shipped preset changes.
- Softline's life-based color is `color_by = 1` in gradient
  (`ht_paletteT` returns `1 - life`, so `mix(a, b, 1 - life)` is softline's
  `mix(b, a, life)`: `helpers/palette.glsl`). Its `soft` is `softness`.
- `demo-thread`, `demo-helix` (sheath) and `demo-tether` (three layers) now use
  `prefab:gradient.frag` (the `prefab:` form, since demo presets load from
  disk) with `color_by = 1` and `softness` in place of `soft`. Their `alpha`
  and colors were already set explicitly, so nothing else needed carrying.
- Not carried over: softline's `1e-4` floor on the edge antialiasing width.
  Gradient keeps its own (`max(softness, fwidth)`); a strip tapering to a point
  is the case to check on the host.
- Older entries naming `softline.frag` are left as the record.

## The `demo/` folder dissolved into `hyprtail/` (built; compiled and unit-tested only)

- Moved into `hyprtail/` and embedded (`src/ShaderSource.cpp`, `src/Preset.cpp`):
  `strands.frag`, `taper.vert`, and the presets `helix` (was `demo-helix`),
  `tether` (`demo-tether`), `thread` (`demo-thread`) and `ribbon-helix`
  (`demo-comet-helix`). `demo/` is gone.
- References changed from the `prefab:<name>` form (disk presets) to the
  `shaders/<name>` form the shipped manifests require; descriptions and headers
  lost "demo". Only wording changed, no parameter values.
- `testDemoPresets` and the demo half of the link matrix are removed
  (`tests/unit/unit.cpp`). The shipped-manifest loop already runs the same
  checks (header, layers, shader pairing, parameter names, types, ranges), and
  the matrix globs `hyprtail/shaders` only. The manifest minimum is 11, the
  matrix is 5 vertex x 6 fragment, and the layer-uniqueness list now covers the
  eight reworked presets.
- Layer `thread` shares its name with preset `thread` (as `ink` and `embers`
  already do).
- Older entries naming `demo/hyprtail`, `testDemoPresets` and the `demo-*`
  presets are left as the record.

## Geometry shader renames: `ribbon.vert` -> `taper.vert`, `taper.vert` -> `convex.vert` (built; compiled and unit-tested only)

- Pure rename, swapped through `git mv` in two steps: contents are unchanged
  apart from the "hyprtail geometry" name in each header. Every shipped
  preset, the embedded table (`GEOM_TAPER_VERT`, `GEOM_CONVEX_VERT` in
  `src/ShaderSource.cpp`), the fallback identity in `src/Preset.cpp`, the unit
  and smoke tests, SPEC and docs follow the new names.
- Every entry above this one that says `ribbon.vert` means today's
  `taper.vert`, and one that says `taper.vert` means today's `convex.vert`.
  They are left as the record.
- Not renamed: `helpers/ribbon.glsl`, the `taper` parameter inside the new
  `taper.vert`, the `ribbon-helix` preset and the word "ribbon" in prose and
  comments.
- Follow-up: the `ribbon-helix` preset is now `snake` (`snake.conf`,
  `prefab:snake`). Older entries that say `ribbon-helix` or `demo-comet-helix`
  mean it.

## Open questions

- [x] Hyprland commit to pin: `efb5099` (v0.56.2, host package)
- [x] Backlog: bezier curves for warp interpolation (`warp = "curve"`, phase 6; `interpolateWarps` removed).
- [ ] **Blocked:** verify no trail over direct-scanned-out fullscreen clients (hardware cursors). mpv fullscreen with `render:direct_scanout = 1` dies with a Wayland protocol error (`wl_surface.attach` invalid arguments), with or without the plugin loaded, so Hyprland or mpv, not us. Browsers never qualify (not opaque, subsurfaces). Need another client that actually gets scanned out.
- [x] Data-model fork: discrete point-history buffer vs. accumulation/ping-pong framebuffer. Resolved: discrete point-history ring, VBO-resident (`CTrailRing`/`CNodeBuffer`, SPEC §3-§4); the ping-pong option was never built.
- [x] `SCursorNode` final field list, buffer size, fade/decay function. Resolved by phases 2-4b: 28-byte node with position, birth time, velocity, segment distance, seed and segment flag (SPEC §3, §13.2); `capacity` setting, default 64 (untuned placeholder, SPEC §12); linear, time-based fade (`ht_life`, reserved `fade_ms`, 300 ms in `subtle`, 500 ms in `classic`).
- [x] `addConfigValue`/`getConfigValue` under Lua: V1 doesn't work, V2 does (see "Config and shader loader")
- [ ] Whether hyprtester supports any frame/pixel readback beyond state assertions
- [ ] **Host freezes** (flipped transform; fullscreen game start). Not isolated; see "Freeze analysis". Plugin hardening in, no fix claimed.
- [ ] Cursor-warp tool for the visual test harness (partly answered: `hl.dsp.cursor.move({ x = X, y = Y })` via `hyprctl dispatch`, field names from `LuaBindingsDispatchers.cpp:77-85`, used by the smoke test): `hyprctl dispatch movecursor` is unreliable under the Lua provider (needs the `eval`/`hl.dsp.movecursor` form, field names unconfirmed), `wlrctl pointer move` didn't work in first attempt (likely wrong `WAYLAND_DISPLAY`), neither fully resolved. Not urgent given manual drag + `cursorpos_trace.sh` already gave a usable result, but will matter once scripting the stage-4/5 validation shaders.
- [ ] `hyprctl hyprtail` lifecycle line (`Status.cpp:97-100`) is gated on `topology == "quad"` by hand (path: fade+reach; quad: fade+start+duration+reach), matching which fields `ribbon.vert`/`ring.frag` actually read. Worth revisiting: key the displayed fields off which reserved params (`shader::reservedParams()`, `ShaderSource.cpp:351-369`) a layer's shader pragma actually declares as used, rather than hardcoding per-topology in `Status.cpp`, so the status output stays correct automatically as shader capabilities change (e.g. a future path shader that does read `duration_ms`, or a quad variant that doesn't). Not blocking; `reservedParams()` currently marks all three (`fade_ms`/`start_ms`/`duration_ms`) `uniform=true` unconditionally for both topologies, so there's no existing per-shader "which reserved params does this program use" signal to key off yet — would need one added to `SProgramInfo`/`ShaderSlot` first.
- [x] **"Window under pointer" mode for per-app suppression.** Done, and it
  replaced the focus-based rule instead of sitting beside it (it came up with
  `follow_mouse` = 2/3). The helper is `CViewHitTester::windowAt`, not on
  `Compositor`; see "Rule follows the pointer, not focus" under "Per-app
  suppression". Open there: a hand test on the host, and layer surfaces
  above an excluded window (listed under known limitations).
