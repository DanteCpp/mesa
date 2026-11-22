/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_test.h"

#include <gtest/gtest.h>
#include <vector>

/* M5 copy loops: reorderable image reads issue ahead of the writes that
 * consume them, as Apple's compiler does for a vertex copy loop (four SMP,
 * then four IMGWCI). */

namespace {

struct copy_shader {
   void *mem;
   agx_shader_key key;
   agx_builder *b;
   agx_block *body;
};

static void
copy_shader_init(copy_shader *s, mesa_shader_stage stage, bool loop)
{
   s->mem = ralloc_context(NULL);
   s->key = {};
   s->key.dev.encoding = AGX_ISA_ENCODING_AGX3C;
   s->key.dev.chip = AGX_CHIP_G17S;
   s->b = agx_test_builder(s->mem);
   s->b->shader->key = &s->key;
   s->b->shader->stage = stage;
   s->b->shader->nir = rzalloc(s->mem, nir_shader);
   s->b->shader->nir->info.stage = stage;
   s->body = agx_start_block(s->b->shader);
   if (loop) {
      agx_block *entry = s->body;
      s->body = agx_test_block(s->b->shader);
      agx_block *exit = agx_test_block(s->b->shader);
      s->body->loop_header = true;
      agx_block_add_successor(entry, s->body);
      agx_block_add_successor(s->body, s->body);
      agx_block_add_successor(s->body, exit);
   }
}

/* Copy n texels at constant x coordinates defined in the entry block. */
static void
emit_copies(copy_shader *s, unsigned n, bool reorderable)
{
   agx_builder *b = s->b;
   agx_index y = agx_preload(b, agx_register(2, AGX_SIZE_32));
   agx_index z = agx_preload(b, agx_register(4, AGX_SIZE_32));
   agx_index x[16];
   assert(n <= 16);
   for (unsigned i = 0; i < n; ++i)
      x[i] = agx_mov_imm(b, 32, i);

   b->cursor = agx_after_block(s->body);
   for (unsigned i = 0; i < n; ++i) {
      agx_index coords = agx_vec_temp(b->shader, AGX_SIZE_32, 3);
      agx_instr *collect = agx_collect_to(b, coords, 3);
      collect->src[0] = x[i];
      collect->src[1] = y;
      collect->src[2] = z;

      agx_index texel = agx_vec_temp(b->shader, AGX_SIZE_32, 4);
      agx_instr *load = agx_image_load_to(
         b, texel, coords, agx_zero(), agx_zero(), agx_immediate(1),
         agx_immediate(0), agx_null(), AGX_DIM_3D, AGX_LOD_MODE_AUTO_LOD, 0,
         false, false, false);
      load->mask = 1;
      load->memory_can_reorder = reorderable;
      agx_image_write(b, texel, coords, agx_zero(),
                      agx_uniform(8, AGX_SIZE_64),
                      agx_register(28, AGX_SIZE_32), AGX_DIM_3D, false);
   }
}

/* Largest number of reads issued ahead of the first unissued write. */
static unsigned
max_reads_in_flight(agx_block *block, unsigned *first_write_at)
{
   unsigned loads = 0, writes = 0, max = 0, pos = 0;
   *first_write_at = ~0u;
   agx_foreach_instr_in_block(block, I) {
      if (I->op == AGX_OPCODE_IMAGE_LOAD) {
         ++loads;
         max = MAX2(max, loads - writes);
      } else if (I->op == AGX_OPCODE_IMAGE_WRITE) {
         if (writes == 0)
            *first_write_at = pos;
         ++writes;
      }
      ++pos;
   }
   return max;
}

static unsigned
loads_before_first_write(agx_block *block)
{
   unsigned loads = 0;
   agx_foreach_instr_in_block(block, I) {
      if (I->op == AGX_OPCODE_IMAGE_WRITE)
         break;
      loads += I->op == AGX_OPCODE_IMAGE_LOAD;
   }
   return loads;
}

/* n device loads, each added into a running sum right after it. */
static void
emit_load_uses(copy_shader *s, unsigned n)
{
   agx_builder *b = s->b;
   b->cursor = agx_after_block(s->body);
   agx_index address = agx_preload(b, agx_register(2, AGX_SIZE_64));
   agx_index sum = agx_preload(b, agx_register(6, AGX_SIZE_32));
   for (unsigned i = 0; i < n; ++i) {
      agx_index value = agx_temp(b->shader, AGX_SIZE_32);
      agx_device_load_to(b, value, address, agx_immediate(i * 4),
                         AGX_FORMAT_I32, 1, 0, false);
      sum = agx_iadd(b, sum, value, 0);
   }
   agx_unit_test(b, sum);
}

static unsigned
loads_before_first_add(agx_block *block)
{
   unsigned loads = 0;
   agx_foreach_instr_in_block(block, I) {
      if (I->op == AGX_OPCODE_IADD)
         break;
      loads += I->op == AGX_OPCODE_DEVICE_LOAD;
   }
   return loads;
}

} /* namespace */

