#version 300 es
#pragma hyprtail contract 2
// hyprtail "classic" preset, layer "trail": ribbon shading. Also the
// reference for fragment shaders (see the contract summary in
// ribbon.vert).
//
// Reads only standard varyings, so it pairs with any geometry shader that
// writes them. Output is PREMULTIPLIED alpha. Only color parameters are
// color-managed; colors computed or hardcoded here are written as-is
// (correct on SDR sRGB outputs, wrong on HDR / wide-gamut ones).

#pragma hyprtail param float speed_ref 2 0.001 1000
#pragma hyprtail param color color_slow rgba(1a66ffff)
#pragma hyprtail param color color_fast rgba(ff1a1aff)

void main() {
    // ~1px antialiased edge across the width, inside the geometry so the
    // damage box still covers it.
    float d   = abs(ht_vLocal.y);
    float w   = fwidth(ht_vLocal.y);
    float cov = 1.0 - smoothstep(1.0 - w, 1.0, d);
    if (cov <= 0.0)
        discard;

    // Palette by speed at birth: slow -> color_slow, speed_ref and above ->
    // color_fast.
    vec4  c = mix(color_slow, color_fast, clamp(ht_vSpeed / speed_ref, 0.0, 1.0));

    float a      = c.a * ht_vLife * cov;
    ht_fragColor = vec4(c.rgb * a, a);
}
