# Copyright 2018 Rob Clark
# Copyright 2019 Collabora, Ltd.
# Copyright 2017 Intel Corporation
# SPDX-License-Identifier: MIT

libasahi_lib_files = files(
  'agx3_cdm_decode_core.c',
  'agx3_cdm_decode_core.h',
  'agx3_cdm_direct_launch_semantic_core.c',
  'agx3_cdm_direct_launch_semantic_core.h',
  'agx3_compute_program_core.c',
  'agx3_compute_program_core.h',
  'agx3_g17p_grid_helper_core.h',
  'agx3_code_container_core.c',
  'agx3_code_container_core.h',
  'agx_ioto_core.c',
  'agx_ioto_core.h',
  'agx_ioto_load_core.c',
  'agx_ioto_load_core.h',
  'agx_ioto_preshader_core.c',
  'agx_ioto_preshader_core.h',
  'agx_ioto_profile_core.c',
  'agx_ioto_profile_core.h',
  'agx_ioto_program_core.c',
  'agx_ioto_program_core.h',
  'agx_ioto_state_core.c',
  'agx_ioto_state_core.h',
  'agx_g17p_draw_state_upload.h',
  'agx_rt_result.h',
  'agx_scs_layout.c',
  'agx_scs_layout.h',
  'agx_bo.c',
  'agx_border.c',
  'agx_chip.c',
  'agx_device.c',
  'agx_bytes.h',
  'agx_linker.c',
  'agx_bg_eot.c',
  'agx_bg_eot_key.c',
  'agx_tilebuffer.c',
  'agx_nir_lower_msaa.c',
  'agx_nir_lower_sample_intrinsics.c',
  'agx_nir_lower_tilebuffer.c',
  'agx_nir_lower_uvs.c',
  'agx_nir_lower_vbo.c',
  'agx_nir_prolog_epilog.c',
  'agx_nir_legacy_epilog.c',
  'agx_ppp.h',
  'agx_ppp_hal200_graphics_core.c',
  'agx_ppp_hal200_graphics_core.h',
  'agx_ppp_hal200_raster_core.c',
  'agx_ppp_hal200_raster_core.h',
  'agx_ppp_hal300_depth_bias_scissor_core.c',
  'agx_ppp_hal300_depth_bias_scissor_core.h',
  'agx_ppp_hal300_graphics_core.c',
  'agx_ppp_hal300_graphics_core.h',
  'agx_ppp_hal300_depth_stencil_core.c',
  'agx_ppp_hal300_depth_stencil_core.h',
  'agx_ppp_decode_core.c',
  'agx_ppp_decode_core.h',
  'agx_ppp_viewport_payload_core.c',
  'agx_ppp_viewport_payload_core.h',
  'agx_ppp_w_clamp_cull2_core.c',
  'agx_ppp_w_clamp_cull2_core.h',
  'agx_pbe_agx3_core.c',
  'agx_pbe_agx3_core.h',
  'agx_scs_layout.c',
  'agx_scs_layout.h',
  'agx_scratch.c',
  'agx_vdm_barrier_core.c',
  'agx_vdm_barrier_core.h',
  'agx_vdm_hal200_bind_tail_core.c',
  'agx_vdm_hal200_bind_tail_core.h',
  'agx_vdm_hal200_direct_draw_core.c',
  'agx_vdm_hal200_direct_draw_core.h',
  'agx_vdm_decode_core.c',
  'agx_vdm_mesh_core.c',
  'agx_vdm_mesh_core.h',
  'agx_vdm_decode_core.h',
  'agx_vdm_index_list_core.c',
  'agx_vdm_index_list_core.h',
  'agx_vdm_ppp_state_core.c',
  'agx_vdm_ppp_state_core.h',
  'agx_vdm_stream_link_core.c',
  'agx_vdm_stream_link_core.h',
  'agx_vdm_stream_terminate_core.c',
  'agx_vdm_stream_terminate_core.h',
  'agx_vdm_tessellate_core.c',
  'agx_vdm_tessellate_core.h',
  'agx_vdm_state_core.c',
  'agx_vdm_state_core.h',
  'agx_vdm_state_update_core.c',
  'agx_vdm_state_update_core.h',
  'agx_va.c',
  'agx_va_runtime_hole.c',
  'agx_va_runtime_hole.h',
  'pool.c',
)

