#version 300 es
precision highp float;
out vec4 outputBuffer;
void main() {
    ivec2 cell = ivec2(gl_FragCoord.xy) / 8;
    outputBuffer = vec4(float(cell.x % 4) * 0.25, float(cell.y % 4) * 0.25, 0.5, 1.0);
}
