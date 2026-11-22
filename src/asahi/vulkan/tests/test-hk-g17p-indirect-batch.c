/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asahi/lib/agx_bytes.h"
#include "asahi/lib/agx_m5_va.h"
#include "asahi/lib/agx_va_runtime_hole.h"
#include "asahi/lib/agx_vdm_state_core.h"
#include "asahi/lib/agx_vdm_stream_link_core.h"
#include "hk_buffer.h"
#include "hk_cmd_buffer.h"
#include "hk_cmd_pool.h"
#include "hk_device.h"
#include "hk_entrypoints.h"
#include "vk_alloc.h"

#define CHECK(c)                                                               \
   do {                                                                        \
      if (!(c)) {                                                              \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
         abort();                                                              \
      }                                                                        \
   } while (0)

#define MAX_TRACKED_BOS 256

struct mock_bo_rec {
   struct agx_bo *bo;
   uint64_t gpu;
   uint64_t alias;
   uint8_t *cpu;
   size_t size;
   bool is_ioto;
};

static struct mock_bo_rec g_bos[MAX_TRACKED_BOS];
static unsigned g_bo_count;
static unsigned g_bo_live;
static unsigned g_main_bo_creates;
static unsigned g_ioto_bo_creates;
static uint64_t g_next_main_va;
static uint64_t g_next_ioto_va;
static uint64_t g_next_main_va_override;

static void
mock_reset(uint64_t shader_base)
{
   CHECK(g_bo_live == 0);
   memset(g_bos, 0, sizeof(g_bos));
   g_bo_count = 0;
   g_main_bo_creates = 0;
   g_ioto_bo_creates = 0;
   g_next_main_va = AGX_M5_DRIVER_BASE + UINT64_C(0x1000000);
   g_next_ioto_va = shader_base + UINT64_C(0x100000);
   g_next_main_va_override = 0;
}

__typeof__(agx_bo_create) __wrap_agx_bo_create;
__typeof__(agx_bo_unreference) __wrap_agx_bo_unreference;
__typeof__(agx_bo_bind) __wrap_agx_bo_bind;

struct agx_bo *
__wrap_agx_bo_create(struct agx_device *dev, size_t size, unsigned align,
                     enum agx_bo_flags flags, const char *label)
{
   struct agx_bo *bo = calloc(1, sizeof(*bo));
   CHECK(bo);
   bo->size = size;
   bo->flags = flags;
   bo->dev = dev;
   bo->refcnt = 1;
   bo->label = label;
   bo->_map = calloc(1, size);
   CHECK(bo->_map);
   bo->va = calloc(1, sizeof(*bo->va));
   CHECK(bo->va);
   bo->va->size_B = size;

   const bool is_ioto = (flags & AGX_BO_LOW_VA) != 0;
   const uint64_t step = ALIGN_POT((uint64_t)size, UINT64_C(0x10000));
   if (is_ioto) {
      bo->va->addr = g_next_ioto_va;
      g_next_ioto_va += step;
      ++g_ioto_bo_creates;
   } else {
      if (g_next_main_va_override) {
         bo->va->addr = g_next_main_va_override;
         g_next_main_va_override = 0;
      } else {
         bo->va->addr = g_next_main_va;
         g_next_main_va += step;
      }
      ++g_main_bo_creates;
   }

   CHECK(g_bo_count < MAX_TRACKED_BOS);
   g_bos[g_bo_count++] = (struct mock_bo_rec){
      .bo = bo,
      .gpu = bo->va->addr,
      .cpu = bo->_map,
      .size = size,
      .is_ioto = is_ioto,
   };
   ++g_bo_live;
   return bo;
}

void
__wrap_agx_bo_unreference(struct agx_device *dev, struct agx_bo *bo)
{
   if (!bo)
      return;
   if (!p_atomic_dec_zero(&bo->refcnt))
      return;
   CHECK(g_bo_live > 0);
   --g_bo_live;
   free(bo->_map);
   free(bo->va);
   free(bo);
}

int
__wrap_agx_bo_bind(struct agx_device *dev, struct agx_bo *bo, uint64_t address,
                   size_t size, uint64_t offset, uint32_t flags)
{
   if (!(flags & DRM_ASAHI_BIND_UNBIND)) {
      for (unsigned i = 0; i < g_bo_count; ++i) {
         if (g_bos[i].bo == bo) {
            g_bos[i].alias = address;
            break;
         }
      }
   }
   return 0;
}

struct fixture {
   struct hk_device dev;
   struct hk_cmd_pool pool;
   struct hk_cmd_buffer cmd;
   struct hk_cs cs;
   struct hk_buffer indirect;
   struct hk_api_shader *vs_api;
   struct hk_api_shader *fs_api;
   struct hk_linked_shader linked_vs;
   struct hk_linked_shader linked_fs;
   struct agx_va vs_va;
   struct agx_bo vs_bo;
   struct agx_bo ioto_bo;
   uint8_t *vdm_map;
   size_t vdm_size;
   uint8_t *ioto_map;
   size_t ioto_size;
   uint64_t ioto_init_base;
   uint8_t *main_map;
   size_t main_size;
   uint64_t main_init_base;
   bool has_alias_heap;
};

