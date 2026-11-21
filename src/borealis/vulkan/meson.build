# Copyright © 2022 Collabora Ltd. and Red Hat Inc.
# SPDX-License-Identifier: MIT
bk_files = files(
  'bk_barrier.c',
  'bk_barrier.h',
  'bk_acceleration_structure.c',
  'bk_m5_acceleration_structure.c',
  'bk_host_as.c',
  'bk_acceleration_structure.h',
  'bk_buffer.c',
  'bk_buffer.h',
  'bk_buffer_view.c',
  'bk_bvh_depth_core.c',
  'bk_bvh_depth_core.h',
  'bk_buffer_view.h',
  'bk_bvh.h',
  'bk_cmd_buffer.c',
  'bk_cmd_upload.c',
  'bk_cmd_buffer.h',
  'bk_conditional_render_core.h',
  'bk_cmd_lifetime.c',
  'bk_cs_root_size.h',
  'bk_cmd_clear.c',
  'bk_cmd_dispatch.c',
  'bk_gen1_query_state.c',
  'bk_cmd_draw.c',
  'bk_cmd_meta.c',
  'bk_cmd_trace_rays.c',
  'bk_cmd_pool.c',
  'bk_cmd_pool.h',
  'bk_descriptor_set.h',
  'bk_descriptor_set.c',
  'bk_descriptor_set_layout.c',
  'bk_descriptor_set_layout_hash.c',
  'bk_descriptor_set_layout.h',
  'bk_descriptor_table.c',
  'bk_descriptor_table.h',
  'bk_compute_kick.c',
  'bk_compute_kick.h',
  'bk_device.c',
  'bk_device.h',
  'bk_device_memory.c',
  'bk_device_memory.h',
  'bk_memory_budget.c',
  'bk_memory_budget.h',
  'bk_host_alias.c',
  'bk_host_alias.h',
  'bk_event.c',
  'bk_event.h',
  'bk_format.c',
  'bk_g17p_meta_copy_core.c',
  'bk_ioto_arena.c',
  'bk_ioto_arena_core.c',
  'bk_ioto_arena_core.h',
  'bk_g17p_meta_copy_core.h',
  'bk_agx3_ioto_program_core.c',
  'bk_agx3_ioto_program_core.h',
  'bk_agx3_vdm_decode.c',
  'bk_agx3_vdm_decode.h',
  'bk_ppp_state.h',
  'bk_image.c',
  'bk_image.h',
  'bk_image_layout_core.c',
  'bk_image_layout_core.h',
  'bk_image_view.c',
  'bk_image_view.h',
  'bk_instance.c',
  'bk_instance.h',
  'bk_nir_forward_image_stores.c',
  'bk_nir_forward_image_stores.h',
  'bk_nir_lower_descriptors.c',
  'bk_nir_lower_gs.c',
  'bk_nir_lower_tes_rq.c',
  'bk_nir_lower_gs.h',
  'bk_nir_vertex_preamble.c',
  'bk_nir_vertex_preamble.h',
  'bk_nir_root_size.c',
  'bk_nir_lower_ray_queries.c',
  'bk_nir_lower_robust_image.c',
  'bk_nir_lower_robust_image.h',
  'bk_nir_ray_tracing.c',
  'bk_ray_tracing_core.c',
  'bk_rt_inlining.c',
  'bk_ray_tracing_core.h',
  'bk_ray_tracing_pipeline.c',
  'bk_ray_tracing_pipeline.h',
  'bk_nir_ray_tracing.h',
  'bk_nir_widen_16bit_varyings.c',
  'bk_nir_widen_16bit_varyings.h',
  'bk_native_descriptor.h',
  'bk_native_vertex_template.h',
  'bk_native_descriptor_pack.c',
  'bk_native_descriptor_pack.h',
  'bk_native_sampler.c',
  'bk_native_sampler.h',
  'bk_physical_device.c',
  'bk_physical_device.h',
  'bk_pipeline_statistics_core.c',
  'bk_pipeline_statistics_core.h',
  'bk_private.h',
  'bk_query_oq.c',
  'bk_query_pool.c',
  'bk_query_reset.c',
  'bk_query_pool.h',
  'bk_drm_render_core.c',
  'bk_drm_render_core.h',
  'bk_execution_allowance.h',
  'bk_pack.c',
  'bk_pack.h',
  'bk_queue.c',
  'bk_queue.h',
  'bk_frame_anatomy.c',
  'bk_frame_anatomy.h',
  'bk_sampler.c',
  'bk_sampler.h',
  'bk_shader.c',
  'bk_nir_lower_image_view_min_lod.c',
  'bk_shader.h',
  'bk_sync.c',
  'bk_sync.h',
  'bk_tess_batch.c',
  'bk_tess_batch.h',
  'bk_wsi.c',
  'bk_wsi.h'
)

