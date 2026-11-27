/* Copyright 2026 Aurora Silicon; SPDX-License-Identifier: MIT */
#include <gtest/gtest.h>
#include <vector>
#include "agx_test.h"
#include "compiler/nir/nir_builder.h"
extern "C" {
#include "agx_nir.h"
#include "asahi/isa/disasm.h"
#include "util/memstream.h"
}

static void
check_offset(unsigned bits, unsigned add, int base, uint32_t mask,
             bool store, bool expected)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "shared-byte-offset");
   nir_def *v = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .range = 4);
   v = nir_iand_imm(&b, v, mask);
   nir_def *sum = nir_iadd_imm(&b, v, add);
   nir_def *index = nir_u2u16(&b, nir_ushr_imm(&b, sum, util_logbase2(bits / 8)));
   nir_intrinsic_instr *intr;
   if (store) {
      intr = nir_store_shared(&b, nir_imm_intN_t(&b, 0, bits), index,
                              .base = base, .align_mul = bits / 8);
   } else {
      nir_def *value = nir_load_shared(&b, 1, bits, index, .base = base,
                                        .align_mul = bits / 8);
      intr = nir_def_as_intrinsic(value);
   }
   EXPECT_EQ(agx_nir_opt_shared_byte_offsets(b.shader), expected);
   EXPECT_EQ(nir_intrinsic_base(intr), base + (expected ? add : 0));
   EXPECT_EQ(nir_get_io_offset_src(intr)->ssa == index, !expected);
   nir_validate_shader(b.shader, "shared-byte-offset result");
   ralloc_free(b.shader);
}

TEST(SharedByteOffsets, Load32) { check_offset(32, 12, 0, 4095, false, true); }
TEST(SharedByteOffsets, Store32) { check_offset(32, 56, 0, 4095, true, true); }
TEST(SharedByteOffsets, Load16) { check_offset(16, 14, 0, 4095, false, true); }
TEST(SharedByteOffsets, ExistingBase) { check_offset(32, 12, 16, 4095, false, true); }
TEST(SharedByteOffsets, ByteOffsetTooLarge) { check_offset(32, 256, 0, 4095, false, false); }
TEST(SharedByteOffsets, BaseOverflow) { check_offset(32, 16, 240, 4095, false, false); }
TEST(SharedByteOffsets, Misaligned) { check_offset(32, 3, 0, 4095, false, false); }
TEST(SharedByteOffsets, AdditionWrap) { check_offset(32, 12, 0, UINT32_MAX, false, false); }
TEST(SharedByteOffsets, NarrowingWrap) { check_offset(32, 12, 0, 262143, false, false); }
TEST(SharedByteOffsets, ExactBoundary) { check_offset(32, 12, 0, 262131, false, true); }

static nir_builder
workgroup_shader(const char *name)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                                                  &agx_nir_options, "%s", name);
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;
   return b;
}

TEST(SharedByteOffsets, G13xDynamicByteOffset)
{
   const struct {
      unsigned bits, add, mask;
      bool fold;
   } cases[] = {
      {8, 16, 63, false}, {16, 16, 255, false}, {32, 64, 255, true},
      {32, 16, 65519, true}, {32, 16, 65535, false},
      {32, 4, 255, false}, {32, 12, 7, false}, {32, 65520, 15, true},
      {32, 65536, 7, false},
   };

   for (const auto &c : cases) {
      for (bool store : {false, true}) {
         SCOPED_TRACE(::testing::Message() << c.bits << " bits, offset " << c.add
                                           << ", mask " << c.mask << ", store " << store);
         nir_builder b = workgroup_shader("G13X shared byte offset");
         nir_def *v = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                             .range = 4);
         nir_def *variable = nir_iand_imm(&b, v, c.mask);
         nir_def *offset = nir_u2u16(&b, nir_iadd_imm(&b, variable, c.add));
         nir_intrinsic_instr *intr = store
            ? nir_store_shared(&b, nir_imm_intN_t(&b, 0, c.bits), offset,
                               .align_mul = 1)
            : nir_def_as_intrinsic(nir_load_shared(&b, 1, c.bits, offset,
                                                    .align_mul = 1));
         EXPECT_EQ(agx_nir_opt_g13x_shared_offsets(b.shader), c.fold);
         EXPECT_EQ(nir_intrinsic_base(intr), c.fold ? c.add : 0);
         if (c.fold) {
            nir_def *base = nir_get_io_offset_src(intr)->ssa;
            ASSERT_TRUE(nir_def_is_alu(base));
            EXPECT_EQ(nir_def_as_alu(base)->op, nir_op_u2u16);
            EXPECT_EQ(nir_def_as_alu(base)->src[0].src.ssa, variable);
         } else {
            EXPECT_EQ(nir_get_io_offset_src(intr)->ssa, offset);
         }
         nir_validate_shader(b.shader, "G13X shared byte offset");
         ralloc_free(b.shader);
      }
   }
}

