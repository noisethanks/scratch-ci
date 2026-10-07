

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

### Nix

A plugin only loads into the exact Hyprland it was built against, so build
hyprtail against your own Hyprland flake input:

```nix
inputs = {
  hyprland.url = "github:hyprwm/Hyprland";
  hyprtail = {
    url = "github:noisethanks/hyprtail";
    inputs.hyprland.follows = "hyprland";
  };
};
```

With Home Manager:

```nix
{ inputs, pkgs, ... }:
{
  wayland.windowManager.hyprland = {
    enable = true;
    plugins = [
      inputs.hyprtail.packages.${pkgs.stdenv.hostPlatform.system}.hyprtail
    ];
  };
}
```

Without Home Manager, load `lib/libhyprtail.so` from that package with
`hyprctl plugin load`.

`overlays.default` adds `hyprlandPlugins.hyprtail`, built against whatever
`hyprland` your pkgs has. hyprtail needs Hyprland 0.55.0 or newer.

## Configuring

```
hl.config({
    plugin = {
        hyprtail = {
            trail       = "prefab:subtle", -- available prefabs:  subtle, classic, comet, embers, jitter, spray, spring, vivid
            -- trail = "presets/subtle.conf", -- or a path to config file relative to hyprtail root 
            capacity     = 64, -- maximum tail length in terms of vertices, int from 2 to 4096
            min_spacing  = 2, -- minimum amount of movement necessary to trigger polling, int from 0 to 256
            warp         = "break", -- warping path calculation method, valid strings: break, line, curve
            damage_padding = 0, -- extra redraw area for wide trails, int from 0 to 4096, increase if trail leaves artifacts
            emit_from    = "hotspot", -- normalized emission point, string, either `hotspot` or two space separated floats eg `0.5 0.5` 
            emit_offset  = {0, 0}, -- additional offset for emission point, vec2 from -128 to 128
            screenshare  = "exclude", -- if trail is visible to screen share, valid strings: 'include','exclude'
            params       = "", -- advanced, refer to CONFIG.md
        },
    },
}) 

```
To disable the trail over specific clients, use this window rule:

```

if hl.plugin.hyprtail then
    hl.window_rule({
        match = { class = "^(mpv)$" },
        ["hyprtail:no_trail"] = true,
    })
end

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

## Notifications and diagnostics

- `hyprctl hyprtail` (add `-j` for JSON) shows the active preset, each
  layer's shader and current parameter values, per-monitor render stats,
  and the errors.log path — useful for checking that a config change
  actually took effect.


## Gallery

## WIP
- Bezier curves for warp transitions.
- Benchmarking
- Catmul-Rom splines
- Cursor graphic shader source.