TEST(M5CopySchedule, VertexReadsIssueAheadOfWrites)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_VERTEX, false);
   emit_copies(&s, 8, true);
   agx_ilp_schedule(s.b->shader);

   unsigned first_write;
   EXPECT_EQ(loads_before_first_write(s.body), 4u);
   EXPECT_EQ(max_reads_in_flight(s.body, &first_write), 4u);
   ralloc_free(s.mem);
}

TEST(M5CopySchedule, OrderedReadsStayBehindWrites)
{
   /* A read that may observe an earlier write keeps memory order. */
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_VERTEX, false);
   emit_copies(&s, 8, false);
   agx_ilp_schedule(s.b->shader);

   unsigned first_write;
   EXPECT_EQ(loads_before_first_write(s.body), 1u);
   EXPECT_EQ(max_reads_in_flight(s.body, &first_write), 1u);
   ralloc_free(s.mem);
}

TEST(M5CopySchedule, LoopConstantsRematerializedInBody)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_VERTEX, true);
   emit_copies(&s, 8, true);
   agx_ilp_schedule(s.b->shader);

   /* Every coordinate constant is now defined in the loop body. */
   unsigned body_constants = 0;
   agx_foreach_instr_in_block(s.body, I)
      body_constants += I->op == AGX_OPCODE_MOV_IMM;
   EXPECT_EQ(body_constants, 8u);
   agx_foreach_instr_in_block(agx_start_block(s.b->shader), I)
      EXPECT_NE(I->op, AGX_OPCODE_MOV_IMM);

   unsigned first_write;
   EXPECT_EQ(loads_before_first_write(s.body), 4u);
   EXPECT_EQ(max_reads_in_flight(s.body, &first_write), 4u);
   ralloc_free(s.mem);
}

TEST(M5CopySchedule, OtherGenerationsUnchanged)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_VERTEX, false);
   s.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   s.key.dev.chip = AGX_CHIP_G17P;
   emit_copies(&s, 8, true);
   agx_ilp_schedule(s.b->shader);

   unsigned first_write;
   EXPECT_EQ(loads_before_first_write(s.body), 1u);
   EXPECT_EQ(max_reads_in_flight(s.body, &first_write), 1u);
   ralloc_free(s.mem);
}

/* M5 compute device loads issue ahead of their uses, as samples do. */
TEST(M5CopySchedule, ComputeDeviceLoadsIssueAheadOfUses)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
   emit_load_uses(&s, 6);
   agx_ilp_schedule(s.b->shader);
   EXPECT_GT(loads_before_first_add(s.body), 1u);
   ralloc_free(s.mem);
}

/* G18G compiles as AGX3C with emit_agx3d and keeps the pressure order. */
TEST(M5CopySchedule, G18GComputeDeviceLoadsKeepTheirOrder)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
   s.key.dev.chip = AGX_CHIP_G17G;
   s.key.dev.emit_agx3d = true;
   emit_load_uses(&s, 6);
   agx_ilp_schedule(s.b->shader);
   EXPECT_EQ(loads_before_first_add(s.body), 1u);
   ralloc_free(s.mem);
}

