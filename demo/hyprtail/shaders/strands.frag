#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail expects path
// demo look "strands": two thin strands winding around the strip's
// centerline, a helix seen from the side. The strand nearer the viewer is
// drawn over the farther one and a little brighter, and optional thin
// rungs join the strands.
//
// Everything is drawn per pixel in strip space, from the contract's own
// varyings:
//
//   along the strip  ht_vDist, px of path length (restarts at a break)
//   across the strip ht_vLocal.y, -1..1, a fraction of the local half-width
//
// Strand A is y = amp * sin(phase), strand B its mirror, with
// phase = 2 pi * ht_vDist / pitch_px + spin * age. The distance from a
// pixel to a strand is taken in screen pixels, from the screen-space
// derivatives of y and ht_vDist (distance to an implicit curve g = 0 is
// |g| / |grad g|), so strands keep a constant strand_px thickness however
// the strip bends, tapers or fades, and nothing is sampled at nodes: the
// helix does not alias when the pointer moves fast. Amplitude is a fraction
// of the half-width, so the helix narrows with the strip. No node loop.
// Because the derivatives are per device pixel, strand_px and the rung line
// are in DEVICE pixels, not logical ones: on a 2x output they are half as
// thick as on a 1x one. pitch_px and rung_px are path length, logical px.
//
// expects path, on purpose: ht_vDist and ht_vLocal.y mean arc length and
// across-width only for a connected strip. Pairs with any path geometry
// shader. Output is PREMULTIPLIED alpha; only color parameters are
// color-managed.

const float TAU = 6.28318530718;

#pragma hyprtail param color color_a rgba(8f7fa8ff)
#pragma hyprtail param color color_b rgba(6f879fff)
// Path length of one full turn, px.
#pragma hyprtail param float pitch_px 40 8 256
// How far the strands swing from the centerline, as a fraction of the half-width.
#pragma hyprtail param float amp 0.55 0 1
// Strand thickness, device px.
#pragma hyprtail param float strand_px 1.2 0.3 6
// Rotation of the helix with a node's age, radians per second.
#pragma hyprtail param float spin 1.2 -20 20
// How much dimmer the strand behind is: 0 = same, 1 = invisible.
#pragma hyprtail param float depth 0.5 0 1
// Path length between rungs, px; 0 = no rungs.
#pragma hyprtail param float rung_px 0 0 128
#pragma hyprtail param float rung_alpha 0.3 0 1
// Overall opacity on top of the colors' own alpha.
#pragma hyprtail param float alpha 0.7 0 1

// Coverage 0..1 of a line of half-thickness halfPx, for an implicit function
// g (zero on the line) with screen-space gradient gradG.
float lineCover(float g, vec2 gradG, float halfPx) {
    float px = abs(g) / max(length(gradG), 1e-4);
    return 1.0 - smoothstep(halfPx - 0.5, halfPx + 0.5, px);
}

void main() {
    float y = ht_vLocal.y;
    float d = ht_vDist;

    float ph = TAU * d / pitch_px + spin * ht_vAge * 0.001;
    float s  = sin(ph);
    float c  = cos(ph);
    float f  = amp * s;
    float df = amp * c * (TAU / pitch_px); // dy/dd along the strand

    vec2 gy = vec2(dFdx(y), dFdy(y));
    vec2 gd = vec2(dFdx(d), dFdy(d));

    // Strand A at +f, strand B at -f; c is the depth of A (B has -c).
    float covA = lineCover(y - f, gy - df * gd, 0.5 * strand_px);
    float covB = lineCover(y + f, gy + df * gd, 0.5 * strand_px);
    float kA   = mix(1.0 - depth, 1.0, 0.5 + 0.5 * c);
    float kB   = mix(1.0 - depth, 1.0, 0.5 - 0.5 * c);
    float aA   = color_a.a * covA * kA;
    float aB   = color_b.a * covB * kB;

    // Nearer strand over the farther one, premultiplied.
    vec4 sa = vec4(color_a.rgb * aA, aA);
    vec4 sb = vec4(color_b.rgb * aB, aB);
    vec4 front = c >= 0.0 ? sa : sb;
    vec4 back  = c >= 0.0 ? sb : sa;
    vec4 col   = front + back * (1.0 - front.a);

    // Rungs: a thin cross line every rung_px, only between the strands.
    if (rung_px > 0.0) {
        float w     = fract(d / rung_px + 0.5) - 0.5;
        float rung  = 1.0 - smoothstep(0.0, 1.0, abs(w) * rung_px / max(length(gd), 1e-4));
        float inner = clamp((abs(f) - abs(y)) / max(length(gy), 1e-4) + 0.5, 0.0, 1.0);
        float ra    = rung * inner * rung_alpha * 0.5 * (color_a.a + color_b.a);
        vec4  rc    = vec4(0.5 * (color_a.rgb + color_b.rgb) * ra, ra);
        col         = col + rc * (1.0 - col.a);
    }

    col *= alpha * max(ht_vLife, 0.0);
    if (col.a <= 0.0)
        discard;

    ht_fragColor = col;
}
