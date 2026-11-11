#version 450

layout(push_constant) uniform FrameVariant {
  uint variant;
} frame;
layout(location = 0) out vec4 color;

void main() {
  const vec3 colors[4] = vec3[4](
    vec3(1.0, 0.125, 0.0625),
    vec3(0.0625, 1.0, 0.125),
    vec3(0.125, 0.0625, 1.0),
    vec3(1.0, 1.0, 0.0625));
  color = vec4(colors[frame.variant & 3u], 1.0);
}
