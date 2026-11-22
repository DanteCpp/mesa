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

/* The headroom rule at its edges: AGX3C's 64-half top occupancy step caps at
 * 61 halves, above 64 at least 16 halves free below the top of the step
 * enables the windows, the step lookup includes its top, and the last step
 * (256 halves) keeps the issue-order schedule. */
TEST(M5CopySchedule, DeviceLoadCapBoundaries)
{
   EXPECT_EQ(agx_ilp_m5_device_load_cap(0), 61);
   EXPECT_EQ(agx_ilp_m5_device_load_cap(61), 61); /* AGX3C 64-half top step */
   EXPECT_EQ(agx_ilp_m5_device_load_cap(62), 0);
   EXPECT_EQ(agx_ilp_m5_device_load_cap(64), 0);
   EXPECT_EQ(agx_ilp_m5_device_load_cap(65), 96); /* 16 free below 104 */
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

/* Within each occupancy step (the 64-half top step and the 104-half step),
 * adding live values reduces the in-flight window monotonically (4 -> 2 -> 1),
 * and every window is used. */
TEST(M5CopySchedule, ComputeDeviceLoadWindowsFallBack)
{
   const struct {
      unsigned lo, hi;
   } steps[] = {
      {0, 25},   /* 64-half occupancy step (peak <= 64) */
      {26, 50},  /* 104-half occupancy step (peak > 64) */
   };
   for (const auto &step : steps) {
      unsigned last = ~0u;
      bool seen[5] = {false};
      for (unsigned live = step.lo; live <= step.hi; ++live) {
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
}

/* The program's peak decides, whichever block sets it: a light load block
 * after a block holding many values gets load_cap == 0 and keeps the
 * issue-order schedule instead of falling through to unbounded device-load
 * hoisting, while the same load block alone takes the in-flight windows. */
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
         EXPECT_EQ(in_flight, 1u);
      else
         EXPECT_LE(in_flight, 4u);
      ralloc_free(s.mem);
   }
}

/* On AGX3C (non-G18G, non-preamble), register allocation caps max_regs at 64
 * halves whenever aligned demand fits in 64 halves so unrelated short-lived
 * allocations do not cross the 64-half occupancy cliff, while G18G and AGX3B
 * keep the 104-half step. */
TEST(M5CopySchedule, Agx3cRegisterAllocatorRespects64HalfOccupancyCap)
{
   struct variant {
      const char *name;
      enum agx_isa_encoding enc;
      enum agx_chip chip;
      bool emit_agx3d;
      bool expect_capped_64;
   };

   const variant cases[] = {
      {"agx3c", AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17S, false, true},
      {"g18g", AGX_ISA_ENCODING_AGX3C, AGX_CHIP_G17G, true, false},
      {"agx3b", AGX_ISA_ENCODING_AGX3B, AGX_CHIP_G17P, false, false},
   };

   for (const variant &c : cases) {
      SCOPED_TRACE(c.name);
      void *mem = ralloc_context(NULL);
      agx_shader_key key = {};
      key.dev.encoding = c.enc;
      key.dev.chip = c.chip;
      key.dev.emit_agx3d = c.emit_agx3d;

      agx_builder *b = agx_test_builder(mem);
      agx_shader_info info = {};
      b->shader->key = &key;
      b->shader->out = &info;
      b->shader->stage = MESA_SHADER_COMPUTE;
      b->shader->nir = rzalloc(mem, nir_shader);
      b->shader->nir->info.stage = MESA_SHADER_COMPUTE;
      b->shader->nir->info.workgroup_size[0] = 32;
      b->shader->nir->info.workgroup_size[1] = 1;
      b->shader->nir->info.workgroup_size[2] = 1;

      /* Preload 30 32-bit values filling halves 0..59, consume four non-adjacent
       * ones so peak demand stays <= 64 halves (52 live + 8 for the vec4 = 60)
       * with no contiguous 8-half hole below half 60, then allocate a vec4.
       * With max_regs = 104 (G18G/AGX3B), RA places the vec4 at half 64 without
       * shuffling; with max_regs = 64 (AGX3C), RA repacks within halves 0..63. */
      agx_index live[30];
      for (unsigned i = 0; i < ARRAY_SIZE(live); ++i)
         live[i] = agx_preload(b, agx_register(2 * i, AGX_SIZE_32));
      agx_index acc = agx_iadd(b, live[1], live[5], 0);
      acc = agx_iadd(b, acc, live[11], 0);
      acc = agx_iadd(b, acc, live[17], 0);
      agx_index v4 = agx_vec_temp(b->shader, AGX_SIZE_32, 4);
      agx_device_load_to(b, v4, agx_uniform(0, AGX_SIZE_64), agx_immediate(0),
                         AGX_FORMAT_I32, BITFIELD_MASK(4), 0, false);
      agx_unit_test(b, v4);
      for (unsigned i = 0; i < ARRAY_SIZE(live); ++i) {
         if (i != 1 && i != 5 && i != 11 && i != 17)
            acc = agx_iadd(b, acc, live[i], 0);
      }
      agx_unit_test(b, acc);

      agx_ra(b->shader);
      ASSERT_FALSE(b->shader->ra_failed);
      if (c.expect_capped_64)
         EXPECT_LE(b->shader->max_reg, 64u);
      else
         EXPECT_GT(b->shader->max_reg, 64u);
      ralloc_free(mem);
   }
}
