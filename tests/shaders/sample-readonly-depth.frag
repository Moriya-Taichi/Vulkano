#version 450
layout(set=0,binding=0) uniform sampler2D depthTexture;
layout(location=0) out vec4 color;
void main() { color=vec4(texelFetch(depthTexture,ivec2(gl_FragCoord.xy),0).rrr,1); }
