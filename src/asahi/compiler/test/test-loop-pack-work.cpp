/* Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 */
#include <gtest/gtest.h>
#include "agx_test.h"
extern "C" {
#include "compiler/nir/nir_builder.h"
#include "agx_compile.h"
#include "agx_nir.h"
}

/* Submit packing narrows agx_shader_info::has_loops to loops whose trip count
 * NIR loop analysis cannot bound at compile time, and weights compile-time-
 * bounded loop bodies by their iteration bound (max_trip_count + 1) inside
 * agx_shader_info::pack_instrs with saturating 64-bit arithmetic. */

namespace {

static nir_def *
mix_step(nir_builder *b, nir_def *v, nir_def *salt)
{
   v = nir_ixor(b, v, salt);
   v = nir_imul_imm(b, nir_ixor(b, v, nir_ushr_imm(b, v, 13)), 0x85ebca6bu);
   v = nir_imul_imm(b, nir_ixor(b, v, nir_ushr_imm(b, v, 16)), 0xc2b2ae35u);
   return nir_ixor(b, v, nir_ushr_imm(b, v, 15));
}

struct loop_spec {
   /* Non-zero constant trip count, or 0 for an unbounded runtime trip count. */
   uint32_t trips;
};

static agx_shader_info
compile_nested_loops(const loop_spec *loops, unsigned depth,
                     agx_isa_encoding encoding, agx_chip chip, bool *ok)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                                  &agx_nir_options, "pack_loop");
   nir_def *addr = agx_test_load_preamble(&b, 1, 64, 0);
   nir_def *hdr = nir_load_global(&b, 4, 32, addr, .align_mul = 16);
   nir_def *acc = nir_channel(&b, hdr, 0);
   nir_def *salt = nir_channel(&b, hdr, 1);
   nir_def *runtime_limit = nir_channel(&b, hdr, 2);

   nir_loop *nloops[4] = {};
   nir_phi_instr *i_phis[4] = {};
   nir_phi_instr *acc_phis[4] = {};
   nir_def *limits[4] = {};

   assert(depth <= ARRAY_SIZE(nloops));
   for (unsigned d = 0; d < depth; ++d) {
      limits[d] = loops[d].trips ? nir_imm_int(&b, loops[d].trips) : runtime_limit;
      nir_def *i0 = nir_imm_int(&b, 0);
      nir_def *acc0 = acc;

      nloops[d] = nir_push_loop(&b);
      nir_block *preheader =
         nir_cf_node_as_block(nir_cf_node_prev(&nloops[d]->cf_node));

      i_phis[d] = nir_phi_instr_create(b.shader);
      acc_phis[d] = nir_phi_instr_create(b.shader);
      nir_def_init(&i_phis[d]->instr, &i_phis[d]->def, 1, 32);
      nir_def_init(&acc_phis[d]->instr, &acc_phis[d]->def, 1, 32);
      nir_phi_instr_add_src(i_phis[d], preheader, i0);
      nir_phi_instr_add_src(acc_phis[d], preheader, acc0);
      nir_builder_instr_insert(&b, &i_phis[d]->instr);
      nir_builder_instr_insert(&b, &acc_phis[d]->instr);

      nir_push_if(&b, nir_uge(&b, &i_phis[d]->def, limits[d]));
      nir_jump(&b, nir_jump_break);
      nir_pop_if(&b, NULL);

      acc = &acc_phis[d]->def;
   }

   /* Non-linear recurrence so NIR cannot fold the loop to a closed form. */
   acc = mix_step(&b, acc, salt);
   if (depth)
      acc = nir_iadd(&b, acc, &i_phis[depth - 1]->def);

   for (int d = (int)depth - 1; d >= 0; --d) {
      nir_def *next_i = nir_iadd_imm(&b, &i_phis[d]->def, 1);
      nir_block *tail = nir_cursor_current_block(b.cursor);
      nir_block *header = nir_loop_first_block(nloops[d]);
      /* Phi sources only join their definitions' use lists on insertion. */
      nir_instr_remove(&acc_phis[d]->instr);
      nir_instr_remove(&i_phis[d]->instr);
      nir_phi_instr_add_src(i_phis[d], tail, next_i);
      nir_phi_instr_add_src(acc_phis[d], tail, acc);
      nir_instr_insert(nir_before_block(header), &acc_phis[d]->instr);
      nir_instr_insert(nir_before_block(header), &i_phis[d]->instr);
      nir_pop_loop(&b, nloops[d]);
      acc = &acc_phis[d]->def;
   }

   nir_store_global(&b, acc, nir_iadd_imm(&b, addr, 16), .align_mul = 4);
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   agx_shader_key key = {};
   key.dev.encoding = encoding;
   key.dev.chip = chip;
   key.no_preamble = true;
   key.reserved_preamble = 16;

   agx_preprocess_nir(b.shader);
   agx_shader_part part = {};
   *ok = agx_compile_shader_nir(b.shader, &key, &part);
   agx_shader_info info = part.info;
   free(part.binary);
   ralloc_free(b.shader);
   return info;
}

