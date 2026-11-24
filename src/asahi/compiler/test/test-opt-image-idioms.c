/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_nir.h"
#include "compiler/glsl_types.h"
#include "nir_builder.h"

#undef NDEBUG
#include <assert.h>

static const nir_shader_compiler_options opts = {0};

static void
test_loop_invariant_ior(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &opts, "test_loop_ior");
   nir_def *y = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0));
   nir_def *lo = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 4));
   nir_def *hi = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 8));
   nir_def *inv1 = nir_ilt(&b, y, lo);
   nir_def *inv2 = nir_ige(&b, y, hi);
   nir_def *base = nir_imm_int64(&b, 0x1000);

   nir_variable *x_var = nir_local_variable_create(b.impl, glsl_int_type(), "x");
   nir_store_var(&b, x_var, nir_imm_int(&b, 0), 0x1);

   nir_push_loop(&b);
   {
      nir_def *x = nir_load_var(&b, x_var);
      nir_push_if(&b, nir_ige(&b, x, hi));
      nir_jump(&b, nir_jump_break);
      nir_pop_if(&b, NULL);

      /* Mixed bcsel-or chain: bcsel(inv2, true, bcsel(x >= hi, true, bcsel(inv1, true, x < lo))) */
      nir_def *c0 = nir_bcsel(&b, inv1, nir_imm_true(&b), nir_ilt(&b, x, lo));
      nir_def *c1 = nir_bcsel(&b, nir_ige(&b, x, hi), nir_imm_true(&b), c0);
      nir_def *c = nir_bcsel(&b, inv2, nir_imm_true(&b), c1);
      nir_store_agx(&b, nir_b2i32(&b, c), base, x, .format = PIPE_FORMAT_R32_UINT);
      nir_store_var(&b, x_var, nir_iadd_imm(&b, x, 1), 0x1);
   }
   nir_pop_loop(&b, NULL);

   nir_lower_vars_to_ssa(b.shader);
   assert(agx_nir_opt_image_idioms_early(b.shader));
   nir_validate_shader(b.shader, "after loop_invariant_ior");

   /* Verify inv1 | inv2 was hoisted into the preheader block. */
   nir_block *preheader = nir_start_block(b.impl);
   bool found_hoisted_ior = false;
   nir_foreach_instr(instr, preheader) {
      if (instr->type == nir_instr_type_alu &&
          nir_instr_as_alu(instr)->op == nir_op_ior)
         found_hoisted_ior = true;
   }
   assert(found_hoisted_ior);
   ralloc_free(b.shader);
}

static void
test_tri_zero_sink_and_narrow_u8(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &opts, "test_tri_zero");
   nir_def *base = nir_imm_int64(&b, 0x1000);
   nir_def *lo = nir_iadd(&b, nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0)),
                          nir_imm_int(&b, -10));
   nir_def *hi = nir_iadd(&b, nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0)),
                          nir_imm_int(&b, 10));
   nir_def *b1 = nir_u2u8(&b, nir_load_agx(&b, 1, 16, base, nir_imm_int(&b, 0),
                                           .format = PIPE_FORMAT_R8_UINT));
   nir_def *b2 = nir_u2u8(&b, nir_load_agx(&b, 1, 16, base, nir_imm_int(&b, 1),
                                           .format = PIPE_FORMAT_R8_UINT));
   nir_def *v1 = nir_u2u32(&b, b1);
   nir_def *v2 = nir_u2u32(&b, b2);

   nir_def *t1 = nir_bcsel(&b, nir_ilt(&b, v1, lo), nir_imm_int(&b, -1),
                           nir_bcsel(&b, nir_ilt(&b, hi, v1), nir_imm_int(&b, 1),
                                     nir_imm_int(&b, 0)));
   nir_def *t2 = nir_bcsel(&b, nir_ilt(&b, v2, lo), nir_imm_int(&b, -1),
                           nir_bcsel(&b, nir_ilt(&b, hi, v2), nir_imm_int(&b, 1),
                                     nir_imm_int(&b, 0)));
   nir_def *both_zero = nir_ieq_imm(&b, nir_umax(&b, t1, t2), 0);

   nir_if *nif = nir_push_if(&b, both_zero);
   {
      nir_store_agx(&b, nir_imm_int(&b, 0), base, nir_imm_int(&b, 4),
                    .format = PIPE_FORMAT_R32_UINT);
   }
   nir_push_else(&b, nif);
   {
      /* Downstream consumer in the cold non-early-exit arm. */
      nir_store_agx(&b, nir_iadd(&b, t1, t2), base, nir_imm_int(&b, 4),
                    .format = PIPE_FORMAT_R32_UINT);
   }
   nir_pop_if(&b, nif);

   assert(agx_nir_opt_image_idioms_early(b.shader));
   nir_opt_dce(b.shader);
   nir_validate_shader(b.shader, "after tri_zero_sink_and_narrow_u8");

   /* Verify both bcsels were sunk out of the start block into the else arm. */
   nir_block *start_blk = nir_start_block(b.impl);
   unsigned start_bcsels = 0, else_bcsels = 0;
   nir_foreach_instr(instr, start_blk) {
      if (instr->type == nir_instr_type_alu &&
          nir_instr_as_alu(instr)->op == nir_op_bcsel)
         start_bcsels++;
   }
   nir_block *else_blk = nir_if_first_else_block(nif);
   nir_foreach_instr(instr, else_blk) {
      if (instr->type == nir_instr_type_alu &&
          nir_instr_as_alu(instr)->op == nir_op_bcsel)
         else_bcsels++;
   }
   assert(start_bcsels == 0);
   assert(else_bcsels == 4);
   ralloc_free(b.shader);
}

static void
test_coalesce_guard_ifs(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &opts, "test_guard_ifs");
   nir_def *base = nir_imm_int64(&b, 0x2000);
   nir_def *ix = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0));
   nir_def *val = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 4));

   /* 4 consecutive side-effect-only guard ifs. */
   for (unsigned i = 0; i < 4; i++) {
      nir_push_if(&b, nir_ilt_imm(&b, ix, (int)i));
      nir_store_agx(&b, val, base, nir_imm_int(&b, i + 1),
                    .format = PIPE_FORMAT_R32_UINT);
      nir_pop_if(&b, NULL);
   }

   assert(agx_nir_opt_image_idioms_late(b.shader));
   nir_validate_shader(b.shader, "after coalesce_guard_ifs");

   /* Expect the 4 guard ifs to be wrapped in a single outer if. */
   unsigned top_level_ifs = 0;
   foreach_list_typed(nir_cf_node, n, node, &b.impl->body) {
      if (n->type == nir_cf_node_if)
         top_level_ifs++;
   }
   assert(top_level_ifs == 1);
   ralloc_free(b.shader);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   test_loop_invariant_ior();
   test_tri_zero_sink_and_narrow_u8();
   test_coalesce_guard_ifs();
   glsl_type_singleton_decref();
   return 0;
}
