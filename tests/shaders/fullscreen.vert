#version 450
layout(location = 0) out vec2 uv;
const vec2 positions[3] = vec2[](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
void main() {
    vec2 p = positions[gl_VertexIndex];
    gl_Position = vec4(p, 0.5, 1.0);
    uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
}
