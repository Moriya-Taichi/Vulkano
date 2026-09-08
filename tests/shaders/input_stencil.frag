#version 450
layout(input_attachment_index = 0, set = 0, binding = 0) uniform usubpassInput source;
layout(location = 0) out vec4 color;
void main() { color = vec4(float(subpassLoad(source).r) / 255.0, 0, 0, 1); }
