/*
 * Copyright 2023 Rose Hudson
 * Copyright 2022 Amazon.com, Inc. or its affiliates.
 * Copyright 2018 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>

#include "asahi/compiler/agx_debug.h"
#include "compiler/shader_enums.h"
#include "util/blob.h"
#include "util/build_id.h"
#include "util/disk_cache.h"
#include "util/mesa-blake3.h"
#include "agx_bo.h"
#include "agx_device.h"
#include "agx_disk_cache.h"
#include "agx_state.h"

/* Flags that are allowed and do not disable the disk cache */
#define ALLOWED_FLAGS (AGX_DBG_NO16)

/* Explicit blob header so cache entries from mismatched formats or truncated
 * writes are rejected cleanly instead of misparsed. */
#define AGX_DISK_CACHE_MAGIC   0x41475833u
#define AGX_DISK_CACHE_VERSION 1u

/**
 * Compute a disk cache key for the given uncompiled shader and shader key.
 */
static void
agx_disk_cache_compute_key(struct disk_cache *cache,
                           const struct agx_uncompiled_shader *uncompiled,
                           const union asahi_shader_key *shader_key,
                           cache_key cache_key)
{
   uint8_t data[sizeof(uncompiled->nir_blake3) + sizeof(*shader_key)];
   int hash_size = sizeof(uncompiled->nir_blake3);
   int key_size;
   if (uncompiled->type == MESA_SHADER_VERTEX ||
       uncompiled->type == MESA_SHADER_TESS_EVAL)
      key_size = sizeof(shader_key->vs);
   else if (uncompiled->type == MESA_SHADER_FRAGMENT)
      key_size = sizeof(shader_key->fs);
   else if (uncompiled->type == MESA_SHADER_COMPUTE)
      key_size = sizeof(shader_key->cs);
   else if (uncompiled->type == MESA_SHADER_GEOMETRY)
      key_size = sizeof(shader_key->gs);
   else
      key_size = 0;

   memcpy(data, uncompiled->nir_blake3, hash_size);

   if (key_size)
      memcpy(data + hash_size, shader_key, key_size);

   disk_cache_compute_key(cache, data, hash_size + key_size, cache_key);
}

static void
write_shader(struct blob *blob, const struct agx_compiled_shader *binary,
             bool is_root_gs)
{
   /* Geometry variants contain a pre-GS compute helper with no API shader
    * owner. Preserve each component's identity instead of inheriting the
    * parent geometry stage and its resource bindings on a cache hit. */
   blob_write_uint8(blob, binary->stage);
   blob_write_uint8(blob, binary->so != NULL);
   blob_write_bytes(blob, &binary->b.info, sizeof(binary->b.info));

   if (binary->b.info.binary_size) {
      blob_write_bytes(blob, binary->b.binary, binary->b.info.binary_size);
   }

   blob_write_bytes(blob, &binary->uvs, sizeof(binary->uvs));
   blob_write_bytes(blob, &binary->attrib_components_read,
                    sizeof(binary->attrib_components_read));
   blob_write_bytes(blob, &binary->epilog_key, sizeof(binary->epilog_key));
   blob_write_uint8(blob, binary->g17p_texture_resources);
   blob_write_uint8(blob, binary->g17p_custom_border_emulation);
   blob_write_uint8(blob, binary->g17g_samples_unrolled);
   blob_write_uint32(blob, binary->push_range_count);
   blob_write_bytes(blob, binary->push,
                    sizeof(binary->push[0]) * binary->push_range_count);

   const uint32_t inline_blend_nir_size =
      (binary->inline_blend_nir && binary->inline_blend_nir_size &&
       binary->inline_blend_nir_size <= UINT32_MAX)
         ? (uint32_t)binary->inline_blend_nir_size
         : 0;
   blob_write_uint32(blob, inline_blend_nir_size);
   if (inline_blend_nir_size) {
      blob_write_bytes(blob, binary->inline_blend_nir, inline_blend_nir_size);
      struct agx_shader_key inline_key = binary->inline_blend_key;
      inline_key.g17p_rodata_hint = NULL;
      inline_key.g17p_rodata_hint_base = 0;
      inline_key.g17p_rodata_hint_size_16 = 0;
      blob_write_bytes(blob, &inline_key, sizeof(inline_key));
   }

   if (is_root_gs) {
      blob_write_bytes(blob, &binary->gs, sizeof(binary->gs));
      write_shader(blob, binary->pre_gs, false);

      blob_write_uint8(blob, binary->gs_copy != NULL);
      if (binary->gs_copy)
         write_shader(blob, binary->gs_copy, false);

      blob_write_uint8(blob, binary->gs_count != NULL);
      if (binary->gs_count)
         write_shader(blob, binary->gs_count, false);
   }
}

