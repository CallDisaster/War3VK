d3d9_res = wrc_generator.process('version.rc')

d3d9_shaders = files([
  'shaders/d3d9_convert_yuy2_uyvy.comp',
  'shaders/d3d9_convert_l6v5u5.comp',
  'shaders/d3d9_convert_x8l8v8u8.comp',
  'shaders/d3d9_convert_a2w10v10u10.comp',
  'shaders/d3d9_convert_w11v11u10.comp',
  'shaders/d3d9_convert_nv12.comp',
  'shaders/d3d9_convert_yv12.comp',
  'shaders/d3d9_fixed_function_vert.vert',
  'shaders/d3d9_fixed_function_frag.frag',
])

d3d9_shaders += war3fx_shaders

# War3 GPU 蒙皮使用独立的 fixed-function VS 变体。stock shader 仍由通用
# generator 原样生成，避免所有 D3D9 fixed-function draw 都承担私有 binding 契约。
d3d9_war3_gpu_skin_ff_vs_args = [
  '--quiet',
  '--target-env', 'vulkan1.3',
  '--vn', 'd3d9_war3_gpu_skin_fixed_function_vert',
  '--depfile', '@DEPFILE@',
  '-DD3D9_WAR3_GPU_SKIN=1',
  '@INPUT@',
  '-o', '@OUTPUT@',
]
if get_option('debug')
  d3d9_war3_gpu_skin_ff_vs_args += ['-gVS']
endif
d3d9_war3_gpu_skin_ff_vs = custom_target(
  'd3d9_war3_gpu_skin_fixed_function_vert',
  input   : 'shaders/d3d9_fixed_function_vert.vert',
  output  : 'd3d9_war3_gpu_skin_fixed_function_vert.h',
  depfile : 'd3d9_war3_gpu_skin_fixed_function_vert.h.d',
  command : [glsl_compiler] + d3d9_war3_gpu_skin_ff_vs_args,
)

