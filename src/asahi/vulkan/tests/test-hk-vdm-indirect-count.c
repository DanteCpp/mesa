/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asahi/lib/agx_ioto_load_core.h"
#include "asahi/lib/agx_m5_va.h"
#include "asahi/lib/agx_va_runtime_hole.h"
#include "asahi/lib/agx_vdm_state_core.h"
#include "asahi/lib/agx_vdm_stream_link_core.h"
#include "asahi/libagx/libagx_dgc.h"
#include "asahi/vulkan/hk_g17p_patch.h"

#define CHECK(cond)                                                            \
   do {                                                                        \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
         return 1;                                                             \
      }                                                                        \
      checks++;                                                                \
   } while (0)

static unsigned checks = 0;

static int
test_span_coalescing_and_lookup(void)
{
   /* Simulate 16,384 draws where draw 0 includes state flush (420 B), draws
    * 1..N-1 are 28 B each, and chunks wrap every 2,000 draws with both higher
    * and lower chunk base addresses. */
   enum { MAX_DRAWS = 16384 };
   static const uint64_t chunk_bases[9] = {
      0x100000000ULL, 0x100020000ULL, 0x0f0000000ULL,
      0x200000000ULL, 0x180000000ULL, 0x200040000ULL,
      0x200080000ULL, 0x0c0000000ULL, 0x300000000ULL,
   };

   uint64_t *expected = malloc(sizeof(uint64_t) * MAX_DRAWS);
   CHECK(expected != NULL);

   struct libagx_vdm_draw_span spans[32];
   uint32_t num_spans = 0;

   uint64_t cur = chunk_bases[0];
   for (uint32_t i = 0; i < MAX_DRAWS; ++i) {
      if (i > 0 && (i % 2000) == 0)
         cur = chunk_bases[i / 2000];

      expected[i] = cur;
      CHECK(libagx_vdm_span_append(spans, &num_spans, ARRAY_SIZE(spans), i,
                                   cur));
      cur += (i == 0) ? 420u : 28u;
   }

   /* Draw 0 + draw 1 coalesce into span 0, then one span per chunk. */
   CHECK(num_spans <= 12);

   for (uint32_t i = 0; i < MAX_DRAWS; ++i) {
      CHECK(libagx_vdm_span_find_addr(spans, num_spans, i) == expected[i]);
   }
   CHECK(libagx_vdm_span_find_addr(spans, num_spans, MAX_DRAWS) == 0);
   CHECK(libagx_vdm_span_find_addr(spans, num_spans, UINT32_MAX) == 0);

   /* Capacity limit returns false without corrupting num_spans. */
   uint32_t small_n = 0;
   struct libagx_vdm_draw_span one_span[1];
   CHECK(libagx_vdm_span_append(one_span, &small_n, 1, 0, 0x2000));
   CHECK(libagx_vdm_span_append(one_span, &small_n, 1, 1, 0x2020));
   CHECK(!libagx_vdm_span_append(one_span, &small_n, 1, 2, 0x1000));
   CHECK(small_n == 1);

   free(expected);
   return 0;
}

