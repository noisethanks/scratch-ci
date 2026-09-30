
// Topology instanced: K copies of a 4-vertex triangle strip for every visible
// node (age < fade_ms), oldest node first. K is the number in "topology
// instanced K", or the value of the int param it names; it is at most 64.
// Copy i of a node is ht_instance(), 0 .. ht_K - 1. The node's attributes
// advance once per K instances, so every copy of a node reads the same node.
layout(location = 0) in vec2  ht_a_pos;
layout(location = 1) in float ht_a_birth;
layout(location = 2) in vec2  ht_a_vel;
layout(location = 3) in float ht_a_dist;
layout(location = 4) in uint  ht_a_bits;

uniform int ht_K; // copies per node

HtNode ht_node() {
    return ht_makeNode(ht_a_pos, ht_a_birth, ht_a_vel, ht_a_dist, ht_a_bits);
}

// Which copy of the node this vertex belongs to, 0 .. ht_K - 1.
int ht_instance() {
    return gl_InstanceID % ht_K;
}

// Corner of this vertex's quad, -1..1 on both axes. Stay within the layer's
// padding of the node position: that is what is damaged (plus 1px).
vec2 ht_corner() {
    return vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;
}
