/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include "hk_g17p_b2i_batch.h"

static void
test_image_eligibility(void)
{
   /* Native AGX3 (G16G & G17P) compressed 1x 2D colour image in supported
    * transfer destination layouts. */
   assert(hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G17P, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_GENERAL));

   /* Non-AGX3 chips stay on their existing paths. */
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G13G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G14X, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G18G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));

   /* Uncompressed, meta, in-pass, non-2D, MSAA, multi-plane, depth/stencil,
    * block-compressed, and unsupported layouts are excluded. */
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, false, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, true, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, true, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_3D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_1D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_4_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 2,
      false, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      true, false, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, true, true, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
   assert(!hk_g17p_b2i_batch_image_eligible(
      AGX_CHIP_G16G, false, false, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, 1,
      false, false, true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

static void
test_coalescing_rules(void)
{
   int img_a = 0, img_b = 0;
   const struct hk_deferred_b2i_batch_key base = {
      .image = &img_a,
      .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .mip_level = 0,
      .array_layer = 0,
   };

   /* Empty batch cannot coalesce; matching subresource coalesces up to cap. */
   assert(!hk_g17p_b2i_batch_can_coalesce(&base, 0, &base));
   assert(hk_g17p_b2i_batch_can_coalesce(&base, 1, &base));
   assert(hk_g17p_b2i_batch_can_coalesce(&base, 63, &base));
   assert(hk_g17p_b2i_batch_can_coalesce(&base, HK_MAX_DEFERRED_B2I - 1, &base));
   assert(!hk_g17p_b2i_batch_can_coalesce(&base, HK_MAX_DEFERRED_B2I, &base));

   /* Any change in image, layout, mip level, or array layer forces flush. */
   struct hk_deferred_b2i_batch_key diff_img = base;
   diff_img.image = &img_b;
   assert(!hk_g17p_b2i_batch_can_coalesce(&base, 1, &diff_img));

   struct hk_deferred_b2i_batch_key diff_layout = base;
   diff_layout.layout = VK_IMAGE_LAYOUT_GENERAL;
   assert(!hk_g17p_b2i_batch_can_coalesce(&base, 1, &diff_layout));

   struct hk_deferred_b2i_batch_key diff_mip = base;
   diff_mip.mip_level = 1;
   assert(!hk_g17p_b2i_batch_can_coalesce(&base, 1, &diff_mip));

   struct hk_deferred_b2i_batch_key diff_layer = base;
   diff_layer.array_layer = 1;
   assert(!hk_g17p_b2i_batch_can_coalesce(&base, 1, &diff_layer));
}

static void
test_render_plan_and_load_op(void)
{
   /* 64-tile glyph-atlas pattern (128x128 tiles across 1024x1024): union covers
    * the full mip, but each individual rect is partial, so LOAD must stay on
    * (a sparse subset with the same bounding box would corrupt holes if
    * DONT_CARE were chosen based on union bbox alone). */
   struct hk_deferred_b2i_item corners[2] = {
      {.buf_addr = 0x1000, .row_stride_B = 512, .x = 0, .y = 0, .width = 128, .height = 128},
      {.buf_addr = 0x2000, .row_stride_B = 512, .x = 896, .y = 896, .width = 128, .height = 128},
   };
   struct hk_deferred_b2i_render_plan p =
      hk_g17p_b2i_batch_plan_render(corners, 2, 1024, 1024);
   assert(p.render_area.offset.x == 0 && p.render_area.offset.y == 0);
   assert(p.render_area.extent.width == 1024 &&
          p.render_area.extent.height == 1024);
   assert(p.max_x == 1024 && p.max_y == 1024);
   assert(p.load_op == VK_ATTACHMENT_LOAD_OP_LOAD);

   /* Sub-region cluster computes tight union renderArea and LOAD. */
   struct hk_deferred_b2i_item cluster[3] = {
      {.x = 64, .y = 32, .width = 48, .height = 16},
      {.x = 120, .y = 96, .width = 32, .height = 64},
      {.x = 80, .y = 48, .width = 40, .height = 40},
   };
   p = hk_g17p_b2i_batch_plan_render(cluster, 3, 1024, 1024);
   assert(p.render_area.offset.x == 64 && p.render_area.offset.y == 32);
   assert(p.render_area.extent.width == 88 && p.render_area.extent.height == 128);
   assert(p.max_x == 152 && p.max_y == 160);
   assert(p.load_op == VK_ATTACHMENT_LOAD_OP_LOAD);

   /* Single full-mip upload (even followed by sub-rect patches in the same
    * batch) overwrites every pixel and safely uses DONT_CARE. */
   struct hk_deferred_b2i_item full_then_patch[2] = {
      {.x = 0, .y = 0, .width = 1024, .height = 1024},
      {.x = 16, .y = 16, .width = 32, .height = 32},
   };
   p = hk_g17p_b2i_batch_plan_render(full_then_patch, 2, 1024, 1024);
   assert(p.render_area.offset.x == 0 && p.render_area.offset.y == 0);
   assert(p.render_area.extent.width == 1024 &&
          p.render_area.extent.height == 1024);
   assert(p.load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE);

   /* Non-zero mip level uses minified dimensions for full-mip check. */
   struct hk_deferred_b2i_item mip3 = {.x = 0, .y = 0, .width = 128, .height = 128};
   p = hk_g17p_b2i_batch_plan_render(&mip3, 1, 128, 128);
   assert(p.load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
   p = hk_g17p_b2i_batch_plan_render(&mip3, 1, 256, 256);
   assert(p.load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
}

int
main(void)
{
   test_image_eligibility();
   test_coalescing_rules();
   test_render_plan_and_load_op();
   printf("test-hk-g17p-b2i-batch: PASS\n");
   return 0;
}