# honeykrisp-h1-execution.md SS5.2 item 1: the winsys backend is
# OS-specific -- agx_device_virtio.c is Linux/virtio-gpu-only (needs
# libdrm's vdrm.h, HAVE_LIBDRM always set when it's reachable, see root
# meson.build's system_has_kms_drm gating of dep_libdrm); the Windows
# equivalent (agx_device_win.c, SS1) talks DeviceIoControl instead. Also
# add the ntagx sync provider (SS3.1) on Windows. decode.c (POSIX-only,
# <sys/mman.h>) is swapped for decode_stub.c inside libasahi_decode below:
# the runtime still links that library's PPP/VDM/CDM encoder cores.
# The Windows backends. Three winsys backends implement agx_device_ops_t on
# Windows:
#
#  - the G18 render node (win/agx_device_g18.c, \\.\G18Win0): asahi DRM
#    ioctls framed in DeviceIoControl calls on the g18win driver, user-mode
#    sync objects. Self-contained, always built on Windows.
#  - the D3DKMT winsys (agx_device_d3dkmt.c and friends) and
#  - the private AGXKMD node (agx_device_win.c, \\.\AGXKMD).
#
# The last two compile sources and headers from a separate drivers tree
# (kmd-uapi-core, AppleAgxWddm/core) that the build names through the
# cross-file properties ntagx_kmd_uapi_core and ntagx_wddm_contract_core.
# They are built only when those properties are set, so a cross build
# without that tree still configures and links the G18 backend.
# hk_win_c_args (below) tells hk which of them exist.
asahi_win_agxkmd = false
asahi_win_d3dkmt = false
libasahi_lib_inc_win = []
if host_machine.system() == 'windows'
  kmd_uapi_core_dir = meson.get_external_property('ntagx_kmd_uapi_core', '')
  wddm_contract_dir = meson.get_external_property('ntagx_wddm_contract_core',
                                                  '')
  asahi_win_agxkmd = kmd_uapi_core_dir != ''
  asahi_win_d3dkmt = asahi_win_agxkmd or wddm_contract_dir != ''
  # Without them, these give the entry points hk and agx_device.c name
  # (enumeration finds nothing; the bridge helpers take the no-bridge path).
  if not asahi_win_agxkmd
    libasahi_lib_files += files('win/agx_win_no_agxkmd.c')
  endif
  if not asahi_win_d3dkmt
    libasahi_lib_files += files('win/agx_win_no_d3dkmt.c')
  endif
endif

if asahi_win_agxkmd
  libasahi_lib_files += files(
    'agx_device_win.c',
    'agx_device_win.h',
    'agx_sync_win.c',
  )
  # BUILD-STATUS.md stage-2: agx_device_win.c calls ntasi_kmd_uapi_cmd_to_ctl
  # (kmd-uapi-core/kmd_uapi.c); until now that tree was only ever put on the
  # include path (-Dc_args' -I, for kmd_uapi.h), never actually compiled in
  # -- link failed with "undefined symbol: ntasi_kmd_uapi_cmd_to_ctl".
  # kmd-uapi-core/kmd-syncobj-core live outside this Mesa tree entirely (in
  # the windows-on-apple-silicon repo, passed in via -Dc_args at `meson
  # setup`, same as the honeykrisp-port/overlay directory) with no fixed
  # relative path from here, so this can't be a plain files('../../relative')
  # reference; meson's `files()` does accept an absolute path string (it
  # only special-cases and resolves *relative* strings against
  # current_source_dir(), an absolute string passes through unchanged) --
  # VERIFIED by this successfully compiling. kmd_syncobj.c (kmd-syncobj-core)
  # is NOT added here: nothing under src/asahi currently calls a
  # kmd_syncobj_* symbol (grep, stage-2), only kmd_uapi.h types/macros are
  # used so far by agx_sync_win.c.
  libasahi_lib_files += files(kmd_uapi_core_dir / 'kmd_uapi.c')
endif

