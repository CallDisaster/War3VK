project('imgui', 'cpp', version : '0.0.0', default_options : [ 'cpp_std=c++17' ])

# 上游 Dear ImGui 仓库默认不提供 Meson 构建文件。
# WarVK 通过 git submodule 方式拉取上游源码，再由 build32_safe.cmd
# 将本模板同步到 subprojects/imgui/meson.build，避免直接污染 upstream 子模块。

imgui_inc = include_directories('.', 'backends')

imgui_src = files([
  'imgui.cpp',
  'imgui_draw.cpp',
  'imgui_tables.cpp',
  'imgui_widgets.cpp',
  'imgui_demo.cpp',
  'backends/imgui_impl_win32.cpp',
  'backends/imgui_impl_dx9.cpp',
])

imgui_lib = static_library(
  'imgui',
  imgui_src,
  include_directories : imgui_inc,
)

imgui_dep = declare_dependency(
  link_with           : imgui_lib,
  include_directories : imgui_inc,
)

set_variable('imgui_dep', imgui_dep)
