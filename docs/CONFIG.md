# hyprtail configuration

All keys live under `plugin:hyprtail:` in hyprlang, or under
`plugin.hyprtail` in Lua config.

```
# hyprlang
plugin:hyprtail:preset = classic
plugin:hyprtail:capacity = 128

# Lua
hl.config({ plugin = { hyprtail = { preset = "classic", capacity = 128 } } })
```

Bad values (out of range, not one of the allowed strings, malformed
`params`/`emit_from` text) are reported as a warning and the previous value
is kept — they never break your config.

## Presets

A **preset** is a named look: an ordered stack of up to 4 **layers**, each
with its own shader pair and default parameter values. Pick one with:

| Key | Type | Default | Values |
|---|---|---|---|
| `preset` | string | `subtle` | `subtle`, `classic`, or the name of a directory under `~/.config/hypr/hyprtail/presets/<name>/` |

Shipped presets:

- **`subtle`** (default): one thin trail layer, short fade, low-alpha
  neutral color, no idle effect.
- **`classic`**: a wider, speed-tinted trail (slow motion tints one color,
  fast motion tints another) plus an optional idle ring around a stationary
  cursor (off by default).

A user preset is a directory `~/.config/hypr/hyprtail/presets/<name>/`
containing a `preset.conf`:

```
contract    = 2
description = Thin neutral trail
layers      = core

core:vertex   = hyprtail/ribbon.vert   # built-in, or a path relative to this directory
core:fragment = solid.frag             # a file next to this preset.conf
core:fade_ms  = 350                    # any other key sets a layer parameter
core:width    = 4
core:color    = rgba(ffffffa0)
```

- `contract = 2` is required.
- `layers = <name>[, <name>...]` lists 1–4 layer names, draw order first =
  bottom. No duplicates.
- Every other line is `<layer>:<key> = <value>`, where `<layer>` must be one
  of the names in `layers`.
  - `<layer>:vertex` / `<layer>:fragment` pick that layer's shader: either a
    built-in name (e.g. `hyprtail/ribbon.vert`) or a path relative to the
    preset's own directory (`~` and absolute paths also work).
  - Any other `<key>` sets a default value for a parameter that layer's
    shader declares (see SHADERS.md for how shaders declare parameters).
- `#` starts a comment to end of line; blank lines are ignored.
- A user preset directory shadows a built-in preset of the same name.
- If the selected preset fails to load (missing, parse error), hyprtail
  reports it and falls back to the built-in `subtle` preset.

## Overriding a preset's shaders

