/* SPDX-License-Identifier: MIT */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hk_cmd_buffer.h"
#include "nir_builder.h"

static uint64_t checks;
#define CHECK(condition)                                                       \
   do {                                                                        \
      ++checks;                                                                \
      if (!(condition)) {                                                      \
         fprintf(stderr, "line %d: %s\n", __LINE__, #condition);               \
         exit(1);                                                              \
      }                                                                        \
   } while (0)

/* Independent reference: select by comparing the actual published roots. */
static uint64_t
fresh_select(struct hk_graphics_state *gfx, uint8_t reads)
{
   const uint8_t hidden = HK_ROOT_READS_ADDRESS | HK_ROOT_READS_VERTEX_INPUT |
                          HK_ROOT_READS_DYNAMIC_BUFFERS | HK_ROOT_READS_PUSH;
   if (gfx->fs_root && gfx->fs_root != gfx->root && gfx->root_cpu &&
       !(reads & hidden) &&
       hk_root_matches_for_fragment(gfx->fs_root_cpu, gfx->root_cpu))
      return gfx->fs_root;
   gfx->fs_root = gfx->root;
   gfx->fs_root_cpu = gfx->root_cpu;
   return gfx->root;
}

static void
select_and_check(struct hk_graphics_state *fresh,
                 struct hk_graphics_state *cached, uint8_t reads, bool memo,
                 bool shadow)
{
   CHECK(fresh_select(fresh, reads) ==
         hk_m5_fragment_root_select(cached, reads, memo, shadow));
   CHECK(fresh->fs_root == cached->fs_root);
   CHECK(fresh->fs_root_cpu == cached->fs_root_cpu);
}

/* Publication uses a mutable cached CPU source and an immutable GPU copy.
 * Poison skipped dynamic bytes so accidental observation cannot pass. */
static void
publish(struct hk_graphics_state *fresh, struct hk_graphics_state *cached,
        struct hk_root_descriptor_table *gpu,
        const struct hk_root_descriptor_table *source, unsigned dynamic_end,
        uint64_t va, bool shadow)
{
   const size_t head = hk_root_descriptor_offset(dynamic_buffers) +
                       dynamic_end * sizeof(source->dynamic_buffers[0]);
   const size_t tail = hk_root_descriptor_offset(set_dynamic_buffer_start);
   memset(gpu, 0xa7, sizeof(*gpu));
   if (shadow)
      hk_root_fragment_snapshot_publish(cached, gpu, source);
   memcpy(gpu, source, head);
   memcpy((uint8_t *)gpu + tail, (const uint8_t *)source + tail,
          sizeof(*gpu) - tail);
   fresh->root = cached->root = va;
   fresh->root_cpu = cached->root_cpu = gpu;
   if (shadow) {
      size_t out = 0;
      for (size_t b = 0; b < sizeof(*gpu); ++b) {
         if (!hk_root_reads_of(b, b + 1)) {
            CHECK(cached->root_fragment_snapshot.bytes[out] ==
                  ((const uint8_t *)gpu)[b]);
            ++out;
         }
      }
      CHECK(out == HK_ROOT_FRAGMENT_VISIBLE_SIZE);
   }
}

static void
test_nir_root_use_reads(void)
{
   static const nir_shader_compiler_options opts = {0};
   const uint8_t any = HK_ROOT_READS_ADDRESS | HK_ROOT_READS_VERTEX_INPUT |
                       HK_ROOT_READS_DYNAMIC_BUFFERS | HK_ROOT_READS_PUSH;

   /* Constant-offset push-constant load -> HK_ROOT_READS_PUSH only. */
   {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                     &opts, "push_const");
      nir_def *root = nir_load_root_agx(&b);
      nir_def *addr =
         nir_iadd_imm(&b, root, hk_root_descriptor_offset(push) + 4);
      nir_load_global_constant(&b, 1, 32, addr);

      uint8_t use_reads = 0;
      nir_foreach_use_including_if(use, root)
         use_reads |= hk_nir_root_use_reads(use);
      CHECK(use_reads == HK_ROOT_READS_PUSH);
      CHECK(hk_nir_root_reads(b.shader) == HK_ROOT_READS_PUSH);
      ralloc_free(b.shader);
   }

   /* Unfolded constant push-constant expression (as emitted before opts). */
   {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                     &opts, "push_unfolded");
      nir_def *root = nir_load_root_agx(&b);
      nir_def *off32 =
         nir_iadd(&b, nir_imm_int(&b, hk_root_descriptor_offset(push)),
                  nir_ishl_imm(&b, nir_imm_int(&b, 3), 2));
      nir_def *addr = nir_iadd(&b, root, nir_u2u64(&b, off32));
      nir_load_global(&b, 1, 32, addr);

      CHECK(hk_nir_root_reads(b.shader) == HK_ROOT_READS_PUSH);
      ralloc_free(b.shader);
   }

   /* Constant-offset descriptor-set load -> visible span (reads == 0). */
   {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                     &opts, "sets_const");
      nir_def *root = nir_load_root_agx(&b);
      nir_def *addr = nir_iadd_imm(&b, root, hk_root_descriptor_offset(sets));
      nir_load_global_constant(&b, 1, 64, addr);

      CHECK(hk_nir_root_reads(b.shader) == 0);
      ralloc_free(b.shader);
   }

   /* Constant load straddling push and dynamic_buffers -> both classes. */
   {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                     &opts, "push_straddle");
      nir_def *root = nir_load_root_agx(&b);
      nir_def *addr = nir_iadd_imm(
         &b, root, hk_root_descriptor_offset(dynamic_buffers) - 4);
      nir_load_global_constant(&b, 2, 32, addr);

      CHECK(hk_nir_root_reads(b.shader) ==
            (HK_ROOT_READS_PUSH | HK_ROOT_READS_DYNAMIC_BUFFERS));
      ralloc_free(b.shader);
   }

   /* Dynamically indexed push load -> conservative fallback to every class. */
   {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                     &opts, "push_dynamic");
      nir_def *root = nir_load_root_agx(&b);
      nir_def *dyn_idx = nir_load_primitive_id(&b);
      nir_def *off32 =
         nir_iadd_imm(&b, nir_ishl_imm(&b, dyn_idx, 2),
                      hk_root_descriptor_offset(push));
      nir_def *addr = nir_iadd(&b, root, nir_u2u64(&b, off32));
      nir_load_global_constant(&b, 1, 32, addr);

      uint8_t use_reads = 0;
      nir_foreach_use_including_if(use, root)
         use_reads |= hk_nir_root_use_reads(use);
      CHECK(use_reads == any);
      CHECK(hk_nir_root_reads(b.shader) == any);
      ralloc_free(b.shader);
   }

   /* Direct non-iadd load from load_root_agx -> conservative fallback. */
   {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                     &opts, "root_direct");
      nir_def *root = nir_load_root_agx(&b);
      nir_load_global_constant(&b, 1, 64, root);

      CHECK(hk_nir_root_reads(b.shader) == any);
      ralloc_free(b.shader);
   }
}

