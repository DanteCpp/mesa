/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Host tests for the v6 primary-write contract in Honeykrisp:
 *  (a) BDA buffer transfers accepted under v6 while preserving BDA sync hazards
 *  (b) sparse/ambiguous buffer and image transfers refused as transfer-dst-unknown
 *  (c) hk_CmdCopyImageToBuffer2 tight region bounds calculation
 *  (d) hk_own_timestamps query pool BO non-shared + query helpers proving
 *      private writes without triggering compute-no-scope / helper-unproven
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/ralloc.h"
#include "hk_buffer.h"
#include "hk_cmd_buffer.h"
#include "hk_device.h"
#include "hk_image.h"
#include "hk_query_pool.h"

static unsigned checks, failures;
static void *test_ctx;

#define CHECK(cond)                                                            \
   do {                                                                        \
      checks++;                                                                \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
         failures++;                                                           \
      }                                                                        \
   } while (0)

static struct hk_cs *precomp_target_cs;
static unsigned precomp_calls;

/* Stubs required only by unused paths in hk_sync.c. */
struct hk_cs *
hk_cmd_buffer_new_cs(struct hk_cmd_buffer *cmd, bool compute)
{
   (void)cmd;
   (void)compute;
   abort();
}

void
hk_dispatch_imm_writes(struct hk_cmd_buffer *cmd, struct hk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

bool
hk_defer_oq_available(struct hk_cmd_buffer *cmd, struct hk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

void
hk_cdm_end_stream_flush(struct hk_cmd_buffer *cmd, struct hk_cs *cs)
{
   (void)cmd;
   (void)cs;
}

bool
hk_cs_init_graphics(struct hk_cmd_buffer *cmd, struct hk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

void
hk_tess_batch_flush(struct hk_cmd_buffer *cmd)
{
   (void)cmd;
   abort();
}

void
hk_g17p_meta_fill_addr(struct hk_cmd_buffer *cmd, struct hk_cs **slot,
                       uint64_t address, uint64_t size, uint32_t value)
{
   (void)cmd;
   (void)slot;
   (void)address;
   (void)size;
   (void)value;
   abort();
}

void
hk_flush_query_resets(struct hk_cmd_buffer *cmd, struct hk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

struct agx_ptr
hk_pool_alloc_from_slow(struct hk_cmd_buffer *cmd, struct hk_uploader *uploader,
                         uint32_t size, uint32_t alignment,
                         enum hk_cmd_bo_type type)
{
   (void)cmd;
   (void)uploader;
   (void)size;
   (void)alignment;
   (void)type;
   abort();
}

struct agx_ptr
hk_pool_cs_alloc(struct hk_cmd_buffer *cmd, uint32_t size, uint32_t alignment)
{
   (void)cmd;
   (void)size;
   (void)alignment;
   abort();
}

void
hk_drm_attachment_set_init(struct hk_drm_attachment_set *set)
{
   (void)set;
   abort();
}

void
hk_flush_deferred_clears_why(struct hk_cmd_buffer *cmd, const char *why)
{
   (void)cmd;
   (void)why;
   abort();
}

void
hk_dispatch_precomp(struct hk_cmd_buffer *cmd, struct agx_grid grid,
                    enum agx_barrier barrier, enum libagx_program idx,
                    void *data, size_t data_size)
{
   (void)grid;
   (void)barrier;
   (void)idx;
   (void)data;
   (void)data_size;
   precomp_calls++;
   if (precomp_target_cs)
      hk_primary_note_compute(cmd, precomp_target_cs);
}

static struct hk_cmd_buffer *
new_v6_cmd(void)
{
   struct hk_device *dev = rzalloc(test_ctx, struct hk_device);
   dev->dev.chip = AGX_CHIP_G17G;
   dev->dev.primary_write_contract = true;
   struct hk_cmd_buffer *cmd = rzalloc(test_ctx, struct hk_cmd_buffer);
   cmd->vk.base.device = &dev->vk;
   list_inithead(&cmd->control_streams);
   return cmd;
}

static struct hk_cs *
new_cdm_cs(struct hk_cmd_buffer *cmd)
{
   struct hk_cs *cs = rzalloc(test_ctx, struct hk_cs);
   cs->cmd = cmd;
   cs->type = HK_CS_CDM;
   hk_sync_stream_created(cmd, cs, &cmd->current_cs.cs, true);
   list_addtail(&cs->node, &cmd->control_streams);
   return cs;
}

static void
test_bda_buffer_transfer_accepted(void)
{
   struct hk_cmd_buffer *cmd = new_v6_cmd();
   struct hk_cs *cs = new_cdm_cs(cmd);

   /* DXVK sets VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT on every slab. */
   struct hk_buffer bda_private = {
      .vk = {
         .usage = VK_BUFFER_USAGE_2_TRANSFER_DST_BIT |
                  VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
         .size = UINT64_C(64) << 20,
         .device_address = 0x1500000000ull,
      },
      .va = NULL,
      .primary_private = true,
   };
   CHECK(!hk_primary_buffer_ambiguous(&bda_private));

   const uint64_t base = vk_buffer_address(&bda_private.vk, 0);
   const bool device_address =
      (bda_private.vk.usage & VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT) ||
      bda_private.va != NULL;
   const bool ambiguous = hk_primary_buffer_ambiguous(&bda_private);
   CHECK(device_address);
   CHECK(!ambiguous);

   /* Private non-shared BDA buffer: accepted under v6 with 0 non-primary
    * pages declared, while hk_sync_note_xfer_write still treats it as BDA. */
   for (unsigned i = 0; i < AGX_PRIMARY_WRITE_RANGES + 5; ++i) {
      const uint64_t off = (uint64_t)i * 0x10000u;
      hk_sync_xfer_begin_dst(cmd, base + off, base + off + 0x4000u,
                             device_address, ambiguous,
                             !ambiguous && bda_private.primary_private);
      hk_sync_note_xfer_write(cmd, cs);
      hk_primary_note_compute(cmd, cs);
      hk_sync_xfer_end(cmd);
   }
   CHECK(cmd->sync.xfer_any == cs);
   CHECK(cs->primary_writes.observed);
   CHECK(!cs->primary_writes.incomplete);
   CHECK(cs->primary_writes.causes == 0);
   CHECK(cs->primary_writes.count == 0);

   /* Shared/shareable BDA buffer (primary_private == false): accepted under
    * v6 and declares its exact tight VA range. */
   struct hk_cs *cs_shared = new_cdm_cs(cmd);
   struct hk_buffer bda_shared = bda_private;
   bda_shared.primary_private = false;
   hk_sync_xfer_begin_dst(cmd, base + 0x4000u, base + 0x14000u, true,
                          hk_primary_buffer_ambiguous(&bda_shared),
                          bda_shared.primary_private);
   hk_primary_note_compute(cmd, cs_shared);
   hk_sync_xfer_end(cmd);
   CHECK(cs_shared->primary_writes.observed);
   CHECK(!cs_shared->primary_writes.incomplete);
   CHECK(cs_shared->primary_writes.causes == 0);
   CHECK(cs_shared->primary_writes.count == 1);
   CHECK(cs_shared->primary_writes.ranges[0].address == base + 0x4000u);
   CHECK(cs_shared->primary_writes.ranges[0].size == 0x10000u);
}

static void
test_sparse_and_ambiguous_transfers_refused(void)
{
   struct hk_cmd_buffer *cmd = new_v6_cmd();

   /* Sparse buffer via buffer->va != NULL must be refused as transfer-dst-unknown. */
   struct agx_va dummy_va = {.addr = 0x1600000000ull, .size_B = 0x100000u};
   struct hk_buffer sparse_va_buf = {
      .vk = {
         .usage = VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
         .size = 0x100000u,
         .device_address = dummy_va.addr,
      },
      .va = &dummy_va,
      .primary_private = false,
   };
   CHECK(hk_primary_buffer_ambiguous(&sparse_va_buf));
   struct hk_cs *cs1 = new_cdm_cs(cmd);
   hk_sync_xfer_begin_dst(cmd, dummy_va.addr, dummy_va.addr + 0x4000u, true,
                          hk_primary_buffer_ambiguous(&sparse_va_buf), false);
   hk_primary_note_compute(cmd, cs1);
   hk_sync_xfer_end(cmd);
   CHECK(cs1->primary_writes.incomplete);
   CHECK(cs1->primary_writes.causes == AGX_PRIMARY_WRITE_CAUSE_TRANSFER_DST);

   /* Sparse create flags without va still mark the buffer ambiguous. */
   const VkBufferCreateFlags sparse_flags[] = {
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT,
      VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
      VK_BUFFER_CREATE_SPARSE_ALIASED_BIT,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(sparse_flags); ++i) {
      struct hk_buffer sb = {
         .vk = {
            .create_flags = sparse_flags[i],
            .size = 0x10000u,
            .device_address = 0x1610000000ull,
         },
         .primary_private = true, /* ambiguity must still win and fail closed */
      };
      CHECK(hk_primary_buffer_ambiguous(&sb));
      struct hk_cs *cs = new_cdm_cs(cmd);
      hk_sync_xfer_begin_dst(cmd, 0x1610000000ull, 0x1610004000ull, false,
                             hk_primary_buffer_ambiguous(&sb), false);
      hk_primary_note_compute(cmd, cs);
      hk_sync_xfer_end(cmd);
      CHECK(cs->primary_writes.incomplete);
      CHECK(cs->primary_writes.causes == AGX_PRIMARY_WRITE_CAUSE_TRANSFER_DST);
   }

   /* Ambiguous image (aliased/sparse) is also refused as transfer-dst-unknown. */
   struct hk_image alias_img = {
      .vk = {.create_flags = VK_IMAGE_CREATE_ALIAS_BIT},
   };
   CHECK(hk_primary_image_ambiguous(&alias_img));
   struct hk_cs *cs_img = new_cdm_cs(cmd);
   hk_sync_xfer_begin_dst(cmd, 0x1700000000ull, 0x1700010000ull, false,
                          hk_primary_image_ambiguous(&alias_img), false);
   hk_primary_note_compute(cmd, cs_img);
   hk_sync_xfer_end(cmd);
   CHECK(cs_img->primary_writes.incomplete);
   CHECK(cs_img->primary_writes.causes == AGX_PRIMARY_WRITE_CAUSE_TRANSFER_DST);

   /* Dispatch outside any transfer or private helper scope: compute-no-scope. */
   struct hk_cs *cs_raw = new_cdm_cs(cmd);
   hk_primary_note_compute(cmd, cs_raw);
   CHECK(cs_raw->primary_writes.incomplete);
   CHECK(cs_raw->primary_writes.causes == AGX_PRIMARY_WRITE_CAUSE_COMPUTE_NO_SCOPE);
}

static void
test_copy_image_to_buffer_tight_bounds(void)
{
   const uint64_t slab_size = UINT64_C(256) << 20;
   uint64_t lo = 0, hi = 0;

   /* Small 128x128 RGBA8 copy at 2 MiB offset inside a 256 MiB DXVK slab. */
   CHECK(agx_copy_image_to_buffer_bounds(
      UINT64_C(2) << 20, 0, 0, 128, 128, 1, 1, 1, 1, 1, 4, slab_size, &lo, &hi));
   CHECK(lo == (UINT64_C(2) << 20));
   CHECK(hi == (UINT64_C(2) << 20) + 65536u);
   /* Page count is 4 (<< 16384 validator cap), never the whole 256 MiB slab. */
   const uint64_t pages = ((hi - 1u) >> 14) - (lo >> 14) + 1u;
   CHECK(pages == 4u);

   /* Padded rows and multiple layers. */
   CHECK(agx_copy_image_to_buffer_bounds(
      0x1000u, 256, 128, 200, 100, 1, 2, 1, 1, 1, 4, slab_size, &lo, &hi));
   CHECK(lo == 0x1000u);
   CHECK(hi == 0x1000u + (128u * 256u * 4u) + (99u * 256u * 4u) + (200u * 4u));

   /* Compressed 4x4 block format with non-multiple-of-4 mip extent. */
   CHECK(agx_copy_image_to_buffer_bounds(
      0, 0, 0, 3, 5, 1, 1, 4, 4, 1, 16, slab_size, &lo, &hi));
   CHECK(lo == 0u);
   CHECK(hi == 32u);

   /* Overflowing strides clamp safely to buffer size. */
   CHECK(agx_copy_image_to_buffer_bounds(
      64, UINT32_MAX, UINT32_MAX, 1024, 1024, 16, 1, 1, 1, 1, 16,
      slab_size, &lo, &hi));
   CHECK(lo == 64u);
   CHECK(hi == slab_size);
}

static void
test_query_pool_own_timestamps_and_helpers(void)
{
   /* When hk_own_timestamps() is true, timestamp and stamped occlusion pools
    * must NOT set AGX_BO_SHARED so hk_primary_private_proof succeeds. */
   const enum agx_bo_flags own_ts_flags =
      hk_query_pool_bo_flags(true, false, true);
   const enum agx_bo_flags own_oq_flags =
      hk_query_pool_bo_flags(false, true, true);
   const enum agx_bo_flags kernel_ts_flags =
      hk_query_pool_bo_flags(true, false, false);
   const enum agx_bo_flags kernel_oq_flags =
      hk_query_pool_bo_flags(false, true, false);

   CHECK((own_ts_flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE)) == 0);
   CHECK((own_oq_flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE)) == 0);
   CHECK((kernel_ts_flags & AGX_BO_SHARED) != 0);
   CHECK((kernel_oq_flags & AGX_BO_SHARED) != 0);

   struct agx_va pool_va = {.addr = 0x1800000000ull, .size_B = 0x4000u};
   struct agx_bo private_pool_bo = {
      .va = &pool_va,
      .size = 0x4000u,
      .flags = own_ts_flags,
   };
   struct agx_bo shared_pool_bo = {
      .va = &pool_va,
      .size = 0x4000u,
      .flags = kernel_ts_flags,
   };

   CHECK(hk_primary_private_bo(&private_pool_bo));
   CHECK(hk_primary_private_proof(&private_pool_bo, pool_va.addr + 16, 8));
   CHECK(!hk_primary_private_proof(&private_pool_bo, pool_va.addr + 0x4000u, 8));
   CHECK(!hk_primary_private_bo(&shared_pool_bo));
   CHECK(!hk_primary_private_proof(&shared_pool_bo, pool_va.addr + 16, 8));

   /* Pipeline statistic destination proof accepts disabled (0 / scratch) and
    * in-bounds private query pool BO addresses, rejecting shared/OOB. */
   CHECK(hk_primary_stat_proof(NULL, 0));
   CHECK(hk_primary_stat_proof(NULL, AGX_SCRATCH_PAGE_ADDRESS));
   CHECK(hk_primary_stat_proof(&private_pool_bo, pool_va.addr + 64));
   CHECK(!hk_primary_stat_proof(&shared_pool_bo, pool_va.addr + 64));
   CHECK(!hk_primary_stat_proof(&private_pool_bo, pool_va.addr + 0x4000u));

   /* Proven helper scope produces observed=1, incomplete=0, causes=0, count=0. */
   struct hk_cmd_buffer *cmd = new_v6_cmd();
   struct hk_cs *cs_ok = new_cdm_cs(cmd);
   hk_primary_private_begin(
      cmd, hk_primary_private_proof(&private_pool_bo, pool_va.addr + 16, 8));
   hk_primary_note_compute(cmd, cs_ok);
   hk_primary_private_end(cmd);
   CHECK(cs_ok->primary_writes.observed);
   CHECK(!cs_ok->primary_writes.incomplete);
   CHECK(cs_ok->primary_writes.causes == 0);
   CHECK(cs_ok->primary_writes.count == 0);
   CHECK(cmd->primary_private.depth == 0);
   CHECK(!cmd->primary_private.unproven);

   /* Unproven nested helper scope refuses as helper-unproven, not compute-no-scope. */
   struct hk_cs *cs_bad = new_cdm_cs(cmd);
   hk_primary_private_begin(cmd, true);
   hk_primary_private_begin(
      cmd, hk_primary_private_proof(&shared_pool_bo, pool_va.addr + 16, 8));
   hk_primary_note_compute(cmd, cs_bad);
   hk_primary_private_end(cmd);
   hk_primary_private_end(cmd);
   CHECK(cs_bad->primary_writes.incomplete);
   CHECK(cs_bad->primary_writes.causes == AGX_PRIMARY_WRITE_CAUSE_HELPER_UNPROVEN);
   CHECK(cmd->primary_private.depth == 0);
   CHECK(!cmd->primary_private.unproven);

   /* Real hk_sync_write_timestamp() second end-stamp in a render pass must
    * bracket libagx_copy_timestamp with a proven private scope using req.bo. */
   cmd = new_v6_cmd();
   struct hk_cs *gfx = rzalloc(test_ctx, struct hk_cs);
   gfx->cmd = cmd;
   gfx->type = HK_CS_VDM;
   hk_sync_begin_rendering(cmd, false);
   hk_sync_stream_created(cmd, gfx, &cmd->current_cs.gfx, false);
   list_addtail(&gfx->node, &cmd->control_streams);
   cmd->current_cs.gfx = gfx;
   gfx->timestamp.end = (struct agx_timestamp_req){
      .addr = pool_va.addr,
      .bo = &private_pool_bo,
      .handle = 1,
      .offset_B = 0,
   };

   struct hk_cs *ts_cs = new_cdm_cs(cmd);
   precomp_target_cs = ts_cs;
   precomp_calls = 0;
   const struct agx_timestamp_req second = {
      .addr = pool_va.addr + 8,
      .bo = &private_pool_bo,
      .handle = 1,
      .offset_B = 8,
   };
   hk_sync_write_timestamp(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, second);
   precomp_target_cs = NULL;
   CHECK(precomp_calls == 1);
   CHECK(ts_cs->primary_writes.observed);
   CHECK(!ts_cs->primary_writes.incomplete);
   CHECK(ts_cs->primary_writes.causes == 0);
   CHECK(cmd->primary_private.depth == 0);
}

int
main(void)
{
   test_ctx = ralloc_context(NULL);
   test_bda_buffer_transfer_accepted();
   test_sparse_and_ambiguous_transfers_refused();
   test_copy_image_to_buffer_tight_bounds();
   test_query_pool_own_timestamps_and_helpers();
   printf("hk_primary_write: %u checks, %u failures\n", checks, failures);
   ralloc_free(test_ctx);
   return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