TEST(SharedByteOffsets, G13xFixedByteOffset)
{
   for (unsigned bytes : {0u, 12u, 16u, 1024u, 65520u, 65532u}) {
      SCOPED_TRACE(bytes);
      nir_builder b = workgroup_shader("G13X fixed byte offset");
      nir_def *offset = nir_imm_intN_t(&b, bytes, 16);
      nir_def *value = nir_load_shared(&b, 4, 32, offset, .align_mul = 4);
      nir_intrinsic_instr *store =
         nir_store_shared(&b, value, offset, .align_mul = 4);
      bool fold = bytes > 0 && bytes % 16 == 0;
      EXPECT_EQ(agx_nir_opt_g13x_shared_offsets(b.shader), fold);
      for (auto *intr : {nir_def_as_intrinsic(value), store}) {
         EXPECT_EQ(nir_intrinsic_base(intr), fold ? bytes : 0);
         EXPECT_EQ(nir_src_as_uint(*nir_get_io_offset_src(intr)), fold ? 0 : bytes);
      }
      EXPECT_FALSE(agx_nir_opt_g13x_shared_offsets(b.shader));
      nir_validate_shader(b.shader, "G13X fixed byte offset");
      ralloc_free(b.shader);
   }
}

TEST(SharedByteOffsets, G13xEncodesI32IndexInSixteenByteUnits)
{
   for (auto chip : {AGX_CHIP_G13X, AGX_CHIP_G13G}) {
      SCOPED_TRACE(chip);
      nir_builder b = workgroup_shader("shared displacement encoding");
      b.shader->info.internal = false;
      b.shader->info.shared_size = 2048;
      nir_def *word = nir_load_shared(&b, 1, 32, nir_imm_int(&b, 1024),
                                      .align_mul = 4);
      nir_store_shared(&b, word, nir_imm_int(&b, 1040), .align_mul = 4);
      agx_preprocess_nir(b.shader);
      agx_shader_key key = {};
      key.dev.encoding = AGX_ISA_ENCODING_AGX2;
      key.dev.chip = chip;
      key.no_preamble = true;
      agx_shader_part part = {};
      ASSERT_TRUE(agx_compile_shader_nir(b.shader, &key, &part));

      char *assembly = nullptr;
      size_t len = 0;
      u_memstream stream;
      ASSERT_TRUE(u_memstream_open(&stream, &assembly, &len));
      EXPECT_FALSE(agx2_disassemble(static_cast<uint8_t *>(part.binary) +
                                       part.info.main_offset,
                                    part.info.main_size, stream.f));
      u_memstream_close(&stream);
      EXPECT_NE(strstr(assembly, "lload"), nullptr) << assembly;
      EXPECT_NE(strstr(assembly, "lstore"), nullptr) << assembly;
      EXPECT_EQ(strstr(assembly, ", 64, i32") != nullptr, chip == AGX_CHIP_G13X)
         << assembly;
      EXPECT_EQ(strstr(assembly, ", 65, i32") != nullptr, chip == AGX_CHIP_G13X)
         << assembly;
      free(assembly);
      free(part.binary);
      ralloc_free(b.shader);
   }
}

