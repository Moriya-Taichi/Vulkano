#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler filtering;
void main(){color=texture(sampler2D(image,filtering),uv);}
