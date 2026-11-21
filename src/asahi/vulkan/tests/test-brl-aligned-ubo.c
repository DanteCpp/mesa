/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#include "brl_nir_lower_aligned_ubo.h"
#include "asahi/lib/agx_abi.h"
#include "compiler/glsl_types.h"
#include "nir_builder.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);          \
         exit(1);                                                            \
      }                                                                      \
   } while (0)

#define RECORD    64u
#define MAX_RANGE 65536u
#define BASE      UINT64_C(0x1234500000)

/* A robust UBO binding of `bound` bytes at BASE, read as an array of `type`
 * elements (stride `stride`): load element `index` (and, for an array of
 * arrays, element `inner` of it). An index given as UINT64_MAX is dynamic.
 * The bound is read from memory, as from a buffer descriptor, or, for
 * `constant_bound`, an immediate, as for an inline uniform block. The dynamic
 * index is then set to `value`, the bound to `bound`, and the load's address
 * folded. Returns the address, or 0 if the pass left the load alone. */
static uint64_t
lower(const struct glsl_type *elem, unsigned stride, unsigned count,
      unsigned components, uint32_t bound, bool constant_bound, uint64_t index,
      uint64_t inner, uint32_t value)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                                  &options, "aligned-ubo");
   nir_def *dynamic = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                             .base = 0, .range = 4);
   nir_def *loaded_bound = nir_load_push_constant(
      &b, 1, 32, nir_imm_int(&b, 0), .base = 4, .range = 4);
   nir_def *desc_bound = constant_bound ? nir_imm_int(&b, bound) : loaded_bound;
   nir_def *desc = nir_vec4(&b, nir_imm_int(&b, (uint32_t)BASE),
                            nir_imm_int(&b, (uint32_t)(BASE >> 32)), desc_bound,
                            nir_imm_int(&b, 0));
   const struct glsl_type *type = glsl_array_type(elem, count, stride);
   nir_deref_instr *deref =
      nir_build_deref_cast(&b, desc, nir_var_mem_ubo, type, 0);
   deref->cast.align_mul = RECORD;
   deref->cast.align_offset = 0;
   deref = nir_build_deref_array(
      &b, deref, index == UINT64_MAX ? dynamic : nir_imm_int(&b, index));
   if (glsl_type_is_array(elem)) {
      deref = nir_build_deref_array(
         &b, deref, inner == UINT64_MAX ? dynamic : nir_imm_int(&b, inner));
   }
   CHECK(deref->type == glsl_vector_type(GLSL_TYPE_UINT, components));
   nir_def *value_def = nir_load_deref(&b, deref);
   nir_store_global(&b, value_def, nir_imm_int64(&b, 0x8000), .align_mul = 4,
                    .write_mask = BITFIELD_MASK(components));
   nir_validate_shader(b.shader, "before brl_nir_lower_aligned_ubo");

   uint64_t address = 0;
   if (brl_nir_lower_aligned_ubo(b.shader, RECORD, MAX_RANGE)) {
      nir_validate_shader(b.shader, "after brl_nir_lower_aligned_ubo");
      b.cursor = nir_after_instr(nir_def_instr(loaded_bound));
      nir_def_rewrite_uses(dynamic, nir_imm_int(&b, value));
      nir_def_rewrite_uses(loaded_bound, nir_imm_int(&b, bound));
      bool progress;
      do {
         progress = false;
         NIR_PASS(progress, b.shader, nir_opt_copy_prop);
         NIR_PASS(progress, b.shader, nir_opt_constant_folding);
         NIR_PASS(progress, b.shader, nir_opt_algebraic);
         NIR_PASS(progress, b.shader, nir_opt_dce);
      } while (progress);

      unsigned loads = 0;
      nir_foreach_function_impl(impl, b.shader) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               CHECK(intr->intrinsic != nir_intrinsic_load_deref);
               if (intr->intrinsic != nir_intrinsic_load_global)
                  continue;
               CHECK(intr->def.num_components == components);
               CHECK(nir_src_is_const(intr->src[0]));
               address = nir_src_as_uint(intr->src[0]);
               loads++;
            }
         }
      }
      CHECK(loads == 1);
   }
   ralloc_free(b.shader);
   return address;
}

