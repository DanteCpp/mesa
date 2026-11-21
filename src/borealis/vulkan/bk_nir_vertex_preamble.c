/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "bk_nir_vertex_preamble.h"
#include "nir_builder.h"

struct bk_vertex_preamble_shift {
   unsigned halves;
   /* Preamble loads below this half are fixed ABI state, not VBO uniforms. */
   unsigned first;
};

static bool
shift_vertex_preamble(UNUSED nir_builder *b, nir_intrinsic_instr *intr,
                      void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_preamble)
      return false;

   const struct bk_vertex_preamble_shift *shift = data;
   const unsigned old = nir_intrinsic_base(intr);
   if (old < shift->first || old > UINT16_MAX - shift->halves)
      return false;

   nir_intrinsic_set_base(intr, old + shift->halves);
   return true;
}

bool
bk_nir_shift_vertex_preamble(nir_shader *nir, unsigned halves,
                             bool native_query)
{
   struct bk_vertex_preamble_shift shift = {
      .halves = halves,
      .first = native_query ? BK_NATIVE_QUERY_STATE_HALFS : 0,
   };
   return nir_shader_intrinsics_pass(nir, shift_vertex_preamble,
                                     nir_metadata_control_flow, &shift);
}
