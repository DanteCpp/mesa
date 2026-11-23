/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Direct3D 10/11 SM4.0 / SM4.1 / SM5.0 DDI tokenized shader -> Mesa NIR.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler/nir/nir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gallium SSBO slot split:
 *   slots 0 .. AURORA_SSBO_UAV_COUNT-1   -> UAV raw/structured buffers u0..u7
 *   slots AURORA_SSBO_SRV_BASE .. 31     -> SRV raw/structured buffers t0..t23
 */
#define AURORA_SSBO_UAV_COUNT 8u
#define AURORA_SSBO_SRV_BASE  8u
#define AURORA_MAX_SRV_SLOTS  128u
#define AURORA_MAX_UAV_SLOTS  8u
#define AURORA_MAX_CB_SLOTS   14u

struct aurora_sm5_sig_elem {
   uint32_t reg;
   uint32_t sysval; /* D3D10_SB_NAME / enum sm5_sysval_name */
   uint8_t mask;
};

struct aurora_sm5_info {
   mesa_shader_stage stage;
   uint32_t srv_raw_or_struct_mask; /* bit t = 1 if t# is raw/structured SRV */
   uint32_t srv_stride[32];
   uint32_t uav_raw_or_struct_mask; /* bit u = 1 if u# is raw/structured UAV */
   uint32_t uav_typed_mask;         /* bit u = 1 if u# is typed UAV */
   uint32_t uav_stride[AURORA_MAX_UAV_SLOTS];
   uint32_t cb_mask;
   uint32_t sampler_mask;
   uint32_t workgroup_size[3];
   char err[256];
};

/*
 * Translate a D3D10/11 DDI shader token stream (starting at VerTok, LenTok, ...)
 * directly into a validated Mesa NIR shader ready for Gallium Asahi.
 *
 * `options` must be `pscreen->nir_options[stage]` (or `&agx_nir_options` in host
 * compiler tests). `info_out` is optional and receives resource binding metadata.
 * Returns NULL on malformed bytecode or unsupported shader stage/instruction.
 */
nir_shader *aurora_sm5_to_nir(const uint32_t *code, size_t max_dwords,
                              const struct aurora_sm5_sig_elem *in_sig,
                              unsigned num_in_sig,
                              const struct aurora_sm5_sig_elem *out_sig,
                              unsigned num_out_sig,
                              const nir_shader_compiler_options *options,
                              struct aurora_sm5_info *info_out);

#ifdef __cplusplus
}
#endif
