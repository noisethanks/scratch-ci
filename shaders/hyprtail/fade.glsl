// hyprtail prefab: time-based fade helpers.
//
//   #include "hyprtail/fade.glsl"
//
// Functions only, prefixed ht_. Fade is time-based (SPEC section 4): driven by
// a node's age (nowMs - birthMs), not its position along the trail.

// Remaining life of a node, 1 at birth, 0 once age >= fadeMs. Linear.
float ht_life(float age, float fadeMs) {
    return clamp(1.0 - age / fadeMs, 0.0, 1.0);
}

bool ht_faded(float age, float fadeMs) {
    return age >= fadeMs;
}
