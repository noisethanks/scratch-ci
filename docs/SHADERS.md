# Writing hyprtail shaders (contract 2)

hyprtail shaders are plain GLSL ES 3.00 `.vert`/`.frag` files. You never
declare the plugin's attributes or uniforms yourself — a prelude the loader
injects does that. This document covers everything the prelude and loader
provide.

A **layer** is one vertex shader (geometry) + one fragment shader (shading)
+ its own parameter values. A preset stacks up to 4 layers; see CONFIG.md
for how a preset picks which shaders a layer uses.

Two topologies are currently implemented: `path` (the trail ribbon) and
`quad` (a fixed square around the pointer, for idle/presence effects).
Contract 2's design also names `path smooth N` and `instanced K`, but
those are not built yet — don't rely on them.

## Minimum required pragma

Every shader file (both vertex and fragment) must start:

```glsl
#version 300 es
#pragma hyprtail contract 2
```

`#pragma hyprtail contract 2` must be the very first thing after
`#version` — before any other `#pragma hyprtail` line and before any
`#include`. The loader replaces this line with the prelude, which sets
`precision highp float`/`int` and declares everything below. A missing or
unsupported contract version is refused with a message naming the
supported range.

## All pragmas

| Pragma | Where | Required | Meaning |
|---|---|---|---|
| `#pragma hyprtail contract 2` | every file, first line after `#version` | yes | Declares contract version; triggers prelude injection. |
| `#pragma hyprtail topology path\|quad` | vertex (geometry) shader, main file only | yes, exactly once | What kind of geometry this shader produces. |
| `#pragma hyprtail expects <kind>[,<kind>...]` | fragment shader, main file only | no, at most once | Refuses to pair with a vertex shader whose topology isn't in this list. Comma-separated, no spaces, e.g. `path,quad`. |
| `#pragma hyprtail param <type> <name> <default> [<min> <max>]` | either stage, anywhere | no | Declares a shader-controlled parameter. Becomes `uniform <glsl-type> <name>;` in place. |
| `#pragma hyprtail padding <expr>` | either stage or an include | no | How far past the node/anchor position this layer draws, in px. The largest declaration across the whole program counts. |

Anything else spelled `#pragma hyprtail ...` that isn't one of the above is
an error.

### `topology`

```glsl
#pragma hyprtail topology path   // per-segment ribbon geometry
#pragma hyprtail topology quad   // one fixed quad around the pointer
```

A vertex shader with no topology pragma, more than one, or an unrecognized
kind is refused.

### `expects`

```glsl
#pragma hyprtail expects path
```

If the fragment shader you pair this with a vertex shader whose declared
topology isn't in the list, you get a plain refusal, e.g.:

> `glow.frag expects topology path; dots.vert declares instanced 8`

Leaving `expects` off means the fragment shader accepts any topology (as
long as it only reads the standard varyings — see below).

### `param`

```glsl
#pragma hyprtail param float width 8 0 512
#pragma hyprtail param float speed_ref 2 0.001 1000
#pragma hyprtail param color color_slow rgba(1a66ffff)
#pragma hyprtail param bool glow false
#pragma hyprtail param vec2 jitter 0,0 -8 8
```

- Types: `float`, `int`, `bool`, `vec2`, `color`. `min`/`max` apply to
  `float`, `int`, `vec2` (both components); `bool` and `color` take no
  range.
- Names: lowercase letter or `_` first, then letters/digits/`_`. Names
  starting with `ht_` or `gl_` are reserved and refused, as are the
  reserved lifecycle names below.
- A parameter declared in both stages must match exactly (type, default,
  range) — not just have the same type.
