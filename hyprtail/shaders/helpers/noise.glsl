// hyprtail builtin: hash and noise helpers, for scattering copies of a node.
//
//   #include "helpers/noise.glsl"
//
// Functions only, prefixed ht_, usable in either stage. Deterministic: the
// same inputs always give the same result, so a node's copies keep their
// place from frame to frame.

// 32-bit integer hash (lowbias32): every input bit affects every output bit.
uint ht_hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A value in [0, 1) for a node seed (HtNode.seed) and a salt. Vary the salt
// per use (per copy, per axis) to get independent values from one seed.
float ht_rand(float seed, uint salt) {
    uint h = ht_hash(floatBitsToUint(seed) ^ ht_hash(salt + 0x9e3779b9u));
    return float(h >> 8) / 16777216.0;
}

// Hash of an integer lattice point, in [0, 1).
float ht_hash2(ivec2 p) {
    uint h = ht_hash(uint(p.x) * 0x1f123bb5u ^ ht_hash(uint(p.y) + 0x68e31da4u));
    return float(h >> 8) / 16777216.0;
}

// 2D value noise in [0, 1): smooth interpolation of lattice hashes.
float ht_noise(vec2 p) {
    vec2  i = floor(p);
    vec2  f = fract(p);
    vec2  u = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(i);
    float a = ht_hash2(c);
    float b = ht_hash2(c + ivec2(1, 0));
    float d = ht_hash2(c + ivec2(0, 1));
    float e = ht_hash2(c + ivec2(1, 1));
    return mix(mix(a, b, u.x), mix(d, e, u.x), u.y);
}
