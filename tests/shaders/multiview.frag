#version 450
layout(location=0) flat in int view;
layout(location=0) out vec4 color;
void main() { color=view==0?vec4(1,0,0,1):vec4(0,1,0,1); }