if asahi_win_d3dkmt
  # The D3DKMT winsys: the same agx_device_ops_t surface, but through
  # dxgkrnl and a WDDM miniport instead of a private ioctl node, so that
  # Vulkan and D3D share one allocator, one GPU VA regime and one handle
  # namespace. Built alongside the AGXKMD backend rather than replacing it:
  # the two are selected at runtime by hk_enumerate_physical_devices_win()
  # (NTAGX_DEVICE_PATH forces the legacy path, which patch 0009's user-mode
  # shim still needs), so this patch adds a capability instead of removing
  # one while the miniport does not yet exist.
  #
  # agx_d3dkmt_core.c has no Mesa, Windows or generated-header dependency on
  # purpose -- gpu/honeykrisp-port/tests/run-host-tests.sh compiles and runs
  # it natively. Nothing else in this list can be tested off-target.
  libasahi_lib_files += files(
    'agx_d3dkmt.c',
    'agx_d3dkmt.h',
    'agx_d3dkmt_core.c',
    'agx_d3dkmt_core.h',
    'agx_d3dkmt_priv.h',
    'agx_device_d3dkmt.c',
    'agx_d3dkmt_bridge.c',
    'agxw_bridge_core.c',
    'agxw_bridge_escape.c',
    'agx_bridge_vma.c',
    'agx_bridge_vma_core.c',
    'agx_device_d3dkmt.h',
    'agx_sync_d3dkmt.c',
  )
  # The WDDM miniport's kernel<->userspace contract
  # (drivers/AppleAgxWddm/core). agxw_contract.h is INCLUDED by the D3DKMT
  # winsys rather than transcribed, and agxw_contract.c is COMPILED IN so the
  # UMD runs the same validators the kernel runs -- a rejection here means
  # exactly what a rejection in the miniport would mean. That is only possible
  # because that core is deliberately WDK-free.
  #
  # Located from its own cross-file property when the build sets one, else
  # derived from the AppleAgxGpu core path, which is a fixed sibling in the
  # same repository. Deriving keeps this patch applicable without editing
  # honeykrisp-port/cross-*.txt; setting the property explicitly is the
  # tidier long-term form.
  agxw_contract_dir = wddm_contract_dir != '' ? wddm_contract_dir : \
    kmd_uapi_core_dir / '..' / '..' / '..' / 'AppleAgxWddm' / 'core'
  libasahi_lib_files += files(agxw_contract_dir / 'agxw_contract.c')
  libasahi_lib_inc_win = include_directories(agxw_contract_dir)
endif

# The virtio backend needs libdrm/libvdrm, which Windows does not have, even
# though with_vdrm is true there (it follows with_asahi_vk).
asahi_lib_has_virtio = with_vdrm and host_machine.system() != 'windows'
if asahi_lib_has_virtio
  libasahi_lib_files += files('agx_device_virtio.c')
endif

# honeykrisp-h1-execution.md SS2.2 row P9: "is virtio available" define,
# guards the two dev->dev.is_virtio blocks in hk_queue.c and the mirror
# bookkeeping in hk_device_memory.c that name `struct asahi_ccmd_submit_res`
# by value (needs the complete type, which agx_device.h only
# forward-declares when !HAVE_LIBDRM, SS2.1 row D2). Not consumed in this
# file (agx_device_virtio.c's own file-list gating just above doesn't need
# it) -- exported here as a variable so src/asahi/vulkan/meson.build (where
# hk_queue.c/hk_device_memory.c actually compile, Step 8/9) can add it to
# libhk's c_args without duplicating the host_machine.system() check.
# add_project_arguments() has already run by the time subdir('src') gets
# here (root meson.build), so appending to the global pre_args list from a
# subdir meson.build would silently do nothing -- pass it as per-target
# c_args instead.
asahi_win_c_args = []
if asahi_win_agxkmd
  asahi_win_c_args += '-DAGX_WIN_AGXKMD=1'
endif
if asahi_win_d3dkmt
  asahi_win_c_args += '-DAGX_WIN_D3DKMT=1'
endif
hk_win_c_args = host_machine.system() == 'windows' ? asahi_win_c_args \
                                                   : ['-DHAVE_VDRM']