static void
fixture_init_sized(struct fixture *f, enum agx_chip chip,
                   uint64_t indirect_addr, bool uses_base_param,
                   bool uses_draw_id, size_t vdm_size, size_t ioto_size,
                   size_t main_size)
{
   memset(f, 0, sizeof(*f));
   f->vdm_size = vdm_size;
   f->ioto_size = ioto_size;
   f->main_size = main_size;
   f->vdm_map = calloc(1, vdm_size);
   f->ioto_map = calloc(1, ioto_size);
   f->main_map = calloc(1, main_size);
   CHECK(f->vdm_map && f->ioto_map && f->main_map);

   f->dev.dev.chip = chip;
   f->dev.dev.shader_base = UINT64_C(0x100000000);
   f->dev.vk.base.type = VK_OBJECT_TYPE_DEVICE;
   f->dev.vk.alloc = *vk_default_allocator();
   simple_mtx_init(&f->dev.dev.vma_lock, mtx_plain);
   simple_mtx_init(&f->dev.dev.bo_mmap_lock, mtx_plain);
   simple_mtx_init(&f->dev.vdm.lock, mtx_plain);
   for (unsigned c = 0; c < HK_CMD_SLAB_COUNT; ++c) {
      list_inithead(&f->dev.vdm.slabs[c]);
      list_inithead(&f->dev.vdm.full[c]);
   }
   if (agx_chip_native_agx3(chip)) {
      CHECK(agx_g17p_vdm_alias_heap_init(&f->dev.dev.g17p_vdm_alias_heap));
      f->has_alias_heap = true;
   }

   f->pool.vk.base.device = &f->dev.vk;
   f->pool.vk.base.type = VK_OBJECT_TYPE_COMMAND_POOL;
   f->pool.vk.alloc = f->dev.vk.alloc;
   for (enum hk_cmd_bo_type t = 0; t < HK_CMD_BO_TYPE_COUNT; ++t) {
      list_inithead(&f->pool.free_bos[t]);
      list_inithead(&f->pool.free_spans[t]);
   }

   f->cmd.vk.base.device = &f->dev.vk;
   f->cmd.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   f->cmd.vk.pool = &f->pool.vk;
   f->cmd.current_cs.gfx = &f->cs;
   list_inithead(&f->cmd.uploader.main.bos);
   list_inithead(&f->cmd.uploader.streams.bos);
   list_inithead(&f->cmd.uploader.usc.bos);
   list_inithead(&f->cmd.uploader.root.bos);
   list_inithead(&f->cmd.vdm_allocations);
   list_inithead(&f->cmd.control_streams);
   list_inithead(&f->cmd.alias_link);
   util_dynarray_init(&f->cmd.large_bos, NULL);
   util_dynarray_init(&f->cmd.alias_sites, NULL);

   f->main_init_base = AGX_M5_DRIVER_BASE + UINT64_C(0x300000);
   f->cmd.uploader.main.map = f->main_map;
   f->cmd.uploader.main.base = f->main_init_base;
   f->cmd.uploader.main.capacity = (uint32_t)main_size;

   f->ioto_init_base = f->dev.dev.shader_base + 0x40000;
   f->ioto_bo.dev = &f->dev.dev;
   f->ioto_bo.size = ioto_size;
   f->ioto_bo.refcnt = 1;
   f->cmd.ioto.bo = &f->ioto_bo;
   f->cmd.ioto.map = f->ioto_map;
   f->cmd.ioto.base = f->ioto_init_base;
   f->cmd.ioto.capacity = (uint32_t)ioto_size;

   f->cs.cmd = &f->cmd;
   f->cs.type = HK_CS_VDM;
   f->cs.start = f->vdm_map;
   f->cs.current = f->vdm_map;
   f->cs.end = f->vdm_map + vdm_size;
   f->cs.agx3_active_context_selector_valid = true;
   f->cs.agx3_raw_active_context_selector = 0;

   /* Pre-populate scissor, depth bias, and cached draw-state offset. */
   uint8_t zero_scissor[AGX_SCISSOR_LENGTH] = {0};
   uint8_t zero_bias[AGX_DEPTH_BIAS_LENGTH] = {0};
   util_dynarray_init(&f->cs.scissor, NULL);
   util_dynarray_init(&f->cs.depth_bias, NULL);
   memcpy(util_dynarray_grow_bytes(&f->cs.scissor, 1, AGX_SCISSOR_LENGTH),
          zero_scissor, AGX_SCISSOR_LENGTH);
   memcpy(util_dynarray_grow_bytes(&f->cs.depth_bias, 1, AGX_DEPTH_BIAS_LENGTH),
          zero_bias, AGX_DEPTH_BIAS_LENGTH);
   f->cs.g17p_draw_state.offset = 0x100;
   f->cs.g17p_draw_state.scissor_size = f->cs.scissor.size;
   f->cs.g17p_draw_state.depth_bias_size = f->cs.depth_bias.size;
   f->cs.g17p_draw_state.viewport_count = 1;

   f->vs_api = calloc(1, hk_api_shader_size(MESA_SHADER_VERTEX, false));
   f->fs_api = calloc(1, hk_api_shader_size(MESA_SHADER_FRAGMENT, false));
   CHECK(f->vs_api && f->fs_api);
   f->vs_api->vk.stage = MESA_SHADER_VERTEX;
   f->fs_api->vk.stage = MESA_SHADER_FRAGMENT;

   struct hk_shader *hw_vs = &f->vs_api->variants[HK_VS_VARIANT_HW];
   hw_vs->compiled = true;
   hw_vs->info.stage = MESA_SHADER_VERTEX;
   hw_vs->info.g17p_default_layer_zero = true;
   hw_vs->b.info.uses_base_param = uses_base_param;
   hw_vs->b.info.uses_draw_id = uses_draw_id;
   hw_vs->b.info.push_count = 16;
   hw_vs->agx3_vdm_state_update.source_layout =
      AGX_VDM_STATE_UPDATE_SOURCE_LAYOUT_AGX3_VERTEX_PROGRAM_VARIANT;
   hw_vs->agx3_vdm_state_update.template_info.raw_state_load_count_12 = 1;
   hw_vs->agx3_vdm_state_update.template_info.raw_state_load_count_18 = 1;
   hw_vs->agx3_vdm_state_update.template_info.raw_vs_output_count_0 = 4;
   hw_vs->agx3_vdm_state_update.template_info.raw_vs_output_count_2 = 1;
   hw_vs->agx3_vdm_state_update.template_info.raw_vs_output_selector = 1;

   struct hk_shader *fs = &f->fs_api->variants[0];
   fs->compiled = true;
   fs->info.stage = MESA_SHADER_FRAGMENT;

   f->vs_va.addr = f->dev.dev.shader_base + 0x20000;
   f->vs_bo.dev = &f->dev.dev;
   f->vs_bo.va = &f->vs_va;
   f->vs_bo.size = 4096;
   f->vs_bo.refcnt = 1;
   f->linked_vs.b.bo = &f->vs_bo;
   f->linked_vs.b.size = 64;
   f->linked_vs.b.nr_gprs = 8;
   f->linked_vs.b.uses_base_param = uses_base_param;
   f->linked_vs.b.g17p_compiler_d_valid = true;
   f->linked_vs.b.g17p_compiler_d = 0x10;
   hw_vs->only_linked = &f->linked_vs;

   struct hk_graphics_state *gfx = &f->cmd.state.gfx;
   gfx->shaders[MESA_SHADER_VERTEX] = f->vs_api;
   gfx->shaders[MESA_SHADER_FRAGMENT] = f->fs_api;
   gfx->linked[MESA_SHADER_VERTEX] = &f->linked_vs;
   gfx->linked[MESA_SHADER_FRAGMENT] = &f->linked_fs;
   gfx->fast_link_vs.valid = true;
   gfx->fast_link_fs.valid = true;
   gfx->topology = AGX_PRIMITIVE_TRIANGLES;
   gfx->root = UINT64_C(0x200000);
   gfx->index.buffer.addr = UINT64_C(0x400000);
   gfx->index.buffer.range = 4096;
   gfx->index.size = AGX_INDEX_SIZE_U16;
   f->cmd.vk.dynamic_graphics_state.ia.primitive_topology =
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   f->cmd.vk.dynamic_graphics_state.vp.viewport_count = 1;

   /* Prepare a matching native bind tail for HAL200 tests. */
   struct hk_g17p_prepared_bind_tail *tail = &gfx->g17p_bind_tail;
   tail->valid = true;
   tail->root = gfx->root;
   tail->fragment_shader = fs;
   tail->shaders[MESA_SHADER_VERTEX] = f->vs_api;
   tail->shaders[MESA_SHADER_FRAGMENT] = f->fs_api;
   tail->linked_vs = &f->linked_vs;
   tail->linked_fs = &f->linked_fs;
   tail->topology = gfx->topology;
   tail->viewport_count = 1;
   tail->tail.length = 32;
   for (unsigned i = 0; i < tail->tail.length; ++i)
      tail->tail.bytes[i] = (uint8_t)(0xa0 + i);

   f->indirect.vk.base.type = VK_OBJECT_TYPE_BUFFER;
   f->indirect.vk.device_address = indirect_addr;
   f->indirect.vk.size = 16 * 1024 * 1024;
}

