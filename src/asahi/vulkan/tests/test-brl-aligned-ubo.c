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

static bool
shader_has_element_mask(nir_shader *nir)
{
   const uint32_t mask = MAX_RANGE / 16 - 1;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op != nir_op_iand)
               continue;
            for (unsigned s = 0; s < 2; s++) {
               if (nir_src_is_const(alu->src[s].src) &&
                   nir_src_as_uint(alu->src[s].src) == mask)
                  return true;
            }
         }
      }
   }
   return false;
}

/* Struct-wrapped vec4 UBO array (layout(std140) uniform U { uvec4 a[4096]; }):
 * exercises the element-bounds fast path when field_offset == 0,
 * initial_offset == 0, index == UINT64_MAX, and !constant_bound, and the
 * fallback paths otherwise. */
static uint64_t
lower_struct_vec4(uint32_t bound, bool constant_bound, uint32_t initial_offset,
                  int field_offset, uint64_t index, uint32_t value,
                  bool *used_element_path)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options,
                                                  "aligned-ubo-vec4");
   nir_def *dynamic = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                             .base = 0, .range = 4);
   nir_def *loaded_bound = nir_load_push_constant(
      &b, 1, 32, nir_imm_int(&b, 0), .base = 4, .range = 4);
   nir_def *desc_bound = constant_bound ? nir_imm_int(&b, bound) : loaded_bound;
   nir_def *desc = nir_vec4(&b, nir_imm_int(&b, (uint32_t)BASE),
                            nir_imm_int(&b, (uint32_t)(BASE >> 32)), desc_bound,
                            nir_imm_int(&b, initial_offset));
   const struct glsl_type *arr = glsl_array_type(
      glsl_vector_type(GLSL_TYPE_UINT, 4), MAX_RANGE / 16, 16);
   struct glsl_struct_field field = {
      .type = arr,
      .name = "a",
      .offset = field_offset,
   };
   const struct glsl_type *block =
      glsl_struct_type(&field, 1, "Ubo", false);
   nir_deref_instr *deref =
      nir_build_deref_cast(&b, desc, nir_var_mem_ubo, block, 0);
   deref->cast.align_mul = RECORD;
   deref->cast.align_offset = 0;
   deref = nir_build_deref_struct(&b, deref, 0);
   deref = nir_build_deref_array(
      &b, deref, index == UINT64_MAX ? dynamic : nir_imm_int(&b, index));
   nir_def *value_def = nir_load_deref(&b, deref);
   nir_store_global(&b, value_def, nir_imm_int64(&b, 0x8000), .align_mul = 16,
                    .write_mask = 0xf);
   nir_validate_shader(b.shader, "before brl_nir_lower_aligned_ubo");

   uint64_t address = 0;
   if (used_element_path)
      *used_element_path = false;
   if (brl_nir_lower_aligned_ubo(b.shader, RECORD, MAX_RANGE)) {
      nir_validate_shader(b.shader, "after brl_nir_lower_aligned_ubo");
      if (used_element_path)
         *used_element_path = shader_has_element_mask(b.shader);
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
         nir_foreach_block(block_bb, impl) {
            nir_foreach_instr(instr, block_bb) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               CHECK(intr->intrinsic != nir_intrinsic_load_deref);
               if (intr->intrinsic != nir_intrinsic_load_global)
                  continue;
               CHECK(intr->def.num_components == 4);
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
struct_vec4(uint32_t bound, uint32_t index)
{
   bool used_element_path = false;
   uint64_t addr = lower_struct_vec4(bound, false, 0, 0, UINT64_MAX, index,
                                     &used_element_path);
   CHECK(used_element_path);
   return addr;
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

   /* Vec4 element-bounds fast path: null descriptor and every tail 0..64. */
   for (uint32_t bound = 0; bound <= 64; bound++) {
      const uint32_t elems = ((bound + 63u) / 64u) * 4u;
      if (elems == 0) {
         CHECK(struct_vec4(bound, 0) == zero);
         CHECK(struct_vec4(bound, 1) == zero + 16);
      } else {
         CHECK(struct_vec4(bound, 0) == BASE);
         CHECK(struct_vec4(bound, elems - 1) == BASE + (elems - 1) * 16u);
      }
      CHECK(struct_vec4(bound, elems) == zero + ((elems & 4095u) * 16u));
      CHECK(struct_vec4(bound, 4095) == zero + 4095u * 16u);
      CHECK(struct_vec4(bound, 4096) == zero);
      CHECK(struct_vec4(bound, 4097) == zero + 16u);
      CHECK(struct_vec4(bound, UINT32_MAX) == zero + 4095u * 16u);
   }

   /* Non-64-byte-aligned bounds above 64 and full 64 KiB range, including
    * indices >= 4096 that must select the zero page before masking. */
   const uint32_t bounds[] = {65, 80, 127, 128, 129, 65472, 65473, 65520,
                              65535, 65536};
   const uint32_t high_indices[] = {4096, 4097, 8191, 8192, 1u << 28,
                                    0x80000000u, UINT32_MAX};
   for (unsigned i = 0; i < ARRAY_SIZE(bounds); i++) {
      const uint32_t bound = bounds[i];
      const uint32_t elems = ((bound + 63u) / 64u) * 4u;
      CHECK(struct_vec4(bound, 0) == BASE);
      CHECK(struct_vec4(bound, elems - 1) == BASE + (elems - 1) * 16u);
      CHECK(struct_vec4(bound, elems) == zero + ((elems & 4095u) * 16u));
      for (unsigned j = 0; j < ARRAY_SIZE(high_indices); j++) {
         const uint32_t idx = high_indices[j];
         CHECK(struct_vec4(bound, idx) == zero + ((idx & 4095u) * 16u));
      }
   }

   /* Fast-path exclusions fall back to the record path or leave inline blocks
    * untouched. */
   bool used_element_path = true;
   CHECK(lower_struct_vec4(128, false, 16, 0, UINT64_MAX, 2,
                           &used_element_path) == BASE + 48);
   CHECK(!used_element_path);
   CHECK(lower_struct_vec4(128, false, 0, 16, UINT64_MAX, 2,
                           &used_element_path) == BASE + 48);
   CHECK(!used_element_path);
   CHECK(lower_struct_vec4(128, false, 0, 0, 2, 0, &used_element_path) ==
         BASE + 32);
   CHECK(!used_element_path);
   CHECK(lower_struct_vec4(64, true, 0, 0, UINT64_MAX, 0,
                           &used_element_path) == 0);
   CHECK(!used_element_path);

   glsl_type_singleton_decref();
   printf("brl_aligned_ubo: OK\n");
   return 0;
}
