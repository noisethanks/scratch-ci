#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology quad
// hyprtail "classic" preset, layer "idle": the square around the pointer.
// Also the reference for quad-topology geometry shaders; most effects keep
// this one and only replace the fragment shader.
//
// The square covers ht_anchor +- ht_extentPx (the padding expression plus
// damage_padding), exactly what is damaged. ht_vLocal gets the quad
// coordinates (-1..1).

#pragma hyprtail param float radius 24 1 1024
#pragma hyprtail padding radius

void main() {
    ht_initVaryings();

    vec2 c      = ht_corner();
    ht_vLocal   = c;
    ht_vAge     = ht_stillMs;
    gl_Position = ht_toClip(ht_anchor + c * ht_extentPx);
}