static unsigned
count_alu(nir_shader *shader, nir_op op)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            n += instr->type == nir_instr_type_alu &&
                 nir_instr_as_alu(instr)->op == op;
         }
      }
   }
   return n;
}

/* A byte offset that scales thread indices by multiples of the element size,
 * including through address multiplies, becomes an element index without a
 * shift per access. */
TEST(SharedByteOffsets, ExactElementIndex)
{
   nir_builder b = workgroup_shader("shared-exact-index");
   nir_def *id = nir_load_local_invocation_id(&b);
   nir_def *x = nir_ishl_imm(&b, nir_channel(&b, id, 0), 1);
   nir_def *y = nir_ishl_imm(&b, nir_channel(&b, id, 1), 1);
   nir_def *offset = nir_iadd(&b, nir_amul(&b, x, nir_imm_int(&b, 72)),
                              nir_amul(&b, y, nir_imm_int(&b, 4)));
   nir_store_shared(&b, nir_imm_int(&b, 0), offset, .align_mul = 4);
   EXPECT_TRUE(agx_nir_lower_shared_bitsize(b.shader, true));
   EXPECT_EQ(count_alu(b.shader, nir_op_ushr), 0u);
   nir_validate_shader(b.shader, "shared-exact-index result");
   ralloc_free(b.shader);
}

/* An odd scale still needs the shift. */
TEST(SharedByteOffsets, InexactElementIndex)
{
   nir_builder b = workgroup_shader("shared-inexact-index");
   nir_def *id = nir_load_local_invocation_id(&b);
   nir_def *offset = nir_amul(&b, nir_channel(&b, id, 0), nir_imm_int(&b, 6));
   nir_store_shared(&b, nir_imm_int(&b, 0), offset, .align_mul = 4);
   EXPECT_TRUE(agx_nir_lower_shared_bitsize(b.shader, true));
   EXPECT_EQ(count_alu(b.shader, nir_op_ushr), 1u);
   ralloc_free(b.shader);
}

static void
check_element_addend(bool bounded, unsigned addend, unsigned expected_base)
{
   nir_builder b = workgroup_shader("shared-element-addend");
   nir_def *id = nir_load_local_invocation_id(&b);
   nir_def *x = nir_channel(&b, id, 0);
   if (!bounded)
      x = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .range = 4);
   nir_def *row = nir_imul_imm(&b, x, 36);
   nir_def *col = nir_imadshl_agx(&b, nir_imm_int(&b, 0), nir_imm_int(&b, 1),
                                  nir_channel(&b, id, 1), nir_imm_int(&b, 1));
   nir_def *index =
      nir_u2u16(&b, nir_iadd(&b, nir_iadd_imm(&b, row, addend), col));
   nir_def *value = nir_load_shared(&b, 1, 32, index, .align_mul = 4);
   nir_intrinsic_instr *intr = nir_def_as_intrinsic(value);
   EXPECT_EQ(agx_nir_opt_shared_byte_offsets(b.shader), expected_base != 0);
   EXPECT_EQ(nir_intrinsic_base(intr), expected_base);
   if (expected_base) {
      /* The constant left the index: u2u16(row + col). */
      nir_scalar narrow = nir_scalar_resolved(intr->src[0].ssa, 0);
      ASSERT_TRUE(nir_scalar_is_alu(narrow));
      ASSERT_EQ(nir_scalar_alu_op(narrow), nir_op_u2u16);
      nir_scalar sum = nir_scalar_chase_alu_src(narrow, 0);
      ASSERT_TRUE(nir_scalar_is_alu(sum));
      ASSERT_EQ(nir_scalar_alu_op(sum), nir_op_iadd);
      for (unsigned s = 0; s < 2; ++s)
         EXPECT_FALSE(nir_scalar_is_const(nir_scalar_chase_alu_src(sum, s)));
   }
   nir_validate_shader(b.shader, "shared-element-addend result");
   ralloc_free(b.shader);
}

