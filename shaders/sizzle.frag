#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail expects quad
// hyprtail look "sizzle": while the pointer rests, short jagged sparks
// crackle out from it now and then. Pairs with halo.vert.
//
// Time is the pointer's stillness (ht_stillMs) past start_ms, cut into
// windows of interval_ms. Each window fires a burst with probability
// `chance`; a burst draws `arcs` sparks at random angles for crackle_ms,
// re-jagging every few frames and dimming out. Everything is hashed from the
// window number, so a burst looks the same on every monitor and every frame
// of it is consistent.
//
// Power: the layer keeps the monitor rendering every frame while it shows,
// bursts or not (see halo.vert). Give it a finite duration_ms to stop.
// Output is PREMULTIPLIED alpha; only color parameters are color-managed.

#include "helpers/noise.glsl"
#include "helpers/sdf.glsl"

#pragma hyprtail param float radius 24 1 1024
// Inner end of a spark (color_a) to its outer tip (color_b).
#pragma hyprtail param color color_a rgba(fff2c0ff)
#pragma hyprtail param color color_b rgba(ff6a1aff)
#pragma hyprtail param int arcs 3 1 8
#pragma hyprtail param float interval_ms 900 50 10000
#pragma hyprtail param float crackle_ms 180 10 2000
// Chance that a window fires a burst, 0..1.
#pragma hyprtail param float chance 0.7 0 1
// Spark core thickness, px.
#pragma hyprtail param float thickness 1.5 0.5 8

const float TAU      = 6.28318530718;
const float JAG_MS   = 40.0; // how often a spark re-jags during a burst
const int   MAX_ARCS = 8;
const int   SEGMENTS = 3; // straight pieces per spark

// Distance from p to segment a-b, and how far along it (0..1) the closest
// point is.
vec2 segment(vec2 p, vec2 a, vec2 b) {
    vec2  pa = p - a;
    vec2  ba = b - a;
    float h  = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return vec2(length(pa - ba * h), h);
}

void main() {
    float e = ht_stillMs - start_ms;
    if (e < 0.0)
        discard;

    float window = floor(e / interval_ms);
    float u      = e - window * interval_ms; // ms into this window
    float seed   = window + 0.5;              // any distinct float works
    if (u >= crackle_ms || ht_rand(seed, 0u) >= chance)
        discard;

    float jag = floor(u / JAG_MS);
    float env = (1.0 - u / crackle_ms) * (0.6 + 0.4 * ht_rand(seed + jag * 0.001, 1u));

    vec2  p     = ht_vLocal * ht_extentPx; // px from the pointer
    float best  = 1e9;                     // distance to the nearest spark
    float along = 0.0;                     // 0 inner end .. 1 outer tip there

    for (int j = 0; j < MAX_ARCS; ++j) {
        if (j >= arcs)
            break;
        uint  salt  = 16u * uint(j) + 2u;
        float angle = TAU * ht_rand(seed, salt);
        vec2  dir   = vec2(cos(angle), sin(angle));
        vec2  side  = vec2(-dir.y, dir.x);
        float r0    = radius * mix(0.3, 0.5, ht_rand(seed, salt + 1u));
        float r1    = radius * 0.8;

        vec2 prev = dir * r0;
        for (int s = 1; s <= SEGMENTS; ++s) {
            float f    = float(s) / float(SEGMENTS);
            float off  = s == SEGMENTS ? 0.0 : (2.0 * ht_rand(seed + jag * 0.001, salt + 2u + uint(s)) - 1.0) * radius * 0.12;
            vec2  next = dir * mix(r0, r1, f) + side * off;
            vec2  hit  = segment(p, prev, next);
            if (hit.x < best) {
                best  = hit.x;
                along = (float(s - 1) + hit.y) / float(SEGMENTS);
            }
            prev = next;
        }
    }

    // Sharp core plus a faint glow around it.
    float core = ht_coverage(best - 0.5 * thickness);
    float glow = 0.35 * exp(-best / (2.0 * thickness));
    vec4  c    = mix(color_a, color_b, along);
    float a    = c.a * env * max(core, glow);
    if (a <= 0.0)
        discard;

    ht_fragColor = vec4(c.rgb * a, a);
}
