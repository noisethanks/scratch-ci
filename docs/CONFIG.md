# Basic Customization

## Terms
- **Prefabs** are default trail styles, internal to hyprtail. They can be referenced in the configs as such:
```
trail = "prefab:subtle"
```
- **Presets** are identical copies of the prefabs. These copies are intended to provide a simple starting point for basic configurations, such as changing color or tail width. This document focuses on these kinds of basic customizations.
- A **stage** is one step in the rendering pipeline of a shader. A shader willl always have a vertex stage, and, in hyprtail, will always have a fragment stage.
- A **layer** in hyprtail is one draw layer of a shader. For example `glow` and `core` in the vivid presets. 
- The **hyprtail config root** is `$XDG_CONFIG_HOME/hypr/hyprtail/`
  (`~/.config/hypr/hyprtail/` if `XDG_CONFIG_HOME` is unset). Every
  relative path hyprtail reads from your config resolves against it.


## Getting started with Presets

1. Begin with downloading the presets and shaders directory from the project root and placing them in `~/.config/hypr/hyprtail/` 
2. Edit the files freely. Presets are re-read on every Hyprland config reload,
   not when th$e preset file itself changes. You can use **hyprctl reload**
3. Preset trails are composed of preset shaders. These built in shaders are referenced by their stage as such:
```
trail:vertex = shaders/ribbon.vert
trail:fragment = shaders/gradient.frag  
```
4. To modify a preset shader, reference them by path. Relative paths resolves
    against the hyprtail config root. `~` and absolute paths also work.
    See SHADERS.md for more detailed documentation about shader definitions.

### Preset file format

A preset is a file `~/.config/hypr/hyprtail/presets/<name>.conf`:

```
contract    = 2
description = Thin neutral trail
layers      = core

core:vertex   = prefab:ribbon.vert   # can be a prefab shader, or
core:fragment = /shaders/solid.frag  # your file, relative to `#XDG_CONFIG_HOME/hypr/hyprtail`
# core:fragment = ~/.config/hypr/hyprtail/shaders/solid.frag # or absolute path
core:fade_ms  = 350                  # any other key sets a layer parameter
core:width    = 4
core:color    = rgba(ffffffa0)
```

- `contract = 2` is required.
- `layers = <name>[, <name>...]` lists 1–4 layer names, draw order first =
  bottom. No duplicates allowed. `source` can't be a layer name.
- `source = pointer | spring` (optional, default `pointer`) picks what
  produces the trail's points, see "Sources" below.
- `source:<name> = <value>` sets a setting of that source, see below.
- Every other line is `<layer>:<key> = <value>`, where `<layer>` must be one
  of the names in `layers`.
  - `<layer>:vertex` / `<layer>:fragment` pick that layer's shader:
    `prefab:<name>` for an embedded one (geometry: `prefab:ribbon.vert`,
    `prefab:scatter.vert`, `prefab:drift.vert`, `prefab:halo.vert`; looks:
    `prefab:gradient.frag`, `prefab:dots.frag`, `prefab:pulse.frag`,
    `prefab:sizzle.frag`), or
    anything else as a path to your own file. 
  - Any other `<key>` sets a default value for a parameter that layer's
    shader declares (see SHADERS.md for how shaders declare parameters).
- `#` starts a comment to end of line; blank lines are ignored.
- If the selected preset fails to load (missing file, parse error, bad
  shader reference), hyprtail reports it and uses `prefab:subtle` instead.

### Sources

Every layer of a preset draws from one shared source of points, so the
source belongs to the preset (`source = ...`), not to a layer.

- **`pointer`** (default): the recorded history of the pointer, one point per
  `min_spacing` px of travel, kept in a buffer of `capacity` points. No
  settings.