TEST(SharedByteOffsets, NestedElementAddend) { check_element_addend(true, 18, 72); }
TEST(SharedByteOffsets, NestedElementAddendTooLarge) { check_element_addend(true, 64, 0); }
TEST(SharedByteOffsets, NestedElementAddendUnbounded) { check_element_addend(false, 18, 0); }

/* Lowering bit size first (as agx_preprocess_nir() and deferred AGX3 compute
 * lowering do) and then running agx_nir_lower_shared_element_index() must
 * chase through u2u16 in 32 bits so constant byte addends still fold into
 * .base without leaving a 16-bit ushr. */
TEST(SharedByteOffsets, NarrowedByteOffsetElementIndexFolds)
{
   nir_builder b = workgroup_shader("shared-narrowed-element-index");
   nir_def *tid = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
   nir_intrinsic_instr *store = nir_store_shared(
      &b, nir_imm_int(&b, 7), nir_iadd_imm(&b, nir_imul_imm(&b, tid, 8), 16),
      .align_mul = 8);

   nir_def *v = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .range = 4);
   v = nir_iand_imm(&b, v, 4095);
   nir_def *loaded = nir_load_shared(&b, 1, 32, nir_iadd_imm(&b, v, 12),
                                     .align_mul = 4);
   nir_intrinsic_instr *load = nir_def_as_intrinsic(loaded);
   nir_store_global(&b, loaded, nir_imm_int64(&b, 0), .align_mul = 4);

   EXPECT_TRUE(agx_nir_lower_shared_bitsize(b.shader, false));
   EXPECT_TRUE(agx_nir_lower_shared_element_index(b.shader));
   EXPECT_TRUE(b.shader->info.shared_element_index_agx);
   EXPECT_TRUE(agx_nir_opt_shared_byte_offsets(b.shader));
   nir_opt_dce(b.shader);

   EXPECT_EQ(nir_intrinsic_base(store), 16);
   EXPECT_EQ(nir_intrinsic_base(load), 12);
   EXPECT_EQ(count_alu(b.shader, nir_op_ushr), 1u);

   nir_scalar narrow = nir_scalar_resolved(load->src[0].ssa, 0);
   ASSERT_TRUE(nir_scalar_is_alu(narrow));
   ASSERT_EQ(nir_scalar_alu_op(narrow), nir_op_u2u16);
   nir_scalar sh = nir_scalar_chase_alu_src(narrow, 0);
   ASSERT_TRUE(nir_scalar_is_alu(sh));
   EXPECT_EQ(sh.def->bit_size, 32u);
   EXPECT_EQ(nir_scalar_alu_op(sh), nir_op_ushr);

   nir_validate_shader(b.shader, "shared-narrowed-element-index result");
   ralloc_free(b.shader);
}

/* A strided cluster spanning multiple 256-byte windows splits each element
 * offset into a shared chunk base (V + 64*k) plus immediate byte offset
 * (0/64/128/192), letting CSE collapse 8 taps into 2 adds instead of 15. */
