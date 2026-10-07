
// Topology path: one instance per segment p0 -> p1 of the time-ordered trail
// (p0 older, p1 newer), a 4-vertex triangle strip. prev and next are the
// neighbors for joins; only their pos, seed and segmentStart are set. At the
// ends of the trail they're copies of the end node (zero-length direction).
layout(location = 0) in vec2   ht_a_prevPos;
layout(location = 1) in uint   ht_a_prevBits;
layout(location = 2) in vec2   ht_a_p0Pos;
layout(location = 3) in float  ht_a_p0Birth;
layout(location = 4) in vec2   ht_a_p0Vel;
layout(location = 5) in float  ht_a_p0Dist;
layout(location = 6) in uint   ht_a_p0Bits;
layout(location = 7) in vec2   ht_a_p1Pos;
layout(location = 8) in float  ht_a_p1Birth;
layout(location = 9) in vec2   ht_a_p1Vel;
layout(location = 10) in float ht_a_p1Dist;
layout(location = 11) in uint  ht_a_p1Bits;
layout(location = 12) in vec2  ht_a_nextPos;
layout(location = 13) in uint  ht_a_nextBits;

HtNode ht_prev() {
    return ht_makeNode(ht_a_prevPos, ht_nowMs, vec2(0.0), 0.0, ht_a_prevBits);
}
HtNode ht_p0() {
    return ht_makeNode(ht_a_p0Pos, ht_a_p0Birth, ht_a_p0Vel, ht_a_p0Dist, ht_a_p0Bits);
}
HtNode ht_p1() {
    return ht_makeNode(ht_a_p1Pos, ht_a_p1Birth, ht_a_p1Vel, ht_a_p1Dist, ht_a_p1Bits);
}
HtNode ht_next() {
    return ht_makeNode(ht_a_nextPos, ht_nowMs, vec2(0.0), 0.0, ht_a_nextBits);
}

// Corner of this vertex: at p1 (the segment's end) or p0, and which side.
bool ht_atEnd() {
    return (gl_VertexID & 1) == 1;
}
float ht_side() {
    return float((gl_VertexID >> 1) & 1) * 2.0 - 1.0;
}
