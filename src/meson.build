subdir('wsi')
subdir('util')
subdir('spirv')
subdir('vulkan')
subdir('dxvk')

# D3D9 module (War3 Reforge - WarVK)
if get_option('enable_d3d9') or get_option('warvk_render_host_core_dev')
  subdir('dxso')
endif
if get_option('enable_d3d9')
  subdir('d3d9')
elif not get_option('warvk_render_host_core_dev')
  warning('D3D9 is not enabled. This WarVK build requires D3D9.')
endif
