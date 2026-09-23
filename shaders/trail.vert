#version 320 es

layout(location = 0) in vec2 inPosPx;

uniform vec2 uMonitorPosPx;
uniform vec2 uMonitorSizePx;
uniform float uPointSize;

vec2 canvasToNDC(vec2 canvasPx, vec2 monitorPos, vec2 monitorSize) {
    vec2 local = (canvasPx - monitorPos) / monitorSize;
    return vec2(local.x * 2.0 - 1.0, 1.0 - local.y * 2.0);
}

void main() {
    vec2 ndc = canvasToNDC(inPosPx, uMonitorPosPx, uMonitorSizePx);
    gl_Position = vec4(ndc, 0.0, 1.0);
    gl_PointSize = uPointSize;
}
