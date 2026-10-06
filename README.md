

[![CI](https://github.com/<owner>/<repo>/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/<owner>/<repo>/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/<owner>/<repo>)](LICENSE)
[![Hyprland](https://img.shields.io/badge/hyprland-0.56%2B-blue)]()

Performant shader-driven cursor trails for Hyprland.

## Features
- GPU accelerated
- Highly performant.
- Hot reloads.
- Supports all hyprcursor configurations.
- Hide from screen/window sharing.
- Window rules for visibilty.
- Animated warp transitions.
- Infinitely customizable with GLSL shaders.
- Particle spring animations.
- Prefabs ranging from professional to flashy.


## Installation
```
hyprpm add https://github.com/noisethanks/hyprtail
hyprpm enable hyprtail
```

## Configuring

```
hl.config({
    plugin = {
        hyprtail = {
            preset       = "prefab:subtle", -- subtle, classic, comet, embers, jitter,spray, spring, vivid
            capacity     = 64,
            min_spacing  = 2,
            warp         = "break",
            damage_padding = 0,
            params       = "",
            emit_from    = "hotspot",
            emit_offset  = {0, 0},
            screenshare  = "exclude",
        },
    },
})

```

To disable the trail over specific clients, use this window rule:

```

hl.window_rule({
    match = { 
        class = "^(mpv)$" 
    }, 
    ["hyprtail:no_trail"] = true 
})

```


Shipped presets:

- **`prefab:subtle`** (default): one thin trail layer (`thread`), short
  fade, low-alpha neutral color, no idle effect.
- **`prefab:ink`**: a calligraphy stroke, one ribbon (`ink`) drawn as if by
  a flat nib, so it swells moving across the nib and thins moving along it;
  paper white cooling to slate as it fades. Try `params = "ink:nib_angle=-30"`
  to turn the pen, or `ink:nib=0` for a round one.
- **`prefab:classic`**: a wider, speed-tinted trail (slow motion tints one
  color, fast motion tints another) plus an optional idle ring around a
  stationary cursor (off by default).
- **`prefab:jitter`**: a cloud of small dots scattered around every trail
  point, `copies` of them (1–64, default 6) at random offsets of at most
  `spread` px. Try `params = "trail:copies=16 trail:spread=24"`.
- **`prefab:spray`**: particles thrown off the trail, `count` per point
  (1–64, default 4), drifting away (mostly behind the pointer's motion) at up
  to `speed` px/s while shrinking and fading over `fade_ms`. The redrawn area
  grows with `speed * fade_ms`, so a fast, long-lived spray costs more to
  draw. Try `params = "trail:count=12 trail:speed=120"`.
- **`prefab:vivid`**: a glowing ribbon, two layers over the same trail
  points: `glow`, a wide (26 px), faint, longer-lived ribbon underneath, and
  `core`, a narrow (4 px), opaque, brighter ribbon on top. Both shade cyan
  (slow) to magenta (fast). Layers composite with ordinary alpha blending
  (there is no per-layer blend mode), so the glow is a translucent halo, not
  additive light. Try `params = "glow:width=40 core:width=6"`.
- **`prefab:comet`**: a narrow ribbon (`tail`, 5 px) with a very short fade
  (180 ms), so it tapers to a point right behind the pointer, over a few
  twinkling star-shaped `sparks` thrown straight back. Ice blue shifts to
  amber with speed; `speed_ref` (px/ms) is set high (4), so the amber only
  appears on fast flicks. Try `params = "tail:speed_ref=1.5"` to make it
  flare on lighter movement.
- **`prefab:embers`**: fire. Soft particles (`embers`, 2 per point) drift
  slowly, rise up the screen and cool from yellow to red over 1.4 s; while
  the pointer rests, short sparks crackle around it now and then
  (`crackle`). Points pushed out of the buffer take their particles with
  them, so raise `capacity` (for example 256) for long strokes. The crackle
  runs until the pointer moves, which keeps the monitor redrawing every frame
  while it shows: `params = "crackle:duration_ms=8000"` stops it after 8 s,
  `crackle:enabled=false` turns it off.

- **`prefab:spring`**: a springy rope instead of a pointer history (source
  `spring`, see "Sources" below): a chain of `capacity` points, the first
  chasing the pointer and each other one chasing the point before it, drawn
  as one ribbon. The tail swings and settles rather than replaying the
  pointer's path, and when the pointer stops the rope springs together under
  the cursor and fades. Try `params = "source:damping=120 source:age_step_ms=6"`
  for a livelier, longer rope.

`jitter`, `spray`, `embers` and comet's `sparks` draw *(visible points) x (copies per point)* quads, so `capacity` and
`fade_ms` together with the copy count set the GPU cost: the most a single
layer can draw is 64 copies x 4096 points.

### Settings

All keys live under `plugin:hyprtail:` (hyprlang) or `plugin.hyprtail` (Lua).
Out-of-range or invalid values are rejected, reported as a plugin warning, and
the previous value is kept. Values are re-read on every config reload.

| Setting | Type | Default | Acceptable values | Description |
|---|---|---|---|---|
| `preset` | string | `"prefab:subtle"` | `"prefab:<name>"` for a built-in (`subtle`, `classic`, `comet`, `embers`, `jitter`, `spray`, `spring`, `vivid`), or a bare `"<name>"` for `<hyprtail root>/presets/<name>.conf` | Which preset to draw. A bare name never falls back to a built-in; a missing file is an error. The hyprtail root is `$XDG_CONFIG_HOME/hypr/hyprtail`, else `~/.config/hypr/hyprtail`. |
| `capacity` | int, points | `64` | `2`–`4096` | Maximum number of trail points kept. For the `spring` source, the number of points in the chain. Higher allows a longer trail and costs more GPU work. |
| `min_spacing` | float, logical px | `2` | `0`–`256` | Minimum pointer travel before a new trail point is recorded. |
| `warp` | string | `"break"` | `"break"`, `"line"`, `"curve"` | How the trail crosses a pointer warp (workspace switch, monitor change, dispatcher warp). `break` starts a new segment, `line` draws a straight sweep, `curve` draws a quadratic Bézier sweep. |
| `damage_padding` | float, logical px | `0` | `0`–`4096` | Extra redraw margin around each layer's geometry. It adds to, and does not replace, the stock extent (`width / 2 * miter_limit + 1px`) and the layer shader's own `#pragma hyprtail padding <px>`. Raise it if a custom shader draws outside its declared reach and leaves artifacts. |
| `params` | string | `""` | Space-separated `<layer>:<name>=<value>` overrides | Per-layer shader parameter overrides. See ADVANCED.md. |
| `emit_from` | string | `"hotspot"` | `"hotspot"`, or `"fx fy"`: two space-separated floats, normalized to the cursor image box (`"0 0"` top-left, `"0.5 0.5"` center, `"1 1"` bottom-right) | Where on the cursor image new trail points are created. Each component must be within `0`–`1` inclusive; anything else (including out-of-range values) is rejected with a warning and the previous value is kept. With no cursor image available it falls back to the pointer position. |
| `emit_offset` | vec2, logical px | `{0, 0}` (hyprlang: `0 0`) | Each component `-128`–`128` inclusive; Lua `{x, y}` or `"x y"` | Fixed offset added to the emit point after `emit_from` is resolved, for both the `"hotspot"` and `"fx fy"` forms. A nudge, not a placement: out-of-range values are rejected with a warning and the previous value is kept. |
| `screenshare` | string | `"exclude"` | `"exclude"`, `"include"` | `exclude` keeps the trail out of monitor/region captures and mirrored outputs. `include` draws it in them. |

`emit_from` and `emit_offset` move the main trail only. Idle-style layers stay
anchored to the raw pointer position. With a non-default `emit_from`, a cursor
shape change that alters the image box starts a new trail segment.

## Gallery

## WIP
- Bezier curves for warp transitions.
- Benchmarking
- Catmul-Rom splines
- Cursor graphic shader source.
