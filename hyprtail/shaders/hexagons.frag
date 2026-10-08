#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail expects path
// hyprtail look "hexagons": the strip tiled with hexagonal cells, laid out in
// strip space. Each cell has a hash of its own (computed here, in the
// fragment shader), which gives it a tint, a flicker phase and a place in
// the order cells die out as the trail fades.
//
// Strip space, from the contract's varyings:
//
//   along the strip  ht_vDist / cell_px, path length in units of cell_px
//   across the strip ht_vLocal.y * rows / 2: rows cells span the full width
//
// One unit is one cell height. The cells are regular hexagons when cell_px
// equals the strip's width in px divided by rows; the fragment shader cannot
// see the width (the contract keeps the half-width in the vertex stage), so
// the preset sets both. When the strip tapers the cells squash with it
// instead of staying regular, which reads as scales on a tapering body. The
// lattice is anchored to the path (ht_vDist is stable per node), so it does
// not slide as the trail moves.
//
// Brightness: each cell flickers on its own phase with the node's age
// (shimmer), and cells drop out in a random order as ht_vLife falls
// (dissolve), so the trail dissolves into a few last cells instead of
// fading as a whole. No node loop, no texture.
//
// expects path, on purpose: ht_vDist and ht_vLocal.y need a connected strip.
// Pairs with any path geometry shader. Output is PREMULTIPLIED alpha; only
// color parameters are color-managed.

#include "helpers/noise.glsl"

const float TAU    = 6.28318530718;
const float COL_DX = 0.8660254; // distance between hexagon columns, in cell heights

#pragma hyprtail param color color_a rgba(7a9a86ff)
#pragma hyprtail param color color_b rgba(6f879fff)
// Cell length along the path, px. Regular hexagons: strip width / rows.
#pragma hyprtail param float cell_px 9 3 64
// Cells across the full width of the strip.
#pragma hyprtail param float rows 4 2 24
// Gap between cells, as a fraction of a cell.
#pragma hyprtail param float gap 0.16 0 0.6
// How much of the fade is cells dying one by one: 0 = all fade together.
#pragma hyprtail param float dissolve 0.85 0 1
// Depth of each cell's flicker, and its rate in cycles per second.
#pragma hyprtail param float shimmer 0.35 0 1
#pragma hyprtail param float shimmer_hz 0.4 0 8
// Overall opacity on top of the colors' own alpha.
#pragma hyprtail param float alpha 0.7 0 1

void main() {
    vec2 p = vec2(ht_vDist / cell_px, ht_vLocal.y * rows * 0.5);

    // Nearest hexagon center: columns COL_DX apart, every other column
    // shifted half a cell up. A point's nearest center lies in the column
    // it falls in or the next one.
    float col0 = floor(p.x / COL_DX);
    vec2  rel  = vec2(0.0);
    ivec2 cell = ivec2(0);
    float best = 1e9;
    for (int k = 0; k < 2; ++k) {
        float col  = col0 + float(k);
        float odd  = mod(col, 2.0);
        float row  = floor(p.y - 0.5 * odd + 0.5);
        vec2  r    = p - vec2(col * COL_DX, row + 0.5 * odd);
        float dist = dot(r, r);
        if (dist < best) {
            best = dist;
            rel  = r;
            cell = ivec2(int(col), int(row));
        }
    }

    // Flat-topped hexagon of height 1: distance measure that is 0.5 on its edge.
    float hd     = max(abs(rel.y), COL_DX * abs(rel.x) + 0.5 * abs(rel.y));
    float inside = 0.5 - hd - 0.5 * gap;
    float cover  = smoothstep(0.0, max(fwidth(hd), 1e-4), inside);
    float body   = 0.8 + 0.2 * smoothstep(0.0, 0.4, 0.5 - hd);

    float h     = ht_hash2(cell);
    float h2    = ht_hash2(cell + ivec2(31, 17));
    float life  = max(ht_vLife, 0.0);
    float die   = h * dissolve * 0.85;
    float alive = smoothstep(die, die + 0.15, life);
    float pulse = mix(1.0, 0.5 + 0.5 * sin(TAU * (ht_vAge * 0.001 * shimmer_hz + h)), shimmer);

    // Soft clip at the strip's edges.
    float aaY  = max(fwidth(ht_vLocal.y), 1e-4);
    float clip = 1.0 - smoothstep(1.0 - 1.5 * aaY, 1.0, abs(ht_vLocal.y));

    vec4  c = mix(color_a, color_b, h2);
    float a = c.a * alpha * cover * body * alive * pulse * clip;
    if (a <= 0.0)
        discard;

    ht_fragColor = vec4(c.rgb * a, a);
}