TEST(SharedByteOffsets, MultiChunkClusterSharesChunkBases)
{
   nir_builder b = workgroup_shader("shared-multichunk-cluster");
   b.shader->info.workgroup_size[0] = 16;
   b.shader->info.workgroup_size[1] = 16;
   nir_def *id = nir_load_local_invocation_id(&b);
   nir_def *lx = nir_channel(&b, id, 0);
   nir_def *ly = nir_channel(&b, id, 1);
   nir_def *row = nir_ishl_imm(&b, ly, 4);

   nir_intrinsic_instr *loads[8];
   nir_def *sum = NULL;
   for (unsigned t = 0; t < 8; ++t) {
      nir_def *row_t = t ? nir_iadd_imm(&b, row, t * 16) : row;
      nir_def *idx = nir_u2u16(&b, nir_iadd(&b, row_t, lx));
      nir_def *val = nir_load_shared(&b, 1, 32, idx, .align_mul = 4);
      loads[t] = nir_def_as_intrinsic(val);
      sum = sum ? nir_iadd(&b, sum, val) : val;
   }
   nir_store_global(&b, sum, nir_imm_int64(&b, 0), .align_mul = 4);

   EXPECT_TRUE(agx_nir_opt_shared_byte_offsets(b.shader));
   nir_opt_cse(b.shader);
   nir_opt_dce(b.shader);

   for (unsigned t = 0; t < 8; ++t)
      EXPECT_EQ(nir_intrinsic_base(loads[t]), (t % 4) * 64);

   /* First 4 taps share u2u16(V); next 4 share u2u16(V + 64). */
   for (unsigned t = 1; t < 4; ++t)
      EXPECT_EQ(loads[t]->src[0].ssa, loads[0]->src[0].ssa);
   for (unsigned t = 5; t < 8; ++t)
      EXPECT_EQ(loads[t]->src[0].ssa, loads[4]->src[0].ssa);
   EXPECT_NE(loads[0]->src[0].ssa, loads[4]->src[0].ssa);

   /* Only 2 address adds remain (V = lx + row, V + 64), plus 7 value adds. */
   EXPECT_EQ(count_alu(b.shader, nir_op_iadd), 9u);

   nir_validate_shader(b.shader, "shared-multichunk-cluster result");
   ralloc_free(b.shader);
}

/* Unbounded multi-chunk clusters must not split across a possible u16 wrap. */
TEST(SharedByteOffsets, MultiChunkClusterUnboundedDoesNotSplit)
{
   nir_builder b = workgroup_shader("shared-multichunk-unbounded");
   nir_def *x = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .range = 4);
   nir_def *y = nir_channel(&b, nir_load_local_invocation_id(&b), 1);

   nir_intrinsic_instr *loads[2];
   for (unsigned t = 0; t < 2; ++t) {
      nir_def *idx = nir_u2u16(&b, nir_iadd(&b, nir_iadd_imm(&b, x, 64 + t * 16), y));
      loads[t] = nir_def_as_intrinsic(
         nir_load_shared(&b, 1, 32, idx, .align_mul = 4));
   }

   EXPECT_FALSE(agx_nir_opt_shared_byte_offsets(b.shader));
   EXPECT_EQ(nir_intrinsic_base(loads[0]), 0);
   EXPECT_EQ(nir_intrinsic_base(loads[1]), 0);
   nir_validate_shader(b.shader, "shared-multichunk-unbounded result");
   ralloc_free(b.shader);
}

static std::vector<uint32_t>
eval_shared_byte_addrs(const nir_shader *orig, uint32_t lx, uint32_t ly,
                       uint32_t pc)
{
   nir_shader *s = nir_shader_clone(NULL, orig);
   nir_foreach_function_impl(impl, s) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            b.cursor = nir_before_instr(instr);
            if (intr->intrinsic == nir_intrinsic_load_local_invocation_id) {
               nir_def_replace(&intr->def, nir_imm_ivec3(&b, lx, ly, 0));
            } else if (intr->intrinsic == nir_intrinsic_load_push_constant) {
               nir_def_replace(&intr->def, nir_imm_int(&b, pc));
            }
         }
      }
   }

   bool progress;
   do {
      progress = false;
      progress |= nir_opt_constant_folding(s);
      progress |= nir_opt_copy_prop(s);
      progress |= nir_opt_dce(s);
   } while (progress);

   std::vector<uint32_t> addrs;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_shared &&
                intr->intrinsic != nir_intrinsic_store_shared)
               continue;
            unsigned bits = intr->intrinsic == nir_intrinsic_load_shared
                               ? intr->def.bit_size
                               : nir_src_bit_size(intr->src[0]);
            unsigned shift = util_logbase2(bits / 8);
            nir_src *off = nir_get_io_offset_src(intr);
            EXPECT_TRUE(nir_src_is_const(*off));
            EXPECT_LE(nir_intrinsic_base(intr), 255u);
            uint32_t elem = nir_src_as_uint(*off) & 0xffffu;
            addrs.push_back((elem << shift) + nir_intrinsic_base(intr));
         }
      }
   }

   ralloc_free(s);
   return addrs;
}