TEST(LoopPackWork, StraightLineMatchesStaticInstrs)
{
   bool ok = false;
   const agx_shader_info info =
      compile_nested_loops(NULL, 0, AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S, &ok);
   ASSERT_TRUE(ok);
   EXPECT_FALSE(info.has_loops);
   EXPECT_EQ(info.stats.loops, 0u);
   EXPECT_GT(info.stats.instrs, 0u);
   EXPECT_EQ(info.pack_instrs, info.stats.instrs);
}

TEST(LoopPackWork, BoundedLoopScalesPackInstrsAndClearsHasLoops)
{
   const struct {
      agx_isa_encoding encoding;
      agx_chip chip;
   } targets[] = {
      {AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S},
      {AGX_ISA_ENCODING_AGX2, AGX_CHIP_G13G},
   };

   for (auto t : targets) {
      const loop_spec loop100[] = {{100}};
      const loop_spec loop200[] = {{200}};
      bool ok100 = false, ok200 = false;
      const agx_shader_info info100 =
         compile_nested_loops(loop100, 1, t.encoding, t.chip, &ok100);
      const agx_shader_info info200 =
         compile_nested_loops(loop200, 1, t.encoding, t.chip, &ok200);
      ASSERT_TRUE(ok100);
      ASSERT_TRUE(ok200);

      /* Exceeds max_unroll_iterations (64 on M5, 32 on AGX2), so the loop stays
       * a real hardware loop whose trip count NIR loop analysis bounds. */
      EXPECT_EQ(info100.stats.loops, 1u);
      EXPECT_EQ(info200.stats.loops, 1u);
      EXPECT_FALSE(info100.has_loops);
      EXPECT_FALSE(info200.has_loops);
      EXPECT_GT(info100.pack_instrs, info100.stats.instrs * 10ull);
      EXPECT_GT(info200.pack_instrs, info100.pack_instrs);
      /* Doubling the trip count nearly doubles the dynamic instruction bound. */
      EXPECT_GT(info200.pack_instrs * 10ull, info100.pack_instrs * 18ull);
      EXPECT_LT(info200.pack_instrs * 10ull, info100.pack_instrs * 22ull);
   }
}

TEST(LoopPackWork, UnboundedLoopSetsHasLoops)
{
   const loop_spec unbounded[] = {{0}};
   bool ok = false;
   const agx_shader_info info = compile_nested_loops(
      unbounded, 1, AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S, &ok);
   ASSERT_TRUE(ok);
   EXPECT_EQ(info.stats.loops, 1u);
   EXPECT_TRUE(info.has_loops);
}

TEST(LoopPackWork, NestedBoundedLoopsMultiplyAndUnboundedInnerMarksHasLoops)
{
   const loop_spec single[] = {{100}};
   const loop_spec nested[] = {{100}, {80}};
   const loop_spec mixed[] = {{100}, {0}};
   bool ok_single = false, ok_nested = false, ok_mixed = false;

   const agx_shader_info info_single = compile_nested_loops(
      single, 1, AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S, &ok_single);
   const agx_shader_info info_nested = compile_nested_loops(
      nested, 2, AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S, &ok_nested);
   const agx_shader_info info_mixed = compile_nested_loops(
      mixed, 2, AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S, &ok_mixed);

   ASSERT_TRUE(ok_single);
   ASSERT_TRUE(ok_nested);
   ASSERT_TRUE(ok_mixed);

   EXPECT_EQ(info_nested.stats.loops, 2u);
   EXPECT_FALSE(info_nested.has_loops);
   EXPECT_GT(info_nested.pack_instrs, info_single.pack_instrs * 50ull);

   EXPECT_EQ(info_mixed.stats.loops, 2u);
   EXPECT_TRUE(info_mixed.has_loops);
}

TEST(LoopPackWork, DeepNestedBoundedLoopsSaturateAtUint64Max)
{
   /* 10^7 * 10^7 * 10^7 = 10^21 > UINT64_MAX: multiplication must saturate
    * cleanly instead of wrapping around. */
   const loop_spec huge[] = {{10000000u}, {10000000u}, {10000000u}};
   bool ok = false;
   const agx_shader_info info = compile_nested_loops(
      huge, 3, AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S, &ok);
   ASSERT_TRUE(ok);
   EXPECT_EQ(info.stats.loops, 3u);
   EXPECT_FALSE(info.has_loops);
   EXPECT_EQ(info.pack_instrs, UINT64_MAX);
}

} // namespace
