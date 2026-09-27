
// Standard varyings (the fragment prelude declares the same set as inputs).
// ht_initVaryings() zeroes them; write the ones your geometry means.
out vec2  ht_vLocal; // path: x along the segment (0 newer end, 1 older end), y across (-1..1);
                     // quad: quad coordinates (-1..1, -1..1)
out float ht_vAge;   // ms
out float ht_vLife;  // 1 -> 0 over the visibility window
out float ht_vSpeed; // px/ms at birth
out float ht_vDist;  // path distance from the node's segment start, px
out float ht_vSeed;  // 0..1, stable per node

void ht_initVaryings() {
    ht_vLocal = vec2(0.0);
    ht_vAge   = 0.0;
    ht_vLife  = 0.0;
    ht_vSpeed = 0.0;
    ht_vDist  = 0.0;
    ht_vSeed  = 0.0;
}

// Global layout px -> gl_Position.
vec4 ht_toClip(vec2 globalPos) {
    return vec4(ht_proj * vec3(globalPos, 1.0), 1.0);
}

struct HtNode {
    vec2  pos;          // global layout px, emit offset applied
    float age;          // ms since birth
    vec2  vel;          // px/ms at birth, 0 at a segment start
    float dist;         // px from the start of the node's segment
    float seed;         // 0..1, stable for the node's life
    bool  segmentStart; // not connected to the node before it
};

HtNode ht_makeNode(vec2 pos, float birthMs, vec2 vel, float dist, uint bits) {
    HtNode n;
    n.pos          = pos;
    n.age          = ht_nowMs - birthMs;
    n.vel          = vel;
    n.dist         = dist;
    n.seed         = float(bits >> 1u) / 2147483648.0;
    n.segmentStart = (bits & 1u) != 0u;
    return n;
}