/* Nonzero .base whose sum crosses a 256-byte chunk boundary: taps whose
 * base + (e_lo << shift) would exceed 255 stay untouched while in-range taps
 * fold cleanly into chunks, preserving every effective byte address. */
TEST(SharedByteOffsets, ValueEquivalenceNonzeroBaseCrossesChunk)
{
   nir_builder b = workgroup_shader("shared-nonzero-base-chunk");
   b.shader->info.workgroup_size[0] = 16;
   b.shader->info.workgroup_size[1] = 16;
   nir_def *id = nir_load_local_invocation_id(&b);
   nir_def *lx = nir_channel(&b, id, 0);
   nir_def *ly = nir_channel(&b, id, 1);
   nir_def *row = nir_ishl_imm(&b, ly, 4);

   const unsigned elems[] = {0, 16, 48, 60, 64, 80, 120, 124};
   nir_intrinsic_instr *loads[8];
   nir_def *sum = NULL;
   for (unsigned t = 0; t < 8; ++t) {
      nir_def *row_t = elems[t] ? nir_iadd_imm(&b, row, elems[t]) : row;
      nir_def *idx = nir_u2u16(&b, nir_iadd(&b, row_t, lx));
      nir_def *val = nir_load_shared(&b, 1, 32, idx, .base = 16, .align_mul = 4);
      loads[t] = nir_def_as_intrinsic(val);
      sum = sum ? nir_iadd(&b, sum, val) : val;
   }
   nir_store_global(&b, sum, nir_imm_int64(&b, 0), .align_mul = 4);

   nir_shader *orig = nir_shader_clone(NULL, b.shader);
   EXPECT_TRUE(agx_nir_opt_shared_byte_offsets(b.shader));
   nir_opt_cse(b.shader);
   nir_opt_dce(b.shader);

   const unsigned expected_base[] = {16, 80, 208, 16, 16, 80, 240, 16};
   for (unsigned t = 0; t < 8; ++t)
      EXPECT_EQ(nir_intrinsic_base(loads[t]), expected_base[t]);
   EXPECT_EQ(loads[4]->src[0].ssa, loads[5]->src[0].ssa);
   EXPECT_EQ(loads[4]->src[0].ssa, loads[6]->src[0].ssa);

   const uint32_t coords[][2] = {{0, 0}, {15, 0}, {0, 15}, {15, 15}, {7, 9}};
   for (const auto &c : coords) {
      EXPECT_EQ(eval_shared_byte_addrs(b.shader, c[0], c[1], 0),
                eval_shared_byte_addrs(orig, c[0], c[1], 0));
   }

   nir_validate_shader(b.shader, "shared-nonzero-base-chunk result");
   ralloc_free(orig);
   ralloc_free(b.shader);
}

/* Multi-chunk split is accepted when variable upper bound + elements == 0xffff
 * and rejected when it reaches 0x10000. */
