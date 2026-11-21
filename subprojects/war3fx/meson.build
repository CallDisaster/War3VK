project('war3fx', 'cpp', version : '0.1.0', default_options : [ 'cpp_std=c++17' ])

war3fx_shaders = files([
  'shaders/war3_fullscreen_vert.vert',
  'shaders/war3_shadow_caster_vert.vert',
  'shaders/war3_shadow_caster_frag.frag',
  'shaders/war3_shadow_caster_point_frag.frag',
  'shaders/war3_shadow_caster_mask.frag',
  'shaders/war3_shadow_visibility.frag',
  'shaders/war3_shadow_receiver.frag',
  'shaders/war3_motion_vector.frag',
  'shaders/war3_gpu_skin.comp',
  'shaders/war3_hiz_seed.comp',
  'shaders/war3_hiz_reduce.comp',
  'shaders/war3_point_contact_hiz.comp',
  'shaders/war3_volumetric_light.frag',
  'shaders/war3_volumetric_froxel_inject.comp',
  'shaders/war3_volumetric_froxel_temporal.comp',
  'shaders/war3_volumetric_froxel_integrate.comp',
  'shaders/war3_volumetric_composite.frag',
  'shaders/war3_fxaa.frag',
  'shaders/war3_smaa_edge.frag',
  'shaders/war3_smaa_blend.frag',
  'shaders/war3_smaa_neighbor.frag',
  'shaders/war3_outline_mask.frag',
  'shaders/war3_outline_edge.frag',
  'shaders/war3_unit_outline.frag',
  'shaders/war3_unit_outline_vert.vert',
  'shaders/war3_outline_expand_vert.vert',
  'shaders/war3_ssao.frag',
  'shaders/war3_weather_snow_vert.vert',
  'shaders/war3_weather_snow_frag.frag',
  'shaders/war3_bloom.frag',
  'shaders/war3_water_depth_vert.vert',
  'shaders/war3_water_surface.frag',
])

set_variable('war3fx_shaders', war3fx_shaders)

# Shaders of the effect executors (src/d3d9/war3/effects), which the x64
# render host builds too. A subset of war3fx_shaders: keep the two in step.
war3fx_executor_shaders = files([
  'shaders/war3_fullscreen_vert.vert',
  'shaders/war3_ssao.frag',
  'shaders/war3_weather_snow_vert.vert',
  'shaders/war3_weather_snow_frag.frag',
  'shaders/war3_fxaa.frag',
  'shaders/war3_smaa_edge.frag',
  'shaders/war3_smaa_blend.frag',
  'shaders/war3_smaa_neighbor.frag',
  'shaders/war3_bloom.frag',
  # Phase 4c: the shadow pass (d3d9_war3_shadow*.cpp) is built into the host as is.
  'shaders/war3_shadow_caster_vert.vert',
  'shaders/war3_shadow_caster_frag.frag',
  'shaders/war3_shadow_caster_point_frag.frag',
  'shaders/war3_shadow_caster_mask.frag',
  'shaders/war3_shadow_visibility.frag',
  'shaders/war3_shadow_receiver.frag',
  'shaders/war3_motion_vector.frag',
  'shaders/war3_hiz_seed.comp',
  'shaders/war3_hiz_reduce.comp',
  'shaders/war3_point_contact_hiz.comp',
  'shaders/war3_outline_mask.frag',
  'shaders/war3_outline_edge.frag',
  'shaders/war3_unit_outline.frag',
  'shaders/war3_unit_outline_vert.vert',
  'shaders/war3_outline_expand_vert.vert',
  # Phase 4d: the volumetric light pass (d3d9_war3_volumetric_light.cpp).
  'shaders/war3_volumetric_light.frag',
  'shaders/war3_volumetric_froxel_inject.comp',
  'shaders/war3_volumetric_froxel_temporal.comp',
  'shaders/war3_volumetric_froxel_integrate.comp',
  'shaders/war3_volumetric_composite.frag',
  # The water's surface depth (d3d9_war3_shadow.cpp): the water draws' replay.
  'shaders/war3_water_depth_vert.vert',
  'shaders/war3_water_surface.frag',
])
set_variable('war3fx_executor_shaders', war3fx_executor_shaders)

# The host compiles these with the fixed-function stages it uses for the game's draws.
war3fx_water_in_place = files('shaders/war3_water_in_place_vert.vert.in',
                              'shaders/war3_water_in_place.frag.in')