static int
test_reusable_patch_state_machine(void)
{
   /* Simulate 3 disjoint chunks holding 12 draws (each 16 B = 4 uint32_t) and
    * verify every boundary count: 0, 1, mid, chunk boundary, max-1, max, >max,
    * and back to 0/1 across repeated submissions of the same command buffer. */
   enum { MAX_DRAWS = 12, WORDS_PER_DRAW = 4 };
   uint32_t chunk0[4 * WORDS_PER_DRAW];
   uint32_t chunk1[4 * WORDS_PER_DRAW];
   uint32_t chunk2[4 * WORDS_PER_DRAW];
   uint32_t orig[MAX_DRAWS][WORDS_PER_DRAW];
   uint32_t *draw_ptrs[MAX_DRAWS];

   struct libagx_vdm_draw_span spans[8];
   uint32_t num_spans = 0;

   for (uint32_t i = 0; i < MAX_DRAWS; ++i) {
      uint32_t *base = (i < 4) ? chunk0 : (i < 8) ? chunk1 : chunk2;
      uint32_t *p = base + (i % 4) * WORDS_PER_DRAW;
      draw_ptrs[i] = p;
      for (uint32_t w = 0; w < WORDS_PER_DRAW; ++w) {
         uint32_t val = 0x40000000u | (i << 8) | w;
         p[w] = val;
         orig[i][w] = val;
      }
      CHECK(libagx_vdm_span_append(spans, &num_spans, ARRAY_SIZE(spans), i,
                                   (uintptr_t)p));
   }

   struct agx_vdm_stream_link_core link;
   const uint64_t end_target = UINT64_C(0x123456780);
   CHECK(agx_vdm_stream_link_core_pack(AGX_CHIP_G17P, end_target, &link));
   uint32_t link_w[2];
   memcpy(link_w, link.bytes, sizeof(link_w));

   struct libagx_vdm_truncate_state state = {0};

   /* Sequence exercising every boundary and repeated restore/repatch cycles. */
   static const uint32_t counts[] = {
      0, 1, 3, 4, 7, 8, 11, 12, 500, 0, 12, 1, 9, 1000000, 0,
   };

   for (unsigned step = 0; step < ARRAY_SIZE(counts); ++step) {
      uint32_t cnt = counts[step];
      libagx_vdm_patch_indirect_count_impl(&state, spans, &cnt, num_spans,
                                           MAX_DRAWS, link_w[0], link_w[1]);

      if (cnt >= MAX_DRAWS) {
         CHECK(state.patched_addr == 0);
      } else {
         CHECK(state.patched_addr == (uintptr_t)draw_ptrs[cnt]);
         CHECK(state.orig_w0 == orig[cnt][0]);
         CHECK(state.orig_w1 == orig[cnt][1]);
      }

      /* Verify every draw slot: patched slot holds the exact VDM_STREAM_LINK
       * (with trailing words intact); every other slot holds its original
       * words bit-for-bit. */
      for (uint32_t i = 0; i < MAX_DRAWS; ++i) {
         if (i == cnt && cnt < MAX_DRAWS) {
            CHECK(draw_ptrs[i][0] == link_w[0]);
            CHECK(draw_ptrs[i][1] == link_w[1]);
            CHECK(draw_ptrs[i][2] == orig[i][2]);
            CHECK(draw_ptrs[i][3] == orig[i][3]);

            uint64_t decoded = 0;
            CHECK(agx_vdm_stream_link_core_unpack(
               AGX_CHIP_G17P, draw_ptrs[i], AGX_VDM_STREAM_LINK_CORE_LENGTH,
               &decoded));
            CHECK(decoded == end_target);
         } else {
            for (uint32_t w = 0; w < WORDS_PER_DRAW; ++w)
               CHECK(draw_ptrs[i][w] == orig[i][w]);
         }
      }
   }

   return 0;
}

static int
test_cross_chip_link_roundtrip(void)
{
   static const enum agx_chip supported_chips[] = {
      AGX_CHIP_G15G,
      AGX_CHIP_G16G,
      AGX_CHIP_G17P,
      AGX_CHIP_G17G,
   };
   const uint64_t target = UINT64_C(0x3a4b5c6d8);

   for (unsigned i = 0; i < ARRAY_SIZE(supported_chips); ++i) {
      struct agx_vdm_stream_link_core link;
      CHECK(agx_vdm_stream_link_core_supported(supported_chips[i]));
      CHECK(agx_vdm_stream_link_core_pack(supported_chips[i], target, &link));
      CHECK(link.length == AGX_VDM_STREAM_LINK_CORE_LENGTH);

      uint64_t unpacked = 0;
      CHECK(agx_vdm_stream_link_core_unpack(supported_chips[i], link.bytes,
                                            link.length, &unpacked));
      CHECK(unpacked == target);
   }

   /* AGX2 witnessed chips use the record-table capability and emit an 8-byte
    * non-returning VDM_STREAM_LINK; unverified chips must fail closed. */
   static const enum agx_chip agx2_chips[] = {
      AGX_CHIP_G13G,
      AGX_CHIP_G13X,
      AGX_CHIP_G14G,
      AGX_CHIP_G14X,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(agx2_chips); ++i) {
      CHECK(agx_chip_vdm_stream_link_supported(agx2_chips[i]));
      CHECK(!agx_vdm_stream_link_core_supported(agx2_chips[i]));
   }
   uint32_t agx2_link[2] = {0, 0};
   uint32_t *agx2_end = agx_cs_jump(agx2_link, target, true);
   CHECK(agx2_end == agx2_link + 2);

   static const enum agx_chip closed_chips[] = {
      AGX_CHIP_G15P,
      AGX_CHIP_G16P,
      AGX_CHIP_G17A,
      AGX_CHIP_G18G,
      AGX_CHIP_UNKNOWN,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(closed_chips); ++i) {
      CHECK(!agx_chip_vdm_stream_link_supported(closed_chips[i]));
      CHECK(!agx_vdm_stream_link_core_supported(closed_chips[i]));
   }
   return 0;
}