TEST(SharedByteOffsets, ValueEquivalenceExact0xffffBound)
{
   for (unsigned over = 0; over < 2; ++over) {
      nir_builder b = workgroup_shader("shared-exact-0xffff");
      nir_def *pc = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                           .range = 4);
      uint32_t mask = (0xffffu - 80u) + over;
      nir_def *v = nir_iand_imm(&b, pc, mask);
      nir_def *z = nir_imm_int(&b, 0);

      const unsigned elems[] = {64, 80};
      nir_def *sum = NULL;
      for (unsigned t = 0; t < 2; ++t) {
         nir_def *idx = nir_u2u16(&b, nir_iadd(&b, nir_iadd_imm(&b, v, elems[t]), z));
         nir_def *val = nir_load_shared(&b, 1, 32, idx, .align_mul = 4);
         sum = sum ? nir_iadd(&b, sum, val) : val;
      }
      nir_store_global(&b, sum, nir_imm_int64(&b, 0), .align_mul = 4);

      nir_shader *orig = nir_shader_clone(NULL, b.shader);
      EXPECT_EQ(agx_nir_opt_shared_byte_offsets(b.shader), over == 0);
      nir_opt_cse(b.shader);
      nir_opt_dce(b.shader);

      const uint32_t pcs[] = {0u, 1234u, mask, 0xffffffffu};
      for (uint32_t p : pcs) {
         EXPECT_EQ(eval_shared_byte_addrs(b.shader, 0, 0, p),
                   eval_shared_byte_addrs(orig, 0, 0, p));
      }

      nir_validate_shader(b.shader, "shared-exact-0xffff result");
      ralloc_free(orig);
      ralloc_free(b.shader);
   }
}

/* 16-bit elements (shift = 1, 128 elements per 256-byte chunk) and
 * store_shared multi-chunk clustering preserve exact effective byte addresses. */
TEST(SharedByteOffsets, ValueEquivalence16BitElementsAndStoreShared)
{
   for (unsigned is_store = 0; is_store < 2; ++is_store) {
      nir_builder b = workgroup_shader("shared-16bit-multichunk");
      b.shader->info.workgroup_size[0] = 16;
      b.shader->info.workgroup_size[1] = 16;
      nir_def *id = nir_load_local_invocation_id(&b);
      nir_def *lx = nir_channel(&b, id, 0);
      nir_def *ly = nir_channel(&b, id, 1);
      nir_def *row = nir_ishl_imm(&b, ly, 4);

      nir_intrinsic_instr *intrs[8];
      nir_def *sum = NULL;
      for (unsigned t = 0; t < 8; ++t) {
         nir_def *row_t = t ? nir_iadd_imm(&b, row, t * 32) : row;
         nir_def *idx = nir_u2u16(&b, nir_iadd(&b, row_t, lx));
         if (is_store) {
            intrs[t] = nir_store_shared(&b, nir_imm_intN_t(&b, t, 16), idx,
                                        .base = 8, .align_mul = 2);
         } else {
            nir_def *val = nir_load_shared(&b, 1, 16, idx, .base = 8,
                                           .align_mul = 2);
            intrs[t] = nir_def_as_intrinsic(val);
            sum = sum ? nir_iadd(&b, sum, val) : val;
         }
      }
      if (!is_store)
         nir_store_global(&b, nir_u2u32(&b, sum), nir_imm_int64(&b, 0),
                          .align_mul = 4);

      nir_shader *orig = nir_shader_clone(NULL, b.shader);
      EXPECT_TRUE(agx_nir_opt_shared_byte_offsets(b.shader));
      nir_opt_cse(b.shader);
      nir_opt_dce(b.shader);

      for (unsigned t = 0; t < 8; ++t)
         EXPECT_EQ(nir_intrinsic_base(intrs[t]), 8u + (t % 4) * 64u);
      for (unsigned t = 1; t < 4; ++t)
         EXPECT_EQ(nir_get_io_offset_src(intrs[t])->ssa,
                   nir_get_io_offset_src(intrs[0])->ssa);
      for (unsigned t = 5; t < 8; ++t)
         EXPECT_EQ(nir_get_io_offset_src(intrs[t])->ssa,
                   nir_get_io_offset_src(intrs[4])->ssa);
      EXPECT_NE(nir_get_io_offset_src(intrs[0])->ssa,
                nir_get_io_offset_src(intrs[4])->ssa);

      const uint32_t coords[][2] = {{0, 0}, {15, 0}, {0, 15}, {15, 15}};
      for (const auto &c : coords) {
         EXPECT_EQ(eval_shared_byte_addrs(b.shader, c[0], c[1], 0),
                   eval_shared_byte_addrs(orig, c[0], c[1], 0));
      }

      nir_validate_shader(b.shader, "shared-16bit-multichunk result");
      ralloc_free(orig);
      ralloc_free(b.shader);
   }
}