static void
fixture_init(struct fixture *f, enum agx_chip chip, uint64_t indirect_addr,
             bool uses_base_param, bool uses_draw_id)
{
   fixture_init_sized(f, chip, indirect_addr, uses_base_param, uses_draw_id,
                      64 * 1024, 128 * 1024, HK_CMD_BO_SIZE);
}

static void
free_cmd_bo_list(struct fixture *f, struct list_head *bos)
{
   list_for_each_entry_safe(struct hk_cmd_bo, bo, bos, link) {
      list_del(&bo->link);
      if (bo->span)
         hk_device_free_span(&f->dev, bo->span);
      else
         agx_bo_unreference(&f->dev.dev, bo->bo);
      vk_free(&f->dev.vk.alloc, bo);
   }
}

static void
fixture_finish(struct fixture *f)
{
   hk_g17p_linked_graphics_cache_finish(&f->dev, &f->linked_vs);
   free_cmd_bo_list(f, &f->cmd.uploader.main.bos);
   free_cmd_bo_list(f, &f->cmd.uploader.streams.bos);
   free_cmd_bo_list(f, &f->cmd.uploader.usc.bos);
   free_cmd_bo_list(f, &f->cmd.uploader.root.bos);
   hk_device_finish_vdm_slabs(&f->dev);
   util_dynarray_foreach(&f->cmd.large_bos, struct agx_bo *, bo)
      agx_bo_unreference(&f->dev.dev, *bo);
   util_dynarray_fini(&f->cmd.large_bos);
   util_dynarray_fini(&f->cmd.alias_sites);
   util_dynarray_fini(&f->cs.scissor);
   util_dynarray_fini(&f->cs.depth_bias);
   if (f->has_alias_heap)
      util_vma_heap_finish(&f->dev.dev.g17p_vdm_alias_heap);
   simple_mtx_destroy(&f->dev.vdm.lock);
   simple_mtx_destroy(&f->dev.dev.bo_mmap_lock);
   simple_mtx_destroy(&f->dev.dev.vma_lock);
   free(f->vs_api);
   free(f->fs_api);
   free(f->vdm_map);
   free(f->ioto_map);
   free(f->main_map);
}