static int
test_stream_split_runtime_guard(void)
{
   uint8_t cs_a_storage = 0;
   uint8_t cs_b_storage = 0;
   uint8_t buf[64];

   /* Same stream, >= 8 bytes written in same chunk: valid. */
   CHECK(libagx_vdm_draw_site_valid(&cs_a_storage, &cs_a_storage, 0x1000,
                                    0x1000, &buf[0], &buf[8]));
   CHECK(libagx_vdm_draw_site_valid(&cs_a_storage, &cs_a_storage, 0x1000,
                                    0x1000, &buf[0], &buf[28]));

   /* Same stream, draw wrapped onto a new chunk: valid. */
   CHECK(libagx_vdm_draw_site_valid(&cs_a_storage, &cs_a_storage, 0x1000,
                                    0x2000, &buf[32], &buf[4]));

   /* Same stream, < 8 bytes written in same chunk (or reversed): rejected. */
   CHECK(!libagx_vdm_draw_site_valid(&cs_a_storage, &cs_a_storage, 0x1000,
                                     0x1000, &buf[0], &buf[0]));
   CHECK(!libagx_vdm_draw_site_valid(&cs_a_storage, &cs_a_storage, 0x1000,
                                     0x1000, &buf[0], &buf[4]));
   CHECK(!libagx_vdm_draw_site_valid(&cs_a_storage, &cs_a_storage, 0x1000,
                                     0x1000, &buf[16], &buf[8]));

   /* Simulate a mid-loop graphics stream split (hk_flush_gfx_state ended cs_a
    * and opened cs_b on draw 3 of 8): the runtime guard must trip immediately
    * and prevent emitting a cross-stream VDM_STREAM_LINK. */
   struct libagx_vdm_draw_span spans[8];
   uint32_t num_spans = 0;
   bool aborted = false;
   uint32_t recorded = 0;
   for (uint32_t draw_id = 0; draw_id < 8; ++draw_id) {
      const void *cs_after = (draw_id == 3) ? &cs_b_storage : &cs_a_storage;
      const uint64_t chunk_after = (draw_id == 3) ? 0x2000 : 0x1000;
      const uint8_t *cur_before = &buf[draw_id * 8];
      const uint8_t *cur_after = &buf[(draw_id + 1) * 8];
      if (!libagx_vdm_draw_site_valid(&cs_a_storage, cs_after, 0x1000,
                                      chunk_after, cur_before, cur_after)) {
         aborted = true;
         break;
      }
      CHECK(libagx_vdm_span_append(spans, &num_spans, ARRAY_SIZE(spans),
                                   draw_id, 0x1000 + draw_id * 8));
      recorded++;
   }
   CHECK(aborted);
   CHECK(recorded == 3);
   CHECK(libagx_vdm_span_find_addr(spans, num_spans, 3) == 0);
   return 0;
}

