#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology instanced copies
// hyprtail geometry "scatter": K scattered copies of every trail node. Also
// the reference for instanced-topology geometry shaders whose reach is
// BOUNDED: each copy sits at a fixed random offset (at most `spread`) from its
// node, so the padding is a constant.
//
// Instanced topology in short (SPEC section 13.3):
//   - "#pragma hyprtail topology instanced <K>": K is a literal 1..64, or the
//     name of an int param declared with a range inside 1..64 (as here, so
//     `params = "trail:copies=12"` changes it live). The layer draws K
//     4-vertex triangle strips for every visible node (age < fade_ms).
//   - ht_node() is this vertex's node (pos, age, vel, dist, seed,
//     segmentStart). ht_instance() is its copy, 0 .. K-1, and ht_corner() the
//     corner of the quad, -1..1 on both axes.
//   - Everything else is as in ribbon.vert: standard varyings, param and
//     padding pragmas. The padding is how far a copy can get from its node,
//     counting its own size; it must hold at every age below fade_ms.
//   - helpers/noise.glsl gives seeded randomness: ht_rand(node.seed, salt)
//     is 0..1 and stable for a (node, salt) pair.

#include "helpers/fade.glsl"
#include "helpers/noise.glsl"

#pragma hyprtail param int copies 6 1 64
#pragma hyprtail param float size 3 0 128
#pragma hyprtail param float spread 10 0 512
// Furthest a copy's edge gets from its node, plus the ~1px soft edge.
#pragma hyprtail padding spread + size + 1

const float TAU = 6.28318530718;

void main() {
    ht_initVaryings();

    HtNode n    = ht_node();
    float  life = ht_life(n.age, fade_ms);
    if (ht_faded(n.age, fade_ms)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    // A point in the disc of radius `spread` around the node, from the
    // node's seed and this copy's number: fixed for the copy's life.
    uint  i      = uint(ht_instance());
    float angle  = TAU * ht_rand(n.seed, 2u * i);
    float radius = spread * sqrt(ht_rand(n.seed, 2u * i + 1u));
    vec2  offset = radius * vec2(cos(angle), sin(angle));

    // Copies shrink as they age, like the trail's own fade.
    vec2 c = ht_corner();
    vec2 p = n.pos + offset + c * size * life;

    ht_vLocal   = c;
    ht_vAge     = n.age;
    ht_vLife    = life;
    ht_vSpeed   = length(n.vel);
    ht_vDist    = n.dist;
    ht_vSeed    = ht_rand(n.seed, 4096u + i); // per copy, for color variation
    gl_Position = ht_toClip(p);
}