libasahi_decode_files = files(
  'agx3_cdm_decode_core.c',
  'agx3_cdm_direct_launch_semantic_core.c',
  'agx_ioto_core.c',
  'agx_ppp_decode_core.c',
  'agx_ppp_hal200_graphics_core.c',
  'agx_ppp_hal300_depth_bias_scissor_core.c',
  'agx_ppp_hal300_graphics_core.c',
  'agx_ppp_hal300_graphics_core.h',
  'agx_ppp_hal300_depth_stencil_core.c',
  'agx_ppp_viewport_payload_core.c',
  'agx_ppp_w_clamp_cull2_core.c',
  'agx_vdm_barrier_core.c',
  'agx_vdm_hal200_bind_tail_core.c',
  'agx_vdm_hal200_direct_draw_core.c',
  'agx_vdm_decode_core.c',
  'agx_vdm_mesh_core.c',
  'agx_vdm_mesh_core.h',
  'agx_vdm_index_list_core.c',
  'agx_vdm_ppp_state_core.c',
  'agx_vdm_state_core.c',
  'agx_vdm_state_update_core.c',
  'agx_vdm_stream_link_core.c',
  'agx_vdm_stream_terminate_core.c',
  'agx_vdm_tessellate_core.c',
)
if host_machine.system() == 'windows'
  libasahi_decode_files += files('decode_stub.c')
else
  libasahi_decode_files += files('decode.c')
endif

libasahi_decode = static_library(
  'asahi_decode',
  [libasahi_decode_files, agx_pack],
  dependencies : [dep_valgrind, idep_mesautil],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  gnu_symbol_visibility : 'hidden',
  link_with : [libagx2_disasm, libagx3d_disasm],
  build_by_default : false,
)

libasahi_decode_shared = shared_library(
  'asahi_decode',
  [libasahi_decode_files, agx_pack],
  dependencies : [dep_valgrind, idep_mesautil],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  build_by_default : with_tools.contains('asahi'),
  link_with : [libagx2_disasm, libagx3d_disasm],
)

if asahi_lib_has_virtio
  libasahi_lib_includes = [inc_asahi, inc_virtio_gpu, inc_virtio_vdrm]
  libasahi_lib_links = [libasahi_decode, libvdrm, libpoly_nir]
  libasahi_lib_deps = [
    dep_libdrm, dep_valgrind, idep_nir, idep_mesautil, idep_libagx,
    idep_libpoly,
  ]
else
  libasahi_lib_includes = [inc_asahi, libasahi_lib_inc_win]
  libasahi_lib_links = [libasahi_decode, libpoly_nir]
  libasahi_lib_deps = [
    dep_valgrind, idep_nir, idep_mesautil, idep_libagx, idep_libpoly,
  ]
endif

libagx_shaders = custom_target(
  input : libagx_spv,
  output : ['libagx_shaders.h', 'libagx_shaders.c'],
  command : [prog_asahi_clc, libagx_spv, '@OUTPUT0@', '@OUTPUT1@'],
)

idep_libagx_shaders_h = declare_dependency(
  sources : [libagx_shaders],
  include_directories : include_directories('.'),
)

libasahi_lib = static_library(
  'asahi_lib',
  [libasahi_lib_files, libagx_shaders, agx_pack, sha1_h],
  include_directories : libasahi_lib_includes,
  c_args : [
    no_override_init_args,
    '-Wno-c2x-extensions',
    asahi_win_c_args,
  ],
  gnu_symbol_visibility : 'hidden',
  link_with: libasahi_lib_links,
  dependencies: libasahi_lib_deps,
  build_by_default : false,
)