bk_drirc = custom_target(
  'bk_drirc',
  input  : [files('bk_drirc_gen.py'), drirc_gen, '00-borealis-defaults.conf'],
  output : ['bk_drirc.c', 'bk_drirc.h'],
  command : [
    prog_python, '@INPUT0@',
    '--import-path', join_paths(dir_source_root, 'src/util'),
    '--drirc-src', '@OUTPUT0@',
    '--drirc-hdr', '@OUTPUT1@',
    '--validate', '@INPUT2@',
  ],
)

if with_tests
  test('bk_execution_allowance',
    executable('bk_execution_allowance_test',
      files('tests/test-bk-execution-allowance.c'),
      include_directories : [
        include_directories('.'), inc_gallium, inc_gallium_aux, inc_include,
        inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm,
      ],
      dependencies : [
        idep_nir, idep_mesautil, idep_libagx,
        idep_libagx_shaders_h, idep_agx_pack,
        idep_vulkan_runtime_headers, idep_vulkan_util_headers,
      ]),
    suite : ['asahi'])

  test('bk_memory_budget', executable('bk_memory_budget_test',
    files('tests/test-bk-memory-budget.c', 'bk_memory_budget.c'),
    include_directories : include_directories('.')),
    suite : ['asahi'])
  test('bk_host_alias', executable('bk_host_alias_test',
    files('tests/test-bk-host-alias.c', 'bk_host_alias.c'),
    include_directories : include_directories('.')),
    suite : ['asahi'])
  test(
    'bk_barrier',
    executable(
      'bk_barrier_test',
      files('tests/test-bk-barrier.c', 'bk_barrier.c'),
      include_directories : [include_directories('.'), inc_include],
    ),
    suite : ['asahi'],
  )
  test(
    'bk_g17p_storage_image',
    executable(
      'bk_g17p_storage_image_test',
      files('tests/test-bk-g17p-storage-image.c'),
      include_directories : include_directories('.'),
    ),
    suite : ['asahi'],
  )

  test(
    'bk_gen1_query_state',
    executable(
      'bk_gen1_query_state_test',
      files('tests/test-bk-gen1-query-state.c', 'bk_gen1_query_state.c',
            '../../asahi/lib/agx_scs_layout.c'),
      include_directories : [
        include_directories('.'), inc_gallium, inc_gallium_aux, inc_include,
        inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm,
      ],
      dependencies : [
        idep_nir, idep_mesautil, idep_libagx,
        idep_libagx_shaders_h, idep_agx_pack,
        idep_vulkan_runtime_headers, idep_vulkan_util_headers,
      ],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test('bk_image_store_forward', executable('bk_image_store_forward_test',
    files('tests/test-bk-image-store-forward.c', 'bk_nir_forward_image_stores.c'),
    include_directories : [include_directories('.'), inc_include, inc_src],
    dependencies : [idep_nir, idep_mesautil]), suite : ['asahi'])

  test('bk_root_size', executable('bk_root_size_test',
    files('tests/test-bk-root-size.c', 'bk_nir_root_size.c'),
    include_directories : [include_directories('.'), inc_include, inc_src],
    dependencies : [idep_nir, idep_mesautil]), suite : ['asahi'])
  # Unused recording functions require section garbage collection so the
  # test can link the production resolver without the rest of the driver.
  if ld_args_gc_sections.length() > 0
    test(
      'bk_tess_batch',
      executable(
        'bk_tess_batch_test',
        files('tests/test-bk-tess-batch.c', 'bk_tess_batch.c',
              'bk_drm_render_core.c'),
        include_directories : [
          include_directories('.'),
          inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
          inc_virtio_gpu, inc_virtio_vdrm,
        ],
        dependencies : [
          idep_nir_headers, idep_mesautil, idep_libagx,
          idep_libagx_shaders_h, idep_agx_pack,
          idep_vulkan_runtime_headers, idep_vulkan_util_headers,
        ],
        link_args : [ld_args_gc_sections],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['asahi'],
    )
    test(
      'bk_query_reset',
      executable(
        'bk_query_reset_test',
        files('tests/test-bk-query-reset.c', 'bk_query_reset.c'),
        include_directories : [
          include_directories('.'),
          inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
          inc_virtio_gpu, inc_virtio_vdrm,
        ],
        dependencies : [
          idep_nir_headers, idep_mesautil, idep_libagx,
          idep_libagx_shaders_h, idep_agx_pack,
          idep_vulkan_runtime_headers, idep_vulkan_util_headers,
        ],
        link_args : [ld_args_gc_sections],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['asahi'],
    )

    # Exercise the real resolver; unused command-recording entry points in
    # bk_sync.c need no device implementation in this CPU-only executable.
    test(
      'bk_sync_prior',
      executable(
        'bk_sync_prior_test',
        files('tests/test-bk-sync-prior.c', 'bk_sync.c'),
        include_directories : [
          include_directories('.'),
          inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
          inc_virtio_gpu, inc_virtio_vdrm,
        ],
        dependencies : [
          idep_nir_headers, idep_mesautil, idep_libagx,
          idep_libagx_shaders_h, idep_agx_pack,
          idep_vulkan_runtime_headers, idep_vulkan_util_headers,
        ],
        link_args : [ld_args_gc_sections],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['asahi'],
    )

    # Submit packing: the member check, the planner and the dependency fold,
    # resolved through the real bk_sync resolver; no device or GPU.
    test(
      'bk_pack',
      executable(
        'bk_pack_test',
        files('tests/test-bk-pack.c', 'bk_pack.c', 'bk_sync.c'),
        include_directories : [
          include_directories('.'),
          inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
          inc_virtio_gpu, inc_virtio_vdrm,
        ],
        dependencies : [
          idep_nir_headers, idep_mesautil, idep_libagx,
          idep_libagx_shaders_h, idep_agx_pack,
          idep_vulkan_runtime_headers, idep_vulkan_util_headers,
        ],
        link_args : [ld_args_gc_sections],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['asahi'],
    )

    # The deferred occlusion-availability resolver against mock control
    # streams, with the real bk_sync stream bookkeeping it drops streams from.
    test(
      'bk_query_oq',
      executable(
        'bk_query_oq_test',
        files('tests/test-bk-query-oq.c', 'bk_query_oq.c', 'bk_sync.c'),
        include_directories : [
          include_directories('.'),
          inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
          inc_virtio_gpu, inc_virtio_vdrm,
        ],
        dependencies : [
          idep_nir_headers, idep_mesautil, idep_libagx,
          idep_libagx_shaders_h, idep_agx_pack,
          idep_vulkan_runtime_headers, idep_vulkan_util_headers,
        ],
        link_args : [ld_args_gc_sections],
        gnu_symbol_visibility : 'hidden',
      ),
      suite : ['asahi'],
    )

  endif

  test(
    'bk_root_fragment',
    executable(
      'bk_root_fragment_test',
      files('tests/test-bk-root-fragment.c'),
      include_directories : [
        include_directories('.'),
        inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
        inc_virtio_gpu, inc_virtio_vdrm,
      ],
      dependencies : [
        idep_nir, idep_mesautil, idep_libagx,
        idep_libagx_shaders_h, idep_agx_pack,
        idep_vulkan_runtime_headers, idep_vulkan_util_headers,
      ],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_render_resource_span',
    executable(
      'bk_render_resource_span_test',
      files('tests/test-bk-render-resource-span.c', 'bk_drm_render_core.c'),
      include_directories : [include_directories('.'), inc_include],
      c_args : [c_msvc_compat_args],
    ),
    suite : ['asahi'],
  )

  test(
    'bk_query_program',
    executable(
      'bk_query_program_test',
      files('tests/test-bk-query-program.c'),
      include_directories : [inc_include, inc_src],
      dependencies : [
        idep_nir_headers, idep_mesautil, idep_libagx, idep_libagx_shaders_h,
      ],
      link_with : [libasahi_lib],
    ),
    suite : ['asahi'],
  )

  test(
    'bk_query_copy',
    executable(
      'bk_query_copy_test',
      files('tests/test-bk-query-copy.c'),
      include_directories : [inc_include, inc_src],
    ),
    suite : ['asahi'],
  )

  test(
    'bk_drm_render_core',
    executable(
      'bk_drm_render_core_test',
      files('tests/test-bk-drm-render-core.c',
            'bk_drm_render_core.c'),
      include_directories : [inc_include, inc_src],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_agx3_ioto_program_core',
    executable(
      'bk_agx3_ioto_program_core_test',
      files('tests/test-bk-agx3-ioto-program-core.c',
            'bk_agx3_ioto_program_core.c',
            '../../asahi/compiler/agx3_compute_program.c',
            '../../asahi/compiler/agx3_graphics_program.c',
            '../../asahi/compiler/agx3_m5_graphics_program.c',
            '../../asahi/compiler/agx3_copy_core.c',
            '../../asahi/compiler/agx3_ioto_uniform_program.c',
            '../../asahi/compiler/agx3_ioto_load_packer.c',
            '../../asahi/compiler/agx3_cprog_input.c',
            '../../asahi/compiler/agx3_static_spill_core.c',
            '../../asahi/compiler/agx3_mem_core.c',
            '../../asahi/compiler/agx3_binder.c',
            '../../asahi/compiler/agx3_compose/agx3_compose.c',
            '../../asahi/compiler/agx3_compose/agx3b_tables.c',
            '../../asahi/compiler/agx3_compose/agx3c_tables.c'),
      include_directories : [
        include_directories('.'), inc_include, inc_src, inc_asahi,
      ],
      dependencies : [idep_mesautil],
      link_with : [libasahi_lib, libasahi_layout],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_agx3_vdm_decode',
    executable(
      'bk_agx3_vdm_decode_test',
      files('tests/test-bk-agx3-vdm-decode.c',
            'bk_agx3_vdm_decode.c'),
      include_directories : [
        include_directories('.'), inc_include, inc_src, inc_asahi,
      ],
      dependencies : [idep_agx_pack, idep_mesautil],
      link_with : [libasahi_lib],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_zls_stride',
    executable(
      'bk_zls_stride_test',
      files('tests/test-bk-zls-stride.c'),
      include_directories : [include_directories('.')],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_sampler_index',
    executable(
      'bk_sampler_index_test',
      files('tests/test-bk-sampler-index.c'),
      include_directories : [include_directories('.')],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

    executable(
      'g17p-render-only',
      files('tests/g17p_render_only.c'),
      include_directories : [inc_include],
      dependencies : [dep_dl],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    )
  test(
    'bk_g17p_compression',
    executable(
      'bk_g17p_compression_test',
      files('tests/test-bk-g17p-compression.c'),
      include_directories : [inc_include, inc_src],
    ),
    suite : ['asahi'],
  )

  test(
    'bk_g16s_compression',
    executable(
      'bk_g16s_compression_test',
      files('tests/test-bk-g16s-compression.c'),
      include_directories : [inc_include, inc_src],
    ),
    suite : ['asahi'],
  )

  test(
    'bk_g17p_zls',
    executable(
      'bk_g17p_zls_test',
      files('tests/test-bk-g17p-zls.c', 'bk_drm_render_core.c'),
      include_directories : [include_directories('.'), inc_include, inc_src,
                             inc_asahi],
      dependencies : [idep_mesautil, idep_nir, idep_libagx,
                      idep_libagx_shaders_h, idep_agx_pack],
      link_with : [libasahi_layout],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_g17p_restart_state',
    executable(
      'bk_g17p_restart_state_test',
      files('tests/test-bk-g17p-restart-state.c'),
      include_directories : [include_directories('.')],
    ),
    suite : ['asahi'],
  )

  test(
    'bk_g17p_metadata_grid',
    executable(
      'bk_g17p_metadata_grid_test',
      files('tests/test-bk-g17p-metadata-grid.c',
            'bk_g17p_meta_copy_core.c'),
      include_directories : [include_directories('.'), inc_include, inc_src,
                             inc_asahi],
      dependencies : [idep_mesautil, idep_vulkan_util,
                      idep_vulkan_runtime_headers],
      link_with : [libasahi_layout],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_image_layout_core',
    executable(
      'bk_image_layout_core_test',
      files('tests/test-bk-image-layout-core.c',
            'bk_image_layout_core.c'),
      include_directories : [include_directories('.')],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_native_descriptor',
    executable(
      'bk_native_descriptor_test',
      files('tests/test-bk-native-descriptor.c',
            'bk_native_descriptor_pack.c',
            '../../asahi/lib/agx_pbe_agx3_core.c'),
      include_directories : [
        include_directories('.'), inc_include, inc_src, inc_asahi,
      ],
      dependencies : [idep_agx_pack, idep_mesautil],
      c_args : [c_msvc_compat_args, no_override_init_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_native_sampler',
    executable(
      'bk_native_sampler_test',
      files('tests/test-bk-native-sampler.c', 'bk_native_sampler.c'),
      include_directories : [
        include_directories('.'), inc_include, inc_src, inc_asahi,
      ],
      dependencies : [idep_agx_pack, idep_mesautil],
      link_with : [libasahi_lib],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )
  test(
    'bk_gs_ray_query',
    executable(
      'bk_gs_ray_query_test',
      files('tests/test-bk-gs-ray-query.c', 'bk_nir_lower_gs.c'),
      include_directories : [include_directories('.'), inc_asahi, inc_poly],
      dependencies : [idep_nir, idep_mesautil, idep_libpoly],
      link_with : [libpoly_nir, libasahi_compiler],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_vertex_preamble_shift',
    executable(
      'bk_vertex_preamble_shift_test',
      files('tests/test-bk-vertex-preamble-shift.c',
            'bk_nir_vertex_preamble.c'),
      include_directories : [include_directories('.'), inc_asahi],
      dependencies : [idep_nir, idep_mesautil],
      link_with : [libasahi_compiler],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

  test(
    'bk_sampler_bias',
    executable(
      'bk_sampler_bias_test',
      files('tests/test-bk-sampler-bias.c'),
      include_directories : [include_directories('.'), inc_include, inc_src, inc_asahi],
      dependencies : [idep_nir, idep_mesautil],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )

endif

bk_entrypoints = custom_target(
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['bk_entrypoints.h', 'bk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'bk',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

subdir('bvh')

bk_deps = [
  dep_libdrm,
  idep_nir,
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_wsi,
  idep_vulkan_wsi_headers,
  idep_agx_pack,
  idep_libagx,
  idep_libagx_shaders_h,
]

libbk = static_library(
  'bk',
  [
    bk_entrypoints,
    bk_drirc,
    bk_files,
    bk_bvh_spv,
    sha1_h,
  ],
  include_directories : [
    inc_gallium,
    inc_gallium_aux,
    inc_include,
    inc_src,
    inc_asahi,
    inc_virtio_gpu,
    inc_virtio_vdrm,
    include_directories('.'),
  ],
  link_with : [libasahi_lib, libasahi_layout, libasahi_compiler],
  c_args : ['-Wno-c2x-extensions'],
  dependencies : [bk_deps],
  gnu_symbol_visibility : 'hidden',
)

# The tests below replace driver functions with -Wl,--wrap. Link-time
# optimisation resolves calls inside the driver libraries before the linker
# applies the wrap, so link them from the regular objects of fat LTO builds.
bk_wrap_test_link_args = cc.get_supported_link_arguments('-fno-lto')

if with_tests
  test('bk_sparse_alias', executable('bk_sparse_alias_test',
    files('tests/test-bk-sparse-alias.c'),
    include_directories : [inc_gallium, inc_gallium_aux, inc_include, inc_src,
      inc_asahi, inc_virtio_gpu, inc_virtio_vdrm, include_directories('.')],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections, bk_wrap_test_link_args,
      '-Wl,--wrap=__vk_errorf']),
    suite : ['asahi'])
  test('bk_rt_inlining', executable('bk_rt_inlining_test',
    files('tests/test-bk-rt-inlining.c'),
    include_directories : [inc_gallium, inc_gallium_aux, inc_include, inc_src,
      inc_asahi, inc_virtio_gpu, inc_virtio_vdrm, include_directories('.')],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections]), suite : ['asahi'])
  test('bk_memory_oom', executable('bk_memory_oom_test',
    files('tests/test-bk-memory-oom.c'),
    include_directories : [inc_gallium, inc_gallium_aux, inc_include, inc_src,
      inc_asahi, inc_virtio_gpu, inc_virtio_vdrm, include_directories('.')],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections, bk_wrap_test_link_args,
      '-Wl,--wrap=agx_bo_bind', '-Wl,--wrap=agx_bo_cache_evict_all',
      '-Wl,--wrap=agx_bo_export',
      '-Wl,--wrap=bk_get_cgroup_memory_available', '-Wl,--wrap=__vk_errorf',
      '-Wl,--wrap=os_get_available_system_memory',
      '-Wl,--wrap=os_get_total_physical_memory']),
    suite : ['asahi'])
  test('bk_tes_query', executable('bk_tes_query_test', files('tests/test-bk-tes-query.c'),
    include_directories : [inc_gallium, inc_gallium_aux, inc_include, inc_src,
      inc_asahi, inc_virtio_gpu, inc_virtio_vdrm, include_directories('.')],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections]), suite : ['asahi'])
  test('bk_coherent_image', executable('bk_coherent_image_test',
    files('tests/test-bk-coherent-image.c'),
    include_directories : [inc_gallium, inc_gallium_aux, inc_include, inc_src,
      inc_asahi, inc_virtio_gpu, inc_virtio_vdrm, include_directories('.')],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections]), suite : ['asahi'])
  test('bk_m5_store_window', executable('bk_m5_store_window_test',
    files('tests/test-bk-m5-store-window.c'),
    include_directories : [inc_gallium, inc_gallium_aux, inc_include, inc_src,
      inc_asahi, inc_virtio_gpu, inc_virtio_vdrm, include_directories('.')],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections]), suite : ['asahi'])
  test(
    'bk_rt_pipeline_executable',
    executable(
      'bk_rt_pipeline_executable_test',
      files('tests/test-bk-rt-pipeline-executable.c'),
      include_directories : [
        include_directories('.'), inc_gallium, inc_gallium_aux,
        inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm,
      ],
      link_with : [libbk],
      dependencies : [bk_deps],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )
endif

libvulkan_borealis = shared_library(
  'vulkan_borealis',
  link_whole : [libbk],
  link_args: [ld_args_build_id],
  gnu_symbol_visibility : 'hidden',
  install : true,
)

icd_file_name = libname_prefix + 'vulkan_borealis.' + libname_suffix

borealis_icd = custom_target(
  input : [vk_icd_gen, vk_api_xml],
  output : 'borealis_icd.' + vulkan_manifest_suffix,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', vulkan_icd_lib_path,
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'borealis_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())

if with_tests and ld_args_gc_sections.length() > 0
  test(
    'bk_sparse_formats', executable('bk_sparse_formats_test',
      files('tests/test-bk-sparse-formats.c'),
      include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
        inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
      dependencies : [bk_deps], link_with : [libbk, libasahi_layout],
      link_args : [ld_args_gc_sections],
      gnu_symbol_visibility : 'hidden'), suite : ['asahi'])
  test(
    'bk_host_as', executable('bk_host_as_test',
      files('tests/test-bk-host-as.c', 'bk_host_as.c'),
      include_directories : [include_directories('.'), inc_include, inc_src],
      dependencies : [bk_deps],
      link_args : [ld_args_gc_sections, bk_wrap_test_link_args,
        '-Wl,--wrap=util_flush_range']), suite : ['asahi'])
  test('bk_agx3_as_format', executable('bk_agx3_as_format_test',
    files('tests/test-bk-agx3-as-format.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections, bk_wrap_test_link_args]),
    suite : ['asahi'])
  test('bk_as_registry', executable('bk_as_registry_test',
    files('tests/test-bk-as-registry.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections, bk_wrap_test_link_args, '-Wl,--wrap=agx_bo_create',
      '-Wl,--wrap=agx_bo_bind', '-Wl,--wrap=_mesa_hash_table_insert',
      '-Wl,--wrap=agx_va_alloc', '-Wl,--wrap=agx_va_free']),
    timeout : 60, suite : ['asahi'])
  test('bk_host_as_lifetime', executable('bk_host_as_lifetime_test',
    files('tests/test-bk-host-as-lifetime.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections, bk_wrap_test_link_args, '-Wl,--wrap=bk_meta_begin',
      '-Wl,--wrap=bk_meta_end']), suite : ['asahi'])
  test('bk_native_feature_defaults', executable('bk_native_feature_defaults_test',
    files('tests/test-native-feature-defaults.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections]), suite : ['asahi'])
  test('bk_rt_on_demand', executable('bk_rt_on_demand_test',
    files('tests/test-bk-rt-on-demand.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [ld_args_gc_sections, bk_wrap_test_link_args,
      '-Wl,--wrap=agx_va_alloc', '-Wl,--wrap=agx_va_free',
      '-Wl,--wrap=agx_bo_create', '-Wl,--wrap=agx_bo_unreference',
      '-Wl,--wrap=agx_bo_bind', '-Wl,--wrap=__vk_errorf']), suite : ['asahi'])
  test(
    'bk_rt_cache_control',
    executable(
      'bk_rt_cache_control_test',
      files('tests/test-bk-rt-cache-control.c'),
      include_directories : [
        include_directories('.'), inc_gallium, inc_gallium_aux,
        inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm,
      ],
      dependencies : [bk_deps],
      link_with : [libbk],
      link_args : [
        ld_args_gc_sections,
        bk_wrap_test_link_args,
        '-Wl,--wrap=vk_pipeline_shader_stage_to_nir',
        '-Wl,--wrap=bk_compile_shader',
        '-Wl,--wrap=bk_api_shader_destroy',
        '-Wl,--wrap=ralloc_free',
      ],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )
  test(
    'bk_rt_pipeline_binary',
    executable(
      'bk_rt_pipeline_binary_test',
      files('tests/test-bk-rt-pipeline-binary.c'),
      include_directories : [
        include_directories('.'), inc_gallium, inc_gallium_aux,
        inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm,
      ],
      dependencies : [bk_deps],
      link_with : [libbk],
      link_args : [
        ld_args_gc_sections,
        bk_wrap_test_link_args,
        '-Wl,--wrap=vk_pipeline_shader_stage_to_nir',
        '-Wl,--wrap=vk_common_CreatePipelineBinariesKHR',
        '-Wl,--wrap=vk_common_GetPipelineKeyKHR',
        '-Wl,--wrap=bk_compile_shader',
        '-Wl,--wrap=bk_api_shader_destroy',
        '-Wl,--wrap=ralloc_free',
      ],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )
endif

if with_tests
  bk_dual_source_gpl_test = executable(
    'bk-dual-source-gpl',
    files('tests/bk_dual_source_gpl.c'),
    include_directories : [
      inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
      inc_virtio_gpu, inc_virtio_vdrm, include_directories('.'),
    ],
    dependencies : [bk_deps, cc.find_library('vulkan')],
    # bk_cmd_buffer.h uses GNU void-pointer arithmetic in inline helpers.
    c_args : [c_msvc_compat_args, '-Wno-pointer-arith'],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-xfb-resume',
    files('tests/vulkan_xfb_resume.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )
  executable(
    'vulkan-mipmap-layers',
    files('tests/vulkan_mipmap_layers.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-render-area',
    files('tests/vulkan_render_area.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-tess-batch',
    files('tests/vulkan_tess_batch.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  vulkan_tess_batch_order = executable(
    'vulkan-tess-batch-order',
    files('tests/vulkan_tess_batch_order.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-ubo-record-loop',
    files('tests/vulkan_ubo_record_loop.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan'), cc.find_library('m')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-sample-variant-cache',
    files('tests/vulkan_sample_variant_cache.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-border-emulation-cache',
    files('tests/vulkan_border_emulation_cache.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-msaa-xfer',
    files('tests/vulkan_msaa_xfer.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'aurora-r11g11b10f',
    files('tests/aurora_r11g11b10f.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'aurora-tile-visit',
    files('tests/aurora_tile_visit.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'aurora-restart-switch',
    files('tests/aurora_restart_switch.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-msaa-mrt',
    files('tests/vulkan_msaa_mrt.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'g17p-vulkan-multiview-clear',
    files('tests/g17p_vulkan_multiview_clear.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'aurora-tilesubmit',
    files('tests/aurora_tilesubmit.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  # Submit packing on hardware: run with HK_PACK_CDM_SUBMIT=0 and =1.
  executable(
    'aurora-packtest',
    files('tests/aurora_packtest.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan'), dep_thread],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  # Also no-op shim tests, registered with the shim in ../meson.build.
  g17p_vulkan_compressed_mip_blit = executable(
    'g17p-vulkan-compressed-mip-blit',
    files('tests/g17p_vulkan_compressed_mip_blit.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  bk_vulkan_global_priority = executable(
    'bk-vulkan-global-priority',
    files('tests/bk_vulkan_global_priority.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  bk_vulkan_sparse_bind_first_submit = executable(
    'bk-vulkan-sparse-bind-first-submit',
    files('tests/bk_vulkan_sparse_bind_first_submit.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  bk_vulkan_descriptor_buffer_sizes = executable(
    'bk-vulkan-descriptor-buffer-sizes',
    files('tests/bk_vulkan_descriptor_buffer_sizes.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  bk_vulkan_descriptor_buffer_texel = executable(
    'bk-vulkan-descriptor-buffer-texel',
    files('tests/bk_vulkan_descriptor_buffer_texel.c'),
    include_directories : [inc_include, inc_src],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  bk_vulkan_bg_eot_threads = executable(
    'bk-vulkan-bg-eot-threads',
    files('tests/bk_vulkan_bg_eot_threads.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan'), dep_thread],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  bk_vulkan_explicit_plane_layout = executable(
    'bk-vulkan-explicit-plane-layout',
    files('tests/bk_vulkan_explicit_plane_layout.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  g16s_vulkan_buffer_upload = executable(
    'g16s-vulkan-buffer-upload',
    files('tests/g16s_vulkan_buffer_upload.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  g17p_vulkan_compressed_layer_copy = executable(
    'g17p-vulkan-compressed-layer-copy',
    files('tests/g17p_vulkan_compressed_layer_copy.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  g17p_vulkan_layered_depth = executable(
    'g17p-vulkan-layered-depth',
    files('tests/g17p_vulkan_layered_depth.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan'), dep_m],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-storage-compression',
    files('tests/vulkan_storage_compression.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan'), dep_m],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

  executable(
    'vulkan-dynamic-offset-draws',
    files('tests/vulkan_dynamic_offset_draws.c'),
    include_directories : [inc_include],
    dependencies : [cc.find_library('vulkan')],
    c_args : [c_msvc_compat_args],
    gnu_symbol_visibility : 'hidden',
  )

endif

if with_tests
  bk_cpu_upload_bench = executable(
    'bk-cpu-upload-bench',
    files('tests/bk_cpu_upload_bench.c'),
    include_directories : [
      inc_gallium, inc_gallium_aux, inc_include, inc_src, inc_asahi,
      inc_virtio_gpu, inc_virtio_vdrm, include_directories('.'),
    ],
    dependencies : [bk_deps],
    link_with : [libbk, libasahi_lib, libasahi_layout, libasahi_compiler],
    gnu_symbol_visibility : 'hidden',
  )
  test('bk_cpu_upload', bk_cpu_upload_bench, args : ['1'], suite : ['asahi'])
endif

if with_tests
  test('bk_submit_admission', executable('bk_submit_admission_test',
    files('tests/test-bk-submit-admission.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk]),
    timeout : 30, suite : ['asahi'])
endif

if with_tests
  test('bk_lost_object_submit', executable('bk_lost_object_submit_test',
    files('tests/test-bk-lost-object-submit.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk],
    link_args : [bk_wrap_test_link_args,
      '-Wl,--wrap=vk_queue_wait_lost_submissions']),
    timeout : 30, suite : ['asahi'])
endif

if with_tests
  test('bk_sparse_submit_wait', executable('bk_sparse_submit_wait_test',
    files('tests/test-bk-sparse-submit-wait.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk]),
    timeout : 30, suite : ['asahi'])
endif

if with_tests
  test('bk_queue_sharing', executable('bk_queue_sharing_test',
    files('tests/test-bk-queue-sharing.c'),
    include_directories : [include_directories('.'), inc_gallium, inc_gallium_aux,
      inc_include, inc_src, inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
    dependencies : [bk_deps], link_with : [libbk]),
    timeout : 30, suite : ['asahi'])
endif
