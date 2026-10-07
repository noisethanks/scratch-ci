// hyprtail prefab: ribbon geometry helpers for trail vertex shaders.
//
//   #include "helpers/ribbon.glsl"
//
// Functions only, no uniforms or attributes: pass everything in. Names are
// prefixed ht_ to stay out of the way of user code. Included at most once
// per shader (the loader ignores repeated includes).

const float HT_EPS = 1e-3;

// Clip-space position that draws nothing (outside the clip volume). Give it
// to all four vertices of an instance to skip that segment.
vec4 ht_collapsedPosition() {
    return vec4(2.0, 2.0, 2.0, 1.0);
}

// Unit direction from a to b, or fallback if they (nearly) coincide.
// Instances meeting at a joint must compute the joint's directions through
// this same function from the same inputs to get identical corners.
vec2 ht_dirBetween(vec2 a, vec2 b, vec2 fallback) {
    vec2  d = b - a;
    float l = length(d);
    return l > HT_EPS ? d / l : fallback;
}

// Offset of the +side corner at a joint between an incoming and an outgoing
// unit direction, for half-width hw. Miter join, length clamped to
// miterLimit * hw (sharp turns get a pulled-in corner instead of a spike).
vec2 ht_jointOffset(vec2 dirIn, vec2 dirOut, float hw, float miterLimit) {
    vec2  nIn  = vec2(-dirIn.y, dirIn.x);
    vec2  nOut = vec2(-dirOut.y, dirOut.x);
    vec2  m    = nIn + nOut;
    float ml   = length(m);
    if (ml < HT_EPS) // full reversal: no usable miter
        return nOut * hw;
    m /= ml;
    float c = dot(m, nOut); // cos(half the turn angle)
    return m * (hw / max(c, 1.0 / miterLimit));
}
