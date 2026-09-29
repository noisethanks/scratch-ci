#version 300 es
#pragma hyprtail contract 2
// hyprtail "classic" preset, layer "idle": a single ring that expands from
// the pointer and fades out, once the pointer has been still for start_ms.
// Output is PREMULTIPLIED alpha; only color parameters are color-managed.

#include "helpers/sdf.glsl"

#pragma hyprtail param float radius 24 1 1024
#pragma hyprtail param color color rgba(1a66ffff)

const float RING_HALF_WIDTH = 1.5;    // logical px
const float LOOP_MS         = 1200.0; // pulse period when duration_ms = 0

void main() {
    // Progress through the pulse: once over duration_ms, or looping when
    // the effect runs until the pointer moves.
    float effectMs = ht_stillMs - start_ms;
    float t        = duration_ms > 0.0 ? clamp(effectMs / duration_ms, 0.0, 1.0) : fract(effectMs / LOOP_MS);

    // Expands from a quarter of the radius to the edge, staying inside
    // radius including its thickness and antialiasing.
    float maxR = max(radius - RING_HALF_WIDTH - 1.0, 0.0);
    float r    = mix(0.25, 1.0, t) * maxR;

    vec2  local = ht_vLocal * ht_extentPx; // px from the pointer
    float cov   = ht_coverage(ht_sdRing(local, r, RING_HALF_WIDTH));
    float a     = color.a * (1.0 - t) * cov;
    if (a <= 0.0)
        discard;

    ht_fragColor = vec4(color.rgb * a, a);
}
