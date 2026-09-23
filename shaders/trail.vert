#version 300 es
// hyprtail trail vertex shader (stock ribbon).
//
// Contract for trail vertex shaders (built-in and user-supplied):
//
// One instance per segment node[i] -> node[i+1] of the time-ordered trail,
// drawn as a 4-vertex TRIANGLE_STRIP; gl_VertexID picks the corner:
//   bit 0: 0 = segment start (p0), 1 = segment end (p1)
//   bit 1: 0 = side -1, 1 = side +1
//
// Per-instance attributes (all read from the same buffer, divisor 1):
//   location 0  a_prevPos    vec2   node i-1 position
//   location 1  a_prevFlags  float  node i-1 flags
//   location 2  a_p0Pos      vec2   node i   position
//   location 3  a_p0BirthMs  float  node i   birth time
//   location 4  a_p0Vel      vec2   node i   velocity, px/ms
//   location 5  a_p0Flags    float  node i   flags
//   location 6  a_p1Pos      vec2   node i+1 position
//   location 7  a_p1BirthMs  float  node i+1 birth time
//   location 8  a_p1Vel      vec2   node i+1 velocity, px/ms
//   location 9  a_p1Flags    float  node i+1 flags
//   location 10 a_nextPos    vec2   node i+2 position
//   location 11 a_nextFlags  float  node i+2 flags
// Positions are global layout (logical) pixels. Flags bit 0 set = this node
// starts a new segment, i.e. it is NOT connected to the node before it
// (workspace switch, trail start). At the ends of the trail, prev/next are
// copies of the end node (zero-length direction), handle that as "no
// neighbor".
//
// Uniforms:
//   mat3  proj        global layout px -> clip space, per monitor
//   float nowMs       current time, same reference as the birth times; only
//                     age = nowMs - birthMs is meaningful
//   float fadeMs      lifetime of a node; age >= fadeMs is fully faded
//   float widthPx     full ribbon width at age 0, logical px
//   float miterLimit  max miter length, in half-widths
//   float speedRef    speed (px/ms) mapped to the fast end of the palette
//
// Damage: the plugin damages the node extent padded by
// widthPx / 2 * miterLimit + 1px. Geometry outside that is not guaranteed to
// be repainted (SPEC section 5).
//
// Instances that should draw nothing are moved outside clip space.

precision highp float;

uniform mat3  proj;
uniform float nowMs;
uniform float fadeMs;
uniform float widthPx;
uniform float miterLimit;
uniform float speedRef;

layout(location = 0) in vec2  a_prevPos;
layout(location = 1) in float a_prevFlags;
layout(location = 2) in vec2  a_p0Pos;
layout(location = 3) in float a_p0BirthMs;
layout(location = 4) in vec2  a_p0Vel;
layout(location = 5) in float a_p0Flags;
layout(location = 6) in vec2  a_p1Pos;
layout(location = 7) in float a_p1BirthMs;
layout(location = 8) in vec2  a_p1Vel;
layout(location = 9) in float a_p1Flags;
layout(location = 10) in vec2 a_nextPos;
layout(location = 11) in float a_nextFlags;

out float v_side;  // -1 .. +1 across the ribbon
out float v_alpha; // time-based fade
out vec3  v_color;

const float EPS = 1e-3;

bool startsSegment(float flags) {
    return mod(flags, 2.0) >= 1.0;
}

void collapse() {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_side      = 0.0;
    v_alpha     = 0.0;
    v_color     = vec3(0.0);
}

// Unit direction from a to b, or fallback if they (nearly) coincide.
// Both instances meeting at a joint compute the joint's directions through
// this same function from the same inputs, so they get the same corners.
vec2 dirBetween(vec2 a, vec2 b, vec2 fallback) {
    vec2  d = b - a;
    float l = length(d);
    return l > EPS ? d / l : fallback;
}

// Offset of the +side corner at a joint between an incoming and an outgoing
// direction, for half-width hw. Miter join, length clamped to
// miterLimit * hw (sharp turns get a pulled-in corner instead of a spike).
vec2 jointOffset(vec2 dirIn, vec2 dirOut, float hw) {
    vec2  nIn  = vec2(-dirIn.y, dirIn.x);
    vec2  nOut = vec2(-dirOut.y, dirOut.x);
    vec2  m    = nIn + nOut;
    float ml   = length(m);
    if (ml < EPS) // full reversal: no usable miter
        return nOut * hw;
    m /= ml;
    float c = dot(m, nOut); // cos(half the turn angle)
    return m * (hw / max(c, 1.0 / miterLimit));
}

float life(float age) {
    return clamp(1.0 - age / fadeMs, 0.0, 1.0);
}

void main() {
    float age0 = nowMs - a_p0BirthMs;
    float age1 = nowMs - a_p1BirthMs;

    // Not connected, or both ends fully faded: nothing to draw.
    if (startsSegment(a_p1Flags) || (age0 >= fadeMs && age1 >= fadeMs)) {
        collapse();
        return;
    }

    // Coincident endpoints: degenerate segment, nothing to draw. Its
    // neighbors fall back to their own direction at this joint.
    vec2 seg = a_p1Pos - a_p0Pos;
    if (length(seg) < EPS) {
        collapse();
        return;
    }

    vec2  dir     = dirBetween(a_p0Pos, a_p1Pos, vec2(1.0, 0.0));
    vec2  dirPrev = startsSegment(a_p0Flags) ? dir : dirBetween(a_prevPos, a_p0Pos, dir);
    vec2  dirNext = startsSegment(a_nextFlags) ? dir : dirBetween(a_p1Pos, a_nextPos, dir);

    bool  atEnd = (gl_VertexID & 1) == 1;
    float side  = float((gl_VertexID >> 1) & 1) * 2.0 - 1.0;

    // Width tapers with age, like the alpha: the tail thins as it fades and
    // pinches to a point at a fully faded end.
    float life0 = life(age0);
    float life1 = life(age1);
    float hw0   = 0.5 * widthPx * life0;
    float hw1   = 0.5 * widthPx * life1;

    vec2  offset = atEnd ? jointOffset(dir, dirNext, hw1) : jointOffset(dirPrev, dir, hw0);
    vec2  pos    = (atEnd ? a_p1Pos : a_p0Pos) + offset * side;

    float speed = clamp(length(atEnd ? a_p1Vel : a_p0Vel) / speedRef, 0.0, 1.0);

    v_side      = side;
    v_alpha     = atEnd ? life1 : life0;
    v_color     = mix(vec3(0.1, 0.4, 1.0), vec3(1.0, 0.1, 0.1), speed);
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
}
