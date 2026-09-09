#version 450
layout(location = 0) in vec4 instanceColor;
layout(location = 0) out vec4 color;
void main() {
    // One quad per instance, one pixel per quad in the four-pixel test target.
    vec2 positions[6] = vec2[](vec2(0, -1), vec2(1, -1), vec2(0, 1),
                               vec2(0, 1), vec2(1, -1), vec2(1, 1));
    vec2 p = positions[gl_VertexIndex];
    gl_Position = vec4((float(gl_InstanceIndex) + p.x) * 0.5 - 1.0, p.y, 0, 1);
    color = instanceColor;
}
