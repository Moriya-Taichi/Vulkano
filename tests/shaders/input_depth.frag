#version 450
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInput source;
layout(location=0) out vec4 color;
void main() { color = vec4(subpassLoad(source).r, 0, 0, 1); }