static void
issue_draw(struct fixture *f, bool indexed, VkDeviceSize offset,
           uint32_t draw_count, uint32_t stride)
{
   VkCommandBuffer cb = hk_cmd_buffer_to_handle(&f->cmd);
   VkBuffer buf = hk_buffer_to_handle(&f->indirect);
   if (indexed)
      hk_CmdDrawIndexedIndirect(cb, buf, offset, draw_count, stride);
   else
      hk_CmdDrawIndirect(cb, buf, offset, draw_count, stride);
   CHECK(!vk_command_buffer_has_error(&f->cmd.vk));
}

static bool
va_in_ioto_arenas(const struct fixture *f, uint64_t pipeline)
{
   if (pipeline >= f->ioto_init_base &&
       pipeline < f->ioto_init_base + f->ioto_size)
      return true;
   for (unsigned i = 0; i < g_bo_count; ++i) {
      if (g_bos[i].is_ioto && pipeline >= g_bos[i].gpu &&
          pipeline < g_bos[i].gpu + g_bos[i].size)
         return true;
   }
   return false;
}

static const uint8_t *
resolve_gpu_ptr(const struct fixture *f, uint64_t gpu, size_t *avail_out)
{
   if (gpu >= f->main_init_base && gpu < f->main_init_base + f->main_size) {
      const size_t off = (size_t)(gpu - f->main_init_base);
      if (avail_out)
         *avail_out = f->main_size - off;
      return f->main_map + off;
   }
   for (unsigned i = 0; i < g_bo_count; ++i) {
      if (!g_bos[i].is_ioto && gpu >= g_bos[i].gpu &&
          gpu < g_bos[i].gpu + g_bos[i].size) {
         const size_t off = (size_t)(gpu - g_bos[i].gpu);
         if (avail_out)
            *avail_out = g_bos[i].size - off;
         return g_bos[i].cpu + off;
      }
   }
   return NULL;
}

static const uint8_t *
resolve_vdm_link_target(const struct fixture *f, enum agx_chip chip,
                        uint64_t target, size_t *chunk_size_out)
{
   const uint64_t base =
      agx_chip_native_agx3(chip) ? AGX_G17P_VDM_USER_BASE : AGX_M5_DRIVER_BASE;
   const uint64_t full = base + target;
   size_t avail = 0;
   const uint8_t *p = resolve_gpu_ptr(f, full, &avail);
   if (p && avail >= 65536) {
      *chunk_size_out = 65536;
      return p;
   }

   if (agx_chip_native_agx3(chip)) {
      for (unsigned i = 0; i < g_bo_count; ++i) {
         if (!g_bos[i].is_ioto && g_bos[i].alias &&
             full >= g_bos[i].alias && full < g_bos[i].alias + g_bos[i].size) {
            const size_t off = (size_t)(full - g_bos[i].alias);
            *chunk_size_out = 65536;
            CHECK(off + *chunk_size_out <= g_bos[i].size);
            return g_bos[i].cpu + off;
         }
      }
   }
   return NULL;
}

