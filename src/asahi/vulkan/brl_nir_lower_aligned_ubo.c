/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "brl_nir_lower_aligned_ubo.h"
#include "asahi/lib/agx_abi.h"
#include "nir_builder.h"
#include "nir_deref.h"

struct aligned_ubo_state {
   unsigned record_size;
   unsigned max_range;
};

/* Keep naturally aligned robust UBO loads together. */
static bool
lower_aligned_ubo(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct aligned_ubo_state *state = data;
   const unsigned record_size = state->record_size;

   if (intr->intrinsic != nir_intrinsic_load_deref ||
       intr->def.bit_size != 32 || intr->num_components > 4 ||
       (nir_intrinsic_access(intr) & ACCESS_VOLATILE))
      return false;

   nir_deref_instr *leaf = nir_src_as_deref(intr->src[0]);
   if (leaf->modes != nir_var_mem_ubo ||
       (glsl_get_explicit_stride(leaf->type) != 0 &&
        glsl_get_explicit_stride(leaf->type) != 4))
      return false;

   uint32_t mul, disp;
   if (!nir_get_explicit_deref_align(leaf, true, &mul, &disp) || mul < 4 ||
       (disp % MIN2(mul, record_size)) + intr->num_components * 4 >
          MIN2(mul, record_size) ||
       mul > AGX_ZERO_PAGE_SIZE)
      return false;

   nir_deref_path path;
   nir_deref_path_init(&path, leaf, NULL);
   nir_deref_instr *root = path.path[0];
   if (root->deref_type != nir_deref_type_cast ||
       root->parent.ssa->num_components != 4 ||
       root->parent.ssa->bit_size != 32) {
      nir_deref_path_finish(&path);
      return false;
   }

   /* Buffer descriptors only: robustUniformBufferAccessSizeAlignment rounds
    * their range to the record. An inline uniform block's size is not rounded,
    * and with descriptor buffers the set layout reports the unrounded size, so
    * the record holding the last block can extend past the descriptor set.
    * hk_nir_lower_descriptors() gives an inline block a constant bound, while a
    * buffer descriptor's bound is read from memory, so a constant bound keeps
    * the exact range check of nir_lower_explicit_io. */
   if (nir_scalar_is_const(nir_scalar_resolved(root->parent.ssa, 2))) {
      nir_deref_path_finish(&path);
      return false;
   }

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *addr = root->parent.ssa;
   /* The deref arithmetic scales each array index into a 32-bit byte offset,
    * which can wrap before the record check below sees it. The limit is
    * ceil(BRL_MAX_UBO_RANGE / stride), computed from max_range (the caller
    * passes BRL_MAX_UBO_RANGE): an index at or above it puts the element at
    * or past BRL_MAX_UBO_RANGE, out of bounds of any UBO, so it selects the
    * zero page. Every index below it keeps its scaled offset under
    * BRL_MAX_UBO_RANGE + stride, so the offset cannot wrap. Constant and
    * dynamic indices use the same limit. */
   nir_def *indices_ok = nir_imm_true(b);
   for (unsigned i = 1; path.path[i]; ++i) {
      nir_deref_instr *deref = path.path[i];
      if (deref->deref_type == nir_deref_type_array ||
          deref->deref_type == nir_deref_type_ptr_as_array) {
         const unsigned stride = MAX2(nir_deref_instr_array_stride(deref), 1);
         const unsigned limit = 1u + (state->max_range - 1u) / stride;
         if (!nir_src_is_const(deref->arr.index)) {
            indices_ok = nir_iand(b, indices_ok,
                                  nir_ult_imm(b, deref->arr.index.ssa, limit));
         } else if (nir_src_as_uint(deref->arr.index) >= limit) {
            indices_ok = nir_imm_false(b);
         }
      }
      addr = nir_explicit_io_address_from_deref(
         b, deref, addr, nir_address_format_64bit_bounded_global);
   }
   nir_deref_path_finish(&path);

   nir_def *base = nir_pack_64_2x32(b, nir_trim_vector(b, addr, 2));
   nir_def *off = nir_channel(b, addr, 3);
   nir_def *bound = nir_channel(b, addr, 2);
   /* Factor neighboring fields onto the same checked record. The zero page
    * includes one whole aligned record. */
   nir_def *record = nir_iand_imm(b, off, ~(record_size - 1));
   nir_def *real = nir_iadd(b, base, nir_u2u64(b, record));
   nir_def *safe =
      nir_bcsel(b, nir_iand(b, nir_ult(b, record, bound), indices_ok), real,
                nir_imm_int64(b, AGX_ZERO_PAGE_ADDRESS));
   nir_def *field = nir_iand_imm(b, off, record_size - 1);
   nir_def *address = nir_iadd(b, safe, nir_u2u64(b, field));
   nir_def *value = nir_load_global(
      b, intr->num_components, 32, address, .align_mul = MIN2(mul, record_size),
      .align_offset = disp % MIN2(mul, record_size),
      .access = nir_intrinsic_access(intr) | ACCESS_NON_WRITEABLE |
                ACCESS_CAN_REORDER);
   nir_def_replace(&intr->def, value);
   return true;
}

bool
brl_nir_lower_aligned_ubo(nir_shader *nir, unsigned record_size,
                          unsigned max_range)
{
   assert(util_is_power_of_two_nonzero(record_size));
   assert(record_size <= AGX_ZERO_PAGE_SIZE && max_range > 0);
   struct aligned_ubo_state state = {
      .record_size = record_size,
      .max_range = max_range,
   };
   return nir_shader_intrinsics_pass(nir, lower_aligned_ubo,
                                     nir_metadata_control_flow, &state);
}