if with_tests
  foreach rt_test : ['rt-bvh', 'scs-layout']
    test('asahi-' + rt_test,
      executable('asahi-' + rt_test,
        ['tests/test-' + rt_test + '.c', 'agx_scs_layout.c'],
        include_directories : include_directories('.'),
        c_args : ['-UNDEBUG'],
      ),
      suite : ['asahi'],
    )
  endforeach
  test(
    'asahi-chip-admission',
    executable(
      'asahi-chip-admission',
      files('tests/test-chip-admission.c'),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_agx_pack, idep_mesautil],
      link_with : [libasahi_lib],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )
  test(
    'asahi-i386-layout',
    executable(
      'asahi-i386-layout',
      files('tests/test-i386-layout.c'),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_agx_pack, idep_mesautil, idep_nir],
      c_args : [c_msvc_compat_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )
  test(
    'asahi-usc-base-page',
    executable(
      'asahi-usc-base-page',
      files('tests/test-usc-base-page.c'),
      include_directories : [inc_asahi],
      dependencies : [idep_agx_pack, idep_mesautil, idep_nir],
      link_with : [libasahi_lib, libasahi_compiler, libasahi_layout],
      c_args : [c_msvc_compat_args, no_override_init_args],
      gnu_symbol_visibility : 'hidden',
    ),
    suite : ['asahi'],
  )
  test(
    'asahi-agx-chip',
    executable(
      'asahi-agx-chip',
      files(
        'agx_chip.c',
        'tests/test-chip.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx-g17g-tile-format-gate',
    executable(
      'asahi-agx-g17g-tile-format-gate',
      files('tests/test-agx-g17g-tile-format-gate.c'),
      include_directories : [
        inc_include, inc_src, inc_asahi, libasahi_lib_includes,
        include_directories('../libagx'),
      ],
      dependencies : [
        idep_nir, idep_mesautil, idep_libagx_shaders_h, idep_agx_pack,
      ],
      link_with : [libasahi_lib, libasahi_compiler, libasahi_layout],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx-linker-container',
    executable(
      'asahi-agx-linker-container',
      files(
        'agx3_code_container_core.c',
        'tests/test-agx-linker-container.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-g17p-srgb-epilog',
    executable(
      'asahi-g17p-srgb-epilog',
      files('tests/test-agx-g17p-srgb-epilog.c'),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_nir, idep_mesautil],
      link_with : [libasahi_lib, libasahi_compiler, libasahi_layout],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-g17p-mrt-epilog',
    executable(
      'asahi-g17p-mrt-epilog',
      files('tests/test-agx-g17p-mrt-epilog.c'),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_nir, idep_mesautil],
      link_with : [libasahi_lib, libasahi_compiler, libasahi_layout],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-g17p-mrt-quad',
    executable(
      'asahi-g17p-mrt-quad',
      files('tests/test-agx-g17p-mrt-quad.c'),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_nir, idep_mesautil],
      link_with : [libasahi_lib, libasahi_compiler, libasahi_layout],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-g17p-mrt-mixed',
    executable(
      'asahi-g17p-mrt-mixed',
      files('tests/test-agx-g17p-mrt-mixed.c'),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_nir, idep_mesautil],
      link_with : [libasahi_lib, libasahi_compiler, libasahi_layout],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx-va-runtime-hole',
    executable(
      'asahi-agx-va-runtime-hole',
      files(
        'agx_va_runtime_hole.c',
        'tests/test-agx-va-runtime-hole.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_mesautil],
      c_args : ['-UNDEBUG'],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx-m5-va',
    executable(
      'asahi-agx-m5-va',
      files('tests/test-agx-m5-va.c'),
      include_directories : [inc_include, inc_src, inc_asahi],
      c_args : ['-UNDEBUG'],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx-ioto-load-core',
    executable(
      'asahi-agx-ioto-load-core',
      files(
        'agx_ioto_core.c',
        'agx_ioto_load_core.c',
        'tests/test-agx-ioto-load-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx-ioto-preshader-core',
    executable(
      'asahi-agx-ioto-preshader-core',
      files(
        'agx_ioto_core.c',
        'agx_ioto_preshader_core.c',
        'tests/test-agx-ioto-preshader-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx-ioto-program-core',
    executable(
      'asahi-agx-ioto-program-core',
      files(
        'agx_ioto_core.c',
        'agx_ioto_preshader_core.c',
        'agx_ioto_profile_core.c',
        'agx_ioto_state_core.c',
        'agx_ioto_load_core.c',
        'agx_ioto_program_core.c',
        'tests/test-agx-ioto-program-core.c',
      ),
      dependencies : [idep_mesautil],
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-vdm-decode-core',
    executable(
      'asahi-vdm-decode-core',
      files(
        'agx_vdm_barrier_core.c',
        'agx_vdm_decode_core.c',
        'agx_vdm_index_list_core.c',
        'agx_vdm_mesh_core.c',
        'agx_vdm_ppp_state_core.c',
        'agx_vdm_state_core.c',
        'agx_vdm_state_update_core.c',
        'agx_vdm_stream_link_core.c',
        'agx_vdm_stream_terminate_core.c',
        'agx_vdm_tessellate_core.c',
        'tests/test-vdm-decode-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-g18g-capture-vectors',
    executable(
      'asahi-g18g-capture-vectors',
      files(
        'agx3_cdm_decode_core.c',
        'agx_pbe_agx3_core.c',
        'agx_ppp_decode_core.c',
        'agx_ppp_hal300_depth_bias_scissor_core.c',
        'agx_ppp_hal300_depth_stencil_core.c',
        'agx_ppp_viewport_payload_core.c',
        'agx_ppp_w_clamp_cull2_core.c',
        'agx_vdm_barrier_core.c',
        'agx_vdm_decode_core.c',
        'agx_vdm_index_list_core.c',
        'agx_vdm_mesh_core.c',
        'agx_vdm_ppp_state_core.c',
        'agx_vdm_state_core.c',
        'agx_vdm_state_update_core.c',
        'agx_vdm_stream_link_core.c',
        'agx_vdm_stream_terminate_core.c',
        'agx_vdm_tessellate_core.c',
        'tests/test-g18g-capture-vectors.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_agx_pack, idep_mesautil],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-ppp-hal300-g18g',
    executable(
      'asahi-ppp-hal300-g18g',
      files(
        'agx3_cdm_decode_core.c',
        'agx_pbe_agx3_core.c',
        'agx_ppp_decode_core.c',
        'agx_ppp_hal300_depth_bias_scissor_core.c',
        'agx_ppp_hal300_depth_stencil_core.c',
        'agx_ppp_hal300_graphics_core.c',
        'agx_ppp_viewport_payload_core.c',
        'agx_ppp_w_clamp_cull2_core.c',
        'agx_vdm_barrier_core.c',
        'agx_vdm_decode_core.c',
        'agx_vdm_index_list_core.c',
        'agx_vdm_mesh_core.c',
        'agx_vdm_ppp_state_core.c',
        'agx_vdm_state_core.c',
        'agx_vdm_state_update_core.c',
        'agx_vdm_stream_link_core.c',
        'agx_vdm_stream_terminate_core.c',
        'agx_vdm_tessellate_core.c',
        'tests/test-ppp-hal300-g18g.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_agx_pack, idep_mesautil],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-agx3-compute-program-core',
    executable(
      'asahi-agx3-compute-program-core',
      files(
        'agx3_cdm_direct_launch_semantic_core.c',
        'agx3_compute_program_core.c',
        'agx_ioto_core.c',
        'agx_ioto_load_core.c',
        'agx_ioto_preshader_core.c',
        'agx_ioto_profile_core.c',
        'agx_ioto_program_core.c',
        'agx_ioto_state_core.c',
        'tests/test-agx3-compute-program-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
      dependencies : [idep_agx_pack, idep_mesautil],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-vdm-index-list-core',
    executable(
      'asahi-vdm-index-list-core',
      files(
        'agx_vdm_index_list_core.c',
        'tests/test-vdm-index-list-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-pbe-agx3-core',
    executable(
      'asahi-pbe-agx3-core',
      files(
        'agx_pbe_agx3_core.c',
        'tests/test-pbe-agx3-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-ppp-hal300-graphics-core',
    executable(
      'asahi-ppp-hal300-graphics-core',
      files(
        'agx_ppp_hal300_graphics_core.c',
        'agx_vdm_state_core.c',
        'tests/test-ppp-hal300-graphics-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'asahi-vdm-state-update-core',
    executable(
      'asahi-vdm-state-update-core',
      files(
        'agx_vdm_state_core.c',
        'agx_vdm_state_update_core.c',
        'tests/test-vdm-state-update-core.c',
      ),
      include_directories : [inc_include, inc_src, inc_asahi],
    ),
    suite : ['asahi'],
  )

  test(
    'libasahi_tests',
    executable(
      'libasahi_tests',
      files(
        'tests/test-chip.cpp',
        'tests/test-packing.cpp',
        'tests/test-tilebuffer.cpp',
        'tests/test-g17p-msaa-epilog.cpp',
        'tests/test-m5-depth-export.cpp',
        'tests/test-primitive-varyings.cpp',
        '../vulkan/hk_nir_widen_16bit_varyings.c',
        'tests/test-dual-source-epilog.cpp',
      ),
      c_args : [c_msvc_compat_args, no_override_init_args],
      gnu_symbol_visibility : 'hidden',
      dependencies: [
        idep_gtest, idep_agx_pack, idep_mesautil, idep_nir,
      ],
      include_directories: [inc_asahi],
      link_with : [libasahi_lib, libasahi_compiler, libasahi_layout,
                   libagx3c_disasm],
    ),
    suite : ['asahi'],
    protocol : 'gtest',
  )
endif

if with_tests
  test('vbo_robustness', executable('vbo_robustness_test',
    files('tests/test-vbo-robustness.c'),
    include_directories : [inc_asahi, inc_include, inc_src],
    dependencies : [idep_nir, idep_mesautil, idep_agx_pack, idep_libagx_shaders_h],
    link_with : [libasahi_lib, libasahi_compiler, libasahi_layout]),
    suite : ['asahi'])
endif

if dep_iokit.found()
  libasahi_wrap = shared_library(
    'wrap',
    'wrap.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [idep_mesautil, dep_iokit],
    c_args : [no_override_init_args, '-Wno-missing-prototypes'],
    gnu_symbol_visibility : 'hidden',
    build_by_default : with_tools.contains('asahi'),
    link_with: libasahi_decode,
  )
endif

if with_tests
  test('agx3_m5_programs', executable('agx3_m5_programs',
    '../compiler/test/test-agx3-m5-program.c',
    include_directories: [inc_include, inc_src, inc_asahi],
    dependencies: [idep_mesautil, idep_nir],
    link_with: [libasahi_compiler, libasahi_lib]), suite: ['asahi'])
endif

if with_tests
  test('agx_rt_envelope', executable('agx_rt_envelope',
    'tests/test-rt-envelope.c',
    c_args: ['-ffp-contract=off'],
    include_directories: include_directories('.')), suite: ['asahi'])
  test('agx_rt_blas_gen3', executable('agx_rt_blas_gen3',
    'tests/test-rt-blas-gen3.c',
    include_directories: include_directories('.')), suite: ['asahi'])
  test('agx_rt_bvh', executable('agx_rt_bvh',
    'tests/test-rt-bvh.c',
    include_directories: include_directories('.')), suite: ['asahi'])
  test('agx_m5_layout', executable('agx_m5_layout',
    ['tests/test-agx-m5-layout.c', 'agx3_code_container_core.c'],
    include_directories: include_directories('.')), suite: ['asahi'])
endif

if with_tests
  test('agx3_m5_instructions', executable('agx3_m5_instructions',
    '../compiler/test/test-agx3-m5-instructions.c',
    include_directories: [inc_include, inc_src, inc_asahi],
    dependencies: [idep_mesautil, idep_nir, idep_agx_opcodes_h],
    link_with: [libasahi_compiler, libasahi_lib]), suite: ['asahi'])
endif

if with_tests
  test('agx_scs_nir', executable('agx_scs_nir',
    '../compiler/test/test-scs-nir.c',
    c_args: ['-UNDEBUG'],
    include_directories: [inc_include, inc_src, inc_asahi],
    dependencies: [idep_mesautil, idep_nir, idep_agx_opcodes_h],
    link_with: [libasahi_compiler, libasahi_lib]), suite: ['asahi'])
endif

if with_tests
  test('agx3-m5-mesh-program', executable('agx3-m5-mesh-program',
    '../compiler/test/test-native-mesh-program.c',
    include_directories: [inc_include, inc_src, inc_asahi],
    dependencies: [idep_mesautil, idep_nir, idep_agx_opcodes_h],
    link_with: [libasahi_compiler, libasahi_lib]), suite: ['asahi'])
endif

if with_tests
  test('agx_scs_query', executable('agx_scs_query',
    '../compiler/test/test-scs-query.c',
    c_args: ['-UNDEBUG'],
    include_directories: [inc_include, inc_src, inc_asahi],
    dependencies: [idep_mesautil, idep_nir, idep_agx_opcodes_h],
    link_with: [libasahi_compiler, libasahi_lib]), suite: ['asahi'])
endif

if with_tests
  test('agx_scs_layout', executable('agx_scs_layout',
    ['tests/test-scs-layout.c', 'agx_scs_layout.c'],
    c_args: ['-UNDEBUG'],
    include_directories: include_directories('.')), suite: ['asahi'])
endif