namespace {

/* n device loads of `channels` words, each consumed right after it, with
 * `live` 32-bit values defined before them and read after the last. */
static void
emit_wide_load_uses(copy_shader *s, unsigned n, unsigned channels,
                    unsigned live)
{
   agx_builder *b = s->b;
   b->cursor = agx_after_block(s->body);
   agx_index address = agx_preload(b, agx_register(2, AGX_SIZE_64));
   agx_index held[64];
   assert(live <= 64);
   for (unsigned i = 0; i < live; ++i)
      held[i] = agx_mov_imm(b, 32, 1000 + i);
   agx_index sum = agx_mov_imm(b, 32, 0);
   for (unsigned i = 0; i < n; ++i) {
      agx_index value = agx_vec_temp(b->shader, AGX_SIZE_32, channels);
      agx_device_load_to(b, value, address, agx_immediate(i * 4 * channels),
                         AGX_FORMAT_I32, BITFIELD_MASK(channels), 0, false);
      agx_index first = agx_temp(b->shader, AGX_SIZE_32);
      agx_instr *split = agx_split(b, channels, value);
      split->dest[0] = first;
      for (unsigned c = 1; c < channels; ++c)
         split->dest[c] = agx_temp(b->shader, AGX_SIZE_32);
      sum = agx_iadd(b, sum, first, 0);
      for (unsigned c = 1; c < channels; ++c)
         sum = agx_iadd(b, sum, split->dest[c], 0);
   }
   for (unsigned i = 0; i < live; ++i)
      sum = agx_iadd(b, sum, held[i], 0);
   agx_unit_test(b, sum);
}

/* Largest number of device loads issued whose results nothing has read yet */
static unsigned
max_device_loads_in_flight(agx_block *block)
{
   unsigned issued = 0, read = 0, max = 0;
   std::vector<unsigned> pending;
   agx_foreach_instr_in_block(block, I) {
      agx_foreach_ssa_src(I, s) {
         for (unsigned p = 0; p < pending.size(); ++p) {
            if (pending[p] == I->src[s].value) {
               pending.erase(pending.begin() + p);
               ++read;
               break;
            }
         }
      }
      if (I->op == AGX_OPCODE_DEVICE_LOAD) {
         pending.push_back(I->dest[0].value);
         ++issued;
         max = MAX2(max, issued - read);
      }
   }
   return max;
}

} /* namespace */

/* Compute device loads issue ahead of their uses a few at a time (four in
 * flight), not all of a block's loads before the first use. */
TEST(M5CopySchedule, ComputeDeviceLoadsStayBoundedInFlight)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
   emit_wide_load_uses(&s, 16, 1, 0);
   agx_ilp_schedule(s.b->shader);
   const unsigned in_flight = max_device_loads_in_flight(s.body);
   EXPECT_GE(in_flight, 2u);
   EXPECT_LE(in_flight, 4u);
   ralloc_free(s.mem);
}

/* Near the top of an occupancy step there is no room for a vec4 load's
 * result ahead of its use: the loads keep their order. */
TEST(M5CopySchedule, ComputeDeviceLoadsStayInsideTheOccupancyStep)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
   emit_wide_load_uses(&s, 8, 4, 46);
   agx_ilp_schedule(s.b->shader);
   EXPECT_EQ(max_device_loads_in_flight(s.body), 1u);
   ralloc_free(s.mem);
}

/* The headroom rule at its edges: at least 16 halves free below the top of
 * the step enables the windows, the step lookup includes its top, and the
 * last step (256 halves) keeps the issue-order schedule. */
TEST(M5CopySchedule, DeviceLoadCapBoundaries)
{
   EXPECT_EQ(agx_ilp_m5_device_load_cap(0), 96);
   EXPECT_EQ(agx_ilp_m5_device_load_cap(88), 96); /* 16 free below 104 */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(89), 0);  /* 15 free */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(104), 0); /* the top of its step */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(112), 0); /* 112 is in the 112 step */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(113), 0); /* 15 free below 128 */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(216), 224); /* 16 free below 232 */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(217), 0);
   EXPECT_EQ(agx_ilp_m5_device_load_cap(233), 0); /* the last step */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(240), 0);
   EXPECT_EQ(agx_ilp_m5_device_load_cap(256), 0);
}

/* With more values held across vec4 loads, four in flight stop fitting and
 * the schedule takes two, then one: the count only falls, and every window
 * is used. */