- **`spring`**: a chain of `capacity` points that chase each other, so the
  points are fixed and always there; nothing is recorded. The first point
  chases the pointer and each of the others the point before it, each axis an
  independent damped spring. Settings, set as `source:<name> = <value>` in the
  preset or `source:<name>=<value>` in `params` (same value rules and
  precedence as layer parameters: the source's default < the preset < `params`;
  applied live):

  | Name | Default | Range | Meaning |
  |---|---|---|---|
  | `mass` | `1` | 0.01–100 | Heavier points answer more slowly. |
  | `stiffness` | `30000` | 1–1000000 | How hard a point is pulled toward the one before it. Large, because every link adds lag. |
  | `damping` | `200` | 0–10000 | Resistance to motion. The damping ratio is `damping / (2 * sqrt(stiffness * mass))`: under 1 bounces, over 1 doesn't. |
  | `age_step_ms` | `10` | 0–1000 | How much older each point is than the one in front of it. It tapers a ribbon toward the tail; with a layer's `fade_ms` it sets how many points are visible (`fade_ms / age_step_ms`). |

  A chain has no severed pieces to fade, so anything that would start a new
  segment in a pointer history (screen lock, pointer constraint, a warp in
  `break` mode, a workspace change, a monitor layout change) restarts the
  chain at the pointer instead. The chain stops redrawing once it has stopped
  moving and every layer's `fade_ms` has passed.

`hyprctl hyprtail` shows which source is active and whether its points are
still moving.

## Overriding a preset's shaders

| Key | Type | Default |
|---|---|---|
| `layer1_vertex` … `layer4_vertex` | path | `""` (use the preset's shader) |
| `layer1_fragment` … `layer4_fragment` | path | `""` (use the preset's shader) |

These pick a shader by the layer's **position** in the active preset's
`layers` list (1 = bottom layer), not by name. `""` means "use whatever the
preset itself specifies for that layer." Setting an index the active preset
doesn't have (e.g. `layer3_vertex` on a 1-layer preset) is a warning and is
ignored. They take file paths only (no `prefab:` form). Path resolution is
the same as for a preset's own shader paths: `~`/`~/` expand to your home
directory, an absolute path is used as-is, and a relative path resolves
against the hyprtail config root (`~/.config/hypr/hyprtail/`).

That last rule is a deliberate choice: one base directory for everything
hyprtail reads, so `solid.frag` means the same file in a preset and in
`layer1_fragment`. Hyprland's own `decoration:screen_shader` resolves
relative paths against the main Hyprland config directory instead, and
hyprtail diverges from that on purpose, trading familiarity with that one
setting for consistency inside hyprtail.

## Overriding layer parameters

| Key | Type | Default |
|---|---|---|
| `params` | string | `""` |

One string, space-separated entries of the form `<layer>:<name>=<value>`:

```
plugin:hyprtail:params = core:width=4 glow:radius=18 core:color=rgba(ffffffa0)
```

- `<layer>` is a layer name from the active preset, or `source` for the
  preset's source (see "Sources").
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
  separate reload needed. That includes the copy count K of an `instanced`
  layer (`prefab:jitter`'s `copies`, `prefab:spray`'s `count`): it changes
  live, without recompiling the shader.

**Reserved parameter names**, settable on any layer the same way as any
other parameter:

| Name | Type | Default | Meaning |
|---|---|---|---|
| `enabled` | bool | `true` | `false` turns this layer off entirely (not even compiled). |
| `draw_when_cursor_hidden` | bool | `true` for `path` and `instanced` layers, `false` for `quad` layers | Whether the layer keeps drawing while the OS cursor is hidden. |
| `fade_ms` | float, ms | `500` | Trail/particle layers: how long a point stays visible after it's created. |
| `start_ms` | float, ms | `500` | Idle-style layers: how long the pointer must sit still before the layer starts showing. |
| `duration_ms` | float, ms | `1500` | Idle-style layers: how long it stays visible once started (`0` = forever, until the pointer moves). |

Example: `params = "glow:enabled=false"` turns off a layer named `glow`
without switching presets.

