
// Standard varyings, see the vertex prelude.
in vec2  ht_vLocal;
in float ht_vAge;
in float ht_vLife;
in float ht_vSpeed;
in float ht_vDist;
in float ht_vSeed;

// Output, PREMULTIPLIED alpha: write vec4(rgb * a, a).
layout(location = 0) out vec4 ht_fragColor;