TEST(M5CopySchedule, ComputeDeviceLoadWindowsFallBack)
{
   unsigned last = ~0u;
   bool seen[5] = {false};
   for (unsigned live = 0; live <= 50; ++live) {
      SCOPED_TRACE(live);
      copy_shader s;
      copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
      emit_wide_load_uses(&s, 8, 4, live);
      agx_ilp_schedule(s.b->shader);
      const unsigned in_flight = max_device_loads_in_flight(s.body);
      EXPECT_TRUE(in_flight == 4 || in_flight == 2 || in_flight == 1);
      EXPECT_LE(in_flight, last);
      if (in_flight <= 4)
         seen[in_flight] = true;
      last = in_flight;
      ralloc_free(s.mem);
   }
   EXPECT_TRUE(seen[4]);
   EXPECT_TRUE(seen[2]);
   EXPECT_TRUE(seen[1]);
}

/* The program's peak decides, whichever block sets it: a light load block
 * after a block holding many values keeps the previous schedule, which hoists
 * all of its loads ahead of their uses, while the same load block alone takes
 * the in-flight windows. */
TEST(M5CopySchedule, ComputeDeviceLoadsFollowTheProgramPeak)
{
   for (bool heavy : {false, true}) {
      SCOPED_TRACE(heavy);
      copy_shader s;
      copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
      agx_builder *b = s.b;
      if (heavy) {
         agx_index held[46];
         for (unsigned i = 0; i < 46; ++i)
            held[i] = agx_mov_imm(b, 32, 1000 + i);
         agx_index sum = agx_mov_imm(b, 32, 0);
         for (unsigned i = 0; i < 46; ++i)
            sum = agx_iadd(b, sum, held[i], 0);
         agx_unit_test(b, sum);
      }
      agx_block *loads = agx_test_block(b->shader);
      agx_block_add_successor(s.body, loads);
      s.body = loads;
      emit_wide_load_uses(&s, 16, 1, 0);
      agx_ilp_schedule(b->shader);
      const unsigned in_flight = max_device_loads_in_flight(loads);
      if (heavy)
         EXPECT_EQ(in_flight, 16u);
      else
         EXPECT_LE(in_flight, 4u);
      ralloc_free(s.mem);
   }
}

/* Multi-channel loads unpacked by SPLIT remain tracked as in-flight loads
 * until a real consumer reads a split lane, and independent ALU chains
 * interleave rather than serializing one chain after another. */
TEST(M5CopySchedule, SplitLoadChainsInterleaveIndependentAlu)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
   agx_builder *b = s.b;
   b->cursor = agx_after_block(s.body);
   agx_index address = agx_preload(b, agx_register(2, AGX_SIZE_64));
   agx_index w = agx_preload(b, agx_register(6, AGX_SIZE_32));
   agx_index acc[4];
   for (unsigned c = 0; c < 4; ++c)
      acc[c] = agx_mov_imm(b, 32, 0);

   for (unsigned i = 0; i < 6; ++i) {
      agx_index vec = agx_vec_temp(b->shader, AGX_SIZE_32, 4);
      agx_device_load_to(b, vec, address, agx_immediate(i * 16),
                         AGX_FORMAT_I32, 0xf, 0, false);
      agx_instr *split = agx_split(b, 4, vec);
      for (unsigned c = 0; c < 4; ++c)
         split->dest[c] = agx_temp(b->shader, AGX_SIZE_32);
      for (unsigned c = 0; c < 4; ++c) {
         agx_index scaled = agx_fmul(b, split->dest[c], w);
         acc[c] = agx_ffma(b, scaled, w, acc[c]);
      }
   }
   agx_index total = agx_fadd(b, agx_fadd(b, acc[0], acc[1]),
                              agx_fadd(b, acc[2], acc[3]));
   agx_unit_test(b, total);

   agx_ilp_schedule(b->shader);
   const unsigned in_flight = max_device_loads_in_flight(s.body);
   EXPECT_GE(in_flight, 2u);
   EXPECT_LE(in_flight, 4u);

   /* Verify that the first two FMULs issue before the first FFMA (interleaving
    * independent chains instead of completing lane 0's FMUL->FFMA before lane 1). */
   unsigned fmuls_before_first_ffma = 0;
   agx_foreach_instr_in_block(s.body, I) {
      if (I->op == AGX_OPCODE_FFMA)
         break;
      fmuls_before_first_ffma += I->op == AGX_OPCODE_FMUL;
   }
   EXPECT_GE(fmuls_before_first_ffma, 2u);
   ralloc_free(s.mem);
}