d3d9_src = [
  'd3d9_main.cpp',
  'd3d9_interface.cpp',
  'd3d9_adapter.cpp',
  'd3d9_monitor.cpp',
  'd3d9_device.cpp',
  'd3d9_state.cpp',
  'd3d9_cursor.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_format.cpp',
  'd3d9_common_texture.cpp',
  'd3d9_constant_buffer.cpp',
  'd3d9_texture.cpp',
  'd3d9_surface.cpp',
  'd3d9_volume.cpp',
  'd3d9_common_buffer.cpp',
  'd3d9_buffer.cpp',
  'd3d9_shader.cpp',
  'd3d9_vertex_declaration.cpp',
  'd3d9_query.cpp',
  'd3d9_shader_validator.cpp',
  'd3d9_multithread.cpp',
  'd3d9_options.cpp',
  'd3d9_stateblock.cpp',
  'd3d9_util.cpp',
  'd3d9_initializer.cpp',
  'd3d9_fixed_function.cpp',
  'd3d9_names.cpp',
  'd3d9_swvp_emu.cpp',
  'd3d9_format_helpers.cpp',
  'd3d9_hud.cpp',
  'd3d9_annotation.cpp',
  'd3d9_mem.cpp',
  'd3d9_window.cpp',
  'd3d9_interop.cpp',
  'd3d9_on_12.cpp',
  'd3d9_bridge.cpp',
  # War3 主文件（保留，逐步迁移）
  'd3d9_war3_hook.cpp',
  'd3d9_war3_csm.cpp',
  'd3d9_war3_pathtrace.cpp',
  'd3d9_war3_pipeline.cpp',
  'd3d9_war3_shadow.cpp',
  'd3d9_war3_shadow_resources.cpp',
  'd3d9_war3_shadow_outline.cpp',
  'd3d9_war3_fog_volume.cpp',
  'd3d9_war3_volumetric_light.cpp',
  'war3/render/war3_hybrid_ray_tracing.cpp',
  'd3d9_war3_ssao.cpp',
  'war3_shader_api.cpp',
  'war3_shaderpack.cpp',
  'war3_shaderpack_image.cpp',
  'd3d9_war3_aa.cpp',
  'd3d9_war3_branding.cpp',
  # Jass 子目录
  'jass/war3_game.cpp',
  'jass/war3_jass.cpp',
  'jass/war3_jass_native.cpp',
  'jass/war3_jass_convert.cpp',
  'jass/war3_hook.cpp',
  # War3 新模块
  'war3/war3.cpp',
  'war3/bridge/war3_visual_bridge_v1.cpp',
  'war3/japi/war3_japi_v1.cpp',
  'war3/core/war3_memory.cpp',
  'war3/core/war3_crash_handler.cpp',
  'war3/core/war3_runtime_profile.cpp',
  'war3/core/war3_semantic_shadow_gate.cpp',
  'war3/platform/war3_module_api.cpp',
  'war3/platform/war3_native_device_resolver.cpp',
  'war3/platform/war3_runtime_bootstrap.cpp',
  'war3/math/war3_math_expression.cpp',
  'war3/math/war3_curve_runtime.cpp',
  'war3/memory/tlsf.c',
  'war3/memory/war3_tlsf_pool.cpp',
  'war3/memory/war3_storm_hook.cpp',
  'war3/memory/war3_storm_native_small_repair.cpp',
  'war3/memory/war3_shadow_arena.cpp',
  'war3/memory/war3_cpu_readable_buffer_span.cpp',
  'war3/handle/war3_handle_resolver.cpp',
  'war3/render/war3_render_objects.cpp',
  'war3/render/war3_render_identity_bridge.cpp',
  'war3/render/war3_shadow_runtime_bridge.cpp',
  'war3/render/war3_shadow_lifecycle.cpp',
  'war3/render/war3_shadow_producer_policy.cpp',
  'war3/render/war3_shadow_replay_validation.cpp',
  'war3/render/war3_point_shadow_cpu_plan.cpp',
  'war3/render/war3_gpu_workload_governor.cpp',
  'war3/render/war3_volumetric_shader_work_admission.cpp',
  'war3/render/war3_union_consumer_visibility.cpp',
  'war3/render/war3_current_draw_contract.cpp',
  'war3/render/war3_scene_collector.cpp',
  'war3/render/war3_renderer.cpp',
  'war3/render/war3_shadow_object_registry.cpp',
  'war3/render/war3_render_dispatcher.cpp',
  'war3/render/war3_frame_graph.cpp',
  'war3/render/war3_canonical_draw.cpp',
  'war3/render/war3_render_queue_tracker.cpp',
  'war3/render/war3_render_exec_batch.cpp',
  'war3/render/war3_render_state.cpp',
  'war3/render/war3_tracked_vk_pipeline.cpp',
  'war3/render/war3_upper_layer_shadow.cpp',
  'war3/render/war3_visible_renderables.cpp',
  'war3/render/war3_shadow_capture_frontend.cpp',
  'war3/render/war3_native_renderer_probe.cpp',
  'war3/render/war3_post_process.cpp',
  'war3/render/war3_lightning_runtime.cpp',
  'war3/state/war3_render_state.cpp',
  'war3/debug/war3_debug.cpp',
  'war3/tools/war3_perf_monitor.cpp',
  'war3/tools/war3_resource_residency_census.cpp',
  'war3/tools/war3_diagnostics_hub.cpp',
  'war3/tools/war3_control_plane.cpp',
  'war3/tools/war3_frame_capture.cpp',
  'war3/tools/war3_internal_test_api.cpp',
  'war3/core/war3_storm.cpp',
  'war3/core/war3_file_manager.cpp',
  'war3/model/war3_model_hook.cpp',
  'war3/model/war3_model_resource_cache.cpp',
  'war3/model/war3_model_registry.cpp',
  'war3/gpu_skin/war3_gpu_skin_compute.cpp',
  'war3/gpu_skin/war3_cpu_skin_mt.cpp',
  'war3/gpu_skin/war3_gpu_skin_manager.cpp',
  'war3/gpu_skin/war3_gpu_skin_native_bridge.cpp',
  'war3/gpu_skin/war3_persistent_gpu_package_observer.cpp',
  'war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.cpp',
  'war3/gpu_skin/war3_persistent_gpu_package_d3d9_observe_owner.cpp',
  'war3/gpu_skin/war3_persistent_gpu_package_owner.cpp',
  'war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.cpp',
  'war3/gpu_skin/war3_persistent_gpu_package_immutable.cpp',
  'war3/gpu_skin/war3_persistent_gpu_package_store.cpp',
  'war3/gpu_skin/war3_gpu_skin_resources.cpp',
  'war3/shadow/war3_shadow_runtime_contract.cpp',
  'war3/shadow/war3_shadow_renderer_core.cpp',
  'war3/shadow/war3_shadow_backend_dxvk.cpp',
  'war3/shadow/war3_shadow_backend_native_d3d9.cpp',
  'war3/shadow/war3_shadow_native_runtime.cpp',
  'war3/shadow/war3_shadow_alpha_test_payload.cpp',
  'war3/native/war3_native_hooks.cpp',
  'war3/native/war3_native_renderer.cpp',
  'war3/native/war3_native_renderer_core.cpp',
  'war3/native/war3_native_shadow.cpp',
  'war3/native/war3_native_shadow_hint.cpp',
  'war3/native/war3_native_symbols.cpp',
  'war3/war3_user_example.cpp',
  'war3/game/war3_unit.cpp',
  'war3/game/war3_agent.cpp',
  'war3/shader/war3_shader_manager.cpp',
  'war3/shader/war3_material.cpp',
  'war3/ui/war3_imgui.cpp',
  'war3/hooks/war3_hook_address_book.cpp',
  'war3/hooks/war3_hook_ui.cpp',
  'war3/hooks/war3_hook_shadow.cpp',
  'war3/hooks/war3_hook_widget_identity.cpp',
  'war3/hooks/war3_hook_install_util.cpp',
  'war3/hooks/war3_hook_render_identity.cpp',
  'war3/hooks/war3_dispatch_contract.cpp',
  'war3/hooks/war3_queue_takeover_policy.cpp',
  'war3/hooks/war3_shadow_filter_policy.cpp',
  'war3/hooks/war3_hook_render.cpp',
  'war3/hooks/war3_hook_lifecycle.cpp',
  'war3/hooks/war3_hook_jass.cpp',
  'war3/hooks/war3_jass_command_bridge.cpp',
  'war3/hooks/war3_jass_native_plan_cache.cpp',
  'war3/hooks/war3_jass_native_invoke_x86.cpp',
  'war3/hooks/war3_stage_tag_map.cpp',
  'war3/reimpl/war3_instance_buffer.cpp',
  'war3/reimpl/war3_shader_patcher.cpp',
  'war3/reimpl/war3_team_color_manager.cpp',
]


