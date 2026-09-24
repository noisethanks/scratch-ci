#version 300 es
// hyprtail idle/presence vertex shader (stock). Also the reference for
// user-supplied idle vertex shaders; most users only replace the fragment
// shader and keep this one.
//
// Contract for idle vertex shaders (SPEC section 7):
//
// One instance, drawn as a 4-vertex TRIANGLE_STRIP covering a square around
// the pointer. No vertex attributes; gl_VertexID picks the corner:
//   bit 0: 0 = left, 1 = right
//   bit 1: 0 = top,  1 = bottom
//
// Uniforms (any you don't declare are simply not set; using one not listed
// here, or any vertex attribute, is rejected at load, since the plugin would
// never set it and it would read 0):
//   mat3  proj        global layout px -> clip space, per monitor
//   vec2  center      pointer position, global layout (logical) px
//   float extentPx    half-size of the square: radiusPx plus the declared and
//                     configured damage padding. Stay inside it.
//   float radiusPx    plugin:hyprtail:idle_radius, logical px
//   float idleMs      time since the effect started (after idle_delay_ms)
//   float durationMs  plugin:hyprtail:idle_duration_ms; 0 = runs until the
//                     pointer moves (loop your animation then)
//   vec4  colorSlow   palette, as in the trail contract: rgb already
//   vec4  colorFast   converted to the output's color space, a = alpha
//
// Color management: only the palette uniforms are converted to the output's
// color space; colors a shader computes itself are unmanaged (correct on SDR
// sRGB outputs, wrong on HDR / wide-gamut color-managed ones).
//
// Outputs: whatever the paired fragment shader reads. The stock fragment
// shader reads v_local (px offset from the center, -extentPx..extentPx) and
// v_uv (0..1 across the square); a custom vertex shader used with it must
// write them.
//
// Damage: the plugin damages the square (center +- extentPx) plus 1px.
// Declare extra reach with #pragma hyprtail padding <px> (it grows extentPx).
//
// Includes work as in the trail contract. Keep files ASCII.

precision highp float;

uniform mat3  proj;
uniform vec2  center;
uniform float extentPx;

out vec2 v_local;
out vec2 v_uv;

void main() {
    vec2 uv     = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 local  = (uv * 2.0 - 1.0) * extentPx;

    v_local     = local;
    v_uv        = uv;
    gl_Position = vec4(proj * vec3(center + local, 1.0), 1.0);
}