static void
destroy_cached_shader(struct agx_device *dev, struct agx_compiled_shader *so)
{
   if (!so)
      return;

   destroy_cached_shader(dev, so->gs_count);
   destroy_cached_shader(dev, so->pre_gs);
   destroy_cached_shader(dev, so->gs_copy);

   free(so->inline_blend_nir);
   free(so->b.binary);
   agx_bo_unreference(dev, so->bo);
   FREE(so);
}

static struct agx_compiled_shader *
read_shader(struct agx_screen *screen, struct blob_reader *blob,
            const struct agx_uncompiled_shader *uncompiled, bool is_root)
{
   struct agx_compiled_shader *binary = CALLOC_STRUCT(agx_compiled_shader);
   if (!binary) {
      blob->overrun = true;
      return NULL;
   }

   binary->stage = blob_read_uint8(blob);
   binary->so = blob_read_uint8(blob) ? uncompiled : NULL;

   blob_copy_bytes(blob, &binary->b.info, sizeof(binary->b.info));
   if (blob->overrun) {
      destroy_cached_shader(&screen->dev, binary);
      return NULL;
   }

   size_t size = binary->b.info.binary_size;
   if (size > (size_t)(blob->end - blob->current)) {
      blob->overrun = true;
      destroy_cached_shader(&screen->dev, binary);
      return NULL;
   }

   if (uncompiled->type == MESA_SHADER_VERTEX ||
       uncompiled->type == MESA_SHADER_TESS_EVAL ||
       uncompiled->type == MESA_SHADER_FRAGMENT) {

      binary->b.binary = malloc(size);
      if (size && !binary->b.binary) {
         blob->overrun = true;
         destroy_cached_shader(&screen->dev, binary);
         return NULL;
      }
      blob_copy_bytes(blob, binary->b.binary, size);

      if (size) {
         binary->bo = agx_bo_create(&screen->dev, size, 0,
                                    AGX_BO_EXEC | AGX_BO_LOW_VA, "Executable");
         if (!binary->bo || !agx_bo_map(binary->bo)) {
            blob->overrun = true;
            destroy_cached_shader(&screen->dev, binary);
            return NULL;
         }
         memcpy(agx_bo_map(binary->bo), binary->b.binary, size);
      }
   } else if (size) {
      /* G17P's native compute Ioto program is built from the shader bytes at
       * dispatch time, so a cached compute shader needs the CPU copy as well
       * as its executable BO.  Keeping only the BO made the uncached dispatch
       * work and the identical cached dispatch fail descriptor validation. */
      binary->bo = agx_bo_create(&screen->dev, size, 0,
                                 AGX_BO_EXEC | AGX_BO_LOW_VA, "Executable");
      if (!binary->bo || !agx_bo_map(binary->bo)) {
         blob->overrun = true;
         destroy_cached_shader(&screen->dev, binary);
         return NULL;
      }
      if (agx_chip_native_or_m5(screen->dev.chip)) {
         /* Native compute Ioto programs need the cached CPU shader bytes. */
         binary->b.binary = malloc(size);
         if (!binary->b.binary) {
            blob->overrun = true;
            destroy_cached_shader(&screen->dev, binary);
            return NULL;
         }
         blob_copy_bytes(blob, binary->b.binary, size);
         memcpy(agx_bo_map(binary->bo), binary->b.binary, size);
      } else {
         blob_copy_bytes(blob, agx_bo_map(binary->bo), size);
      }
   }

   blob_copy_bytes(blob, &binary->uvs, sizeof(binary->uvs));
   blob_copy_bytes(blob, &binary->attrib_components_read,
                   sizeof(binary->attrib_components_read));
   blob_copy_bytes(blob, &binary->epilog_key, sizeof(binary->epilog_key));
   binary->g17p_texture_resources = blob_read_uint8(blob);
   binary->g17p_custom_border_emulation = blob_read_uint8(blob);
   binary->g17g_samples_unrolled = blob_read_uint8(blob);
   binary->push_range_count = blob_read_uint32(blob);
   if (blob->overrun || binary->push_range_count > ARRAY_SIZE(binary->push)) {
      blob->overrun = true;
      destroy_cached_shader(&screen->dev, binary);
      return NULL;
   }
   blob_copy_bytes(blob, binary->push,
                   sizeof(binary->push[0]) * binary->push_range_count);

   const uint32_t inline_blend_nir_size = blob_read_uint32(blob);
   if (inline_blend_nir_size) {
      const size_t remaining = (size_t)(blob->end - blob->current);
      if (blob->overrun || inline_blend_nir_size > remaining ||
          remaining - inline_blend_nir_size < sizeof(binary->inline_blend_key)) {
         blob->overrun = true;
         destroy_cached_shader(&screen->dev, binary);
         return NULL;
      }

      binary->inline_blend_nir = malloc(inline_blend_nir_size);
      if (!binary->inline_blend_nir) {
         blob->overrun = true;
         destroy_cached_shader(&screen->dev, binary);
         return NULL;
      }
      blob_copy_bytes(blob, binary->inline_blend_nir, inline_blend_nir_size);
      binary->inline_blend_nir_size = inline_blend_nir_size;
      blob_copy_bytes(blob, &binary->inline_blend_key,
                      sizeof(binary->inline_blend_key));
      binary->inline_blend_key.g17p_rodata_hint = NULL;
      binary->inline_blend_key.g17p_rodata_hint_base = 0;
      binary->inline_blend_key.g17p_rodata_hint_size_16 = 0;
   }

   if (is_root && uncompiled->type == MESA_SHADER_GEOMETRY) {
      blob_copy_bytes(blob, &binary->gs, sizeof(binary->gs));
      binary->pre_gs = read_shader(screen, blob, uncompiled, false);
      if (!binary->pre_gs) {
         destroy_cached_shader(&screen->dev, binary);
         return NULL;
      }

      if (blob_read_uint8(blob)) {
         binary->gs_copy = read_shader(screen, blob, uncompiled, false);
         if (!binary->gs_copy) {
            destroy_cached_shader(&screen->dev, binary);
            return NULL;
         }
      }

      if (blob_read_uint8(blob)) {
         binary->gs_count = read_shader(screen, blob, uncompiled, false);
         if (!binary->gs_count) {
            destroy_cached_shader(&screen->dev, binary);
            return NULL;
         }
      }
   }

   if (blob->overrun) {
      destroy_cached_shader(&screen->dev, binary);
      return NULL;
   }

   return binary;
}