TEST(M5CopySchedule, AGX3BDeviceLoadCapAndHoisting)
{
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(0, AGX3B_NUM_REGS), 64);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(34, AGX3B_NUM_REGS), 64);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(48, AGX3B_NUM_REGS), 64);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(100, AGX3B_NUM_REGS), 116);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(144, AGX3B_NUM_REGS), 160);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(160, AGX3B_NUM_REGS), 168);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(168, AGX3B_NUM_REGS), 168);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(169, AGX3B_NUM_REGS), 0);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(192, AGX3B_NUM_REGS), 0);

   /* Cube-array / bias-min budget (112 halves): headroom stops growth at 88. */
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(72, 112), 88);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(88, 112), 88);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(89, 112), 0);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(100, 112), 0);

   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
   s.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   s.key.dev.chip = AGX_CHIP_G17P;
   emit_wide_load_uses(&s, 8, 4, 0);
   agx_ilp_schedule(s.b->shader);
   EXPECT_EQ(max_device_loads_in_flight(s.body), 4u);
   ralloc_free(s.mem);

   /* A cube-array bias-min sample lowers RA's register budget to 112 halves;
    * verify ILP hoisting respects that budget rather than AGX3B_NUM_REGS. */
   copy_shader cube;
   copy_shader_init(&cube, MESA_SHADER_COMPUTE, false);
   cube.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   cube.key.dev.chip = AGX_CHIP_G17P;
   cube.b->cursor = agx_after_block(cube.body);
   agx_index coords = agx_vec_temp(cube.b->shader, AGX_SIZE_32, 4);
   agx_collect_to(cube.b, coords, 4);
   agx_texture_sample_to(cube.b, agx_temp(cube.b->shader, AGX_SIZE_16), coords,
                         agx_immediate(0), agx_immediate(0), agx_immediate(0),
                         agx_immediate(0), agx_null(), AGX_DIM_CUBE_ARRAY,
                         AGX_LOD_MODE_AUTO_LOD_BIAS_MIN, 0xf, false, false,
                         false, AGX_GATHER_NONE, false);
   const unsigned cube_budget = agx3_cube_register_budget(cube.b->shader);
   EXPECT_EQ(cube_budget, 112u);
   EXPECT_EQ(agx_ilp_agx3b_device_load_cap(100, cube_budget), 0);
   agx_ilp_schedule(cube.b->shader);
   ralloc_free(cube.mem);
}

/* Contiguous narrow loads from the same (base, index) cache-line stream stay
 * capped at 2 in flight per stream, while distinct-index strided loads plus
 * vec4 loads hoist up to the 32-half register budget. */
