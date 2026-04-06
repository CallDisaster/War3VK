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
  'war3/core/war3_memory.cpp',
  'war3/core/war3_runtime_profile.cpp',
  'war3/platform/war3_module_api.cpp',
  'war3/platform/war3_runtime_bootstrap.cpp',
  'war3/memory/tlsf.c',
  'war3/memory/war3_tlsf_pool.cpp',
  'war3/memory/war3_storm_hook.cpp',
  'war3/memory/war3_shadow_arena.cpp',
  'war3/handle/war3_handle_resolver.cpp',
  'war3/render/war3_render_objects.cpp',
  'war3/render/war3_render_identity_bridge.cpp',
  'war3/render/war3_shadow_runtime_bridge.cpp',
  'war3/render/war3_scene_collector.cpp',
  'war3/render/war3_renderer.cpp',
  'war3/render/war3_shadow_object_registry.cpp',
  'war3/render/war3_render_dispatcher.cpp',
  'war3/render/war3_frame_graph.cpp',
  'war3/render/war3_render_queue_tracker.cpp',
  'war3/render/war3_render_exec_batch.cpp',
  'war3/render/war3_render_state.cpp',
  'war3/render/war3_shadow_capture_frontend.cpp',
  'war3/render/war3_native_renderer_probe.cpp',
  'war3/render/war3_post_process.cpp',
  'war3/state/war3_render_state.cpp',
  'war3/debug/war3_debug.cpp',
  'war3/tools/war3_perf_monitor.cpp',
  'war3/tools/war3_diagnostics_hub.cpp',
  'war3/tools/war3_frame_capture.cpp',
  'war3/tools/war3_internal_test_api.cpp',
  'war3/core/war3_storm.cpp',
  'war3/core/war3_file_manager.cpp',
  'war3/model/war3_model_hook.cpp',
  'war3/model/war3_model_registry.cpp',
  'war3/native/war3_native_shadow_hint.cpp',
  'war3/war3_user_example.cpp',
  'war3/game/war3_unit.cpp',
  'war3/game/war3_agent.cpp',
  'war3/shader/war3_shader_manager.cpp',
  'war3/shader/war3_material.cpp',
  'war3/ui/war3_imgui.cpp',
  'war3/hooks/war3_hook_address_book.cpp',
  'war3/hooks/war3_hook_ui.cpp',
  'war3/hooks/war3_hook_shadow.cpp',
  'war3/hooks/war3_hook_install_util.cpp',
  'war3/hooks/war3_hook_render_identity.cpp',
  'war3/hooks/war3_dispatch_contract.cpp',
  'war3/hooks/war3_queue_takeover_policy.cpp',
  'war3/hooks/war3_shadow_filter_policy.cpp',
  'war3/hooks/war3_hook_render.cpp',
  'war3/hooks/war3_hook_lifecycle.cpp',
  'war3/hooks/war3_hook_jass.cpp',
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

d3d9_dll = shared_library(dxvk_name_prefix+'d3d9', d3d9_src, glsl_generator.process(d3d9_shaders), d3d9_res,
  dependencies        : [ dxso_dep, dxvk_dep, imgui_dep, lib_dwmapi ],
  include_directories : [ dxvk_include_path, minhook_inc_path ],
  install             : true,
  vs_module_defs      : 'd3d9'+def_spec_ext,
  link_args           : d3d9_ld_args,
  link_depends        : [ d3d9_link_depends ],
  kwargs              : dxvk_so_version,
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ dxvk_include_path ],
)

if platform != 'windows'
  pkg.generate(d3d9_dll,
    filebase: dxvk_pkg_prefix + 'd3d9',
    subdirs:  'dxvk',
  )
endif
