#version 450

layout(location = 0) out vec2 o_pos;

void main() {
  vec2 coord = vec2(
    float(gl_VertexIndex & 2),
    float(gl_VertexIndex & 1) * 2.0f);

  o_pos = coord;
  gl_Position = vec4(-1.0 + 2.0 * coord, 0.0, 1.0);
}

