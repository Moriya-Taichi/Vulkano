#version 450
layout(constant_id=0) const uint channel = 0;
layout(push_constant) uniform Control { float value; } control;
layout(location=0) out vec4 color;
void main() { color = vec4(0); color[channel] = control.value; color.a = 1; }
