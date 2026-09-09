#version 450
layout(constant_id=0) const float red=1.0;
layout(location=0) out vec4 color;
void main(){color=vec4(red,0.25,0.5,1);}