static void
verify_vdm_stream(const struct fixture *f, enum agx_chip chip, bool indexed,
                  uint64_t base, uint32_t draws, uint32_t stride,
                  size_t rec_len, unsigned *links_seen_out)
{
   const size_t indirect_off =
      rec_len - (indexed ? (agx_chip_native_agx3(chip) ? 16u : 20u)
                         : (agx_chip_native_agx3(chip) ? 8u : 12u));
   const uint8_t *chunk = f->vdm_map;
   size_t chunk_size = f->vdm_size;
   size_t off = 0;
   unsigned links_seen = 0;

   for (uint32_t i = 0; i < draws; ++i) {
      if (!hk_cs_space_fits(chunk_size - off, HK_GFX_DRAW_RESERVATION,
                            AGX_VDM_STREAM_LINK_LENGTH)) {
         uint64_t target = 0;
         CHECK(agx_vdm_stream_link_core_unpack(
            chip, chunk + off, AGX_VDM_STREAM_LINK_CORE_LENGTH, &target));
         chunk = resolve_vdm_link_target(f, chip, target, &chunk_size);
         CHECK(chunk != NULL);
         off = 0;
         ++links_seen;
      }

      const uint8_t *rec = chunk + off;
      uint64_t pipeline = 0;
      uint32_t raw_tmpl = 0;
      CHECK(agx_vdm_state_vertex_pipeline_pair_unpack(
         chip, rec + 8, 8, &pipeline, &raw_tmpl));
      CHECK((pipeline & 0x3fu) == 0);
      CHECK(va_in_ioto_arenas(f, pipeline));

      const uint64_t expect_addr = base + (uint64_t)stride * i;
      const uint32_t hi = agx_load_le32(rec + indirect_off);
      const uint32_t lo = agx_load_le32(rec + indirect_off + 4);
      CHECK(hi == (uint32_t)((expect_addr >> 32) & 0xffffu));
      CHECK(lo == ((uint32_t)expect_addr & 0xfffffffcu));
      off += rec_len;
   }

   CHECK(chunk + off == (const uint8_t *)f->cs.current);
   if (links_seen_out)
      *links_seen_out = links_seen;
}

static void
test_byte_exact(enum agx_chip chip, bool indexed, uint64_t indirect_addr,
                bool uses_base_param)
{
   const uint32_t draws = 32;
   const uint32_t stride = indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                   : sizeof(VkDrawIndirectCommand);

   struct fixture single, batched;
   mock_reset(UINT64_C(0x100000000));
   fixture_init(&single, chip, indirect_addr, uses_base_param, false);
   for (uint32_t i = 0; i < draws; ++i)
      issue_draw(&single, indexed, (VkDeviceSize)i * stride, 1, stride);

   const size_t single_vdm = (uint8_t *)single.cs.current - single.vdm_map;
   CHECK(single_vdm > 0);
   uint8_t *single_vdm_copy = malloc(single_vdm);
   uint8_t *single_ioto_copy =
      single.cmd.ioto.cursor ? malloc(single.cmd.ioto.cursor) : NULL;
   CHECK(single_vdm_copy && (!single.cmd.ioto.cursor || single_ioto_copy));
   memcpy(single_vdm_copy, single.vdm_map, single_vdm);
   if (single.cmd.ioto.cursor)
      memcpy(single_ioto_copy, single.ioto_map, single.cmd.ioto.cursor);
   const uint32_t single_ioto_cursor = single.cmd.ioto.cursor;
   const uint32_t single_ioto_programs = single.cmd.ioto.programs;
   const uint64_t single_ioto_bytes = single.cmd.ioto.program_bytes;
   const uint64_t single_layer0 = single.cs.g17p_layer_zero_draws;
   const uint64_t single_draw_params = single.cmd.state.gfx.draw_params;
   const uint64_t single_loader_prog =
      single.cmd.state.gfx.g17p_ioto[0].loader_program;
   CHECK(single.cs.stats.calls == draws && single.cs.stats.cmds == draws);
   fixture_finish(&single);

   mock_reset(UINT64_C(0x100000000));
   fixture_init(&batched, chip, indirect_addr, uses_base_param, false);
   issue_draw(&batched, indexed, 0, draws, stride);

   const size_t batched_vdm = (uint8_t *)batched.cs.current - batched.vdm_map;
   CHECK(single_vdm == batched_vdm);
   CHECK(memcmp(single_vdm_copy, batched.vdm_map, single_vdm) == 0);
   CHECK(single_ioto_cursor == batched.cmd.ioto.cursor);
   CHECK(single_ioto_programs == batched.cmd.ioto.programs);
   CHECK(single_ioto_bytes == batched.cmd.ioto.program_bytes);
   if (single_ioto_cursor > 0)
      CHECK(memcmp(single_ioto_copy, batched.ioto_map, single_ioto_cursor) ==
            0);

   CHECK(batched.cs.stats.calls == draws && batched.cs.stats.cmds == draws);
   CHECK(single_layer0 == batched.cs.g17p_layer_zero_draws);
   CHECK(single_draw_params == batched.cmd.state.gfx.draw_params);
   CHECK(single_loader_prog ==
         batched.cmd.state.gfx.g17p_ioto[0].loader_program);

   /* Verify the batch preparation actually admits the resulting loader when
    * addresses fit its LDIMM widths (i.e. batching did not silently bail). */
   if (uses_base_param && indirect_addr < UINT64_C(0x7f00000000)) {
      struct hk_g17p_indirect_loader_batch batch;
      struct hk_shader *hw_vs = &batched.vs_api->variants[HK_VS_VARIANT_HW];
      CHECK(hk_m5_multi_draw_vertex_batch_prepare(
         &batched.cmd, hw_vs, &batched.linked_vs, true, false, &batch));
   }

   free(single_vdm_copy);
   free(single_ioto_copy);
   fixture_finish(&batched);
}

