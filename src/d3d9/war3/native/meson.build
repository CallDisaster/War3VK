# meson.build for War3 Native Renderer
# War3 Native Renderer - 完整渲染链还原

war3_native_renderer_src = files(
  'war3_native_renderer.cpp',
  'war3_native_renderer_core.cpp',
  'war3_native_shadow.cpp',
  'war3_native_symbols.cpp',
)

war3_native_renderer_example_src = files(
  'example_usage.cpp',
)

# 创建native渲染器静态库
war3_native_lib = static_library('war3_native_renderer',
  include_directories : [
    include_directories('..'),
    include_directories('../../../'),
  ],
  sources : war3_native_renderer_src,
  cpp_std : 'c++17',
  warning_level : 3,
)

# 创建示例可执行文件（可选）
if get_option('build_examples')
  war3_native_example = executable('war3_native_example',
    include_directories : [
      include_directories('..'),
      include_directories('../../../'),
    ],
    sources : war3_native_renderer_example_src,
    link_with : [
      war3_native_lib,
    ],
    cpp_std : 'c++17',
    warning_level : 3,
  )
endif

# 安装头文件
war3_native_headers = files(
  'war3_native_renderer.h',
  'war3_native_shadow.h',
)

install_headers('war3_native', war3_native_headers,
  install_dir : get_option('includedir') / 'd3d9' / 'war3' / 'native'
)

# 导出配置
war3_native_dep = declare_dependency(
  link_with : war3_native_lib,
  include_directories : include_directories('.'),
)

summary('War3 Native Renderer',
  summary({
    'Sources' : war3_native_renderer_src.length(),
    'Examples' : war3_native_renderer_example_src.length(),
  }, section : 'War3'),
)
