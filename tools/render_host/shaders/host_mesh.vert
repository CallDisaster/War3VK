#version 450

// RH2 laboratory: real vertex/index buffers are owned by the x64 renderer.
layout(location = 0) in vec2 position;
void main() {
  gl_Position = vec4(position, 0.0, 1.0);
}