int
main(void)
{
   test_nir_root_use_reads();

   struct hk_graphics_state *fresh = calloc(1, sizeof(*fresh));
   struct hk_graphics_state *cached = calloc(1, sizeof(*cached));
   struct hk_root_descriptor_table source, gpu[35];
   CHECK(fresh && cached);

   for (unsigned mode = 0; mode < 4; ++mode) {
      const bool memo = mode & 1, shadow = mode & 2;
      for (unsigned end = 0; end <= HK_MAX_DYNAMIC_BUFFERS; ++end) {
         for (unsigned byte = 0; byte < sizeof(source); ++byte) {
            memset(fresh, 0, sizeof(*fresh));
            memset(cached, 0, sizeof(*cached));
            memset(&source, 0, sizeof(source));
            publish(fresh, cached, &gpu[0], &source, end, 0x1000, shadow);
            select_and_check(fresh, cached, 0, memo, shadow);
            const struct hk_root_fragment_snapshot prior =
               cached->fs_root_fragment_snapshot;
            const struct hk_root_descriptor_table *prior_cpu =
               cached->fs_root_fragment_snapshot_cpu;
            ((uint8_t *)&source)[byte] ^= 0x5a;
            publish(fresh, cached, &gpu[1], &source, end, 0x2000, shadow);
            for (unsigned reads = 0; reads < 16; ++reads) {
               /* Each mask must decide between the old and current roots.
                * A previous hidden-read selection must not pre-adopt it. */
               fresh->fs_root = cached->fs_root = 0x1000;
               fresh->fs_root_cpu = cached->fs_root_cpu = &gpu[0];
               cached->fs_root_checked_root = 0;
               cached->fs_root_fragment_snapshot = prior;
               cached->fs_root_fragment_snapshot_cpu = prior_cpu;
               select_and_check(fresh, cached, reads, memo, shadow);
               select_and_check(fresh, cached, reads, memo, shadow);
            }
         }
      }

      /* Push-constant-only updates reuse fs_root when the fragment shader
       * does not read push constants, and republish when it does. */
      memset(fresh, 0, sizeof(*fresh));
      memset(cached, 0, sizeof(*cached));
      memset(&source, 0, sizeof(source));
      publish(fresh, cached, &gpu[0], &source, 0, 0x1000, shadow);
      select_and_check(fresh, cached, 0, memo, shadow);
      source.push[0] = 0x42;
      publish(fresh, cached, &gpu[1], &source, 0, 0x2000, shadow);
      CHECK(hk_m5_fragment_root_select(cached, 0, memo, shadow) == 0x1000);
      CHECK(fresh_select(fresh, 0) == 0x1000);
      CHECK(hk_m5_fragment_root_select(cached, HK_ROOT_READS_PUSH, memo,
                                       shadow) == 0x2000);
      CHECK(fresh_select(fresh, HK_ROOT_READS_PUSH) == 0x2000);

      /* Every publication-time global and the complete tail are visible. */
      memset(fresh, 0, sizeof(*fresh));
      memset(cached, 0, sizeof(*cached));
      memset(&source, 0, sizeof(source));
      for (unsigned globals = 0; globals < 32; ++globals) {
         source.root_desc_addr = 0x100000000ull + globals;
         source.printf_buffer_address = globals & 1 ? 0x12340000 : 0;
         source.sampler_heap_address = globals & 2 ? 0x23450000 : 0;
         source.physical_sparse_map = globals & 4 ? 0x34560000 : 0;
         source.physical_sparse_base = globals & 8 ? 0x45670000 : 0;
         source.physical_sparse_size = globals & 16 ? 0x100000 : 0;
         publish(fresh, cached, &gpu[globals], &source, 0,
                 0x3000 + globals * 0x1000, shadow);
         select_and_check(fresh, cached, 0, memo, shadow);
      }

      /* Mutable descriptor restoration cannot change a published snapshot. */
      memset(fresh, 0, sizeof(*fresh));
      memset(cached, 0, sizeof(*cached));
      memset(&source, 0, sizeof(source));
      publish(fresh, cached, &gpu[0], &source, 0, 0x1000, shadow);
      select_and_check(fresh, cached, 0, memo, shadow);
      publish(fresh, cached, &gpu[1], &source, 0, 0x2000, shadow);
      memset(&source, 0x6d, sizeof(source));
      select_and_check(fresh, cached, 0, memo, shadow);
      select_and_check(fresh, cached, 15, memo, shadow);

      /* Failed publication retains the old CPU copy while its GPU VA is 0. */
      source.sets[0] ^= 1;
      publish(fresh, cached, &gpu[2], &source, 64, 0x3000, shadow);
      fresh->root = cached->root = 0;
      select_and_check(fresh, cached, 0, memo, shadow);
      select_and_check(fresh, cached, 15, memo, shadow);
      cached->root_fragment_snapshot_cpu = NULL;
      select_and_check(fresh, cached, 0, memo, shadow);

      /* Missing current/prior metadata must use the fresh comparison. */
      memset(fresh, 0, sizeof(*fresh));
      memset(cached, 0, sizeof(*cached));
      memset(&source, 0, sizeof(source));
      publish(fresh, cached, &gpu[0], &source, 0, 0x1000, shadow);
      select_and_check(fresh, cached, 0, memo, shadow);
      source.sets[0] = 1;
      publish(fresh, cached, &gpu[1], &source, 0, 0x2000, shadow);
      cached->root_fragment_snapshot_cpu = NULL;
      select_and_check(fresh, cached, 0, memo, shadow);
      publish(fresh, cached, &gpu[2], &source, 0, 0x3000, shadow);
      cached->fs_root_fragment_snapshot_cpu = NULL;
      select_and_check(fresh, cached, 0, memo, shadow);

      /* Command-buffer reset permits both pointer and VA reuse. */
      memset(fresh, 0, sizeof(*fresh));
      memset(cached, 0, sizeof(*cached));
      publish(fresh, cached, &gpu[0], &source, 0, 0x1000, shadow);
      select_and_check(fresh, cached, 0, memo, shadow);
      fresh->root_cpu = cached->root_cpu = NULL;
      fresh->root = cached->root = 0;
      select_and_check(fresh, cached, 0, memo, shadow);
   }
   printf("root fragment: %" PRIu64 " checks passed\n", checks);
   free(fresh);
   free(cached);
   return 0;
}
