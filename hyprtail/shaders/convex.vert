#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology path
// hyprtail geometry "convex": a thin strip along the trail whose width tapers
// toward BOTH ends, with an optional sideways offset from the path.
//
//   - Tail end: the width follows the node's life raised to tail_curve, so the
//     oldest visible node is a point (the same time-based fade as every
//     other layer, SPEC section 4).
//   - Head end: the width ramps up from 0 over the first head_ms of a node's
//     age, so the newest node is a point too. The contract has no "distance
//     from the head" for a node (ht_vDist counts from the trail's other
//     end), so the head taper is a time ramp: it is sharp while the pointer
//     moves and relaxes once the newest node ages past head_ms.
//   - offset moves the whole strip sideways by that many px, positive to the
//     right of the direction of travel. Several layers with different
//     width/offset make parallel lines (tether.conf).
//
// Writes all six standard varyings, so any path fragment shader pairs with
// it. ht_vLocal is x 0 at the newer end of a segment, 1 at the older, y -1..1
// across the strip (a fraction of the half-width, whatever the taper).
//
// Includes only the contract's own helpers (helpers/ribbon.glsl: joins and
// the degenerate-segment guard; helpers/fade.glsl: ht_life).

#include "helpers/ribbon.glsl"
#include "helpers/fade.glsl"

// Strip width at its fattest, px.
#pragma hyprtail param float width 3 0 64
// Sideways shift of the strip, px, positive to the right of travel.
#pragma hyprtail param float offset 0 -24 24
// Time over which the head grows from a point to full width, ms; 0 = blunt.
#pragma hyprtail param float head_ms 140 0 2000
// Tail shape: 1 = width follows life linearly; above 1 the tail thins sooner.
#pragma hyprtail param float tail_curve 1 0.25 4
#pragma hyprtail param float miter_limit 2 1 16
// Furthest a corner gets from a node: the largest shift plus half the width,
// both scaled by the widest miter. The 24 is the top of offset's range
// (padding cannot take an absolute value).
#pragma hyprtail padding (width * 0.5 + 24) * miter_limit + 1

float halfWidthOf(HtNode n) {
    float life = ht_life(n.age, fade_ms);
    float tail = life > 0.0 ? pow(life, tail_curve) : 0.0;
    float head = head_ms > 0.0 ? smoothstep(0.0, head_ms, n.age) : 1.0;
    return 0.5 * width * head * tail;
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

    // Coincident endpoints: no direction, nothing to draw. The neighbors fall
    // back to their own direction at this joint (ht_dirBetween).
    if (length(p1.pos - p0.pos) < HT_EPS) {
        gl_Position = ht_collapsedPosition();
        return;
    }

    vec2 dir     = ht_dirBetween(p0.pos, p1.pos, vec2(1.0, 0.0));
    vec2 dirPrev = p0.segmentStart ? dir : ht_dirBetween(prev.pos, p0.pos, dir);
    vec2 dirNext = next.segmentStart ? dir : ht_dirBetween(p1.pos, next.pos, dir);

    bool  atEnd = ht_atEnd();
    float side  = ht_side();

    HtNode n  = atEnd ? p1 : p0;
    float  hw = halfWidthOf(n);

    // ht_jointOffset is linear in its width argument, so the sideways shift
    // and the half-width share one call: the strip's centerline is the
    // path shifted by `offset`, its edges +-hw from that.
    float reach = offset + side * hw;
    vec2  shift = atEnd ? ht_jointOffset(dir, dirNext, reach, miter_limit) : ht_jointOffset(dirPrev, dir, reach, miter_limit);

    ht_vLocal = vec2(atEnd ? 0.0 : 1.0, side);
    ht_vAge   = n.age;
    ht_vLife  = ht_life(n.age, fade_ms);
    ht_vSpeed = length(n.vel);
    ht_vDist  = n.dist;
    ht_vSeed  = n.seed;

    gl_Position = ht_toClip(n.pos + shift);
}