static int
test_template_word_patch_equivalence(void)
{
   static const enum agx_chip chips[] = {
      AGX_CHIP_G15G,
      AGX_CHIP_G16G,
      AGX_CHIP_G17P,
      AGX_CHIP_G17G,
   };
   static const uint64_t addrs[] = {
      UINT64_C(0x100000004),
      UINT64_C(0x1fffffffc),
      UINT64_C(0x200000000),
      UINT64_C(0x7fff12345678),
      UINT64_C(0xffffffffffec),
   };
   const uint64_t ib = UINT64_C(0x5a0012340080);

   for (unsigned ci = 0; ci < ARRAY_SIZE(chips); ++ci) {
      for (unsigned indexed = 0; indexed < 2; ++indexed) {
         struct agx_vdm_index_list_indirect_words tmpl;
         CHECK(agx_vdm_index_list_indirect_pack_words(
            chips[ci], (uint8_t)AGX_PRIMITIVE_TRIANGLES, indexed != 0,
            indexed != 0, (uint8_t)AGX_INDEX_SIZE_U32, indexed ? ib : 0,
            addrs[0], indexed ? 0xffffffffu : 0u, indexed ? 0x12345u : 0u,
            &tmpl));
         const unsigned hi_idx = indexed ? 4u : 1u;
         const unsigned lo_idx = indexed ? 5u : 2u;

         for (unsigned ai = 0; ai < ARRAY_SIZE(addrs); ++ai) {
            uint32_t patched[ARRAY_SIZE(tmpl.words)];
            memcpy(patched, tmpl.words, tmpl.count * sizeof(uint32_t));
            patched[hi_idx] = (uint32_t)((addrs[ai] >> 32) & 0xffffu);
            patched[lo_idx] = (uint32_t)addrs[ai];

            struct agx_vdm_index_list_indirect_words ref;
            CHECK(agx_vdm_index_list_indirect_pack_words(
               chips[ci], (uint8_t)AGX_PRIMITIVE_TRIANGLES, indexed != 0,
               indexed != 0, (uint8_t)AGX_INDEX_SIZE_U32, indexed ? ib : 0,
               addrs[ai], indexed ? 0xffffffffu : 0u, indexed ? 0x12345u : 0u,
               &ref));
            CHECK(ref.count == tmpl.count);
            CHECK(memcmp(patched, ref.words, ref.count * sizeof(uint32_t)) ==
                  0);
         }
      }
   }
   return 0;
}

