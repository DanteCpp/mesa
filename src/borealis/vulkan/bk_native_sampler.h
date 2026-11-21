/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "asahi/genxml/agx_pack.h"
#include "asahi/lib/agx_chip.h"

#include <stdbool.h>
#include <stdint.h>

#define BK_NATIVE_SAMPLER_MAX_ELEMENT_SIZE 32

enum bk_sampler_reduction_mode {
   BK_SAMPLER_REDUCTION_WEIGHTED_AVERAGE,
   BK_SAMPLER_REDUCTION_MINIMUM,
   BK_SAMPLER_REDUCTION_MAXIMUM,
};

/* Generation-independent sampler semantics.  The packer below is the only
 * place where Borealis selects the Gen4, HAL200, or Gen5 bit layout. */
struct bk_native_sampler_state {
   float minimum_lod;
   float maximum_lod;
   float lod_bias;
   unsigned maximum_anisotropy;
   enum agx_filter magnify;
   enum agx_filter minify;
   enum agx_mip_filter mip_filter;
   enum agx_wrap wrap_s;
   enum agx_wrap wrap_t;
   enum agx_wrap wrap_r;
   enum agx_compare_func compare_func;
   enum agx_border_colour border_colour;
   enum bk_sampler_reduction_mode reduction_mode;
   bool pixel_coordinates;
   bool compare_enable;
   bool seamful_cube_maps;
};

/* An AGX3 heap element is sparse: its eight-byte packed state word remains at
 * offset zero and the remaining 24 bytes are zero.  Keeping the full element
 * here makes upload size and clearing impossible to confuse with word size. */
struct bk_native_sampler_descriptor {
   uint32_t opaque[BK_NATIVE_SAMPLER_MAX_ELEMENT_SIZE / sizeof(uint32_t)];
};

bool bk_sampler_reduction_supported(
   enum agx_sampler_generation generation,
   enum bk_sampler_reduction_mode reduction_mode);

bool bk_pack_native_sampler(enum agx_chip chip,
                            struct bk_native_sampler_descriptor *out,
                            const struct bk_native_sampler_state *state);

bool bk_pack_native_txf_sampler(enum agx_chip chip,
                                struct bk_native_sampler_descriptor *out);
