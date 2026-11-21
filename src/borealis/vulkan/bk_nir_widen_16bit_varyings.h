/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

struct nir_shader;

enum bk_nir_widen_16bit_varyings_mode {
   BK_NIR_WIDEN_16BIT_INPUTS = 1 << 0,
   BK_NIR_WIDEN_16BIT_OUTPUTS = 1 << 1,
};

bool bk_nir_widen_16bit_varyings(
   struct nir_shader *nir, enum bk_nir_widen_16bit_varyings_mode mode);