| Key | Type | Default |
|---|---|---|
| `layer1_vertex` … `layer4_vertex` | path | `""` (use the preset's shader) |
| `layer1_fragment` … `layer4_fragment` | path | `""` (use the preset's shader) |

These pick a shader by the layer's **position** in the active preset's
`layers` list (1 = bottom layer), not by name. `""` means "use whatever the
preset itself specifies for that layer." Setting an index the active preset
doesn't have (e.g. `layer3_vertex` on a 1-layer preset) is a warning and is
ignored. Path resolution is the same as in a preset manifest: `~`/`~/`
expand to your home directory, a relative path resolves against your
Hyprland config directory, an absolute path is used as-is.

## Overriding layer parameters

| Key | Type | Default |
|---|---|---|
| `params` | string | `""` |

One string, space-separated entries of the form `<layer>:<name>=<value>`:

```
plugin:hyprtail:params = core:width=4 glow:radius=18 core:color=rgba(ffffffa0)
```

- `<layer>` is a layer name from the active preset.
- `<name>` is a parameter that layer's shader declares (or a reserved name,
  below).
- `<value>` syntax depends on the parameter's type: a plain number for
  `float`/`int`, `true`/`false` (also `1`/`0`, `yes`/`no`) for `bool`, `x,y`
  for `vec2`, and `0xAARRGGBB` / `rgba(RRGGBBAA)` / `rgb(RRGGBB)` for
  `color`.
- Precedence: a shader's own built-in default < the preset manifest's value
  < `params`. So `params` always wins.
- An unknown layer, unknown parameter name, or a value that's the wrong
  type or out of the parameter's declared range is a per-entry warning; that
  one entry is ignored and everything else in the string still applies.
- Re-read on every config reload and on every shader file change — no
  separate reload needed.

**Reserved parameter names**, settable on any layer the same way as any
other parameter:

| Name | Type | Default | Meaning |
|---|---|---|---|
| `enabled` | bool | `true` | `false` turns this layer off entirely (not even compiled). |
| `draw_when_cursor_hidden` | bool | `true` | Whether the layer keeps drawing while the OS cursor is hidden. |
| `fade_ms` | float, ms | `500` | Trail/particle layers: how long a point stays visible after it's created. |
| `start_ms` | float, ms | `500` | Idle-style layers: how long the pointer must sit still before the layer starts showing. |
| `duration_ms` | float, ms | `1500` | Idle-style layers: how long it stays visible once started (`0` = forever, until the pointer moves). |

Example: `params = "glow:enabled=false"` turns off a layer named `glow`
without switching presets.

## Trail shape and spacing

| Key | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `capacity` | int, points | `64` | 2–4096 | Max number of trail points kept in the buffer. Higher = longer possible trail (subject to `fade_ms`), more GPU work. |
| `min_spacing` | float, logical px | `2` | 0–256 | Minimum pointer travel before a new point is recorded. Too low makes tight turns fold over themselves. |
| `damage_padding` | float, px | `0` | 0–4096 | Extra screen-redraw margin added on top of what each shader already reaches (its own padding declaration). Raise this if a custom shader draws outside its declared reach and you see trailing artifacts. |

## Where the trail starts (emit point)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `emit_from` | string | `hotspot` | `hotspot` = the cursor's actual click point (default). Or `"x y"`, a position normalized to the cursor image's own box: `"0 0"` = top-left of the cursor image, `"0.5 0.5"` = its center, `"1 1"` = bottom-right. |
| `emit_offset` | vec2, logical px | `0 0` | A fixed pixel offset added after `emit_from`. Lua: `{x, y}`; hyprlang: `"x y"`. |

These move where new trail points are created relative to the visible
cursor image — useful if you want the trail to start from, say, the tip of
an arrow cursor instead of its hotspot. They affect the main trail buffer
only; they don't move idle-style effects, which stay anchored to the raw
pointer position.

## Crossing a pointer warp (teleport)

| Key | Type | Default | Values |
|---|---|---|---|
| `warp` | string | `break` | `break`, `line`, `curve` |

When the pointer jumps instead of moving continuously (workspace switch,
monitor change, a dispatcher-driven warp, etc.):

- **`break`**: the trail doesn't connect across the jump. Points before the
  jump keep aging and fading in place; a new, separate trail segment starts
  at the new position.
- **`line`**: a straight line is drawn connecting the old and new positions.
- **`curve`**: a smooth curved sweep is drawn between the old and new
  positions instead of a straight line, so a warp doesn't look like a
  sudden streak in a fixed direction.

Some warps (a handful of internal call paths, e.g. the pointer-warp
protocol) always connect regardless of this setting.

## Per-app suppression

Not a config key: a dynamic [window rule](https://wiki.hypr.land/Configuring/Window-Rules/)
effect, `hyprtail:no_trail`, that suppresses the trail while the rule's
window holds focus.

```
# native
windowrule {
    name = mpv-no-trail
    match:class = ^(mpv)$
    hyprtail:no_trail = true
}
```

```lua
-- Lua
hl.window_rule({ match = { class = "^(mpv)$" }, ["hyprtail:no_trail"] = true })
```

Value parsing is its own, permissive rule, not the same as a shader `bool`
parameter: `true`, `1`, `yes` or `on` (case-insensitive) suppress; anything
else — `false`, empty, a typo, or the rule simply absent — does not, and
never reports an error.

- **Focus-based, not pointer-based.** The rule is checked against whichever
  window currently holds input focus, not whatever's under the pointer.
  This diverges from expectations under `follow_mouse 2`/`3` (focus follows
  the pointer, possibly across monitors): a window can be under the cursor
  without holding focus, or vice versa near an edge.
- **Fades, doesn't hard-clear.** Like a pointer constraint, this only stops
  new points from being recorded; a trail remnant already in the buffer can
  stay visible for up to `fade_ms` after focus moves to a suppressed app.
  Idle-style effects end immediately (they have no buffered history to
  decay).
- **Unloading hyprtail.** The effect is registered while hyprtail is loaded
  and unregistered when it unloads. If a `hyprtail:no_trail` rule is still
  configured at that point, the next rule (re)evaluation reports an
  unknown-effect error (native config: `"unknown effect
  'hyprtail:no_trail'"`; Lua: `"unknown field 'hyprtail:no_trail'"`) —
  harmless, but remove the rule (or reload your config) around an unload if
  you'd rather not see it.

## Screen sharing / recording

| Key | Type | Default | Values |
|---|---|---|---|
| `screenshare` | string | `exclude` | `exclude`, `include` |

- **`exclude`** (default): the trail is left out of monitor/region screen
  captures and mirrored outputs (screen recorders, screen-share portals,
  `grim`, mirrored monitors). It still draws normally on your own display.
  If the mechanism this needs isn't available for some reason, hyprtail
  falls back to simply not drawing the trail while that capture/mirror is
  active (never leaking it into the capture), and reports it once.
- **`include`**: the trail is captured/mirrored along with everything else
  (the old, pre-this-setting behavior).

Window-only captures (capturing a single application window) never include
the trail either way — the trail only exists as part of a whole monitor's
render.

## Notifications and diagnostics

Not config keys, but relevant to setting things up:

- Config and shader errors always show as a Hyprland notification (not just
  a log line) and are written to
  `$XDG_STATE_HOME/hyprtail/errors.log` (falls back to
  `~/.local/state/hyprtail/errors.log`).
- Multiple errors from one config reload or shader-file save are batched
  into a single notification.
- `hyprctl hyprtail` (add `-j` for JSON) shows the active preset, each
  layer's shader and current parameter values, per-monitor render stats,
  and the errors.log path — useful for checking that a config change
  actually took effect.
