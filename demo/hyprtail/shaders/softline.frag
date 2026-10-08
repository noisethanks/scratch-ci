#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail expects path
// demo look "softline": a plain soft-edged line that fades with the node's
// life, from color_a at birth to color_b as it fades out.
//
// expects path, on purpose: it reads ht_vLocal.y as the position across a
// strip (0 centerline, +-1 edges) and ht_vLife. Quad and instanced geometry
// put quad coordinates in ht_vLocal, which would draw a horizontal band.
// Every path geometry shader pairs with it, shipped or demo.
//
// Edges are antialiased from the screen-space derivative of ht_vLocal.y, so
// a strip tapering to a point stays clean. Output is PREMULTIPLIED alpha;
// only color parameters are color-managed.

#pragma hyprtail param color color_a rgba(6f879fff)
#pragma hyprtail param color color_b rgba(566b80ff)
// Overall opacity on top of the colors' own alpha.
#pragma hyprtail param float alpha 0.7 0 1
// 0 = hard edge, 1 = fades from the centerline.
#pragma hyprtail param float soft 0.35 0 1
// How the opacity falls with life: 1 = linearly, above 1 sooner.
#pragma hyprtail param float fade_curve 1 0.25 4

void main() {
    float d    = abs(ht_vLocal.y);
    float aa   = max(fwidth(ht_vLocal.y), 1e-4);
    float edge = max(soft, aa);
    float cov  = 1.0 - smoothstep(1.0 - edge, 1.0, d);

    float life = max(ht_vLife, 0.0);
    vec4  c    = mix(color_b, color_a, life);
    float a    = c.a * alpha * cov * pow(life, fade_curve);
    if (a <= 0.0)
        discard;

    ht_fragColor = vec4(c.rgb * a, a);
}
