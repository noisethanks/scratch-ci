// hyprtail prefab: signed-distance helpers for drawing shapes in a fragment
// shader (the idle/presence slot, or anything else quad-based).
//
//   #include "hyprtail/sdf.glsl"
//
// FRAGMENT SHADERS ONLY: ht_coverage uses fwidth, which vertex shaders don't
// have, and the whole file fails to compile there.
//
// Distances are in the same units as p (the stock idle shaders use logical
// px): negative inside, positive outside. Functions only, prefixed ht_.

// Filled circle of radius r around the origin.
float ht_sdCircle(vec2 p, float r) {
    return length(p) - r;
}

// Ring (annulus) centered on a circle of radius r, halfWidth thick on each
// side.
float ht_sdRing(vec2 p, float r, float halfWidth) {
    return abs(length(p) - r) - halfWidth;
}

// Antialiased coverage (0..1) of the shape with signed distance d: about one
// pixel of smoothing across the edge, independent of scale.
float ht_coverage(float d) {
    float w = max(fwidth(d), 1e-4);
    return clamp(0.5 - d / w, 0.0, 1.0);
}
