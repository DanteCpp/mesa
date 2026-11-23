# Copyright 2026 Aurora Silicon contributors
# SPDX-License-Identifier: MIT

aurora_d3d11_winddk_dir = get_option('aurora-d3d11-winddk-dir')

aurora_d3d11_c_args = [
  '-UWIN32_LEAN_AND_MEAN',
  '-D_WIN32_WINNT=0x0A00',
  '-DNTDDI_VERSION=0x0A000010',
  '-Wno-unknown-pragmas',
  '-Wno-ignored-attributes',
  '-Wno-microsoft-enum-forward-reference',
  '-I' + aurora_d3d11_winddk_dir,
]

libaurora_d3d11 = static_library(
  'aurora_d3d11',
  files(
    'sm5_to_nir.c',
    'aurora_format.c',
    'aurora_adapter.c',
    'aurora_resource.c',
    'aurora_state.c',
    'aurora_view_shader_query.c',
    'aurora_dxgi.c',
  ),
  c_args : aurora_d3d11_c_args,
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  dependencies : [
    idep_nir_headers,
    idep_mesautil,
  ],
  gnu_symbol_visibility : 'hidden',
)
