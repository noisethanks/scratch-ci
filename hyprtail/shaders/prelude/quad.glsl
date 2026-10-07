
// Topology quad: one 4-vertex triangle strip around ht_anchor (the pointer),
// no nodes. Stay within ht_extentPx of the anchor: that's what is damaged
// (plus 1px).
vec2 ht_corner() { // -1..1 on both axes
    return vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;
}
