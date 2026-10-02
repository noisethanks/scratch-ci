

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


## Installation and Usage
```
hyprpm add https://github.com/noisethanks/hyprtail
hyprpm enable hyprtail
```


## Configuring

Invalid values are blocked from updating, and emit a warning. 

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

```

hl.window_rule({
    match = { 
        class = "^(mpv)$" 
    }, 
    ["hyprtail:no_trail"] = true 
})

```

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
- Cursor SVG shader source.