static int
test_gpu_gen_prep_reusable_state_machine(void)
{
   enum { MAX_DRAWS = 130, REC_WORDS = 15, REC_BYTES = REC_WORDS * 4 };
   uint32_t draw0_buf[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
   uint32_t after_draw0_buf[2] = {0, 0};
   uint32_t sub_vdm[(MAX_DRAWS - 1) * REC_WORDS + 2];
   memset(sub_vdm, 0xa5, sizeof(sub_vdm));

   struct agx_vdm_stream_link_core sub_link, end_link;
   const uint64_t sub_target = UINT64_C(0x100040000);
   const uint64_t end_target = UINT64_C(0x100080000);
   CHECK(agx_vdm_stream_link_core_pack(AGX_CHIP_G17P, sub_target, &sub_link));
   CHECK(agx_vdm_stream_link_core_pack(AGX_CHIP_G17P, end_target, &end_link));
   uint32_t sub_w[2], end_w[2];
   memcpy(sub_w, sub_link.bytes, sizeof(sub_w));
   memcpy(end_w, end_link.bytes, sizeof(end_w));

   struct libagx_vdm_truncate_state state = {0};
   uint32_t grid[3] = {99, 99, 99};

   static const uint32_t counts[] = {
      0, 1, 2, 64, 65, 66, 129, 130, 50000, 0, 130, 1, 65, 0,
   };

   for (unsigned step = 0; step < ARRAY_SIZE(counts); ++step) {
      uint32_t cnt = counts[step];
      libagx_vdm_prep_indirect_count_gen_impl(
         &state, grid, &cnt, (uintptr_t)draw0_buf, (uintptr_t)after_draw0_buf,
         (uintptr_t)sub_vdm, MAX_DRAWS, REC_BYTES, sub_w[0], sub_w[1],
         end_w[0], end_w[1]);

      const uint32_t eff = cnt > MAX_DRAWS ? MAX_DRAWS : cnt;
      if (eff == 0) {
         CHECK(state.patched_addr == (uintptr_t)draw0_buf);
         CHECK(draw0_buf[0] == end_w[0] && draw0_buf[1] == end_w[1]);
         CHECK(draw0_buf[2] == 0x33333333u && draw0_buf[3] == 0x44444444u);
         CHECK(grid[0] == 0 && grid[1] == 1 && grid[2] == 1);
      } else if (eff == 1) {
         CHECK(state.patched_addr == 0);
         CHECK(draw0_buf[0] == 0x11111111u && draw0_buf[1] == 0x22222222u);
         CHECK(after_draw0_buf[0] == end_w[0] &&
               after_draw0_buf[1] == end_w[1]);
         CHECK(grid[0] == 0 && grid[1] == 1 && grid[2] == 1);
      } else {
         const uint32_t rem = eff - 1u;
         const uint32_t expected_wg = (rem + LIBAGX_VDM_GEN_WG_SIZE - 1u) /
                                      LIBAGX_VDM_GEN_WG_SIZE;
         CHECK(state.patched_addr == 0);
         CHECK(draw0_buf[0] == 0x11111111u && draw0_buf[1] == 0x22222222u);
         CHECK(after_draw0_buf[0] == sub_w[0] &&
               after_draw0_buf[1] == sub_w[1]);
         CHECK(sub_vdm[rem * REC_WORDS + 0] == end_w[0]);
         CHECK(sub_vdm[rem * REC_WORDS + 1] == end_w[1]);
         CHECK(grid[0] == expected_wg && grid[1] == 1 && grid[2] == 1);
      }
   }
   return 0;
}

static int
test_gpu_gen_build_vdm_and_ioto_equivalence(void)
{
   /* Verify that libagx_vdm_build_indirect_count_impl produces byte-for-byte
    * identical vertex Ioto programs (vs hk_g17p_patch_plan_apply, covering
    * both long 8-byte and short 2-byte high-half LDIMMs plus draw_params and
    * draw_id relocations) and byte-for-byte identical VDM records (vs
    * agx_vdm_state_vertex_pipeline_pair_pack +
    * agx_vdm_index_list_indirect_pack_words) across HAL200 and HAL300 chips. */
   static const enum agx_chip chips[] = {
      AGX_CHIP_G16G,
      AGX_CHIP_G17P,
      AGX_CHIP_G17G,
   };
   enum { MAX_DRAWS = 8, COUNT = 6, IOTO_LEN = 48, IOTO_STRIDE = 64 };
   const uint64_t ib = UINT64_C(0x5a0012340080);
   const uint32_t in_stride = 20;
   const uint32_t dp_offset = 12;

   for (unsigned ci = 0; ci < ARRAY_SIZE(chips); ++ci) {
      for (unsigned high_short = 0; high_short < 2; ++high_short) {
         const uint64_t in_base =
            high_short ? UINT64_C(0x1012345600) : UINT64_C(0x20012345600);
         uint8_t base_ioto[IOTO_LEN];
         for (unsigned b = 0; b < IOTO_LEN; ++b)
            base_ioto[b] = (uint8_t)(0x30u + b);

         /* Range 0: static root; Range 1: draw_params; Range 2: draw_id. */
         uint32_t draw_ids[MAX_DRAWS - 1] = {0};
         const uint64_t draw_ids_va = (uintptr_t)draw_ids;
         /* Only test high_short on draw_params when draw_ids host VA might
          * exceed 128<<32; keep draw_id reloc always 8+8 long. */
         struct agx3_ioto_uniform_range init_ranges[3] = {
            {.start_halfs = 0, .size_halfs = 16, .address = UINT64_C(0x180000000)},
            {.start_halfs = 16, .size_halfs = 4, .address = in_base + dp_offset},
            {.start_halfs = 20, .size_halfs = 2, .address = draw_ids_va},
         };
         struct agx_ioto_load_relocation relocs[3] = {
            {.address = init_ranges[0].address,
             .offset = 0,
             .dest_half = 4,
             .low_length = 8,
             .high_length = 8},
            {.address = init_ranges[1].address + 4,
             .offset = 18, /* 2-byte aligned, NOT 8-byte aligned! */
             .dest_half = 8,
             .low_length = 8,
             .high_length = high_short ? 2 : 8},
            {.address = init_ranges[2].address,
             .offset = 30, /* 2-byte aligned, NOT 4-byte aligned! */
             .dest_half = 12,
             .low_length = 8,
             .high_length = 8},
         };
         struct hk_g17p_patch_plan plan;
         CHECK(hk_g17p_patch_plan_init(&plan, init_ranges, 3, relocs, 3,
                                       IOTO_LEN, true));

         uint8_t ioto_tmpl[IOTO_LEN];
         memcpy(ioto_tmpl, base_ioto, IOTO_LEN);
         hk_g17p_patch_plan_apply(&plan, init_ranges, ioto_tmpl);

         struct agx_vdm_index_list_indirect_words draw_tmpl;
         CHECK(agx_vdm_index_list_indirect_pack_words(
            chips[ci], (uint8_t)AGX_PRIMITIVE_TRIANGLES, true, true,
            (uint8_t)AGX_INDEX_SIZE_U32, ib, in_base, 0xffffffffu, 0x12345u,
            &draw_tmpl));

         const uint32_t vdm_upd_words = 7;
         const uint32_t vdm_rec_words = vdm_upd_words + draw_tmpl.count;
         const uint32_t raw_tmpl = 0x18u << 3;

         uint32_t sub_vdm[(MAX_DRAWS - 1) * LIBAGX_VDM_GEN_MAX_VDM_WORDS];
         memset(sub_vdm, 0xdd, sizeof(sub_vdm));

         /* Align sub_ioto_va to 64 B for valid vertex pipeline pairs. */
         uint8_t sub_ioto_raw[(MAX_DRAWS - 1) * IOTO_STRIDE + 64];
         uintptr_t sub_ioto_aligned =
            ((uintptr_t)sub_ioto_raw + 63u) & ~(uintptr_t)63u;
         memset((void *)sub_ioto_aligned, 0xcc, (MAX_DRAWS - 1) * IOTO_STRIDE);

         uint32_t runtime_count = COUNT;
         struct libagx_vdm_gen_desc desc = {
            .sub_vdm_va = (uintptr_t)sub_vdm,
            .sub_ioto_va = (uint64_t)sub_ioto_aligned,
            .draw_ids_va = draw_ids_va,
            .in_base_va = in_base,
            .ioto_tmpl_va = (uintptr_t)ioto_tmpl,
            .count_addr = (uintptr_t)&runtime_count,
            .max_draw_count = MAX_DRAWS,
            .in_stride = in_stride,
            .draw_params_offset = dp_offset,
            .vdm_rec_words = vdm_rec_words,
            .vdm_addr_hi_idx = vdm_upd_words + 4u,
            .vdm_addr_lo_idx = vdm_upd_words + 5u,
            .vdm_pipe_raw_tmpl = raw_tmpl,
            .ioto_stride = IOTO_STRIDE,
            .ioto_copy_words = IOTO_LEN / 4u,
            .reloc_count = 2,
         };
         for (uint32_t w = 0; w < vdm_upd_words; ++w)
            desc.vdm_tmpl[w] = 0x80000000u | w;
         memcpy(&desc.vdm_tmpl[vdm_upd_words], draw_tmpl.words,
                draw_tmpl.count * sizeof(uint32_t));

         desc.relocs[0] = (struct libagx_ioto_dyn_reloc){
            .dest_lo = plan.relocs[1].dest[0],
            .dest_hi = plan.relocs[1].dest[1],
            .delta = plan.relocs[1].delta,
            .offset = plan.relocs[1].offset,
            .flags = high_short ? LIBAGX_IOTO_RELOC_HIGH_SHORT : 0u,
         };
         desc.relocs[1] = (struct libagx_ioto_dyn_reloc){
            .dest_lo = plan.relocs[2].dest[0],
            .dest_hi = plan.relocs[2].dest[1],
            .delta = plan.relocs[2].delta,
            .offset = plan.relocs[2].offset,
            .flags = LIBAGX_IOTO_RELOC_KIND_DRAW_ID,
         };

         /* Invoke 64 threads (including OOB threads tid >= COUNT - 1). */
         for (uint32_t tid = 0; tid < LIBAGX_VDM_GEN_WG_SIZE; ++tid)
            libagx_vdm_build_indirect_count_impl(&desc, tid);

         /* Check every active slot 0..COUNT-2 byte-for-byte against host ref. */
         for (uint32_t tid = 0; tid < COUNT - 1; ++tid) {
            CHECK(draw_ids[tid] == tid + 1u);

            const uint64_t cmd_addr = in_base + (uint64_t)tid * in_stride;
            struct agx3_ioto_uniform_range ref_ranges[3] = {
               init_ranges[0],
               {.start_halfs = 16, .size_halfs = 4, .address = cmd_addr + dp_offset},
               {.start_halfs = 20, .size_halfs = 2, .address = draw_ids_va + tid * 4u},
            };
            uint8_t ref_ioto[IOTO_LEN];
            memcpy(ref_ioto, base_ioto, IOTO_LEN);
            hk_g17p_patch_plan_apply(&plan, ref_ranges, ref_ioto);

            const uint8_t *got_ioto =
               (const uint8_t *)(sub_ioto_aligned + (size_t)tid * IOTO_STRIDE);
            CHECK(memcmp(got_ioto, ref_ioto, IOTO_LEN) == 0);

            struct agx_vdm_state_vertex_pipeline_pair ref_pair;
            const uint64_t ioto_va =
               (uint64_t)sub_ioto_aligned + (uint64_t)tid * IOTO_STRIDE;
            CHECK(agx_vdm_state_vertex_pipeline_pair_pack(
               chips[ci], ioto_va, raw_tmpl, &ref_pair));

            struct agx_vdm_index_list_indirect_words ref_draw;
            CHECK(agx_vdm_index_list_indirect_pack_words(
               chips[ci], (uint8_t)AGX_PRIMITIVE_TRIANGLES, true, true,
               (uint8_t)AGX_INDEX_SIZE_U32, ib, cmd_addr, 0xffffffffu,
               0x12345u, &ref_draw));

            const uint32_t *got_vdm = sub_vdm + (size_t)tid * vdm_rec_words;
            CHECK(memcmp(&got_vdm[2], ref_pair.bytes, sizeof(ref_pair.bytes)) == 0);
            CHECK(memcmp(&got_vdm[vdm_upd_words], ref_draw.words,
                         ref_draw.count * sizeof(uint32_t)) == 0);
         }

         /* Inactive slots COUNT-1..MAX_DRAWS-2 must remain untouched. */
         for (uint32_t tid = COUNT - 1; tid < MAX_DRAWS - 1; ++tid) {
            CHECK(draw_ids[tid] == 0);
            CHECK(sub_vdm[tid * vdm_rec_words] == 0xddddddddU);
         }
      }
   }
   return 0;
}

static int
test_substream_range_compactable_alignment(void)
{
   /* Both agx_g17p_vdm_payload_compact() and agx_m5_vdm_compact() require
    * 4-byte-aligned addresses, so range checks must probe the last 4-byte word
    * (primary + size - 4), not primary + size - 1. */
   const uint64_t size = 4096u * 60u + 8u;
   uint64_t c0 = 0, c1 = 0;

   const uint64_t g17p_base = AGX_G17P_VDM_USER_BASE + UINT64_C(0x100000);
   CHECK(agx_g17p_vdm_payload_compact(g17p_base, &c0));
   CHECK(agx_g17p_vdm_payload_compact(g17p_base + size - 4, &c1));
   CHECK(!agx_g17p_vdm_payload_compact(g17p_base + size - 1, &c1));
   CHECK(agx_g17p_vdm_payload_compact(AGX_G17P_VDM_USER_LIMIT - 4, &c1));
   CHECK(!agx_g17p_vdm_payload_compact(AGX_G17P_VDM_USER_LIMIT, &c1));

   const uint64_t m5_base = AGX_M5_DRIVER_BASE + UINT64_C(0x100000);
   const uint64_t m5_limit = AGX_M5_DRIVER_BASE + (UINT64_C(1) << 39);
   CHECK(agx_m5_vdm_compact(m5_base, &c0));
   CHECK(agx_m5_vdm_compact(m5_base + size - 4, &c1));
   CHECK(!agx_m5_vdm_compact(m5_base + size - 1, &c1));
   CHECK(agx_m5_vdm_compact(m5_limit - 4, &c1));
   CHECK(!agx_m5_vdm_compact(m5_limit, &c1));
   return 0;
}

int
main(void)
{
   if (test_span_coalescing_and_lookup() ||
       test_reusable_patch_state_machine() ||
       test_cross_chip_link_roundtrip() ||
       test_stream_split_runtime_guard() ||
       test_template_word_patch_equivalence() ||
       test_gpu_gen_prep_reusable_state_machine() ||
       test_gpu_gen_build_vdm_and_ioto_equivalence() ||
       test_substream_range_compactable_alignment())
      return 1;

   printf("hk_vdm_indirect_count: %u checks passed\n", checks);
   return 0;
}