/* Verify byte-for-byte equivalence with stride > command struct across
 * multiple VDM stream links and slow IOTO arena refills. */
static void
test_byte_exact_large_stride(enum agx_chip chip, bool indexed)
{
   const uint32_t draws = 2048;
   const uint32_t stride = 48;
   const uint64_t base = UINT64_C(0x900000);
   const size_t init_vdm = 16 * 1024;
   const size_t init_ioto = 16 * 1024;
   const size_t init_main = 4 * 1024;

   struct fixture single;
   mock_reset(UINT64_C(0x100000000));
   fixture_init_sized(&single, chip, base, true, false, init_vdm, init_ioto,
                      init_main);
   for (uint32_t i = 0; i < draws; ++i)
      issue_draw(&single, indexed, (VkDeviceSize)i * stride, 1, stride);

   CHECK(single.cs.stream_linked);
   CHECK(g_ioto_bo_creates > 0);
   CHECK(g_main_bo_creates > 0);
   const unsigned single_bo_count = g_bo_count;
   const unsigned single_ioto_bos = g_ioto_bo_creates;
   const unsigned single_main_bos = g_main_bo_creates;
   uint8_t *single_init_vdm = malloc(init_vdm);
   uint8_t *single_init_ioto = malloc(init_ioto);
   uint8_t *single_init_main = malloc(init_main);
   CHECK(single_init_vdm && single_init_ioto && single_init_main);
   memcpy(single_init_vdm, single.vdm_map, init_vdm);
   memcpy(single_init_ioto, single.ioto_map, init_ioto);
   memcpy(single_init_main, single.main_map, init_main);

   uint8_t *single_bo_copies[MAX_TRACKED_BOS] = {0};
   size_t single_bo_sizes[MAX_TRACKED_BOS] = {0};
   for (unsigned i = 0; i < single_bo_count; ++i) {
      single_bo_sizes[i] = g_bos[i].size;
      single_bo_copies[i] = malloc(g_bos[i].size);
      CHECK(single_bo_copies[i]);
      memcpy(single_bo_copies[i], g_bos[i].cpu, g_bos[i].size);
   }
   const uint32_t single_ioto_cursor = single.cmd.ioto.cursor;
   const uint64_t single_ioto_bytes = single.cmd.ioto.program_bytes;
   const uint64_t single_loader_prog =
      single.cmd.state.gfx.g17p_ioto[0].loader_program;
   fixture_finish(&single);

   struct fixture batched;
   mock_reset(UINT64_C(0x100000000));
   fixture_init_sized(&batched, chip, base, true, false, init_vdm, init_ioto,
                      init_main);
   issue_draw(&batched, indexed, 0, draws, stride);

   CHECK(batched.cs.stream_linked);
   CHECK(batched.cs.stats.calls == draws);
   CHECK(batched.cmd.ioto.programs == draws);
   CHECK(batched.cmd.ioto.cursor == single_ioto_cursor);
   CHECK(batched.cmd.ioto.program_bytes == single_ioto_bytes);
   CHECK(batched.cmd.state.gfx.g17p_ioto[0].loader_program ==
         single_loader_prog);
   CHECK(g_bo_count == single_bo_count);
   CHECK(g_ioto_bo_creates == single_ioto_bos);
   CHECK(g_main_bo_creates == single_main_bos);
   CHECK(memcmp(single_init_vdm, batched.vdm_map, init_vdm) == 0);
   CHECK(memcmp(single_init_ioto, batched.ioto_map, init_ioto) == 0);
   CHECK(memcmp(single_init_main, batched.main_map, init_main) == 0);
   for (unsigned i = 0; i < single_bo_count; ++i) {
      CHECK(g_bos[i].size == single_bo_sizes[i]);
      CHECK(memcmp(single_bo_copies[i], g_bos[i].cpu, single_bo_sizes[i]) == 0);
      free(single_bo_copies[i]);
   }
   free(single_init_vdm);
   free(single_init_ioto);
   free(single_init_main);
   fixture_finish(&batched);
}

