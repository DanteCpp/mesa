/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include "agx_test.h"

/* agx_opt_loop_invariant_copies: immediate moves and uniform copies leave
 * loops for the preheader, identical ones are merged, and nothing moves past
 * the register headroom the loop already has. */

namespace {

struct Loop {
   void *mem;
   agx_builder *b;
   agx_shader_key key;
   agx_block *entry, *header, *exit;

   /* entry: `pressure` values live together, then summed;
    * header (its own latch): body; exit: store the body's value. */
   Loop(unsigned pressure)
   {
      mem = ralloc_context(NULL);
      b = agx_test_builder(mem);
      key = {};
      key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
      b->shader->key = &key;
      b->shader->stage = MESA_SHADER_COMPUTE;

      entry = agx_start_block(b->shader);
      header = agx_test_block(b->shader);
      exit = agx_test_block(b->shader);
      header->loop_header = true;
      agx_block_add_successor(entry, header);
      agx_block_add_successor(header, header);
      agx_block_add_successor(header, exit);

      agx_index acc = sr();
      std::vector<agx_index> live;
      for (unsigned i = 0; i < pressure; ++i)
         live.push_back(sr());
      for (agx_index v : live)
         acc = agx_fadd(b, acc, v);
      seed = acc;
      b->cursor = agx_after_block(header);
   }

   ~Loop() { ralloc_free(mem); }

   agx_index sr()
   {
      agx_index t = agx_temp(b->shader, AGX_SIZE_32);
      agx_get_sr_to(b, t, AGX_SR_THREADGROUP_POSITION_IN_GRID_X);
      return t;
   }

   void finish(agx_index value)
   {
      b->cursor = agx_after_block(exit);
      agx_device_store(b, value, agx_uniform(0, AGX_SIZE_64), agx_immediate(0),
                       AGX_FORMAT_I32, 1, 0, false);
      agx_stop(b);
   }

   unsigned count(agx_block *block, enum agx_opcode op)
   {
      unsigned n = 0;
      agx_foreach_instr_in_block(block, I)
         n += I->op == op;
      return n;
   }

   agx_index seed;
};

/* Two copies of 1.0 in the loop, with headroom in the loop: one moves to
 * the preheader, the other is merged into it, and both adds read it. */
TEST(LoopInvariantCopies, HoistsAndMergesImmediates)
{
   Loop l(6);
   agx_index x = l.sr();
   agx_index one = agx_mov_imm(l.b, 32, 0x3f800000);
   agx_index y = agx_fadd(l.b, x, one);
   agx_index one_again = agx_mov_imm(l.b, 32, 0x3f800000);
   agx_index z = agx_fadd(l.b, y, one_again);
   l.finish(z);

   EXPECT_TRUE(agx_opt_loop_invariant_copies(l.b->shader));
   EXPECT_EQ(l.count(l.header, AGX_OPCODE_MOV_IMM), 0u);
   EXPECT_EQ(l.count(l.entry, AGX_OPCODE_MOV_IMM), 1u);

   unsigned reads = 0;
   agx_foreach_instr_in_block(l.header, I) {
      if (I->op == AGX_OPCODE_FADD) {
         EXPECT_EQ(I->src[1].value, one.value);
         reads++;
      }
   }
   EXPECT_EQ(reads, 2u);
}

/* The loop holds the program's peak pressure: hoisting would raise it. */
TEST(LoopInvariantCopies, KeepsCopiesWithoutHeadroom)
{
   Loop l(0);
   agx_index x = l.sr();
   agx_index w = l.sr();
   agx_index one = agx_mov_imm(l.b, 32, 0x3f800000);
   agx_index y = agx_fadd(l.b, x, one);
   l.finish(agx_fadd(l.b, y, w));

   EXPECT_FALSE(agx_opt_loop_invariant_copies(l.b->shader));
   EXPECT_EQ(l.count(l.header, AGX_OPCODE_MOV_IMM), 1u);
}

/* A uniform copy leaves the loop while nothing writes the uniform file,
 * and stays when the program writes uniforms. */
TEST(LoopInvariantCopies, HoistsUniformCopiesOnlyWithoutUniformWrites)
{
   for (bool writes : {false, true}) {
      Loop l(6);
      if (writes) {
         agx_builder eb = agx_init_builder(l.b->shader, agx_after_block(l.entry));
         agx_uniform_store(&eb, l.seed, agx_immediate(8), 1);
      }
      agx_index x = l.sr();
      agx_index u = agx_temp(l.b->shader, AGX_SIZE_32);
      agx_mov_to(l.b, u, agx_uniform(4, AGX_SIZE_32));
      l.finish(agx_fadd(l.b, x, u));

      EXPECT_EQ(agx_opt_loop_invariant_copies(l.b->shader), !writes);
      EXPECT_EQ(l.count(l.header, AGX_OPCODE_MOV), writes ? 1u : 0u);
   }
}

/* Different immediates are different values: nothing is merged. */
TEST(LoopInvariantCopies, KeepsDistinctImmediatesApart)
{
   Loop l(8);
   agx_index x = l.sr();
   agx_index one = agx_mov_imm(l.b, 32, 0x3f800000);
   agx_index two = agx_mov_imm(l.b, 32, 0x40000000);
   agx_index y = agx_fadd(l.b, x, one);
   l.finish(agx_fadd(l.b, y, two));

   EXPECT_TRUE(agx_opt_loop_invariant_copies(l.b->shader));
   EXPECT_EQ(l.count(l.entry, AGX_OPCODE_MOV_IMM), 2u);
   EXPECT_EQ(l.count(l.header, AGX_OPCODE_MOV_IMM), 0u);
}

} // namespace
