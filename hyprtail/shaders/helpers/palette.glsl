// hyprtail prefab: the shared two-color palette of the built-in shaders.
//
//   #include "helpers/palette.glsl"
//
// Functions only, prefixed ht_, usable in either stage. Every built-in look
// colors with the same params (same names, types and ranges; only the
// color_by default differs), so a preset's colors carry over when a layer
// swaps one built-in fragment shader for another:
//
//   #pragma hyprtail param color color_a ...
//   #pragma hyprtail param color color_b ...
//   #pragma hyprtail param int   color_by <mode> 0 4
//   #pragma hyprtail param float speed_ref 2 0.001 1000
//   #pragma hyprtail param float color_period 200 1 100000
//
// color_by picks what moves the color from color_a (0) to color_b (1):
//   0 speed:    pointer speed when the point was made, color_b at speed_ref
//               px/ms and above
//   1 life:     color_a when a point is born, color_b as it fades out
//   2 distance: along the trail, color_a -> color_b -> color_a every
//               color_period px
//   3 seed:     a fixed random mix per point (per copy, for instanced layers)
//   4 cycle:    each point cycles color_a -> color_b -> color_a every
//               color_period ms of its own age, offset by its seed
// Mode 4 runs on age rather than ht_nowMs on purpose: ht_nowMs is rebased on
// the newest trail point, so it jumps whenever the pointer moves and only
// differences of it are meaningful.

const float HT_TAU = 6.28318530718;

// 0..1 smooth back-and-forth wave over a period.
float ht_wave(float x, float period) {
    return 0.5 - 0.5 * cos(HT_TAU * x / period);
}

// Where between color_a (0) and color_b (1) a fragment sits, for color_by
// `mode`; arguments are the standard varyings plus the two palette params.
float ht_paletteT(int mode, float speed, float life, float dist, float seed, float age, float speedRef, float period) {
    if (mode == 0)
        return clamp(speed / speedRef, 0.0, 1.0);
    if (mode == 1)
        return 1.0 - life;
    if (mode == 2)
        return ht_wave(dist, period);
    if (mode == 3)
        return seed;
    return ht_wave(age + seed * period, period);
}
