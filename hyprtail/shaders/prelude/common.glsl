// ---- hyprtail prelude, contract 2 ----
// Injected by the loader in place of "#pragma hyprtail contract 2". Not
// includable. Everything a shader gets from the plugin is declared here;
// shaders never declare attributes or plugin uniforms themselves.

precision highp float;
precision highp int;

// Built-in uniforms, set for every layer.
uniform mat3  ht_proj;     // global layout (logical) px -> clip space, this monitor
uniform float ht_nowMs;    // now, same reference as node birth times; only differences matter
uniform float ht_stillMs;  // time since the pointer last moved, ms
uniform vec2  ht_anchor;   // pointer position, global layout px (quad layers draw around it)
uniform float ht_extentPx; // this layer's reach: its padding expression plus damage_padding, px

// Reserved lifecycle parameters, set for every layer from the preset and
// config. The plugin also uses them for visibility and damage.
uniform float fade_ms;     // path: a node is visible while its age < fade_ms
uniform float start_ms;    // quad: visible once the pointer has been still for start_ms,
uniform float duration_ms; // quad: for duration_ms (0 = until the pointer moves)
