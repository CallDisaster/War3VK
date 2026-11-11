# This executable uses the same dxvk_lib as the shipping renderer, but does not
# link D3D9, Game.dll hooks, JASS, ImGui, or StormBreaker. The root build owns the
# opt-in option and includes this subdirectory only after src/ has been defined.
if not get_option('warvk_render_host_core_dev')
  error('DXVK core host is an explicit development-only target')
endif
if platform != 'windows' or cpu_family != 'x86_64'
  error('DXVK core host requires a Windows x86_64 build')
endif
if get_option('enable_d3d9')
  error('DXVK core host must use enable_d3d9=false; x86 game hooks are not portable')
endif

render_host_core = executable('warvk-dxvk-core-host',
  files('main.cpp'),
  dependencies: [dxvk_dep, vkcommon_dep, dxbc_spirv_dep, lib_gdi32, dependency('threads')],
  install: false,
)

render_host_deadline_test = executable('warvk-condition-deadline-test',
  files('../../../AutoTest/test_condition_variable_deadline.cpp'),
  dependencies: [dependency('threads')], install: false)
test('warvk-condition-deadline', render_host_deadline_test, timeout: 30)

# Same production libraries; real x86 commands arrive on a distinct lab channel.
# Not linked into shipping DLLs, and not the product's asynchronous owner yet.
render_host_buffers = executable('warvk-dxvk-buffer-host',
  files('buffer_host_main.cpp', 'buffer_backend.cpp', '../buffer_commands.cpp',
        '../protocol.cpp', '../win32_transport.cpp', '../win32_security.cpp'),
  dependencies: [dxvk_dep, vkcommon_dep, dxbc_spirv_dep, lib_gdi32,
                 cpp.find_library('bcrypt'), cpp.find_library('advapi32'), dependency('threads')],
  link_args: ['-municode'],
  install: false,
)

# Genuine D3D9 programmable-subset consumer. DXSO and its extracted SPIR-V
# helpers are the same objects linked into shipping D3D9, not copied shaders.
# CPU-only test of the exact canonical owner used by both shipping D3D9 and
# the x64 host. The orchestration runner launches it on its non-input Desktop.
render_host_shader_cache_test = executable('warvk-dxso-shader-cache-test',
  files('../../../AutoTest/test_dxso_shader_module_cache.cpp',
        '../../../src/d3d9/d3d9_options.cpp'),
  dependencies: [dxso_dep, dxvk_dep, vkcommon_dep, dxbc_spirv_dep, lib_gdi32, dependency('threads')],
  install: false,
)

# Original fixed-function source, original generated uber variants. This
# executable only compiles captured keys; adding it does NOT claim that the
# programmable-subset remote host can already execute fixed-function draws.
ff_key_stock_headers = glsl_generator.process(files(
  '../../../src/d3d9/shaders/d3d9_fixed_function_vert.vert',
  '../../../src/d3d9/shaders/d3d9_fixed_function_frag.frag'))
ff_key_skin_args = [
  '--quiet', '--target-env', 'vulkan1.3',
  '--vn', 'd3d9_war3_gpu_skin_fixed_function_vert',
  '--depfile', '@DEPFILE@', '-DD3D9_WAR3_GPU_SKIN=1',
  '@INPUT@', '-o', '@OUTPUT@',
]
if get_option('debug')
  ff_key_skin_args += ['-gVS']
endif
ff_key_skin_header = custom_target('host-d3d9-war3-gpu-skin-ff-vs',
  input: '../../../src/d3d9/shaders/d3d9_fixed_function_vert.vert',
  output: 'd3d9_war3_gpu_skin_fixed_function_vert.h',
  depfile: 'd3d9_war3_gpu_skin_fixed_function_vert.h.d',
  command: [glsl_compiler] + ff_key_skin_args)
render_host_ff_key_compile = executable('warvk-ff-key-compile',
  files('ff_key_compile_main.cpp', '../../../src/d3d9/d3d9_fixed_function.cpp',
        '../../../src/d3d9/d3d9_options.cpp'),
  ff_key_stock_headers, ff_key_skin_header,
  dependencies: [dxso_dep, dxvk_dep, vkcommon_dep, dxbc_spirv_dep,
                 lib_gdi32, dependency('threads')],
  link_args: ['-municode'],
  install: false)

render_host_d3d9 = executable('warvk-d3d9-host-dev',
  ff_key_stock_headers, ff_key_skin_header,
  files('../../../src/d3d9/d3d9_fixed_function.cpp','d3d9_host_main.cpp', 'd3d9_backend.cpp', 'd3d9_factory.cpp', 'host_surface_target.cpp', '../protocol.cpp',
        '../win32_transport.cpp', '../win32_security.cpp', '../process_memory_probe.cpp',
        '../../../src/d3d9/d3d9_options.cpp', '../../../src/d3d9/d3d9_adapter.cpp',
        '../../../src/d3d9/d3d9_format.cpp', '../../../src/d3d9/d3d9_monitor.cpp',
        '../../../src/d3d9/d3d9_names.cpp'),
  dependencies: [dxso_dep, dxvk_dep, vkcommon_dep, dxbc_spirv_dep, lib_gdi32,
                 cpp.find_library('bcrypt'), cpp.find_library('advapi32'),
                 cpp.find_library('psapi'), cpp.find_library('user32'), dependency('threads')],
  link_args: ['-municode'],
  install: false,
)

