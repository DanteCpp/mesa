/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "nir.h"

/* Lower 32-bit robust UBO loads that fit in one record_size-byte record to a
 * global load from that record, or from the zero page when the record is out
 * of bounds, so that loads of one record share its bounds check. Runs before
 * nir_lower_explicit_io on UBO derefs whose descriptor is a 4 x 32-bit
 * bounded address (nir_address_format_64bit_bounded_global). */
bool brl_nir_lower_aligned_ubo(nir_shader *nir, unsigned record_size,
                               unsigned max_range);
