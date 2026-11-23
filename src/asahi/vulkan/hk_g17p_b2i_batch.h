/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "asahi/lib/agx_chip.h"
#include "hk_g17p_meta_copy_core.h"
#include "vulkan/vulkan_core.h"
#include <stdbool.h>
#include <stdint.h>

/* Maximum queued sub-rectangle buffer-to-image copies per deferred batch.
 * Flushing chunks vertex buffers inside a single render pass, so even large
 * glyph-atlas uploads execute in one render pass while bounding host memory. */
#define HK_MAX_DEFERRED_B2I 4096u

struct hk_deferred_b2i_item {
   uint64_t buf_addr;
   uint32_t row_stride_B;
   int32_t x;
   int32_t y;
   uint32_t width;
   uint32_t height;
};

struct hk_deferred_b2i_batch_key {
   const void *image;
   VkImageLayout layout;
   uint32_t mip_level;
   uint32_t array_layer;
};

struct hk_deferred_b2i_render_plan {
   VkRect2D render_area;
   VkAttachmentLoadOp load_op;
   uint32_t max_x;
   uint32_t max_y;
};

/* Eligibility for batched/deferred single-pass compressed 1x 2D colour
 * buffer-to-image uploads on native AGX3 (G16G/G17P). */
static inline bool
hk_g17p_b2i_batch_image_eligible(enum agx_chip chip, bool in_meta, bool in_pass,
                                 VkImageType image_type,
                                 VkSampleCountFlagBits samples,
                                 uint8_t plane_count, bool is_depth_or_stencil,
                                 bool is_block_compressed,
                                 bool layout_compressed,
                                 VkImageLayout dst_layout)
{
   return agx_chip_native_agx3(chip) && !in_meta && !in_pass &&
          image_type == VK_IMAGE_TYPE_2D && samples == VK_SAMPLE_COUNT_1_BIT &&
          plane_count == 1 && !is_depth_or_stencil && !is_block_compressed &&
          layout_compressed &&
          hk_g17p_transfer_dst_layout_supported(dst_layout);
}

static inline bool
hk_g17p_b2i_batch_can_coalesce(const struct hk_deferred_b2i_batch_key *cur,
                               uint32_t cur_count,
                               const struct hk_deferred_b2i_batch_key *next)
{
   return cur_count > 0 && cur_count < HK_MAX_DEFERRED_B2I &&
          cur->image == next->image && cur->layout == next->layout &&
          cur->mip_level == next->mip_level &&
          cur->array_layer == next->array_layer;
}

/* Only a single upload covering the entire mip level makes the prior contents
 * dead; scattered sub-rectangles whose union happens to span the mip still
 * leave untouched pixels between them and must LOAD. */
static inline bool
hk_g17p_b2i_batch_item_covers_mip(const struct hk_deferred_b2i_item *item,
                                  uint32_t mip_width, uint32_t mip_height)
{
   return item->x == 0 && item->y == 0 && item->width == mip_width &&
          item->height == mip_height;
}

static inline struct hk_deferred_b2i_render_plan
hk_g17p_b2i_batch_plan_render(const struct hk_deferred_b2i_item *items,
                              uint32_t count, uint32_t mip_width,
                              uint32_t mip_height)
{
   uint32_t min_x = UINT32_MAX;
   uint32_t min_y = UINT32_MAX;
   uint32_t max_x = 0;
   uint32_t max_y = 0;
   bool full_mip_overwrite = false;

   for (uint32_t i = 0; i < count; ++i) {
      const struct hk_deferred_b2i_item *it = &items[i];
      const uint32_t x0 = (uint32_t)it->x;
      const uint32_t y0 = (uint32_t)it->y;
      const uint32_t x1 = x0 + it->width;
      const uint32_t y1 = y0 + it->height;
      if (x0 < min_x)
         min_x = x0;
      if (y0 < min_y)
         min_y = y0;
      if (x1 > max_x)
         max_x = x1;
      if (y1 > max_y)
         max_y = y1;
      if (hk_g17p_b2i_batch_item_covers_mip(it, mip_width, mip_height))
         full_mip_overwrite = true;
   }

   return (struct hk_deferred_b2i_render_plan){
      .render_area = {
         .offset = {(int32_t)min_x, (int32_t)min_y},
         .extent = {max_x - min_x, max_y - min_y},
      },
      .load_op = full_mip_overwrite ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                    : VK_ATTACHMENT_LOAD_OP_LOAD,
      .max_x = max_x,
      .max_y = max_y,
   };
}
