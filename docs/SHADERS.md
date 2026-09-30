# Writing hyprtail shaders (contract 2)

hyprtail shaders are plain GLSL ES 3.00 `.vert`/`.frag` files. You never
declare the plugin's attributes or uniforms yourself — a prelude the loader
injects does that. This document covers everything the prelude and loader
provide.

A **layer** is one draw layer of a preset (`trail` and `idle` in the shipped
presets): one vertex shader (geometry) + one fragment shader (shading) + its
own parameter values. A preset stacks up to 4 layers; see CONFIG.md for how
a preset picks which shaders a layer uses. "Layer" means only that. The
prelude, below, is not made of layers.

**The prelude** is the text the loader puts in place of your
`#pragma hyprtail contract 2` line. It is assembled from pieces that are
specific to a **stage** (vertex or fragment) and, for the vertex stage, to a
**topology** (`path`, `quad` or `instanced`): a common piece (precision,
built-in uniforms) for both stages, plus the fragment piece for a fragment
shader, or the vertex piece and then the `path`, `quad` or `instanced` piece
for a vertex shader. The prelude is internal (`shaders/prelude/`); don't copy
or edit it.

Three topologies are implemented: `path` (the trail ribbon), `quad` (a fixed
square around the pointer, for idle/presence effects) and `instanced K` (K
quads per trail node, for particles, spray and jitter). Contract 2's design
also names `path smooth N`, but that is not built yet — don't rely on it.

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
| `#pragma hyprtail topology path\|quad\|instanced <K>` | vertex (geometry) shader, main file only | yes, exactly once | What kind of geometry this shader produces. `instanced` takes K, see below. |
| `#pragma hyprtail expects <kind>[,<kind>...]` | fragment shader, main file only | no, at most once | Refuses to pair with a vertex shader whose topology isn't in this list. Kinds: `path`, `quad`, `instanced` (no K). Comma-separated, no spaces, e.g. `quad,instanced`. |
| `#pragma hyprtail param <type> <name> <default> [<min> <max>]` | either stage, anywhere | no | Declares a shader-controlled parameter. Becomes `uniform <glsl-type> <name>;` in place. |
| `#pragma hyprtail padding <expr>` | either stage or an include | no | How far past the node/anchor position this layer draws, in px. The largest declaration across the whole program counts. |

Anything else spelled `#pragma hyprtail ...` that isn't one of the above is
an error.

### `topology`

```glsl
#pragma hyprtail topology path              // per-segment ribbon geometry
#pragma hyprtail topology quad              // one fixed quad around the pointer
#pragma hyprtail topology instanced 8       // 8 quads per visible node
#pragma hyprtail topology instanced copies  // K is the int param "copies"
```

A vertex shader with no topology pragma, more than one, or an unrecognized
kind is refused. `path` and `quad` take no options.

**K of `instanced`** is either an integer literal from 1 to 64, or the name of
an `int` parameter of the same program. A named K must be declared with a range
inside 1..64 (`#pragma hyprtail param int copies 6 1 64`); a missing
declaration, another type, or a missing or wider range is refused. Its value
is read every draw, so changing it in a preset or through `params`
(`params = "trail:copies=12"`) takes effect on the next config reload, with
no recompile. The status command shows the topology as `instanced 8` or, for a
param, `instanced copies`, and the layer's current parameter values.

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
- Names: lowercase letter or `_` first, then letters/digits/`_`, at most 64
  characters, no `__`. Names starting with `ht_` or `gl_` are reserved and
  refused, as are the reserved lifecycle names below.
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
| `ht_K` | `int` | `instanced` topology only: copies per node (the K of the topology pragma). |
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
| `draw_when_cursor_hidden` | bool | `true` for `path` and `instanced`, `false` for `quad` | Whether the layer keeps drawing while the OS cursor is hidden. |
| `fade_ms` | float, ms, 1–60000 | `500` | `path` and `instanced` topologies: a node stops being visible once its age passes this. |
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

## Node accessor API (`instanced` topology, vertex shader)

An `instanced`-topology vertex shader draws **K copies of a 4-vertex
triangle strip for every visible node**, oldest node first. A node is visible
while its age is below `fade_ms`; the plugin draws only those (it re-points the
node attributes at the first visible node before each draw), so a faded node
costs nothing, unlike a `path` layer, which also draws the segment reaching
back to the next older node. Every copy of a node reads that same node:

```glsl
HtNode ht_node();      // this vertex's node (same HtNode as the path API)
int    ht_instance();  // which copy of the node, 0 .. ht_K - 1
vec2   ht_corner();    // this vertex's corner, (-1,-1)..(1,1)
```

There are no neighbors: no `prev`/`next`, no segment. `ht_node().vel` is the
pointer velocity when the node was created, zero for a segment start. Typical
body (`scatter` stands for your own function, e.g. built on `ht_rand`):

```glsl
HtNode n = ht_node();
uint   i = uint(ht_instance());
vec2   p = n.pos + scatter(n.seed, i) + ht_corner() * size;
gl_Position = ht_toClip(p);
```

**Padding is yours to get right.** Damage is the box of the visible nodes'
positions grown by the padding expression (plus `damage_padding`); anything a
copy draws outside it is not guaranteed to be repainted, and leaves ghosts. Two
disciplines, both shipped:

- *Bounded offset* (`prefab:jitter.vert`): each copy sits at a fixed offset of
  at most `spread` from its node, so `padding spread + size + 1`.
- *Growth with age* (`prefab:spray.vert`): copies drift away as the node ages.
  Write the padding for the farthest point before `fade_ms`, e.g. `padding
  speed * fade_ms / 1000 + wobble + size + 1` (`fade_ms` is allowed in padding
  expressions, being a reserved parameter), and cap the age used in the shader
  at `fade_ms` so it can't outrun it. Use only quantities the padding bounds:
  a node's *direction* (`vel` normalized), not its speed, unless a parameter
  limits that.

Because `instanced` draws `visible nodes x K` instances, every node the trail
keeps costs K copies: keep K and `capacity` (CONFIG.md) reasonable; 64 x 4096
is the limit.

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
| `ht_vLocal` | `vec2` | `path`: x = position along the segment (0 at the newer end, 1 at the older end), y = position across the width (-1..1). `quad`, `instanced`: quad coordinates (-1..1, -1..1). |
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
#include "helpers/ribbon.glsl"   // built-in helper (see below), immutable
#include "common.glsl"           // your own file, relative to the including file
#include "~/shaders/common.glsl" // absolute / ~-expanded also work
```

- `helpers/<name>` is always hyprtail's embedded, immutable helper. Any other
  path is your own file. This is the same rule as `prefab:` in presets
  (CONFIG.md): a prefix means embedded, anything else is yours.
- To edit a helper, copy it from `shaders/helpers/` in the repository and
  include your copy by a relative path that does **not** start with the bare
  `helpers/` prefix, e.g. `#include "./helpers/ribbon.glsl"` (or put the copy
  somewhere else). A plain `"helpers/ribbon.glsl"` is always the built-in,
  even if a `helpers/` directory exists next to your shader.
- A built-in shader (one of hyprtail's own) may only include `helpers/`
  built-ins, not arbitrary paths.
- Each file is included at most once per compile; a cycle is an error;
  include depth is capped at 16.
- An included file must not itself contain `#version`, `contract`, or
  `topology` pragmas.
- Compile errors are reported as `file:line` against the actual source
  file, not the merged text.
- Put `#include` after `precision` concerns are settled — the prelude
  already sets precision, so this is automatic once you put your includes
  after the contract pragma.

## Helper library

Function-only helpers, `ht_`-prefixed, no uniforms — pass everything as
arguments. `#include` the ones you want.

**`helpers/ribbon.glsl`** (vertex or fragment):

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

**`helpers/fade.glsl`** (vertex or fragment):

```glsl
float ht_life(float age, float fadeMs); // 1 at age 0, linearly to 0 at fadeMs
bool  ht_faded(float age, float fadeMs);
```

**`helpers/noise.glsl`** (vertex or fragment): deterministic hashing and
noise, for scattering the copies of an `instanced` node.

```glsl
uint  ht_hash(uint x);                 // 32-bit integer hash
float ht_rand(float seed, uint salt);  // 0..1 from a node seed (HtNode.seed) and a salt
float ht_noise(vec2 p);                // 2D value noise, 0..1
```

`ht_rand(n.seed, salt)` is stable for a (node, salt) pair: vary the salt per
copy and per use (`2u * i`, `2u * i + 1u`) for independent values from one
node seed, and the copies stay where they were from frame to frame.

**`helpers/sdf.glsl`** (fragment only — uses `fwidth`, won't compile in a
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

The simplest useful custom shader: replace `prefab:classic`'s speed-tinted
fragment shader with a single flat color, keeping the stock ribbon
geometry (`prefab:ribbon.vert`) unchanged.

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

To use it: save it as `~/.config/hypr/hyprtail/solid.frag`. Then either
write a preset of your own (CONFIG.md's quickstart: copy `presets/subtle.conf`
to `~/.config/hypr/hyprtail/presets/mine.conf`, set `preset = "mine"`) and in
it change:

```
trail:fragment = solid.frag        # bare = your file, relative to ~/.config/hypr/hyprtail/
trail:color    = rgba(ff2266ff)
```

or override it directly without a custom preset:

```
plugin:hyprtail:layer1_fragment = ~/.config/hypr/hyprtail/solid.frag
plugin:hyprtail:params = trail:color=rgba(ff2266ff)
```

(`trail` here assumes layer 1 of the active preset is named `trail`, true
for both shipped presets.)

**Expect one warning with either setup.** Swapping a layer's shader doesn't
touch its parameter defaults: the preset's own `trail:` values stay in place.
Both shipped presets set `trail:color_slow` and `trail:color_fast`, which
`solid.frag` doesn't declare (it declares `color`). Those two values then name
parameters that aren't in the new program, so each load and config reload
reports a `params:trail` warning (`color_slow: not a parameter of this layer;
ignoring`, and the same for `color_fast`). The layer still draws with
`solid.frag` and `trail:color`; the warning is the only effect. With your own
preset file (the first setup) you can avoid it by deleting the
`trail:color_slow` and `trail:color_fast` lines; with `layer1_fragment` (the
second setup) the preset's values can't be removed, so the warning stays.