# MinHook configuration
minhook_lib_path = join_paths(meson.source_root(), 'src/minhook')
minhook_inc_path = include_directories('../MinHook/include')

d3d9_ld_args      = [
  join_paths(minhook_lib_path, 'libMinHook.x86.a')
]
d3d9_link_depends = []

if platform != 'windows'
  d3d9_ld_args      += [ '-Wl,--version-script', join_paths(meson.current_source_dir(), 'd3d9.sym') ]
  d3d9_link_depends += files('d3d9.sym')
endif

lib_dwmapi = cpp.find_library('dwmapi')
lib_dbghelp = cpp.find_library('dbghelp')

d3d9_cpp_args = []
if get_option('warvk_shadow_observers_dev')
  d3d9_cpp_args += [ '-DWARVK_ENABLE_SHADOW_OBSERVERS_DEV=1' ]
endif
if get_option('warvk_rts_shadow_candidate_dev')
  d3d9_cpp_args += [ '-DWARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV=1' ]
endif
if get_option('warvk_coherent_up_index_trim_dev')
  d3d9_cpp_args += [ '-DWARVK_ENABLE_COHERENT_UP_INDEX_TRIM_DEV=1' ]
endif
if get_option('warvk_current_up_shadow_replay_dev')
  d3d9_cpp_args += [ '-DWARVK_ENABLE_CURRENT_UP_SHADOW_REPLAY_DEV=1' ]
endif
if get_option('warvk_coherent_real_perf_candidate_dev')
  d3d9_cpp_args += [
    '-DWARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV=1',
    '-DWARVK_ENABLE_COHERENT_REAL_PERF_CANDIDATE_DEV=1',
  ]
elif get_option('warvk_coherent_real_index_trim_dev')
  d3d9_cpp_args += [ '-DWARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV=1' ]
endif
if get_option('warvk_device_address_binding_report_dev')
  # Keep the fixed-capacity tracker type identical across the DXVK static
  # library and every D3D9 translation unit that includes its value snapshots.
  d3d9_cpp_args += [ '-DWARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV=1' ]
endif

