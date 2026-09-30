#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology instanced count
// hyprtail "spray" preset: K particles per trail node, each drifting away
// from where its node was born as the node ages. Also the reference for
// instanced-topology geometry shaders whose reach GROWS with age: the
// padding has to cover the furthest a particle can get before fade_ms, so it
// is written in terms of fade_ms (a reserved parameter, allowed in padding
// expressions) and the speed and wobble limits below.
//
// A particle's heading comes from its node's seed, biased to trail behind the
// pointer using the node's velocity direction (its DIRECTION only, so the
// reach doesn't depend on how fast the pointer moved). See jitter.vert for
// the instanced-topology summary.

#include "helpers/fade.glsl"
#include "helpers/noise.glsl"

#pragma hyprtail param int count 4 1 64
#pragma hyprtail param float size 3 0 128
// Top speed of a particle, px per second (each gets 40-100% of it).
#pragma hyprtail param float speed 60 0 4000
// Sideways wobble amplitude, px.
#pragma hyprtail param float wobble 6 0 256
// 0 = random heading, 1 = straight behind the pointer's motion.
#pragma hyprtail param float trailing 0.6 0 1
// Farthest a particle can be from its node: top speed for fade_ms, plus the
// wobble, its own size and the ~1px soft edge.
#pragma hyprtail padding speed * fade_ms / 1000 + wobble + size + 1

const float TAU = 6.28318530718;

void main() {
    ht_initVaryings();

    HtNode n = ht_node();
    if (ht_faded(n.age, fade_ms)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    float life = ht_life(n.age, fade_ms);
    float t    = min(n.age, fade_ms) * 0.001; // s, capped: the padding's bound

    uint  i     = uint(ht_instance());
    float angle = TAU * ht_rand(n.seed, 3u * i);
    vec2  dir   = vec2(cos(angle), sin(angle));

    // Lean toward "behind the pointer" when the node has a velocity.
    float vl = length(n.vel);
    if (vl > 1e-4) {
        vec2  lean = mix(dir, -n.vel / vl, trailing);
        float ll   = length(lean); // 0 when the heading cancels the lean exactly
        if (ll > 1e-4)
            dir = lean / ll;
    }

    float v    = speed * (0.4 + 0.6 * ht_rand(n.seed, 3u * i + 1u));
    vec2  side = vec2(-dir.y, dir.x);

    // Slow noise along the flight path, -1..1 scaled by wobble; the noise
    // input moves with age and differs per particle.
    float w = (ht_noise(vec2(t * 3.0, ht_rand(n.seed, 3u * i + 2u) * 64.0)) * 2.0 - 1.0) * wobble;

    vec2 c = ht_corner();
    vec2 p = n.pos + dir * v * t + side * w * min(t, 1.0) + c * size * life;

    ht_vLocal   = c;
    ht_vAge     = n.age;
    ht_vLife    = life;
    ht_vSpeed   = v * 0.001; // px/ms, like the other topologies
    ht_vDist    = n.dist;
    ht_vSeed    = ht_rand(n.seed, 4096u + i);
    gl_Position = ht_toClip(p);
}