static uint64_t
lowered_address(const struct glsl_type *elem, unsigned stride, unsigned count,
                unsigned components, uint32_t bound, uint64_t index,
                uint64_t inner, uint32_t value)
{
   return lower(elem, stride, count, components, bound, false, index, inner,
                value);
}

static uint64_t
scalar(uint32_t bound, uint32_t index)
{
   return lowered_address(glsl_uint_type(), 4, MAX_RANGE / 4, 1, bound,
                          UINT64_MAX, 0, index);
}

static uint64_t
vec4(uint32_t bound, uint32_t index)
{
   return lowered_address(glsl_vector_type(GLSL_TYPE_UINT, 4), 16,
                          MAX_RANGE / 16, 4, bound, UINT64_MAX, 0, index);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   const uint64_t zero = AGX_ZERO_PAGE_ADDRESS;

   /* In bounds: the element itself. */
   CHECK(scalar(MAX_RANGE, 0) == BASE);
   CHECK(scalar(MAX_RANGE, 17) == BASE + 68);
   CHECK(scalar(MAX_RANGE, 16383) == BASE + 65532);
   CHECK(vec4(MAX_RANGE, 4095) == BASE + 65520);

   /* Past the binding: the same field of the zero page. */
   CHECK(scalar(MAX_RANGE, 16384) == zero);
   CHECK(scalar(MAX_RANGE, 16385) == zero + 4);
   CHECK(vec4(MAX_RANGE, 4096) == zero);

   /* The binding is checked by whole records: within the last record, the
    * read is in the range rounded up to robustUniformBufferAccessSizeAlignment.
    */
   CHECK(scalar(16, 3) == BASE + 12);
   CHECK(scalar(16, 15) == BASE + 60);
   CHECK(scalar(16, 16) == zero);
   CHECK(vec4(16, 3) == BASE + 48);
   CHECK(vec4(16, 4) == zero);

   /* Indices whose byte offset wraps 32 bits to an in-bounds record. */
   CHECK(scalar(MAX_RANGE, 1u << 30) == zero);
   CHECK(scalar(MAX_RANGE, (1u << 30) + 1) == zero + 4);
   CHECK(scalar(MAX_RANGE, UINT32_MAX) == zero + 60);
   CHECK(vec4(MAX_RANGE, 1u << 28) == zero);
   CHECK(vec4(MAX_RANGE, (1u << 28) + 5) == zero + 16);

   const struct glsl_type *row = glsl_array_type(glsl_uint_type(), 4, 4);
   /* A constant outer index that wraps, with a dynamic inner one. */
   CHECK(lowered_address(row, 16, MAX_RANGE / 16, 1, MAX_RANGE, 1u << 28,
                         UINT64_MAX, 2) == zero + 8);
   CHECK(lowered_address(row, 16, MAX_RANGE / 16, 1, MAX_RANGE, 7,
                         UINT64_MAX, 2) == BASE + 120);
   /* A dynamic outer index that wraps, with a constant inner one. */
   CHECK(lowered_address(row, 16, MAX_RANGE / 16, 1, MAX_RANGE, UINT64_MAX, 3,
                         1u << 28) == zero + 12);

   /* Constant indices: in bounds, past the binding, and wrapping. */
   CHECK(lowered_address(glsl_uint_type(), 4, MAX_RANGE / 4, 1, MAX_RANGE, 5,
                         0, 0) == BASE + 20);
   CHECK(lowered_address(glsl_uint_type(), 4, MAX_RANGE / 4, 1, 16, 16, 0,
                         0) == zero);
   CHECK(lowered_address(glsl_uint_type(), 4, MAX_RANGE / 4, 1, MAX_RANGE,
                         1u << 30, 0, 0) == zero);
   CHECK(lowered_address(row, 16, MAX_RANGE / 16, 1, MAX_RANGE, 7, 2, 0) ==
         BASE + 120);

   /* An inline uniform block keeps its exact range check: its size is not
    * rounded to the record. */
   CHECK(lower(glsl_uint_type(), 4, MAX_RANGE / 4, 1, 16, true, UINT64_MAX, 0,
               3) == 0);
   CHECK(lower(glsl_vector_type(GLSL_TYPE_UINT, 4), 16, MAX_RANGE / 16, 4, 16,
               true, 0, 0, 0) == 0);

   glsl_type_singleton_decref();
   printf("brl_aligned_ubo: OK\n");
   return 0;
}