/* Measure exact per-draw VDM record length for a given chip/indexed mode. */
static size_t
measure_vdm_rec_len(enum agx_chip chip, bool indexed, bool uses_draw_id)
{
   struct fixture f;
   mock_reset(UINT64_C(0x100000000));
   fixture_init(&f, chip, UINT64_C(0x900000), true, uses_draw_id);
   issue_draw(&f, indexed, 0, 1, 48);
   const size_t rec_len = (size_t)((uint8_t *)f.cs.current - f.vdm_map);
   CHECK(rec_len > 16);
   fixture_finish(&f);
   return rec_len;
}

/* Large multi-draw with gl_DrawID and stride > command struct: exercises VDM
 * stream links, the 16,384-draw gl_DrawID chunk boundary, and slow IOTO
 * arena refills in one batch. */
static void
test_large_draw_id_batch(enum agx_chip chip, bool indexed)
{
   const uint32_t draws = 17000;
   const uint32_t stride = 48;
   const uint64_t base = UINT64_C(0x900000);
   const size_t rec_len = measure_vdm_rec_len(chip, indexed, true);

   struct fixture f;
   mock_reset(UINT64_C(0x100000000));
   fixture_init_sized(&f, chip, base, true, true, 16 * 1024, 16 * 1024,
                      HK_CMD_BO_SIZE);
   issue_draw(&f, indexed, 0, draws, stride);

   CHECK(f.cs.stream_linked);
   CHECK(g_ioto_bo_creates > 0);
   CHECK(g_main_bo_creates > 0);
   CHECK(f.cs.stats.calls == draws);
   CHECK(f.cs.stats.cmds == draws);
   CHECK(f.cmd.ioto.programs == draws);
   CHECK(f.cmd.state.gfx.g17p_draw_id_host_valid);
   CHECK(f.cmd.state.gfx.g17p_draw_id_value == draws - 1);

   /* Verify the final draw ID in chunk 1 (draws 16385..16999) and its start. */
   const uint64_t last_ptr = f.cmd.state.gfx.draw_id_ptr;
   const uint32_t chunk1_len = draws - 16385;
   const uint64_t chunk1_start = last_ptr - (uint64_t)(chunk1_len - 1u) * 4u;
   size_t avail = 0;
   const uint8_t *chunk1_cpu = resolve_gpu_ptr(&f, chunk1_start, &avail);
   CHECK(chunk1_cpu && avail >= chunk1_len * sizeof(uint32_t));
   CHECK(agx_load_le32(chunk1_cpu) == 16385u);
   CHECK(agx_load_le32(chunk1_cpu + (chunk1_len - 1u) * 4u) == draws - 1u);

   /* Verify draw 0 and chunk 0 boundary (draws 1..16384) in the initial pool. */
   CHECK(agx_load_le32(f.main_map) == 0u);
   CHECK(agx_load_le32(f.main_map + 4u) == 1u);
   CHECK(agx_load_le32(f.main_map + 16384u * 4u) == 16384u);

   unsigned links_seen = 0;
   verify_vdm_stream(&f, chip, indexed, base, draws, stride, rec_len,
                     &links_seen);
   CHECK(links_seen >= 2);

   fixture_finish(&f);
}

/* Force chunk_fits == false on the second gl_DrawID chunk (draw 16,385) by
 * placing the new main pool BO at >= 0x8000000000 while the first batch used
 * a short high LDIMM for draw_id_ptr.  Verifies partial-batch exit, single-
 * draw fallback loader re-selection, and resumed batching for the tail. */