d3d9_dll = shared_library(dxvk_name_prefix+'d3d9', d3d9_src,
  glsl_generator.process(d3d9_shaders), d3d9_war3_gpu_skin_ff_vs, d3d9_res,
  dependencies        : [ dxso_dep, dxvk_dep, imgui_dep, lib_dwmapi, lib_dbghelp ],
  include_directories : [ dxvk_include_path, minhook_inc_path ],
  install             : true,
  vs_module_defs      : 'd3d9'+def_spec_ext,
  link_args           : d3d9_ld_args,
  link_depends        : [ d3d9_link_depends ],
  cpp_args            : d3d9_cpp_args,
  kwargs              : dxvk_so_version,
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ dxvk_include_path ],
)

# Public WarVK v1 parser/dispatcher contract. This compiles the exact
# production source with the rendering backend replaced by a fail-closed stub,
# so strict wire parsing can run without loading d3d9.dll or Warcraft.
war3_japi_v1_protocol_test = executable(
  'war3_japi_v1_protocol_test',
  [
    'war3/japi/war3_japi_v1.cpp',
    'war3/japi/tests/war3_japi_v1_protocol_test.cpp',
  ],
  cpp_args            : [ '-DWARVK_JAPI_PROTOCOL_TEST=1' ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_japi_v1_protocol',
  war3_japi_v1_protocol_test,
  timeout : 30,
)

# The old warvk:cmd: bridge is release-disabled.  Build the policy twice so
# both the default-deny binary and the explicit developer-only capability are
# executable contracts; neither path relies on an environment variable.
war3_jass_legacy_command_policy_release_test = executable(
  'war3_jass_legacy_command_policy_release_test',
  [
    'war3/hooks/tests/war3_jass_legacy_command_policy_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_jass_legacy_command_policy_release',
  war3_jass_legacy_command_policy_release_test,
  timeout : 30,
)

war3_jass_legacy_command_policy_dev_test = executable(
  'war3_jass_legacy_command_policy_dev_test',
  [
    'war3/hooks/tests/war3_jass_legacy_command_policy_test.cpp',
  ],
  cpp_args            : [
    '-DWARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV=1',
    '-DWARVK_EXPECT_LEGACY_JASS_COMMANDS_DEV=1',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_jass_legacy_command_policy_dev',
  war3_jass_legacy_command_policy_dev_test,
  timeout : 30,
)

# Raw SPIR-V ShaderPack loading is compile-time developer-only.  Test both the
# production default-deny policy and the one explicit development capability;
# the production d3d9 shared library above never receives the enable macro.
war3_shaderpack_policy_release_test = executable(
  'war3_shaderpack_policy_release_test',
  [
    'war3/render/tests/war3_shaderpack_policy_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shaderpack_policy_release',
  war3_shaderpack_policy_release_test,
  timeout : 30,
)

war3_shaderpack_policy_dev_test = executable(
  'war3_shaderpack_policy_dev_test',
  [
    'war3/render/tests/war3_shaderpack_policy_test.cpp',
  ],
  cpp_args            : [
    '-DWARVK_ENABLE_RAW_SHADERPACK_DEV=1',
    '-DWARVK_EXPECT_RAW_SHADERPACK_DEV=1',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shaderpack_policy_dev',
  war3_shaderpack_policy_dev_test,
  timeout : 30,
)

# Numerical CSM receiver oracle. It deliberately contains no Vulkan or game
# state so its sub-texel, receiver-plane and cascade-boundary contracts run on
# every Win32 build.
war3_shadow_receiver_numeric_contract_test = executable(
  'war3_shadow_receiver_numeric_contract_test',
  [
    'war3/render/tests/war3_shadow_receiver_numeric_contract_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_receiver_numeric_contract',
  war3_shadow_receiver_numeric_contract_test,
  timeout : 30,
)

# Pure one-pass current-draw group-slot aggregate. This locks the historical
# stable identity hash while allowing BuildEligible to reuse the maximum
# palette slot produced by the validating decode pass.
war3_current_draw_group_slot_summary_test = executable(
  'war3_current_draw_group_slot_summary_test',
  [
    'war3/render/tests/war3_current_draw_group_slot_summary_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_current_draw_group_slot_summary',
  war3_current_draw_group_slot_summary_test,
  timeout : 30,
)

# Byte-exact War3 palette 3x4 pack contract. The hot publish path writes this
# representation directly instead of materializing an intermediate vector.
war3_palette_pack_test = executable(
  'war3_palette_pack_test',
  [
    'war3/model/tests/war3_palette_pack_test.cpp',
    '../util/util_matrix.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_palette_pack',
  war3_palette_pack_test,
  timeout : 30,
)

# Palette decoding already touches every matrix. Lock byte-for-byte identity
# with the historical FNV oracle while producing that hash in the same pass.
war3_current_draw_palette_hash_test = executable(
  'war3_current_draw_palette_hash_test',
  [
    'war3/render/tests/war3_current_draw_palette_hash_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_current_draw_palette_hash',
  war3_current_draw_palette_hash_test,
  timeout : 30,
)

# Pure CurrentDraw identity normalization shared by packet resolution and the
# Stage11 exact-owner pre-build gate.
war3_current_draw_identity_contract_test = executable(
  'war3_current_draw_identity_contract_test',
  [
    'war3/render/tests/war3_current_draw_identity_contract_test.cpp',
  ],
  include_directories : [ dxvk_include_path, include_directories('war3') ],
  install             : false,
)
test(
  'war3_current_draw_identity_contract',
  war3_current_draw_identity_contract_test,
  timeout : 30,
)

# DirectGrouped scratch reset keeps allocator capacity only. The runnable
# proves identities, pointer aliases and shared owners never survive reuse.
war3_direct_packet_scratch_test = executable(
  'war3_direct_packet_scratch_test',
  [
    'war3/render/tests/war3_direct_packet_scratch_test.cpp',
  ],
  include_directories : [ dxvk_include_path, include_directories('war3') ],
  install             : false,
)
test(
  'war3_direct_packet_scratch',
  war3_direct_packet_scratch_test,
  timeout : 30,
)

# Pure receiver-band clipmap fit used by the default-off RTS shadow candidate.
# It exercises fixed world-texel density, snapping and fail-closed coverage
# without loading the game or Vulkan.
war3_rts_shadow_stability_contract_test = executable(
  'war3_rts_shadow_stability_contract_test',
  [
    'war3/render/tests/war3_rts_shadow_stability_contract_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_rts_shadow_stability_contract',
  war3_rts_shadow_stability_contract_test,
  timeout : 30,
)

war3_shadow_producer_completeness_policy_test = executable(
  'war3_shadow_producer_completeness_policy_test',
  [
    'war3/render/tests/war3_shadow_producer_completeness_policy_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_producer_completeness_policy',
  war3_shadow_producer_completeness_policy_test,
  timeout : 30,
)

# Release and development policy tests are separate pure executables. The
# production DLL only receives the development macro through the explicit
# Meson option above; neither policy can return Consume.
war3_shadow_observer_build_policy_release_test = executable(
  'war3_shadow_observer_build_policy_release_test',
  [
    'war3/render/tests/war3_shadow_observer_build_policy_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_observer_build_policy_release',
  war3_shadow_observer_build_policy_release_test,
  timeout : 30,
)

war3_shadow_observer_build_policy_dev_test = executable(
  'war3_shadow_observer_build_policy_dev_test',
  [
    'war3/render/tests/war3_shadow_observer_build_policy_test.cpp',
  ],
  cpp_args            : [
    '-DWARVK_ENABLE_SHADOW_OBSERVERS_DEV=1',
    '-DWARVK_EXPECT_SHADOW_OBSERVERS_DEV=1',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_observer_build_policy_dev',
  war3_shadow_observer_build_policy_dev_test,
  timeout : 30,
)

# Exact terrain bounds may consume a non-indexed draw range directly, but an
# indexed draw must first prove its real domain from current IB bytes. This
# policy test deliberately has no D3D9/Vulkan dependency.
war3_terrain_bounds_provenance_test = executable(
  'war3_terrain_bounds_provenance_test',
  [
    'war3/render/tests/war3_terrain_bounds_provenance_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_terrain_bounds_provenance',
  war3_terrain_bounds_provenance_test,
  timeout : 30,
)

# The production pipeline owner uses the same Rc lifetime semantics exercised
# here: a cache may release immediately, while an in-flight command list holds
# the final owner until its GPU completion notification.
war3_tracked_vk_pipeline_test = executable(
  'war3_tracked_vk_pipeline_test',
  [
    'war3/render/tests/war3_tracked_vk_pipeline_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_tracked_vk_pipeline',
  war3_tracked_vk_pipeline_test,
  timeout : 30,
)

# Pure Phase-1 math-expression and curve-runtime test. It compiles and
# evaluates bounded bytecode without loading Warcraft or creating GPU state.
war3_math_curve_runtime_test = executable(
  'war3_math_curve_runtime_test',
  [
    'war3/math/war3_math_expression.cpp',
    'war3/math/war3_curve_runtime.cpp',
    'war3/math/tests/war3_math_curve_runtime_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_math_curve_runtime',
  war3_math_curve_runtime_test,
  timeout : 30,
)

# Pure, runnable policy test. It does not link d3d9.dll and therefore cannot
# mutate or accidentally exercise the live War3 render paths.
war3_union_consumer_visibility_test = executable(
  'war3_union_consumer_visibility_test',
  [
    'war3/render/war3_union_consumer_visibility.cpp',
    'war3/render/tests/war3_union_consumer_visibility_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_union_consumer_visibility',
  war3_union_consumer_visibility_test,
  timeout : 30,
)

# Pure all-or-nothing shadow workload reservation contract. The runnable has
# no Vulkan or Warcraft dependency and covers arithmetic overflow as well as
# per-consumer draw/vertex/index admission.
war3_gpu_workload_governor_test = executable(
  'war3_gpu_workload_governor_test',
  [
    'war3/render/war3_gpu_workload_governor.cpp',
    'war3/render/tests/war3_gpu_workload_governor_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_gpu_workload_governor',
  war3_gpu_workload_governor_test,
  timeout : 30,
)

# Pure-value P0 shader-work admission. It proves that the ray-segment loop,
# directional D32 taps, point-cube probes and full-resolution composite reads
# are checked as one bounded request before the optional volume pass executes.
war3_volumetric_shader_work_admission_test = executable(
  'war3_volumetric_shader_work_admission_test',
  [
    'war3/render/war3_volumetric_shader_work_admission.cpp',
    'war3/render/tests/war3_volumetric_shader_work_admission_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_volumetric_shader_work_admission',
  war3_volumetric_shader_work_admission_test,
  timeout : 30,
)

war3_geoset_local_bounds_test = executable(
  'war3_geoset_local_bounds_test',
  [
    'war3/model/tests/war3_geoset_local_bounds_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_geoset_local_bounds',
  war3_geoset_local_bounds_test,
  timeout : 30,
)

war3_shadow_replay_validation_test = executable(
  'war3_shadow_replay_validation_test',
  [
    'war3/render/war3_shadow_replay_validation.cpp',
    'war3/render/tests/war3_shadow_replay_validation_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_replay_validation',
  war3_shadow_replay_validation_test,
  timeout : 30,
)

war3_shadow_lifecycle_test = executable(
  'war3_shadow_lifecycle_test',
  [
    'war3/render/war3_shadow_lifecycle.cpp',
    'war3/render/tests/war3_shadow_lifecycle_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_lifecycle',
  war3_shadow_lifecycle_test,
  timeout : 30,
)

war3_owned_image_layout_test = executable(
  'war3_owned_image_layout_test',
  [
    'war3/render/tests/war3_owned_image_layout_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_owned_image_layout',
  war3_owned_image_layout_test,
  timeout : 30,
)

war3_shadow_arena_lifecycle_test = executable(
  'war3_shadow_arena_lifecycle_test',
  [
    'war3/memory/tests/war3_shadow_arena_lifecycle_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_arena_lifecycle',
  war3_shadow_arena_lifecycle_test,
  timeout : 30,
)

# Pure P1 residency admission policy. No Vulkan device is created: the test
# proves release fallback, trusted memory-budget clamps and overflow-safe page
# growth decisions independently from the Arena's GPU-owned generations.
war3_shadow_arena_budget_test = executable(
  'war3_shadow_arena_budget_test',
  [
    'war3/memory/tests/war3_shadow_arena_budget_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_shadow_arena_budget',
  war3_shadow_arena_budget_test,
  timeout : 30,
)

# P0 ownership/API policy test. It constructs no DXVK object and proves that
# the new persistent package store cannot create itself at the D3D9 boundary.
war3_persistent_gpu_package_owner_test = executable(
  'war3_persistent_gpu_package_owner_test',
  [
    'war3/gpu_skin/tests/war3_persistent_gpu_package_owner_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_persistent_gpu_package_owner',
  war3_persistent_gpu_package_owner_test,
  timeout : 30,
)

# Value-only P1 lifecycle proof. It records upload and consumer last-use fence
# identities/values for isolated tests, but is not instantiated by the store,
# D3D9 device or any renderer consumer.
war3_persistent_gpu_package_generation_owner_test = executable(
  'war3_persistent_gpu_package_generation_owner_test',
  [
    'war3/gpu_skin/war3_persistent_gpu_package_owner.cpp',
    'war3/gpu_skin/tests/war3_persistent_gpu_package_generation_owner_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_persistent_gpu_package_generation_owner',
  war3_persistent_gpu_package_generation_owner_test,
  timeout : 30,
)

# Value-only P1 package observer. Production compiles the policy/table, but no
# renderer, store or D3D9 object instantiates it and Consume remains denied.
war3_persistent_gpu_package_observer_test = executable(
  'war3_persistent_gpu_package_observer_test',
  [
    'war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.cpp',
    'war3/gpu_skin/war3_persistent_gpu_package_observer.cpp',
    'war3/gpu_skin/tests/war3_persistent_gpu_package_observer_test.cpp',
  ],
  dependencies        : [ dependency('threads') ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_persistent_gpu_package_observer',
  war3_persistent_gpu_package_observer_test,
  timeout : 30,
)

# Runtime-integrated but evidence-only Stage11 adapter. The isolated runnable
# proves source generations, exact accepted-witness gates, hard zero package /
# consumer authority and timing aggregation without constructing DXVK/GPU
# resources.
war3_persistent_gpu_package_stage11_observe_adapter_test = executable(
  'war3_persistent_gpu_package_stage11_observe_adapter_test',
  [
    'war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.cpp',
    'war3/gpu_skin/tests/war3_persistent_gpu_package_stage11_observe_adapter_test.cpp',
  ],
  dependencies        : [ dependency('threads') ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_persistent_gpu_package_stage11_observe_adapter',
  war3_persistent_gpu_package_stage11_observe_adapter_test,
  timeout : 30,
)

# Immutable CPU/value-only package proof publication. The production library
# compiles this catalog, but no store, manager, D3D9 device or renderer path
# instantiates it and shared-package Consume remains denied.
war3_persistent_gpu_package_proof_catalog_test = executable(
  'war3_persistent_gpu_package_proof_catalog_test',
  [
    'war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.cpp',
    'war3/gpu_skin/tests/war3_persistent_gpu_package_proof_catalog_test.cpp',
  ],
  dependencies        : [ dependency('threads') ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_persistent_gpu_package_proof_catalog',
  war3_persistent_gpu_package_proof_catalog_test,
  timeout : 30,
)

# CPU-only immutable model/package authority test. It never constructs a
# device, Store, renderer, atlas or GPU slice.
war3_persistent_gpu_package_immutable_test = executable(
  'war3_persistent_gpu_package_immutable_test',
  [
    'war3/gpu_skin/war3_persistent_gpu_package_immutable.cpp',
    'war3/gpu_skin/tests/war3_persistent_gpu_package_immutable_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_persistent_gpu_package_immutable',
  war3_persistent_gpu_package_immutable_test,
  timeout : 30,
)

# Value-only current-draw/package equivalence gate. It hashes exact strided
# FLOAT3 bytes and rejects every incomplete route before any future renderer
# consumer can publish would-use authority.
war3_persistent_gpu_package_current_draw_equivalence_test = executable(
  'war3_persistent_gpu_package_current_draw_equivalence_test',
  [
    'war3/gpu_skin/war3_persistent_gpu_package_immutable.cpp',
    'war3/gpu_skin/tests/war3_persistent_gpu_package_current_draw_equivalence_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_persistent_gpu_package_current_draw_equivalence',
  war3_persistent_gpu_package_current_draw_equivalence_test,
  timeout : 30,
)

# Runnable ownership/lifetime test for the point-shadow persistent mailbox.
# The production pass lazily instantiates the same mailbox only when its
# explicit Observe/Consume mode is requested; release default remains Off.
war3_point_shadow_prepare_worker_test = executable(
  'war3_point_shadow_prepare_worker_test',
  [
    'war3/render/tests/war3_point_shadow_prepare_worker_test.cpp',
  ],
  dependencies        : [ dependency('threads') ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_point_shadow_prepare_worker_foundation',
  war3_point_shadow_prepare_worker_test,
  timeout : 30,
)

# Pure owned-value planner used by the opt-in persistent point-shadow path and
# independently linked into this runnable for algorithm/ownership oracles.
war3_point_shadow_cpu_plan_test = executable(
  'war3_point_shadow_cpu_plan_test',
  [
    'war3/render/war3_point_shadow_cpu_plan.cpp',
    'war3/render/tests/war3_point_shadow_cpu_plan_test.cpp',
  ],
  dependencies        : [ dependency('threads') ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_point_shadow_cpu_plan_foundation',
  war3_point_shadow_cpu_plan_test,
  timeout : 30,
)

# CPU-only local-medium manager test. It validates finite input bounds,
# normalized shape transforms, immutable frame snapshots and map-reset
# lifetime without constructing a D3D9 device or any GPU resource.
war3_fog_volume_manager_test = executable(
  'war3_fog_volume_manager_test',
  [
    'd3d9_war3_fog_volume.cpp',
    'war3/render/tests/war3_fog_volume_manager_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_fog_volume_manager',
  war3_fog_volume_manager_test,
  timeout : 30,
)

# Runnable boundary proof for CPU shadow-source reads. The guard-page case
# proves that invalid D3D9 binding offsets are rejected from the allocation
# metadata without touching the adjacent inaccessible page.
war3_cpu_readable_buffer_span_test = executable(
  'war3_cpu_readable_buffer_span_test',
  [
    'war3/memory/war3_cpu_readable_buffer_span.cpp',
    'war3/memory/tests/war3_cpu_readable_buffer_span_test.cpp',
  ],
  cpp_args            : [
    '-DWARVK_ENABLE_COHERENT_UP_INDEX_TRIM_DEV=1',
    '-DWARVK_ENABLE_CURRENT_UP_SHADOW_REPLAY_DEV=1',
    '-DWARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV=1',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_cpu_readable_buffer_span',
  war3_cpu_readable_buffer_span_test,
  timeout : 30,
)

# Development-candidate policy test. The production/default D3D9 target never
# receives these macros unless the dedicated Meson option is selected.
war3_coherent_real_perf_candidate_policy_test = executable(
  'war3_coherent_real_perf_candidate_policy_test',
  [
    'war3/memory/tests/war3_coherent_real_perf_candidate_policy_test.cpp',
  ],
  cpp_args            : [
    '-DWARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV=1',
    '-DWARVK_ENABLE_COHERENT_REAL_PERF_CANDIDATE_DEV=1',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_coherent_real_perf_candidate_policy',
  war3_coherent_real_perf_candidate_policy_test,
  timeout : 30,
)

# Value-only crash-capture policy test. It proves that recoverable first-chance
# observations cannot consume the one-shot fatal dump gate and does not load
# DbgHelp or install process exception handlers.
war3_crash_capture_policy_test = executable(
  'war3_crash_capture_policy_test',
  [
    'war3/core/tests/war3_crash_capture_policy_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_crash_capture_policy',
  war3_crash_capture_policy_test,
  timeout : 30,
)

# Bounded text-only VK_EXT_device_fault capture. The runnable links no DXVK
# device and uses a fake dispatch function to prove one-shot terminal policy.
war3_dxvk_device_fault_capture_test = executable(
  'war3_dxvk_device_fault_capture_test',
  [
    '../dxvk/dxvk_device_address_binding.cpp',
    '../dxvk/dxvk_device_fault.cpp',
    'war3/core/tests/dxvk_device_fault_capture_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_dxvk_device_fault_capture',
  war3_dxvk_device_fault_capture_test,
  timeout : 30,
)

# Development-only bounded VK_EXT_device_address_binding_report policy. This
# runnable does not create a Vulkan instance and tests only the lock-free ring
# and fault-range correlation value contract.
war3_dxvk_device_address_binding_test = executable(
  'war3_dxvk_device_address_binding_test',
  [
    '../dxvk/dxvk_device_address_binding.cpp',
    'war3/core/tests/dxvk_device_address_binding_test.cpp',
  ],
  include_directories : [ dxvk_include_path ],
  cpp_args            : [ '-DWARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV=1' ],
  install             : false,
)
test(
  'war3_dxvk_device_address_binding',
  war3_dxvk_device_address_binding_test,
  timeout : 30,
)

if platform != 'windows'
  pkg.generate(d3d9_dll,
    filebase: dxvk_pkg_prefix + 'd3d9',
    subdirs:  'dxvk',
  )
endif