TEST(M5CopySchedule, SameCacheLineStreamCapsScalarHoisting)
{
   /* Case 1: 16 byte-offset scalar loads across 2 base+index streams (+0B..+7B)
    * should keep at most 4 loads in flight (2 per stream). */
   {
      copy_shader s;
      copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
      s.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
      s.key.dev.chip = AGX_CHIP_G17P;
      agx_builder *b = s.b;
      b->cursor = agx_after_block(s.body);
      agx_index base0 = agx_preload(b, agx_register(2, AGX_SIZE_64));
      agx_index base1 = agx_preload(b, agx_register(6, AGX_SIZE_64));
      agx_index idx0 = agx_preload(b, agx_register(10, AGX_SIZE_32));
      agx_index idx1 = agx_preload(b, agx_register(12, AGX_SIZE_32));
      agx_index acc = agx_mov_imm(b, 32, 0);
      for (unsigned i = 0; i < 8; ++i) {
         agx_index v0 = agx_temp(b->shader, AGX_SIZE_32);
         agx_instr *l0 = agx_device_load_to(b, v0, base0, idx0, AGX_FORMAT_I8,
                                            0x1, 0, false);
         l0->memory_byte_offset = i;
         agx_index v1 = agx_temp(b->shader, AGX_SIZE_32);
         agx_instr *l1 = agx_device_load_to(b, v1, base1, idx1, AGX_FORMAT_I8,
                                            0x1, 0, false);
         l1->memory_byte_offset = i;
         acc = agx_iadd(b, acc, agx_iadd(b, v0, v1, 0), 0);
      }
      agx_unit_test(b, acc);
      agx_ilp_schedule(b->shader);
      EXPECT_EQ(max_device_loads_in_flight(s.body), 4u);
      ralloc_free(s.mem);
   }

   /* Case 2: 8 distinct-index scalar loads + 2 vec4 loads (32 halves total)
    * should hoist all 10 loads ahead of their first use. */
   {
      copy_shader s;
      copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
      s.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
      s.key.dev.chip = AGX_CHIP_G17P;
      agx_builder *b = s.b;
      b->cursor = agx_after_block(s.body);
      agx_index base_px = agx_preload(b, agx_register(2, AGX_SIZE_64));
      agx_index base_w = agx_preload(b, agx_register(6, AGX_SIZE_64));
      agx_index idx_px = agx_preload(b, agx_register(10, AGX_SIZE_32));
      agx_index idx_w = agx_preload(b, agx_register(12, AGX_SIZE_32));
      agx_index acc = agx_mov_imm(b, 32, 0);
      agx_index w0 = agx_vec_temp(b->shader, AGX_SIZE_32, 4);
      agx_device_load_to(b, w0, base_w, idx_w, AGX_FORMAT_I32, 0xf, 0, false);
      agx_instr *sw0 = agx_split(b, 4, w0);
      for (unsigned c = 0; c < 4; ++c)
         sw0->dest[c] = agx_temp(b->shader, AGX_SIZE_32);
      for (unsigned i = 0; i < 4; ++i) {
         agx_index row = agx_iadd(b, idx_px, agx_mov_imm(b, 32, i * 1024), 0);
         agx_index px = agx_temp(b->shader, AGX_SIZE_32);
         agx_device_load_to(b, px, base_px, row, AGX_FORMAT_I32, 0x1, 0, false);
         acc = agx_iadd(b, acc, agx_iadd(b, px, sw0->dest[i], 0), 0);
      }
      agx_index w1 = agx_vec_temp(b->shader, AGX_SIZE_32, 4);
      agx_instr *lw1 = agx_device_load_to(b, w1, base_w, idx_w, AGX_FORMAT_I32,
                                          0xf, 0, false);
      lw1->memory_byte_offset = 16;
      agx_instr *sw1 = agx_split(b, 4, w1);
      for (unsigned c = 0; c < 4; ++c)
         sw1->dest[c] = agx_temp(b->shader, AGX_SIZE_32);
      for (unsigned i = 0; i < 4; ++i) {
         agx_index row =
            agx_iadd(b, idx_px, agx_mov_imm(b, 32, (i + 4) * 1024), 0);
         agx_index px = agx_temp(b->shader, AGX_SIZE_32);
         agx_device_load_to(b, px, base_px, row, AGX_FORMAT_I32, 0x1, 0, false);
         acc = agx_iadd(b, acc, agx_iadd(b, px, sw1->dest[i], 0), 0);
      }
      agx_unit_test(b, acc);
      agx_ilp_schedule(b->shader);
      EXPECT_EQ(max_device_loads_in_flight(s.body), 10u);
      ralloc_free(s.mem);
   }

   /* Case 3: With a cube-array bias-min sample lowering RA's budget to 112
    * halves (cap = 112 - 24 = 88), halves-budgeted device-load hoisting falls
    * back to stay within the reduced RA budget instead of hoisting all 10
    * distinct-stride loads. */
   {
      copy_shader s;
      copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
      s.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
      s.key.dev.chip = AGX_CHIP_G17P;
      agx_builder *b = s.b;
      b->cursor = agx_after_block(s.body);
      agx_index coords = agx_vec_temp(b->shader, AGX_SIZE_32, 4);
      agx_collect_to(b, coords, 4);
      agx_texture_sample_to(b, agx_temp(b->shader, AGX_SIZE_16), coords,
                            agx_immediate(0), agx_immediate(0),
                            agx_immediate(0), agx_immediate(0), agx_null(),
                            AGX_DIM_CUBE_ARRAY,
                            AGX_LOD_MODE_AUTO_LOD_BIAS_MIN, 0xf, false, false,
                            false, AGX_GATHER_NONE, false);
      EXPECT_EQ(agx3_cube_register_budget(b->shader), 112u);

      agx_index base = agx_preload(b, agx_register(2, AGX_SIZE_64));
      agx_index idx = agx_preload(b, agx_register(6, AGX_SIZE_32));
      agx_index held[38];
      for (unsigned i = 0; i < 38; ++i)
         held[i] = agx_mov_imm(b, 32, 2000 + i);
      agx_index acc = agx_mov_imm(b, 32, 0);
      for (unsigned i = 0; i < 8; ++i) {
         agx_index row = agx_iadd(b, idx, agx_mov_imm(b, 32, i * 1024), 0);
         agx_index px = agx_temp(b->shader, AGX_SIZE_32);
         agx_device_load_to(b, px, base, row, AGX_FORMAT_I32, 0x1, 0, false);
         acc = agx_iadd(b, acc, px, 0);
      }
      for (unsigned i = 0; i < 38; ++i)
         acc = agx_iadd(b, acc, held[i], 0);
      agx_unit_test(b, acc);
      agx_ilp_schedule(b->shader);
      /* 38 live 32-bit values + base/idx/acc leaves < 8 halves below the
       * 88-half cube-array cap (112 - 24 headroom), whereas the normal
       * 192-half budget would hoist all 8 distinct-stride loads. */
      EXPECT_LT(max_device_loads_in_flight(s.body), 8u);
      ralloc_free(s.mem);
   }
}

