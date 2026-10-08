#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology path
// demo geometry "comet": fat near the head, drawing out to a needle at the
// tail, with an optional swelling right at the newest nodes (the coma).
//
// Width is a function of a node's age only (SPEC section 4: time-based, not
// position-based):
//
//   half-width = 0.5 * width * ( life^curve + bulb * (1 - age/bulb_ms)^2 )
//
// life is 1 at birth and 0 at fade_ms, so the first term is the comet body
// and goes to a point at the faded end. The second is a short swelling that
// is present only for the first bulb_ms of a node's age: while the pointer
// moves it sits at the head, and once the pointer rests it is gone after
// bulb_ms. curve above 1 makes the tail a concave needle; below 1 a fat teardrop.
//
// Writes all six standard varyings, so any path fragment shader pairs with
// it. ht_vLocal is x 0 at the newer end of a segment, 1 at the older, y -1..1
// across the strip.
//
// Includes only the contract's own helpers (helpers/ribbon.glsl, fade.glsl).

#include "helpers/ribbon.glsl"
#include "helpers/fade.glsl"

// Body width at the head, px, before the swelling.
#pragma hyprtail param float width 5 0 64
// Tail shape exponent on life.
#pragma hyprtail param float curve 1.6 0.25 6
// Extra width of the head swelling, as a fraction of width.
#pragma hyprtail param float bulb 0.5 0 3
// How long a node keeps the swelling, ms.
#pragma hyprtail param float bulb_ms 100 1 1000
#pragma hyprtail param float miter_limit 2 1 16
// Widest half-width is width * (1 + bulb) / 2, times the widest miter.
#pragma hyprtail padding width * 0.5 * (1 + bulb) * miter_limit + 1

float halfWidthOf(HtNode n) {
    float life = ht_life(n.age, fade_ms);
    float body = life > 0.0 ? pow(life, curve) : 0.0;
    float coma = max(1.0 - n.age / bulb_ms, 0.0);
    return 0.5 * width * (body + bulb * coma * coma);
}

void main() {
    ht_initVaryings();

    HtNode prev = ht_prev();
    HtNode p0   = ht_p0();
    HtNode p1   = ht_p1();
    HtNode next = ht_next();

    if (p1.segmentStart || (ht_faded(p0.age, fade_ms) && ht_faded(p1.age, fade_ms))) {
        gl_Position = ht_collapsedPosition();
        return;
    }

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

    vec2 shift = atEnd ? ht_jointOffset(dir, dirNext, side * hw, miter_limit) : ht_jointOffset(dirPrev, dir, side * hw, miter_limit);

    ht_vLocal = vec2(atEnd ? 0.0 : 1.0, side);
    ht_vAge   = n.age;
    ht_vLife  = ht_life(n.age, fade_ms);
    ht_vSpeed = length(n.vel);
    ht_vDist  = n.dist;
    ht_vSeed  = n.seed;

    gl_Position = ht_toClip(n.pos + shift);
}
