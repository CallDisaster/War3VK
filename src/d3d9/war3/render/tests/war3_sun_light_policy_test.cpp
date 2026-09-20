#include "../war3_sun_light_policy.h"
#include <d3d9.h>
#include <cstring>
#include <iostream>
#include <limits>

int main() {
  D3DLIGHT9 light = {};
  light.Type = D3DLIGHT_DIRECTIONAL;
  light.Diffuse = {0.9f, 0.8f, 0.7f, 0.6f};
  light.Specular = {0.6f, 0.5f, 0.4f, 0.3f};
  light.Ambient = {0.2f, 0.1f, 0.3f, 1.0f};
  light.Direction = {1.0f, 2.0f, 3.0f};
  light.Range = 10000.0f;
  D3DLIGHT9 expected = light;
  expected.Diffuse.r = expected.Diffuse.g = expected.Diffuse.b = 0.0f;
  expected.Specular.r = expected.Specular.g = expected.Specular.b = 0.0f;
  dxvk::war3::render::ApplyWar3DisabledSun(light);
  if (std::memcmp(&light, &expected, sizeof(light)) != 0) {
    std::cerr << "Sun gate altered ambient/alpha/shape or failed to zero direct RGB\n";
    return 1;
  }
  using dxvk::war3::render::War3SunDirectIntensity;
  if (War3SunDirectIntensity(false, 4.0f) != 0.0f ||
      War3SunDirectIntensity(true, 4.0f) != 4.0f ||
      War3SunDirectIntensity(true, -1.0f) != 0.0f ||
      War3SunDirectIntensity(true, std::numeric_limits<float>::infinity()) != 0.0f ||
      War3SunDirectIntensity(true, std::numeric_limits<float>::quiet_NaN()) != 0.0f)
    return 2;
  std::cout << "sun policy: exact D3DLIGHT9 fields and finite intensity passed\n";
  return 0;
}
