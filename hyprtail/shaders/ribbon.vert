#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology path
// hyprtail geometry "ribbon": the trail as one continuous strip. Every
// built-in ribbon preset uses this file, so a change to how the strip is
// built (joins, smoothing) reaches all of them at once. Also the reference
// for path-topology geometry shaders: copy it and change what you like.
//
// Contract 2 in short (SPEC section 13):
//   - "#pragma hyprtail contract 2" must come right after #version; the
//     loader replaces it with the prelude (shaders/prelude/), which declares
//     everything the plugin provides. Don't declare attributes or plugin
//     uniforms yourself.
//   - Exactly one "#pragma hyprtail topology path|quad" in a geometry
//     (vertex) shader.
//   - Nodes: ht_prev(), ht_p0(), ht_p1(), ht_next() return HtNode (pos, age,
//     vel, dist, seed, segmentStart). ht_atEnd() / ht_side() pick this
//     vertex's corner.
//   - Parameters: "#pragma hyprtail param <type> <name> <default> [<min>
//     <max>]" becomes "uniform <type> <name>;" (color -> vec4, converted to
//     the output's color space, alpha passed through). Reserved lifecycle
//     parameters (fade_ms, start_ms, duration_ms) are always declared.
//   - Damage: "#pragma hyprtail padding <expr>" (numbers, parameter names,
//     + - * /, parentheses) is how far past the node positions this layer
//     draws; the largest one across the program counts, plus
//     damage_padding. Anything drawn outside isn't guaranteed to repaint.
//   - Write the standard varyings (ht_vLocal, ht_vAge, ht_vLife, ht_vSpeed,
//     ht_vDist, ht_vSeed) so any fragment shader pairs with this one; call
//     ht_initVaryings() first.
//   - Keep files ASCII.

#include "helpers/ribbon.glsl"
#include "helpers/fade.glsl"

#pragma hyprtail param float width 8 0 512
#pragma hyprtail param float miter_limit 2 1 16
// How much the width follows a point's life: 0 = full width until it fades
// out, 1 = thins with age and pinches to a point at the faded end.
#pragma hyprtail param float taper 1 0 1
// Calligraphy: 0 = round pen (same width in every direction), 1 = flat nib,
// full width moving across the nib's edge and thin moving along it.
#pragma hyprtail param float nib 0 0 1
// The nib edge's angle on screen, degrees counterclockwise from horizontal.
#pragma hyprtail param float nib_angle 45 -90 90
// Widest possible miter, plus the ~1px antialiased edge. taper and nib only
// ever narrow the ribbon.
#pragma hyprtail padding width * 0.5 * miter_limit + 1

const float NIB_THIN = 0.15; // width fraction left moving along a full nib

// Half-width at a joint. Computed from the joint's own direction (the mean of
// the segments meeting there), so both segments sharing a joint agree on it.
float halfWidth(float life, vec2 dirIn, vec2 dirOut) {
    vec2  j      = ht_dirBetween(vec2(0.0), dirIn + dirOut, dirOut);
    float a      = radians(nib_angle);
    vec2  edge   = vec2(cos(a), -sin(a)); // screen y points down
    float across = abs(j.x * edge.y - j.y * edge.x);
    float pen    = mix(1.0, mix(NIB_THIN, 1.0, across), nib);
    return 0.5 * width * mix(1.0, life, taper) * pen;
}

void main() {
    ht_initVaryings();

    HtNode prev = ht_prev();
    HtNode p0   = ht_p0();
    HtNode p1   = ht_p1();
    HtNode next = ht_next();

    // Not connected, or both ends fully faded: nothing to draw.
    if (p1.segmentStart || (ht_faded(p0.age, fade_ms) && ht_faded(p1.age, fade_ms))) {
        gl_Position = ht_collapsedPosition();
        return;
    }

    // Coincident endpoints: degenerate segment, nothing to draw. Its
    // neighbors fall back to their own direction at this joint.
    if (length(p1.pos - p0.pos) < HT_EPS) {
        gl_Position = ht_collapsedPosition();
        return;
    }

    vec2  dir     = ht_dirBetween(p0.pos, p1.pos, vec2(1.0, 0.0));
    vec2  dirPrev = p0.segmentStart ? dir : ht_dirBetween(prev.pos, p0.pos, dir);
    vec2  dirNext = next.segmentStart ? dir : ht_dirBetween(p1.pos, next.pos, dir);

    bool  atEnd = ht_atEnd();
    float side  = ht_side();

    float life0 = ht_life(p0.age, fade_ms);
    float life1 = ht_life(p1.age, fade_ms);
    float hw0   = halfWidth(life0, dirPrev, dir);
    float hw1   = halfWidth(life1, dir, dirNext);

    vec2  offset = atEnd ? ht_jointOffset(dir, dirNext, hw1, miter_limit) : ht_jointOffset(dirPrev, dir, hw0, miter_limit);
    vec2  pos    = (atEnd ? p1.pos : p0.pos) + offset * side;

    HtNode n  = atEnd ? p1 : p0;
    ht_vLocal = vec2(atEnd ? 0.0 : 1.0, side);
    ht_vAge   = n.age;
    ht_vLife  = atEnd ? life1 : life0;
    ht_vSpeed = length(n.vel);
    ht_vDist  = n.dist;
    ht_vSeed  = n.seed;

    gl_Position = ht_toClip(pos);
}
