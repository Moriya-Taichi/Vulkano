#version 450
layout(binding=0,std430) readonly buffer Data { vec4 values[]; };
layout(location=0) out vec4 vertexColor;
const vec2 positions[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
void main() { gl_Position=vec4(positions[gl_VertexIndex],0.5,1); vertexColor=values[0]; }
