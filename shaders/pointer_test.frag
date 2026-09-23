#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
uniform vec2 pointer_position;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = texture(tex, v_texcoord);
    if (distance(v_texcoord, pointer_position) < 0.02)
        fragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
