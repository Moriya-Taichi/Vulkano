#version 450
layout(set=2,binding=0,std430) readonly buffer Parameters { vec4 factor; };
layout(location=0) in vec4 vertexColor;
layout(location=0) out vec4 color;
void main() { color=vertexColor*factor; }
