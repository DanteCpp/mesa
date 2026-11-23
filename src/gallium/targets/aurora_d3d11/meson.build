# Copyright 2026 Aurora Silicon contributors
# SPDX-License-Identifier: MIT

aurora_d3d11_link_args = [
  '-Wl,--kill-at',
  '-Wl,--gc-sections',
  '-static-libgcc',
  '-static-libstdc++',
  '-ladvapi32',
  '-lgdi32',
  '-lkernel32',
  '-luser32',
]

libaurora_d3d11_dll = shared_library(
  'AuroraD3D11',
  [],
  vs_module_defs : 'AuroraD3D11.def',
  link_whole : [libaurora_d3d11],
  link_with : [
    libasahi,
    libasahi_lib,
    libasahi_compiler,
    libasahi_layout,
    libasahi_decode,
    libgallium,
  ],
  dependencies : [
    idep_libagx,
    idep_nir,
    idep_mesautil,
    dep_ws2_32,
  ],
  link_args : aurora_d3d11_link_args,
  name_prefix : '',
  install : true,
)