- `color` values use `0xAARRGGBB`, `rgba(RRGGBBAA)`, or `rgb(RRGGBB)` (not
  Hyprland's `rgba(r, g, b, a)` decimal form). A `color` param is delivered
  to the shader as `vec4` already converted to the output's color
  management (see **Color management** below) — write it as-is, don't
  reconvert it.
- Values are set, in increasing priority: the pragma's own default < the
  active preset's value for that parameter < the `params` config string
  (CONFIG.md). Setting one from a config file or `params` is validated
  against the pragma's declared type and range; a bad value is a warning
  and the default/previous value is kept, never a hard failure.

**Reserved names** you cannot declare as a `param` (the prelude already
provides them, see below): `fade_ms`, `start_ms`, `duration_ms`, `enabled`,
`draw_when_cursor_hidden`.

### `padding`

```glsl
#pragma hyprtail padding width * 0.5 * miter_limit + 1
```

An expression of numbers, this program's own parameter names, `+ - * /`
and parentheses — nothing else. It tells hyprtail how far outside the raw
node positions (or, for `quad`, outside the anchor point) this layer
actually draws, so the screen-redraw region is big enough to cover it. If
your fragment shader adds a soft glow or blur that reaches past the
geometry the vertex shader emits, declare it here — otherwise you'll see
truncated or ghosted edges. The largest `padding` declaration in the whole
program (vertex + fragment + any include) is the one that counts, and
`damage_padding` (CONFIG.md) adds on top of it.

## Built-in uniforms (every layer, every stage)

Declared by the prelude, always available, never declared by you:

| Uniform | Type | Meaning |
|---|---|---|
| `ht_proj` | `mat3` | Global layout px → clip space, for this monitor. |
| `ht_nowMs` | `float` | Current time, same reference as node birth times — only differences are meaningful. |
| `ht_stillMs` | `float` | Time since the pointer last moved, ms. |
| `ht_anchor` | `vec2` | Pointer position, global layout px (what `quad` layers center on). |
| `ht_extentPx` | `float` | This layer's total reach: its padding expression plus `damage_padding`. |
| `fade_ms` | `float` | Reserved lifecycle parameter, see below. |
| `start_ms` | `float` | Reserved lifecycle parameter, see below. |
| `duration_ms` | `float` | Reserved lifecycle parameter, see below. |

## Reserved lifecycle parameters

Every layer has these regardless of what its shaders declare. They're
settable exactly like a shader `param` (preset manifest, `params` string —
see CONFIG.md), but read on the CPU side for visibility/timing rather than
being arbitrary shader knobs:

| Name | Type | Default | Meaning |
|---|---|---|---|
| `enabled` | bool | `true` | `false` disables the whole layer (not compiled). |
| `draw_when_cursor_hidden` | bool | `true` | Whether the layer keeps drawing while the OS cursor is hidden. |
| `fade_ms` | float, ms, 1–60000 | `500` | `path` topology: a node stops being visible once its age passes this. |
| `start_ms` | float, ms, 0–60000 | `500` | `quad` topology: the layer becomes visible once the pointer has been still this long. |
| `duration_ms` | float, ms, 0–600000 | `1500` | `quad` topology: how long it then stays visible (`0` = forever, until the pointer moves). |

## Node accessor API (`path` topology, vertex shader)

A `path`-topology vertex shader runs once per **visible segment**
(instanced), 4 vertices per instance (a triangle strip). Each instance
connects two consecutive trail points, `p0` (older) → `p1` (newer); `prev`
and `next` are the neighboring points, for computing miter joins. You never
touch the raw vertex attributes — the prelude exposes them through:

```glsl
struct HtNode {
    vec2  pos;          // global layout px, emit offset already applied
    float age;          // ms since this node was created (ht_nowMs - birth)
    vec2  vel;          // px/ms at creation, (0,0) at a segment start
    float dist;         // px traveled since the start of this node's segment
    float seed;         // 0..1, stable for the node's whole life
    bool  segmentStart; // true if this node isn't connected to the one before it
};

HtNode ht_prev();  // neighbor before p0 (only pos/seed/segmentStart meaningful)
HtNode ht_p0();    // older end of this segment
HtNode ht_p1();    // newer end of this segment
HtNode ht_next();  // neighbor after p1 (only pos/seed/segmentStart meaningful)

bool  ht_atEnd();  // true if this vertex is at p1 rather than p0
float ht_side();   // -1.0 or 1.0: which side of the ribbon's centerline
```

At the very start/end of the visible trail, `prev`/`next` are copies of the
nearest real node (so a direction computed from them comes out
zero-length — handle that, or use `ht_dirBetween`'s fallback, below).

## Node accessor API (`quad` topology, vertex shader)

A `quad`-topology vertex shader runs once, 4 vertices (a triangle strip),
no node data at all — just a square centered on `ht_anchor`:

```glsl
vec2 ht_corner(); // this vertex's corner, (-1,-1)..(1,1)
```

Typical body: `gl_Position = ht_toClip(ht_anchor + ht_corner() * ht_extentPx);`

## Shared vertex-shader helpers

```glsl
void ht_initVaryings();       // zero every standard varying; call first in main()
vec4 ht_toClip(vec2 globalPos); // global layout px -> gl_Position
```

Always call `ht_initVaryings()` before writing any standard varying, so an
unset one reads as a defined zero rather than garbage.

## Standard varyings

Declared by the prelude in both stages (`out` in vertex, `in` in fragment).
Writing them in your geometry shader is what lets any fragment shader pair
with it:

| Varying | Type | Meaning |
|---|---|---|
| `ht_vLocal` | `vec2` | `path`: x = position along the segment (0 at the newer end, 1 at the older end), y = position across the width (-1..1). `quad`: quad coordinates (-1..1, -1..1). |
| `ht_vAge` | `float` | ms since this node/point was created. |
| `ht_vLife` | `float` | 1 at birth, sweeping to 0 over the visibility window (implement your own curve using `fade_ms`/age, or use the fade prefab below). |
| `ht_vSpeed` | `float` | px/ms the pointer was moving at this node's creation. |
| `ht_vDist` | `float` | px traveled since the start of this node's segment. |
| `ht_vSeed` | `float` | 0..1, stable per node — use it for per-point randomness (sparkle, hue jitter). |

## Fragment shader output

```glsl
layout(location = 0) out vec4 ht_fragColor;
```

Output is **premultiplied alpha**: write `vec4(rgb * a, a)`, not
`vec4(rgb, a)`. hyprtail blends with `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`.

## Color management

A `color` parameter is converted on the CPU, at draw time, from Hyprland's
color syntax to whatever color space the current framebuffer needs (SDR
sRGB, HDR, wide-gamut) — the same conversion core uses for its own solid
colors. You get it already converted as `vec4` (rgb converted, `a` = the
configured alpha, passed separately). **Colors you compute or hardcode in
the shader are not managed**: correct on an SDR sRGB output, wrong on
HDR/wide-gamut ones. Prefer a `color` param over a hardcoded `vec3` when
correctness across displays matters.

## Includes

GLSL ES has no `#include`; the loader resolves it before compiling:

```glsl
#include "hyprtail/ribbon.glsl"   // built-in prefab (see below)
#include "helpers.glsl"          // relative to the including file
#include "~/shaders/common.glsl" // absolute / ~-expanded also work
```

- A built-in shader (one of hyprtail's own) may only include `hyprtail/`
  prefabs, not arbitrary paths.
- Each file is included at most once per compile; a cycle is an error;
  include depth is capped at 16.
- An included file must not itself contain `#version`, `contract`, or
  `topology` pragmas.
- Compile errors are reported as `file:line` against the actual source
  file, not the merged text.
- Put `#include` after `precision` concerns are settled — the prelude
  already sets precision, so this is automatic once you put your includes
  after the contract pragma.

## Prefab library

Function-only helpers, `ht_`-prefixed, no uniforms — pass everything as
arguments. `#include` the ones you want.

**`hyprtail/ribbon.glsl`** (vertex or fragment):

```glsl
const float HT_EPS = 1e-3;

vec4 ht_collapsedPosition();
// A clip-space position that draws nothing. Assign to gl_Position for all
// 4 vertices of a segment you want to skip (degenerate, fully faded, etc).

vec2 ht_dirBetween(vec2 a, vec2 b, vec2 fallback);
// Unit direction from a to b, or `fallback` if they nearly coincide.

vec2 ht_jointOffset(vec2 dirIn, vec2 dirOut, float hw, float miterLimit);
// Miter-join corner offset at a joint between incoming/outgoing
// directions, half-width hw, clamped to miterLimit * hw.
```

**`hyprtail/fade.glsl`** (vertex or fragment):

```glsl
float ht_life(float age, float fadeMs); // 1 at age 0, linearly to 0 at fadeMs
bool  ht_faded(float age, float fadeMs);
```

**`hyprtail/sdf.glsl`** (fragment only — uses `fwidth`, won't compile in a
vertex shader):

```glsl
float ht_sdCircle(vec2 p, float r);
float ht_sdRing(vec2 p, float r, float halfWidth);
float ht_coverage(float signedDistance); // ~1px antialiased 0..1 coverage
```

## Packaging rules

- A shader stage is one standalone `.vert` or `.frag` file — never both in
  one file.
- You can override just one stage of a layer and leave the other at its
  built-in/preset default (see CONFIG.md's `layerN_vertex`/`layerN_fragment`
  and per-preset `<layer>:vertex`/`<layer>:fragment`).
- A vertex shader is required to declare `topology`; a fragment shader
  never does.
- Keep shader files ASCII — GLSL ES drivers aren't reliable with UTF-8,
  even inside comments.
- If your vertex and fragment shader disagree on a custom (non-standard)
  varying — one declares it, the other doesn't, or with a different type —
  you get a plain message before any raw driver link log, e.g.:

  > `fragment shader glow.frag reads v_glow, which geometry shader ribbon.vert doesn't write (standard varyings: ht_vLocal, ht_vAge, ht_vLife, ht_vSpeed, ht_vDist, ht_vSeed)`

  Sticking to only the standard varyings avoids this class of error
  entirely and keeps your shader portable (any fragment shader pairs with
  any geometry shader).
- If a shader fails to load, preprocess, compile, or link, the previous
  working version keeps running (or the built-in shader, if there wasn't
  one yet) and the error is reported — a mistake while editing never leaves
  you with a blank trail.

## Worked example: a solid-color fragment shader

The simplest useful custom shader: replace `classic`'s speed-tinted
fragment shader with a single flat color, keeping the stock ribbon
geometry (`classic/ribbon.vert`) unchanged.

```glsl
#version 300 es
#pragma hyprtail contract 2

#pragma hyprtail param color color rgba(ffffffcc)

void main() {
    float d   = abs(ht_vLocal.y);
    float w   = fwidth(ht_vLocal.y);
    float cov = 1.0 - smoothstep(1.0 - w, 1.0, d);
    if (cov <= 0.0)
        discard;

    float a      = color.a * ht_vLife * cov;
    ht_fragColor = vec4(color.rgb * a, a);
}
```

Line by line:

- `#version 300 es` / `#pragma hyprtail contract 2`: required opening pair
  for every shader file.
- `#pragma hyprtail param color color rgba(ffffffcc)`: declares one
  parameter named `color`, type `color`, defaulting to white at ~80%
  alpha. This becomes `uniform vec4 color;`, already color-managed. No
  `expects` pragma is declared, so this fragment shader pairs with any
  vertex shader that writes the standard varyings — including the stock
  `path` ribbon and, harmlessly, `quad` shaders too (it just won't get
  useful values for a `quad` layer, since `ht_vLife` means something
  different there).
- `ht_vLocal.y`: for `path` topology this is -1..1 across the ribbon's
  width, 0 at the centerline. `d = abs(ht_vLocal.y)` is distance from the
  centerline toward either edge.
- `w = fwidth(ht_vLocal.y)`: the screen-space rate of change of that
  coordinate, used to size the antialiased edge to about one pixel
  regardless of zoom or ribbon width.
- `cov = 1.0 - smoothstep(1.0 - w, 1.0, d)`: 1.0 in the ribbon's interior,
  smoothly falling to 0.0 right at the edge — a soft anti-aliased edge
  instead of a hard cutoff.
- `if (cov <= 0.0) discard;`: skip fully-transparent pixels outside the
  ribbon entirely, cheaper than blending zero.
- `a = color.a * ht_vLife * cov`: final alpha combines the parameter's own
  alpha, the point's remaining life (so it fades out with age, using
  `fade_ms`), and the edge coverage.
- `ht_fragColor = vec4(color.rgb * a, a)`: premultiplied output, as
  required.

To use it: put this file next to a preset's `preset.conf` (e.g.
`~/.config/hypr/hyprtail/presets/myPreset/solid.frag`), then in that
`preset.conf`:

```
trail:fragment = solid.frag
trail:color    = rgba(ff2266ff)
```

or override it directly without a custom preset:

```
plugin:hyprtail:layer1_fragment = ~/.config/hypr/hyprtail/solid.frag
plugin:hyprtail:params = trail:color=rgba(ff2266ff)
```

(`trail` here assumes layer 1 of the active preset is named `trail`, true
for both shipped presets.)