static void
test_partial_batch_resume(enum agx_chip chip, bool indexed)
{
   const uint32_t draws = 16500;
   const uint32_t stride = 48;
   const uint64_t base = UINT64_C(0x900000);
   const size_t rec_len = measure_vdm_rec_len(chip, indexed, true);
   const uint64_t high_va = UINT64_C(0x8000000000);

   /* Exact initial main-pool space for draw 0 (4B) + chunk 0 (16384 * 4B),
    * so chunk 1 at draw 16,385 allocates a new main BO at high_va. */
   const size_t exact_main = 4u + 16384u * sizeof(uint32_t);

   struct fixture f;
   mock_reset(UINT64_C(0x100000000));
   g_next_main_va_override = high_va;
   fixture_init_sized(&f, chip, base, true, true, 4 * 1024 * 1024, 16 * 1024,
                      exact_main);
   issue_draw(&f, indexed, 0, draws, stride);

   CHECK(g_main_bo_creates == 1);
   CHECK(g_ioto_bo_creates > 0);
   CHECK(f.cs.stats.calls == draws);
   CHECK(f.cs.stats.cmds == draws);
   CHECK(f.cmd.ioto.programs == draws);
   CHECK(f.cmd.state.gfx.g17p_draw_id_host_valid);
   CHECK(f.cmd.state.gfx.g17p_draw_id_value == draws - 1);
   CHECK(f.cmd.state.gfx.draw_id_ptr >= high_va);

   /* Verify draw 16,384 in chunk 0, draw 16,385 (fallback) and draw 16,499
    * (resumed batch) in the high-VA pool BO. */
   CHECK(agx_load_le32(f.main_map + 16384u * 4u) == 16384u);
   size_t avail = 0;
   const uint8_t *last_cpu =
      resolve_gpu_ptr(&f, f.cmd.state.gfx.draw_id_ptr, &avail);
   CHECK(last_cpu && avail >= 4);
   CHECK(agx_load_le32(last_cpu) == draws - 1u);

   const uint8_t *high_base = resolve_gpu_ptr(&f, high_va, &avail);
   CHECK(high_base && avail >= (draws - 16385u) * 4u);
   /* Draw 16,385 hit chunk_fits==false on the allocated chunk before writing,
    * then fallback allocated 4B for 16,385 and resumed batch uploaded 16386..
    * 16499 sequentially after it. */
   const uint32_t tail_count = draws - 16385u;
   const uint64_t fallback_ptr =
      f.cmd.state.gfx.draw_id_ptr - (uint64_t)(tail_count - 1u) * 4u;
   const uint8_t *fb_cpu = resolve_gpu_ptr(&f, fallback_ptr, &avail);
   CHECK(fb_cpu && avail >= tail_count * 4u);
   CHECK(agx_load_le32(fb_cpu) == 16385u);
   CHECK(agx_load_le32(fb_cpu + 4u) == 16386u);
   CHECK(agx_load_le32(fb_cpu + (tail_count - 1u) * 4u) == draws - 1u);

   unsigned links_seen = 0;
   verify_vdm_stream(&f, chip, indexed, base, draws, stride, rec_len,
                     &links_seen);
   CHECK(links_seen == 0);

   fixture_finish(&f);
}

int
main(void)
{
   const enum agx_chip chips[] = {
      AGX_CHIP_G15G,
      AGX_CHIP_G16G,
      AGX_CHIP_G17P,
      AGX_CHIP_G17G,
   };
   for (unsigned c = 0; c < ARRAY_SIZE(chips); ++c) {
      /* Non-republishing fast path (non-indexed and indexed). */
      test_byte_exact(chips[c], false, UINT64_C(0x900000), false);
      test_byte_exact(chips[c], true, UINT64_C(0x900000), false);
      /* Republishing fast path: far from root with short high LDIMM (hi < 128). */
      test_byte_exact(chips[c], false, UINT64_C(0x900000), true);
      test_byte_exact(chips[c], true, UINT64_C(0x900000), true);
      test_byte_exact(chips[c], true, UINT64_C(0x1000400000), true);
      /* Republishing fast path: long high LDIMM (hi >= 128). */
      test_byte_exact(chips[c], true, UINT64_C(0x8000400000), true);
      /* Short high LDIMM crossing the 128 boundary (must fall back cleanly). */
      test_byte_exact(chips[c], true, UINT64_C(0x7fffffffd0), true);
      /* Republishing fast path: adjacent to root (transitions to independent
       * loader after HK_G17P_GFX_LOADER_MAX_SHAPES shapes). */
      test_byte_exact(chips[c], true, UINT64_C(0x200100), true);
      /* Byte-exact across VDM stream links, slow IOTO refills, and stride > struct. */
      test_byte_exact_large_stride(chips[c], false);
      test_byte_exact_large_stride(chips[c], true);
      /* Large draw count with gl_DrawID chunk boundary, stream links, slow IOTO. */
      test_large_draw_id_batch(chips[c], false);
      test_large_draw_id_batch(chips[c], true);
      /* Partial-batch exit on gl_DrawID chunk_fits==false + fallback + resume. */
      test_partial_batch_resume(chips[c], false);
      test_partial_batch_resume(chips[c], true);
   }
   CHECK(g_bo_live == 0);
   printf("PASS: hk_g17p_indirect_batch\n");
   return 0;
}
