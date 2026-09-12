#version 450
layout(binding=0,std430) writeonly buffer Data { vec4 values[]; };
layout(location=0) out vec4 color;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    values[p.y*2+p.x]=vec4(0,1,0,1);
    color=vec4(1,0,0,1);
}
