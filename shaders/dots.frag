#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail expects quad,instanced
// hyprtail look "dots": one small shape per quad -- a soft dot, a ring or a
// four-point sparkle -- that can twinkle. Shades the quad coordinates
// (ht_vLocal, -1..1 on both axes) of an instanced or quad layer, so it pairs
// with any geometry shader of those topologies that writes them.
//
// Colors use the shared palette (helpers/palette.glsl); the default, seed,
// gives every copy its own fixed mix of color_a and color_b. Output is
// PREMULTIPLIED alpha; only color parameters are color-managed.

#include "helpers/palette.glsl"

#pragma hyprtail param color color_a rgba(ffffffff)
#pragma hyprtail param color color_b rgba(1a66ffff)
// 0 speed, 1 life, 2 distance, 3 seed, 4 cycle: see helpers/palette.glsl.
#pragma hyprtail param int color_by 3 0 4
#pragma hyprtail param float speed_ref 2 0.001 1000
#pragma hyprtail param float color_period 200 1 100000
// 0 = hard edge, 1 = fades from the centre.
#pragma hyprtail param float softness 0.6 0 1
// 0 dot, 1 ring, 2 four-point sparkle.
#pragma hyprtail param int shape 0 0 2
// How deep the brightness flickers, 0 = steady, 1 = fully on and off.
#pragma hyprtail param float twinkle 0 0 1
// Flickers per second; each copy is offset by its seed.
#pragma hyprtail param float twinkle_hz 6 0 60

// Distance-like value per shape: below 1 inside, 1 at the edge.
float shapeDistance(vec2 p) {
    if (shape == 1)
        return abs(length(p) - 0.7) / 0.3;
    if (shape == 2)
        return sqrt(abs(p.x)) + sqrt(abs(p.y));
    return length(p);
}

void main() {
    float d   = shapeDistance(ht_vLocal);
    float aa  = fwidth(d);
    float cov = 1.0 - smoothstep(1.0 - max(softness, aa), 1.0, d);

    float flicker = 0.5 + 0.5 * sin(HT_TAU * (ht_vAge * 0.001 * twinkle_hz + ht_vSeed));
    float t       = ht_paletteT(color_by, ht_vSpeed, ht_vLife, ht_vDist, ht_vSeed, ht_vAge, speed_ref, color_period);
    vec4  c       = mix(color_a, color_b, t);

    float a = c.a * ht_vLife * cov * mix(1.0, flicker, twinkle);
    if (a <= 0.0)
        discard;

    ht_fragColor = vec4(c.rgb * a, a);
}
