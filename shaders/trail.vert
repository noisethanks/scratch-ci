#version 300 es
// hyprtail trail vertex shader (stock ribbon). Also the reference for
// user-supplied vertex shaders: copy it and change what you like.
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
// (workspace switch, warp, trail start). At the ends of the trail, prev/next
// are copies of the end node (zero-length direction), handle that as "no
// neighbor".
//
// Uniforms (any you don't declare are simply not set):
//   mat3  proj        global layout px -> clip space, per monitor
//   float nowMs       current time, same reference as the birth times; only
//                     age = nowMs - birthMs is meaningful
//   float fadeMs      lifetime of a node; age >= fadeMs is fully faded
//   float widthPx     full ribbon width at age 0, logical px
//   float miterLimit  max miter length, in half-widths
//   float speedRef    speed (px/ms) mapped to the fast end of the palette
//
// Outputs: whatever the paired fragment shader reads. The stock fragment
// shader reads v_side (-1..1 across the ribbon), v_alpha and v_color; a
// custom vertex shader used with the stock fragment shader must write them.
//
// Damage: the plugin damages the node extent padded by
// widthPx / 2 * miterLimit + 1px, plus any padding declared with
//   #pragma hyprtail padding <px>
// in this file, the fragment shader or their includes (the largest one
// counts), plus plugin:hyprtail:damage_padding from the config. Anything
// drawn outside that is not guaranteed to be repainted.
//
// Includes: #include "hyprtail/<name>" pulls in a built-in prefab, any other
// path is relative to this file. Each file is included once. Keep files
// ASCII.
//
// Instances that should draw nothing are moved outside clip space.

precision highp float;

#include "hyprtail/ribbon.glsl"
#include "hyprtail/fade.glsl"

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

void collapse() {
    gl_Position = ht_collapsedPosition();
    v_side      = 0.0;
    v_alpha     = 0.0;
    v_color     = vec3(0.0);
}

void main() {
    float age0 = nowMs - a_p0BirthMs;
    float age1 = nowMs - a_p1BirthMs;

    // Not connected, or both ends fully faded: nothing to draw.
    if (ht_startsSegment(a_p1Flags) || (ht_faded(age0, fadeMs) && ht_faded(age1, fadeMs))) {
        collapse();
        return;
    }

    // Coincident endpoints: degenerate segment, nothing to draw. Its
    // neighbors fall back to their own direction at this joint.
    if (length(a_p1Pos - a_p0Pos) < HT_EPS) {
        collapse();
        return;
    }

    vec2  dir     = ht_dirBetween(a_p0Pos, a_p1Pos, vec2(1.0, 0.0));
    vec2  dirPrev = ht_startsSegment(a_p0Flags) ? dir : ht_dirBetween(a_prevPos, a_p0Pos, dir);
    vec2  dirNext = ht_startsSegment(a_nextFlags) ? dir : ht_dirBetween(a_p1Pos, a_nextPos, dir);

    bool  atEnd = (gl_VertexID & 1) == 1;
    float side  = float((gl_VertexID >> 1) & 1) * 2.0 - 1.0;

    // Width tapers with age, like the alpha: the tail thins as it fades and
    // pinches to a point at a fully faded end.
    float life0 = ht_life(age0, fadeMs);
    float life1 = ht_life(age1, fadeMs);
    float hw0   = 0.5 * widthPx * life0;
    float hw1   = 0.5 * widthPx * life1;

    vec2  offset = atEnd ? ht_jointOffset(dir, dirNext, hw1, miterLimit) : ht_jointOffset(dirPrev, dir, hw0, miterLimit);
    vec2  pos    = (atEnd ? a_p1Pos : a_p0Pos) + offset * side;

    float speed = clamp(length(atEnd ? a_p1Vel : a_p0Vel) / speedRef, 0.0, 1.0);

    v_side      = side;
    v_alpha     = atEnd ? life1 : life0;
    v_color     = mix(vec3(0.1, 0.4, 1.0), vec3(1.0, 0.1, 0.1), speed);
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
}
