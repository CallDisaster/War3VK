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

# Effect executors: the sources the x86 in-process passes wrap, with their
# shaders. Nothing else of src/d3d9/war3 is built here.
host_effect_glsl = generator(glsl_compiler,
  output: ['@BASENAME@.h'], depfile: '@BASENAME@.h.d',
  arguments: glsl_args + ['-DWARVK_HOST_LAYER_RECEIVER=1'])
render_host_executor_headers = host_effect_glsl.process(war3fx_executor_shaders)
ff_shader_dir = meson.project_source_root() / 'src/d3d9/shaders'
water_glsl = generator(glsl_compiler,
  output: ['@BASENAME@.h'], depfile: '@BASENAME@.h.d',
  arguments: glsl_args + ['-I' + ff_shader_dir])
water_templates = war3fx.get_variable('war3fx_water_in_place')
water_stages = []
foreach stage : [['vert', 'war3_water_in_place_vert'], ['frag', 'war3_water_in_place']]
  # The original stage supplies position, lighting, texture stages, fog and alpha test.
  # Keep a single definition of that contract for both pipelines.
  ff_stage = import('fs').read(ff_shader_dir / ('d3d9_fixed_function_' + stage[0] + '.' + stage[0]))
  water_stages += [configure_file(input: water_templates[water_stages.length()],
    output: stage[1] + '.' + stage[0],
    configuration: {'FIXED_FUNCTION': ff_stage.replace('void main()', 'void gameFixedFunction()')})]
endforeach
render_host_d3d9 = executable('warvk-d3d9-host-dev',
  ff_key_stock_headers, ff_key_skin_header, render_host_executor_headers, water_glsl.process(water_stages),
  files('../../../src/d3d9/war3/effects/war3_snow_executor.cpp',
        '../../../src/d3d9/war3/effects/war3_ssao_executor.cpp',
        '../../../src/d3d9/war3/effects/war3_aa_executor.cpp',
        '../../../src/d3d9/war3/effects/war3_bloom_executor.cpp',
        '../../../src/d3d9/war3/render/war3_tracked_vk_pipeline.cpp'),
  # Phase 4c: the shadow pass itself, the same sources as the x86 device, with
  # the CPU helpers it owns. What it reads from the x86 data layer and reports
  # to diagnostics is provided by shadow_host_services.cpp.
  files('../../../src/d3d9/d3d9_war3_shadow.cpp',
        '../../../src/d3d9/d3d9_war3_shadow_resources.cpp',
        '../../../src/d3d9/d3d9_war3_shadow_outline.cpp',
        '../../../src/d3d9/d3d9_war3_csm.cpp',
        '../../../src/d3d9/war3/render/war3_gpu_workload_governor.cpp',
        '../../../src/d3d9/war3/render/war3_hybrid_ray_tracing.cpp',
        '../../../src/d3d9/war3/render/war3_point_shadow_cpu_plan.cpp',
        '../../../src/d3d9/war3/render/war3_shadow_replay_validation.cpp',
        '../../../src/d3d9/war3/state/war3_fog_volume.cpp',
        'shadow_host_services.cpp', 'shadow_before_ui.cpp', 'evidence_recorder.cpp'),
  # Phase 4d: the volumetric light pass, as is, with the CPU helper it owns. It
  # reads the shadow pass above and nothing else of the device.
  files('../../../src/d3d9/d3d9_war3_volumetric_light.cpp',
        '../../../src/d3d9/war3/render/war3_volumetric_shader_work_admission.cpp'),
  # The water surface pass: it reads the shadow pass's water images.
  files('../../../src/d3d9/d3d9_war3_water.cpp', 'water_in_place.cpp'),
  files('../../../src/d3d9/d3d9_fixed_function.cpp','d3d9_host_main.cpp', 'd3d9_backend.cpp', 'd3d9_factory.cpp', 'host_surface_target.cpp', '../protocol.cpp',
        '../win32_transport.cpp', '../win32_security.cpp', '../process_memory_probe.cpp',
        '../../../src/d3d9/d3d9_options.cpp', '../../../src/d3d9/d3d9_adapter.cpp',
        '../../../src/d3d9/d3d9_format.cpp', '../../../src/d3d9/d3d9_monitor.cpp',
        '../../../src/d3d9/d3d9_names.cpp'),
  dependencies: [dxso_dep, dxvk_dep, vkcommon_dep, dxbc_spirv_dep, lib_gdi32,
                 cpp.find_library('bcrypt'), cpp.find_library('advapi32'),
                 cpp.find_library('psapi'), cpp.find_library('user32'), cpp.find_library('ole32'),
                 dependency('threads')],
  # The shadow pass includes war3 headers the way the x86 DLL builds them.
  include_directories: [include_directories('../../../src/d3d9', '../../../src/d3d9/war3')],
  link_args: ['-municode'],
  install: false,
)

