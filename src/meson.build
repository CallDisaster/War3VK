subdir('wsi')
subdir('util')
subdir('spirv')
subdir('vulkan')
subdir('dxvk')

# D3D9 module (War3 Reforge - WarVK)
if get_option('enable_d3d9')
  subdir('dxso')
  subdir('d3d9')
else
  warning('D3D9 is not enabled. This WarVK build requires D3D9.')
endif