/* Aligned 2x16-bit shared loads widen to one 32-bit shared load plus unpack,
 * both with and without a foldable G13X 16-byte displacement; unaligned 2x16
 * loads stay untouched. */
TEST(SharedByteOffsets, G13xWiden16x2Load)
{
   const struct {
      unsigned add;
      unsigned align_mul;
      bool expect_progress;
      unsigned expect_base;
   } cases[] = {
      {32, 4, true, 32},
      {4, 4, true, 0},
      {32, 2, false, 0},
   };

   for (const auto &c : cases) {
      SCOPED_TRACE(::testing::Message()
                   << "add=" << c.add << " align=" << c.align_mul);
      nir_builder b = workgroup_shader("G13X widen 16x2");
      nir_def *v = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                          .range = 4);
      nir_def *variable = nir_iand_imm(&b, v, 255);
      nir_def *offset = nir_u2u16(&b, nir_iadd_imm(&b, variable, c.add));
      nir_def *pair = nir_load_shared(&b, 2, 16, offset,
                                      .align_mul = c.align_mul);
      nir_store_global(&b, pair, nir_imm_int64(&b, 0), .align_mul = 4);

      EXPECT_EQ(agx_nir_opt_g13x_shared_offsets(b.shader), c.expect_progress);
      nir_opt_dce(b.shader);

      unsigned load32 = 0, load16 = 0;
      nir_foreach_function_impl(impl, b.shader) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               if (intr->intrinsic != nir_intrinsic_load_shared)
                  continue;
               if (intr->def.bit_size == 32) {
                  load32++;
                  EXPECT_EQ(nir_intrinsic_base(intr), c.expect_base);
               } else if (intr->def.bit_size == 16) {
                  load16++;
               }
            }
         }
      }
      EXPECT_EQ(load32, c.expect_progress ? 1u : 0u);
      EXPECT_EQ(load16, c.expect_progress ? 0u : 1u);
      nir_validate_shader(b.shader, "G13X widen 16x2 result");
      ralloc_free(b.shader);
   }
}

/* Deep bitwise/shift bounds around imadshl_agx still prove no u16 wrap so
 * G13X folds constant 16-byte displacements. */
TEST(SharedByteOffsets, G13xDeepBitwiseTermBound)
{
   nir_builder b = workgroup_shader("G13X deep bitwise bound");
   nir_def *v = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0),
                                       .range = 4);
   nir_def *lo = nir_ubitfield_extract_imm(&b, v, 0, 6);
   nir_def *hi = nir_ishl_imm(&b, nir_ushr_imm(&b, nir_iand_imm(&b, v, 0x3ff), 6), 6);
   nir_def *comb = nir_ior(&b, hi, lo);
   nir_def *scaled =
      nir_imadshl_agx(&b, comb, nir_imm_int(&b, 4), lo, nir_imm_int(&b, 2));
   nir_def *offset = nir_u2u16(&b, nir_iadd_imm(&b, scaled, 64));
   nir_intrinsic_instr *load =
      nir_def_as_intrinsic(nir_load_shared(&b, 1, 32, offset, .align_mul = 4));
   nir_store_global(&b, &load->def, nir_imm_int64(&b, 0), .align_mul = 4);

   EXPECT_TRUE(agx_nir_opt_g13x_shared_offsets(b.shader));
   EXPECT_EQ(nir_intrinsic_base(load), 64);
   nir_validate_shader(b.shader, "G13X deep bitwise bound result");
   ralloc_free(b.shader);
}
