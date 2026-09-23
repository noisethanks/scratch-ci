#version 300 es
// hyprtail trail fragment shader (stock ribbon).
//
// Contract for trail fragment shaders (built-in and user-supplied):
//   - Inputs are whatever the paired vertex shader outputs; the stock pair
//     uses v_side (-1..1 across the ribbon), v_alpha (time fade) and v_color.
//   - Output is PREMULTIPLIED alpha: Hyprland blends with
//     GL_ONE, GL_ONE_MINUS_SRC_ALPHA. Write vec4(rgb * a, a).
//   - Draw only inside the damage padding documented in the vertex contract.

precision highp float;

in float v_side;
in float v_alpha;
in vec3  v_color;

layout(location = 0) out vec4 fragColor;

void main() {
    // ~1px antialiased edge across the width, inside the geometry so the
    // damage box still covers it.
    float d   = abs(v_side);
    float w   = fwidth(v_side);
    float cov = 1.0 - smoothstep(1.0 - w, 1.0, d);
    if (cov <= 0.0)
        discard;

    float a   = v_alpha * cov;
    fragColor = vec4(v_color * a, a);
}
