/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * DXGI_FORMAT <-> Gallium pipe_format mapping + WDDM 2.0 / FL 11_1
 * CheckFormatSupport & CheckMultisampleQualityLevels contract.
 */
#pragma once

#include "aurora_d3d11.h"

#ifdef __cplusplus
extern "C" {
#endif

enum pipe_format aurora_dxgi_to_pipe_format(DXGI_FORMAT fmt);
enum pipe_format aurora_dxgi_to_pipe_format_dsv(DXGI_FORMAT fmt);
enum pipe_format aurora_dxgi_to_pipe_format_srv(DXGI_FORMAT fmt);
bool aurora_format_is_depth_stencil(DXGI_FORMAT fmt);
bool aurora_format_is_typeless(DXGI_FORMAT fmt);
bool aurora_format_is_srgb(DXGI_FORMAT fmt);
uint32_t aurora_dxgi_to_d3dddi_format(DXGI_FORMAT fmt);
DXGI_FORMAT aurora_d3dddi_to_dxgi_format(uint32_t d3d_fmt);

#ifdef __cplusplus
}
#endif
