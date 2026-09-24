#version 300 es
// hyprtail idle/presence fragment shader (stock): a single ring that
// expands from the pointer and fades out. Also the reference for
// user-supplied idle fragment shaders.
//
// Contract: see shaders/idle.vert. Output is PREMULTIPLIED alpha:
// write vec4(rgb * a, a). Only the palette uniforms are color-managed.
// Draw inside the square (|v_local| <= extentPx).

precision highp float;

#include "hyprtail/sdf.glsl"

uniform float radiusPx;
uniform float idleMs;
uniform float durationMs;
uniform vec4  colorSlow;

in vec2 v_local;
in vec2 v_uv;

layout(location = 0) out vec4 fragColor;

const float RING_HALF_WIDTH = 1.5;    // logical px
const float LOOP_MS         = 1200.0; // pulse period when durationMs = 0

void main() {
    // Progress through the pulse: once over the configured duration, or
    // looping when the effect runs until the pointer moves.
    float t = durationMs > 0.0 ? clamp(idleMs / durationMs, 0.0, 1.0) : fract(idleMs / LOOP_MS);

    // Expands from a quarter of the radius to the edge, staying inside
    // radiusPx including its thickness and antialiasing.
    float maxR = max(radiusPx - RING_HALF_WIDTH - 1.0, 0.0);
    float r    = mix(0.25, 1.0, t) * maxR;

    float cov = ht_coverage(ht_sdRing(v_local, r, RING_HALF_WIDTH));
    float a   = colorSlow.a * (1.0 - t) * cov;
    if (a <= 0.0)
        discard;

    fragColor = vec4(colorSlow.rgb * a, a);
}