/**
 * Store the given compiled shader in the disk cache.
 *
 * This should only be called on newly compiled shaders.  No checking is
 * done to prevent repeated stores of the same shader.
 */
void
agx_disk_cache_store(struct disk_cache *cache,
                     const struct agx_uncompiled_shader *uncompiled,
                     const union asahi_shader_key *key,
                     const struct agx_compiled_shader *binary)
{
#ifdef ENABLE_SHADER_CACHE
   if (!cache)
      return;

   cache_key cache_key;
   agx_disk_cache_compute_key(cache, uncompiled, key, cache_key);

   struct blob blob;
   blob_init(&blob);

   blob_write_uint32(&blob, AGX_DISK_CACHE_MAGIC);
   blob_write_uint32(&blob, AGX_DISK_CACHE_VERSION);
   write_shader(&blob, binary, uncompiled->type == MESA_SHADER_GEOMETRY);

   if (!blob.out_of_memory)
      disk_cache_put(cache, cache_key, blob.data, blob.size, NULL);
   blob_finish(&blob);
#endif
}

/**
 * Search for a compiled shader in the disk cache.
 */
struct agx_compiled_shader *
agx_disk_cache_retrieve(struct agx_screen *screen,
                        const struct agx_uncompiled_shader *uncompiled,
                        const union asahi_shader_key *key)
{
#ifdef ENABLE_SHADER_CACHE
   struct disk_cache *cache = screen->disk_cache;
   if (!cache)
      return NULL;

   cache_key cache_key;
   agx_disk_cache_compute_key(cache, uncompiled, key, cache_key);

   size_t size;
   void *buffer = disk_cache_get(cache, cache_key, &size);
   if (!buffer)
      return NULL;

   struct blob_reader blob;
   blob_reader_init(&blob, buffer, size);

   const uint32_t magic = blob_read_uint32(&blob);
   const uint32_t version = blob_read_uint32(&blob);
   if (blob.overrun || magic != AGX_DISK_CACHE_MAGIC ||
       version != AGX_DISK_CACHE_VERSION) {
      free(buffer);
      return NULL;
   }

   struct agx_compiled_shader *binary =
      read_shader(screen, &blob, uncompiled, true);
   if (binary && (blob.overrun || blob.current != blob.end)) {
      destroy_cached_shader(&screen->dev, binary);
      binary = NULL;
   }

   free(buffer);
   return binary;
#else
   return NULL;
#endif
}

/**
 * Initialise the on-disk shader cache.
 */
void
agx_disk_cache_init(struct agx_screen *screen)
{
#ifdef ENABLE_SHADER_CACHE
   /* G18G keeps j873's persistent cache off: its compiler A/B gate
    * (agx_get_fs_structure_flags()) selects code paths that must never share
    * persistent shader entries. */
   if (agx_get_compiler_debug() || (screen->dev.debug & ~ALLOWED_FLAGS) ||
       screen->dev.chip == AGX_CHIP_G18G)
      return;

   const char *renderer = screen->pscreen.get_name(&screen->pscreen);

   const struct build_id_note *note =
      build_id_find_nhdr_for_addr(agx_disk_cache_init);
   assert(note && build_id_length(note) == BUILD_ID_EXPECTED_HASH_LENGTH);

   const uint8_t *id_sha1 = build_id_data(note);
   assert(id_sha1);

   char timestamp[BLAKE3_HEX_LEN];
   _mesa_sha1_format(timestamp, id_sha1);

   uint64_t driver_flags = screen->dev.debug;
   screen->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);
#endif
}