/* Address setup for later device loads must not interleave breadth-first ahead
 * of earlier loads: when ordered stores (such as local_store in tiled GEMM
 * staging loops) prevent later loads from hoisting past an earlier store,
 * breadth-first address interleaving inflates live address registers and delays
 * the first load without increasing loads in flight. */
TEST(M5CopySchedule, HoistedLoadSetupKeepsProgramOrder)
{
   copy_shader s;
   copy_shader_init(&s, MESA_SHADER_COMPUTE, false);
   s.key.dev.encoding = AGX_ISA_ENCODING_AGX3B;
   s.key.dev.chip = AGX_CHIP_G17P;
   agx_builder *b = s.b;
   b->cursor = agx_after_block(s.body);

   agx_index base = agx_preload(b, agx_register(2, AGX_SIZE_64));
   agx_index row = agx_preload(b, agx_register(6, AGX_SIZE_32));
   agx_index stride = agx_preload(b, agx_register(8, AGX_SIZE_32));
   agx_index col = agx_preload(b, agx_register(10, AGX_SIZE_32));
   agx_index lidx = agx_preload(b, agx_register(12, AGX_SIZE_16));

   agx_instr *first_load = NULL;
   agx_instr *later_addr = NULL;
   for (unsigned i = 0; i < 6; ++i) {
      agx_index r = agx_iadd(b, row, agx_mov_imm(b, 32, i), 0);
      agx_index idx = agx_temp(b->shader, AGX_SIZE_32);
      agx_instr *addr = agx_imad_to(b, idx, r, stride, col, 0);
      agx_index v = agx_temp(b->shader, AGX_SIZE_32);
      agx_instr *ld = agx_device_load_to(b, v, base, idx, AGX_FORMAT_I32, 0x1,
                                         0, false);
      agx_local_store(b, v, agx_immediate(i * 4), lidx, AGX_FORMAT_I32, 0x1);
      if (i == 0)
         first_load = ld;
      else if (i == 1)
         later_addr = addr;
   }

   agx_ilp_schedule(b->shader);

   unsigned first_load_pos = ~0u, later_addr_pos = ~0u, pos = 0;
   agx_foreach_instr_in_block(s.body, I) {
      if (I == first_load)
         first_load_pos = pos;
      if (I == later_addr)
         later_addr_pos = pos;
      ++pos;
   }
   ASSERT_NE(first_load_pos, ~0u);
   ASSERT_NE(later_addr_pos, ~0u);
   EXPECT_LT(first_load_pos, later_addr_pos);
   ralloc_free(s.mem);
}
