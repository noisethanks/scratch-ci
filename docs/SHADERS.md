# Writing hyprtail shaders

hyprtail draws a trail with one to four **layers**. Each layer is a pair of
GLSL ES 3.00 files: a vertex shader (`.vert`, the geometry) and a fragment
shader (`.frag`, the look). hyprtail injects the attributes, uniforms and
helper functions. You write `main()`.

The shipped shaders in [`hyprtail/shaders/`](../hyprtail/shaders/) are the
reference. Each one opens with a comment on its technique. Preset files and
config keys are in [CONFIG.md](CONFIG.md).

## Learning resources

- [The Book of Shaders](https://thebookofshaders.com/): fragment shaders from
  zero. Shaping functions, color, shapes, noise.
- [LearnOpenGL: Shaders](https://learnopengl.com/Getting-started/Shaders):
  vertex and fragment shaders, and how data flows between them.
- [Inigo Quilez: 2D distance functions](https://iquilezles.org/articles/distfunctions2d/):
  signed distance functions for any shape you draw inside a quad.
- [Shadertoy](https://www.shadertoy.com/): examples to study. See
  [Coming from Shadertoy](#coming-from-shadertoy).
- [GLSL ES 3.00 specification](https://registry.khronos.org/OpenGL/specs/es/3.0/GLSL_ES_Specification_3.00.pdf):
  the exact language hyprtail compiles.

## Quickstart

1. Copy `hyprtail/presets/` and `hyprtail/shaders/` from this repository into
   `~/.config/hypr/hyprtail/`.
2. Set `trail = "presets/ink.conf"`.
3. Save this as `~/.config/hypr/hyprtail/shaders/solid.frag`:

   ```glsl
   #version 300 es
   #pragma hyprtail contract 2     // required; replaced by the prelude
   #pragma hyprtail expects path   // pair only with ribbon geometry

   #pragma hyprtail param color color rgba(ffffffcc)  // -> uniform vec4 color;

   void main() {
       float d   = abs(ht_vLocal.y);     // 0 on the centerline, 1 at the edge
       float w   = fwidth(ht_vLocal.y);  // change per pixel: a 1 px edge
       float cov = 1.0 - smoothstep(1.0 - w, 1.0, d);
       if (cov <= 0.0)
           discard;

       float a      = color.a * ht_vLife * cov;  // fades with node age
       ht_fragColor = vec4(color.rgb * a, a);    // premultiplied alpha
   }
   ```

4. In `presets/ink.conf`, point the `ink` layer at it:

   ```
   ink:fragment = shaders/solid.frag
   ink:color    = rgba(ff2266ff)
   ```

   Delete the `ink:color_by`, `ink:color_a` and `ink:color_b` lines.
   `solid.frag` doesn't declare those parameters, and a preset key for an
   undeclared parameter is a warning.
5. Run `hyprctl reload`. After that, saving `solid.frag` (or a file it
   includes) recompiles it live. Preset edits still need `hyprctl reload`.

A broken shader never blanks the trail. See [Errors](#errors).

## Terms

- **Node:** one trail point: position, birth time, velocity, distance and a
  random seed. The preset's source (`pointer` history or `spring` chain, see
  CONFIG.md) makes the nodes.
- **Segment:** a connected run of nodes. A **break** (warp, workspace change,
  screen lock) ends one segment and starts the next.
- **Span:** two consecutive nodes, `p0` (older) and `p1` (newer).
- **Topology:** the shapes a layer draws: `path`, `quad` or `instanced K`.
  The vertex shader declares it.
- **Prelude:** the declarations hyprtail injects in place of the `contract`
  pragma.
- **Parameter (param):** a value your shader declares with a pragma. Presets
  and the `params` config key set it, without a recompile.
- **Damage:** the screen area hyprtail repaints in a frame.
- **Padding:** how far past its nodes (or the pointer, for `quad`) a layer
  draws, in px. It sizes the damage.
- **Premultiplied alpha:** color already multiplied by its alpha.
- **Builtin:** a preset or shader embedded in the plugin (`builtin:ink`,
  `builtin:taper.vert`). It never reads from disk.

## How a layer draws

1. The **vertex shader** runs once per corner of each shape the topology
   draws. It sets the corner's screen position (`gl_Position`) and writes
   **varyings**.
2. The GPU fills each shape and interpolates the varyings across it.
3. The **fragment shader** runs once per covered pixel. It reads the
   interpolated varyings and writes one color.

Only pixels inside the shapes run the fragment shader: make the shapes big
enough for any glow or soft edge.

| Topology | Shapes | Use |
|---|---|---|
| `path` | one 4-vertex strip per span | ribbons, lines |
| `quad` | one square around the pointer | idle and presence effects |
| `instanced K` | K squares per visible node | particles, sprays, dots |

- **Space:** global layout px (logical, shared by all monitors), y down.
  `ht_toClip()` converts a position to `gl_Position`.
- **Time:** no free-running clock. Animate with a node's age (`ht_vAge`,
  `HtNode.age`), or with `ht_stillMs` in a `quad` layer.
- **Blending:** layers draw bottom to top, in the order of the preset's
  `layers` line, with premultiplied alpha (`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`).
  There are no other blend modes.

### Coming from Shadertoy

| Shadertoy | hyprtail |
|---|---|
| `fragColor` | `ht_fragColor`, premultiplied: `vec4(rgb * a, a)` |
| `fragCoord / iResolution.xy` | `ht_vLocal`, coordinates local to the shape |
| `iTime` | `ht_vAge` (node age, ms) or `ht_stillMs` (`quad`) |
| `iMouse` | `ht_anchor`, pointer position in px |
| `uniform float x;` | `#pragma hyprtail param float x ...`; a plain `uniform` is refused |
| `iChannel0`..`3` | none: no textures, no screen sampling |
| one full-screen pass | runs only inside the shapes your vertex shader emits |

## Paths

| What | Where |
|---|---|
| hyprtail config root | `$XDG_CONFIG_HOME/hypr/hyprtail/`, else `~/.config/hypr/hyprtail/` |
| shipped shaders | `hyprtail/shaders/` in this repository |
| helpers | `hyprtail/shaders/helpers/`, included as `helpers/<name>` |
| prelude (reference only) | `hyprtail/shaders/prelude/` |

A layer gets its shaders from:

- its preset: `<layer>:vertex` and `<layer>:fragment`, each a path or
  `builtin:<file>` (`builtin:taper.vert`). Relative paths resolve against the
  config root. A builtin preset always uses embedded shaders.
- the `layer1_vertex` .. `layer4_fragment` config keys, which override the
  preset by layer position (1 = bottom). Paths only. The preset's other keys
  for that layer still apply.

Shipped shaders:

| File | Topology | Draws |
|---|---|---|
| `taper.vert` | `path` | ribbon that tapers with age, optional calligraphy nib. Reference for `path`. |
| `convex.vert` | `path` | thin strip pointed at both ends |
| `scatter.vert` | `instanced` | copies at fixed random offsets. Reference for bounded `instanced`. |
| `drift.vert` | `instanced` | particles that fly off as they age. Reference for growing `instanced`. |
| `halo.vert` | `quad` | square around the pointer. Reference for `quad`. |
| `gradient.frag` | expects `path` | two-color ribbon, hard line to soft glow |
| `hexagons.frag` | expects `path` | hexagon cells along the strip |
| `strands.frag` | expects `path` | two strands winding as a helix |
| `dots.frag` | expects `quad,instanced` | dot, ring or sparkle per square |
| `pulse.frag` | expects `quad` | ring that expands while the pointer rests |
| `sizzle.frag` | expects `quad` | sparks that crackle while the pointer rests |

## Pragmas

Every file starts with:

```glsl
#version 300 es
#pragma hyprtail contract 2
```

The contract pragma comes right after `#version`, before any other
`#pragma hyprtail` or `#include`. One stage per file. Keep files ASCII:
drivers mishandle UTF-8, even in comments.

| Pragma | Stage | Required |
|---|---|---|
| `contract 2` | both | yes, first after `#version` |
| `topology path\|quad\|instanced <K>` | vertex, main file | yes, once |
| `expects <kind>[,<kind>...]` | fragment, main file | no, at most once |
| `param <type> <name> <default> [<min> <max>]` | both, includes too | no |
| `padding <expr>` | both, includes too | no |

Any other `#pragma hyprtail ...` is an error.

### topology

```glsl
#pragma hyprtail topology path
#pragma hyprtail topology quad
#pragma hyprtail topology instanced 8       // 8 copies per node
#pragma hyprtail topology instanced copies  // K from the int param "copies"
```

K is an integer 1..64, or the name of an `int` param whose range is inside
1..64 (`#pragma hyprtail param int copies 6 1 64`). A param K changes on
config reload, without a recompile.

### expects

```glsl
#pragma hyprtail expects path
#pragma hyprtail expects quad,instanced   // comma, no spaces, no K
```

Refuses to pair with a vertex shader of another topology:

> `glow.frag expects topology path; dots.vert declares instanced 8`

Without `expects`, the fragment shader pairs with any vertex shader. Declare
it when the shader reads varyings in a topology-specific way, such as
`ht_vLocal.y` as the position across a ribbon.

### param

```glsl
#pragma hyprtail param float width  8 0 512
#pragma hyprtail param int   copies 6 1 64
#pragma hyprtail param bool  glow   false
#pragma hyprtail param vec2  offset 0,0 -8 8
#pragma hyprtail param color tint   rgba(1a66ffff)
```

Each becomes a `uniform` of the same name, at the pragma's position.

| Type | GLSL | Value syntax | Range |
|---|---|---|---|
| `float` | `float` | `1.5` | optional |
| `int` | `int` | `3` | optional |
| `bool` | `bool` | `true`/`false`, `1`/`0`, `yes`/`no` | none |
| `vec2` | `vec2` | `x,y`, no spaces | optional, applies to both components |
| `color` | `vec4` | `0xAARRGGBB`, `rgba(RRGGBBAA)`, `rgb(RRGGBB)` | none |

- **Names:** a lowercase letter or `_` first, then letters, digits or `_`.
  At most 64 characters. No `__`, no `ht_` or `gl_` prefix, no
  [reserved name](#reserved-parameters).
- **Both stages:** a param declared in both must match exactly (type,
  default, range).
- **Values:** pragma default < preset `<layer>:<name>` < `params` config key.
  A bad value is a warning, and the previous value stays.
- **Colors** arrive converted to the output's color space. See
  [Color](#color).

### padding

```glsl
#pragma hyprtail padding width * 0.5 * miter_limit + 1
```

How far past its node positions (`path`, `instanced`) or the pointer
(`quad`) the layer draws, in px. Too small, and edges get cut off or leave
ghosts.

- Numbers, `+ - * /`, parentheses, and the names of this program's `float`,
  `int` and `bool` params, reserved ones included. No unary minus.
- The largest `padding` across both stages and their includes counts. It is
  clamped to 0..4096.
- The `damage_padding` config key adds to it. `ht_extentPx` holds the sum.

## Uniforms

Declared by the prelude in both stages, except `ht_K`:

| Uniform | Type | Meaning |
|---|---|---|
| `ht_proj` | `mat3` | layout px to clip space, this monitor. Use `ht_toClip()`. |
| `ht_nowMs` | `float` | ms since the newest node's birth. Jumps as nodes arrive: use only for differences. |
| `ht_stillMs` | `float` | ms since the pointer last moved |
| `ht_anchor` | `vec2` | pointer position, px |
| `ht_extentPx` | `float` | this layer's padding plus `damage_padding`, px |
| `ht_K` | `int` | `instanced` vertex shaders only: copies per node |
| `fade_ms`, `start_ms`, `duration_ms` | `float` | reserved parameters, below |

### Reserved parameters

Every layer has these. Set them like any param, in a preset or `params`.
Never declare them.

| Name | Type, range | Default | Effect |
|---|---|---|---|
| `enabled` | bool | `true` | `false` turns the layer off; it isn't compiled |
| `draw_when_cursor_hidden` | bool | `true`, `false` for `quad` | keep drawing while the cursor is hidden |
| `fade_ms` | float, 1..60000 | `500` | `path`, `instanced`: a node shows while its age < `fade_ms` |
| `start_ms` | float, 0..60000 | `500` | `quad`: shows once the pointer is still this long |
| `duration_ms` | float, 0..600000 | `1500` | `quad`: then shows this long; `0` = until the pointer moves |

Only the three `_ms` names are uniforms. hyprtail also uses them to decide
what to draw.

## Vertex shader API

Every topology:

```glsl
void ht_initVaryings();         // zero all standard varyings; call first in main()
vec4 ht_toClip(vec2 globalPx);  // layout px -> gl_Position

struct HtNode {
    vec2  pos;          // px, emit offset applied
    float age;          // ms since birth
    vec2  vel;          // px/ms at birth; (0,0) at a segment start
    float dist;         // px from the start of the node's segment
    float seed;         // 0..1, fixed for the node's life
    bool  segmentStart; // first node of a segment
};
```

### path

One instance per span, 4 vertices each (a triangle strip).

```glsl
HtNode ht_prev();   // node before p0: only pos, seed, segmentStart set
HtNode ht_p0();     // older end of the span
HtNode ht_p1();     // newer end of the span
HtNode ht_next();   // node after p1: only pos, seed, segmentStart set
bool   ht_atEnd();  // true at p1, false at p0
float  ht_side();   // -1.0 or 1.0: side of the centerline
```

Handle these cases (`taper.vert` does all of them):

- `p1.segmentStart`: the span crosses a break. Collapse it: set
  `gl_Position = ht_collapsedPosition()` on all 4 vertices.
- Both ends past `fade_ms`: collapse it. The oldest span drawn can reach back
  past `fade_ms`.
- `p0.segmentStart` or `next.segmentStart`: that neighbor belongs to another
  segment. Don't use it for joins.
- At the ends of the trail, `prev` and `next` copy the end node, so a
  direction from them has zero length. Use `ht_dirBetween()` with a fallback.

### quad

One instance, 4 vertices, no nodes.

```glsl
vec2 ht_corner();   // this vertex's corner, -1..1 on both axes
```

```glsl
gl_Position = ht_toClip(ht_anchor + ht_corner() * ht_extentPx);  // halo.vert
```

Stay within `ht_extentPx` of `ht_anchor`. A `quad` layer redraws every frame
while it shows. A finite `duration_ms` lets the monitor idle again.

### instanced

K instances per visible node, oldest node first, 4 vertices each. All K
copies read the same node. No neighbors.

```glsl
HtNode ht_node();      // this copy's node
int    ht_instance();  // copy index, 0 .. ht_K - 1
vec2   ht_corner();    // corner of this copy's square, -1..1
```

```glsl
#include "helpers/noise.glsl"
#pragma hyprtail param float spread 10 0 256
#pragma hyprtail param float size 3 0 128
#pragma hyprtail padding spread + size + 1
...
HtNode n = ht_node();
uint   i = uint(ht_instance());
vec2   o = vec2(ht_rand(n.seed, 2u * i), ht_rand(n.seed, 2u * i + 1u)) * 2.0 - 1.0;
gl_Position = ht_toClip(n.pos + o * spread + ht_corner() * size);
```

Every copy must stay within the padding of its node. Two ways:

- **Bounded offset** (`scatter.vert`): each copy sits at a fixed offset of at
  most `spread`, as above.
- **Growth with age** (`drift.vert`): write the padding for the farthest
  point reached before `fade_ms`, such as
  `speed * fade_ms / 1000 + size + 1`. Clamp the age the shader uses to
  `fade_ms`. Use a node's direction (`normalize(vel)`), not its speed, unless
  a param bounds the speed.

Cost: visible nodes x K squares per layer, at most 4096 x 64. Keep K and the
`capacity` config key modest.

## Varyings

The prelude declares six standard varyings: `out` in the vertex stage, `in`
in the fragment stage. Shaders that use only these pair freely. The shipped
vertex shaders fill them as follows:

| Varying | Type | `path` | `instanced` | `quad` (`halo.vert`) |
|---|---|---|---|---|
| `ht_vLocal` | `vec2` | x: along the span, 0 at p1, 1 at p0. y: across, -1..1 | corner, -1..1 | corner, -1..1 |
| `ht_vAge` | `float` | node age, ms | node age, ms | `ht_stillMs` |
| `ht_vLife` | `float` | 1 at birth, 0 at `fade_ms` | same | 0 |
| `ht_vSpeed` | `float` | px/ms at birth | px/ms | 0 |
| `ht_vDist` | `float` | px from segment start | px from segment start | 0 |
| `ht_vSeed` | `float` | node seed, 0..1 | per copy, 0..1 | 0 |

These meanings are conventions: your vertex shader decides what goes in.

Custom varyings work between files that both declare them with the same
type. They tie the fragment shader to that vertex shader.

## Fragment output

```glsl
layout(location = 0) out vec4 ht_fragColor;   // declared by the prelude
```

Write premultiplied alpha: `vec4(rgb * a, a)`, not `vec4(rgb, a)`. `discard`
fully transparent pixels: cheaper than blending zero.

### Color

hyprtail converts `color` params on the CPU to the output's color space (SDR
sRGB, HDR, wide gamut), the same way Hyprland converts its own colors. Colors
you hardcode or compute in GLSL are not converted: right on SDR sRGB, wrong
on HDR and wide gamut. Prefer `color` params.

## Includes

```glsl
#include "helpers/noise.glsl"      // embedded helper
#include "common.glsl"             // your file, relative to this file
#include "~/shaders/common.glsl"   // ~ and absolute paths work
```

- `helpers/<name>` is always the embedded helper, even if a `helpers/` folder
  sits next to your shader. To change a helper, copy it and include the copy
  by another path, such as `"./helpers/noise.glsl"`.
- Each file is included once per compile. A cycle is an error. Depth limit:
  16.
- Included files can't contain `#version`, `contract`, `topology` or
  `expects`.
- Compile errors point at `file:line` in the real file.
- Embedded shaders can include only `helpers/`.

## Helpers

Functions only, `ht_`-prefixed, no uniforms: pass everything in.

`helpers/ribbon.glsl`:

```glsl
const float HT_EPS = 1e-3;
vec4 ht_collapsedPosition();                        // gl_Position that draws nothing
vec2 ht_dirBetween(vec2 a, vec2 b, vec2 fallback);  // unit a -> b, or fallback if a ~ b
vec2 ht_jointOffset(vec2 dirIn, vec2 dirOut, float hw, float miterLimit);
// miter corner offset for half-width hw, capped at miterLimit * hw
```

`helpers/fade.glsl`:

```glsl
float ht_life(float age, float fadeMs);   // 1 at age 0, linear to 0 at fadeMs
bool  ht_faded(float age, float fadeMs);  // age >= fadeMs
```

`helpers/noise.glsl` (deterministic: copies keep their place frame to frame):

```glsl
uint  ht_hash(uint x);                 // 32-bit integer hash
float ht_rand(float seed, uint salt);  // 0..1; vary salt per copy and per use
float ht_hash2(ivec2 p);               // 0..1 hash of a lattice point
float ht_noise(vec2 p);                // 2D value noise, 0..1
```

`helpers/sdf.glsl` (fragment only: uses `fwidth`):

```glsl
float ht_sdCircle(vec2 p, float r);
float ht_sdRing(vec2 p, float r, float halfWidth);
float ht_coverage(float d);   // ~1 px antialiased coverage of signed distance d
```

`helpers/palette.glsl`: the two-color palette of `gradient.frag` and
`dots.frag`. Declare the same params, and a preset's colors carry over
between your shader and theirs:

```glsl
#pragma hyprtail param color color_a rgba(1a66ffff)
#pragma hyprtail param color color_b rgba(ff1a1aff)
#pragma hyprtail param int   color_by 0 0 4
#pragma hyprtail param float speed_ref 2 0.001 1000
#pragma hyprtail param float color_period 200 1 100000

const float HT_TAU = 6.28318530718;
float ht_wave(float x, float period);   // 0 -> 1 -> 0 once per period
float ht_paletteT(int mode, float speed, float life, float dist, float seed,
                  float age, float speedRef, float period);

vec4 c = mix(color_a, color_b, ht_paletteT(color_by, ht_vSpeed, ht_vLife,
             ht_vDist, ht_vSeed, ht_vAge, speed_ref, color_period));
```

`color_by` picks what moves the color from `color_a` to `color_b`:
0 speed (`color_b` at `speed_ref` px/ms), 1 life, 2 distance (cycles every
`color_period` px), 3 seed, 4 cycle (every `color_period` ms of age, offset
by seed).

## Errors

Errors show in three places:

- a Hyprland notification,
- `hyprctl hyprtail` (`-j` for JSON): each layer's shaders, topology,
  parameter values and last result,
- `$XDG_STATE_HOME/hyprtail/errors.log`, else
  `~/.local/state/hyprtail/errors.log`.

A shader that fails to load, compile or link never blanks the layer:

- a failed edit of the active files keeps the last working version,
- failed new files (after a config change) fall back to the builtin shader.

Common refusals:

- a plain `uniform`: "uniform `x` is not provided by the plugin". Use a
  `param`.
- a vertex shader without `topology`, or a fragment shader with one.
- `expects` doesn't list the vertex shader's topology.
- a param declared differently in the two stages.
- a fragment `in` the vertex shader doesn't write: "fragment shader
  glow.frag reads `v_glow`, which geometry shader taper.vert doesn't write".

A preset key or `params` entry for an undeclared param is a warning: that
entry is ignored, and the rest still applies.
