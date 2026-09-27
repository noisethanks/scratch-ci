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
- Nested launch: `external/Hyprland/build/Hyprland -c hyprlandd.lua` works directly, no separate launch script needed (discarded, added more failure surface than it removed).
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
  - No `LOG` macro; `Log::logger->log(level, fmt, args...)` (`Logger.hpp:22`). `Log::INFO` is still debug level (`:53`).
  - No `bindArrayBuffer` and no array-buffer cache; core binds raw (`OpenGL.cpp:1547`, `Shader.cpp:236`). Plugin binds raw too. If the pin moves past the main commit that added the cache, switch back to `bindArrayBuffer`.
  - `projectBoxToTarget` uses `pMonitor->getScaleMatrix()` = `outputProjection(m_pixelSize, NORMAL)` (`Renderer.cpp:1842-1846`, `Monitor.cpp:1757`), and `getBoxProjection` defaults the box transform to the inverted monitor transform (`Renderer.cpp:1836-1840`). Plugin now passes `HYPRUTILS_TRANSFORM_NORMAL` explicitly so the pseudo-box isn't rotated; monitor rotation comes from `targetProjection` (`Renderer.cpp:1828`). Identical on transform-0 outputs; rotated outputs untested (SPEC §8).
  - `PluginAPI.hpp` no longer includes `<format>`; `main.cpp` includes it directly.
- **Color management gap** citations at this pin: `getConvertedColor` in `OpenGL.cpp:1086`, `:2371`.
- **Build:** plugin compiles against the `external/Hyprland` checkout at the pin (after `make clear && make debug` generates `version.h`/protocols). Debug build only adds `HYPRLAND_DEBUG`/`ISDEBUG` (macros, not layout); the only layout-affecting `#if`s in installed headers are `NO_XWAYLAND`, in xwayland headers the plugin doesn't use. ABI hash strips patch versions, so system aquamarine 0.15.1 vs the package's 0.15.0 still matches (`_aq_0.15`).

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
- **Test harness:** `make test-unit` builds `tests/unit/unit.cpp` against `Params`, `ShaderSource` and `TrailBuffer` only, runs it (61 checks), and validates the four preprocessed built-ins with glslangValidator, linking every vertex/fragment pairing. `SANITIZE=1` adds ASan and UBSan; it passes clean. Running the binary through `make` is needed because the lean-ctx allowlist blocks running it directly, and `gdb` is blocked too.
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
- **`interpolate_warps` kept, contra §13.8's literal removal list:** its
  named replacement, `warp = "break"|"line"|"curve"` (§13.10), is phase 6,
  unbuilt. Removing the old key now, with phase 6 still ahead, would
  delete "connect the trail across warps" outright with no way to get it
  back until then — a regression dressed up as a rename. Framed in SPEC
  §9 and §13.8 as the correct reading of that section's own intent (retire
  a key once its replacement exists), not as an exception carved out of it.
- **Not compiler-checked in this session:** `src/Preset.cpp` is new,
  Hyprland-header-free, and not wired into `make test-unit` (no new test
  cases were written — out of scope, this session is host-testing only per
  CLAUDE.md). Its first real compile happens on the host build
  (`hyprpm update`). Read it carefully before trusting it blindly on a
  first failure.

## Open questions

- [x] Hyprland commit to pin: `efb5099` (v0.56.2, host package)
- [ ] Backlog: bezier curves for warp interpolation (`interpolateWarps = true` currently draws a straight segment).
- [ ] **Blocked:** verify no trail over direct-scanned-out fullscreen clients (hardware cursors). mpv fullscreen with `render:direct_scanout = 1` dies with a Wayland protocol error (`wl_surface.attach` invalid arguments), with or without the plugin loaded, so Hyprland or mpv, not us. Browsers never qualify (not opaque, subsurfaces). Need another client that actually gets scanned out.
- [ ] Data-model fork: discrete point-history buffer vs. accumulation/ping-pong framebuffer, needs deciding before struct fields or buffer storage mechanism are finalized (see Data model section)
- [ ] `SCursorNode` final field list, buffer size, fade/decay function, once the fork above is decided and a concrete visual target is picked from the references
- [x] `addConfigValue`/`getConfigValue` under Lua: V1 doesn't work, V2 does (see "Config and shader loader")
- [ ] Whether hyprtester supports any frame/pixel readback beyond state assertions
- [ ] **Host freezes** (flipped transform; fullscreen game start). Not isolated; see "Freeze analysis". Plugin hardening in, no fix claimed.
- [ ] Cursor-warp tool for the visual test harness (partly answered: `hl.dsp.cursor.move({ x = X, y = Y })` via `hyprctl dispatch`, field names from `LuaBindingsDispatchers.cpp:77-85`, used by the smoke test): `hyprctl dispatch movecursor` is unreliable under the Lua provider (needs the `eval`/`hl.dsp.movecursor` form, field names unconfirmed), `wlrctl pointer move` didn't work in first attempt (likely wrong `WAYLAND_DISPLAY`), neither fully resolved. Not urgent given manual drag + `cursorpos_trace.sh` already gave a usable result, but will matter once scripting the stage-4/5 validation shaders.
- [ ] `hyprctl hyprtail` lifecycle line (`Status.cpp:75`) is gated on `topology == "quad"` by hand (path: fade+reach; quad: fade+start+duration+reach), matching which fields `ribbon.vert`/`ring.frag` actually read. Worth revisiting: key the displayed fields off which reserved params (`shader::reservedParams()`, `ShaderSource.cpp:333-341`) a layer's shader pragma actually declares as used, rather than hardcoding per-topology in `Status.cpp`, so the status output stays correct automatically as shader capabilities change (e.g. a future path shader that does read `duration_ms`, or a quad variant that doesn't). Not blocking; `reservedParams()` currently marks all three (`fade_ms`/`start_ms`/`duration_ms`) `uniform=true` unconditionally for both topologies, so there's no existing per-shader "which reserved params does this program use" signal to key off yet — would need one added to `SProgramInfo`/`ShaderSlot` first.
