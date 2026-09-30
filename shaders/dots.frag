#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail expects quad,instanced
// hyprtail "jitter" and "spray" presets: soft round dots. Shades the quad
// coordinates (ht_vLocal, -1..1 on both axes) of an instanced or quad layer,
// so it pairs with any geometry shader of those topologies that writes them.
//
// The color of each dot is picked between color_a and color_b by ht_vSeed,
// which instanced shaders make per copy. Output is PREMULTIPLIED alpha; only
// color parameters are color-managed.

#pragma hyprtail param color color_a rgba(ffffffff)
#pragma hyprtail param color color_b rgba(1a66ffff)
// 0 = hard edge, 1 = fades from the centre.
#pragma hyprtail param float softness 0.6 0 1

void main() {
    float d = length(ht_vLocal);
    float a = 1.0 - smoothstep(1.0 - softness, 1.0, d);
    a *= mix(color_a, color_b, ht_vSeed).a * ht_vLife;
    if (a <= 0.0)
        discard;

    vec3 rgb     = mix(color_a, color_b, ht_vSeed).rgb;
    ht_fragColor = vec4(rgb * a, a);
}
