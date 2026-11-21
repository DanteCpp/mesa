/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bk_device.h"
#include "bk_device_memory.h"
#include "bk_private.h"

#include "agx_abi.h"
#include "vk_buffer.h"

struct bk_device_memory;
struct bk_physical_device;

struct bk_buffer {
   struct vk_buffer vk;

   /** Reserved VA for sparse buffers, NULL otherwise. */
   struct agx_va *va;

   /** Bytes of that VA counted in bk_device::sparse_scratch_B. */
   uint64_t sparse_scratch_B;

   /** One residency bit per sparse-buffer page on Gen9/Gen10 AGX3. */
   struct agx_bo *sparse_map;
   uint32_t sparse_map_entries;
   /** Link in bk_device::sparse_buffers while sparse_map is set. */
   struct list_head sparse_link;
};

/* VK_EXT_descriptor_buffer capture/replay data of a buffer */
struct bk_buffer_capture {
   uint64_t va;
};

struct bk_sparse_buffer_map_header {
   uint64_t resource_base;
   uint32_t page_count;
   uint32_t reserved;
};

static_assert(sizeof(struct bk_sparse_buffer_map_header) == 16,
              "sparse buffer map header must be 16 bytes");
static_assert(offsetof(struct bk_sparse_buffer_map_header, resource_base) ==
                 AGX_SPARSE_BUFFER_MAP_RESOURCE_BASE_OFFSET,
              "sparse buffer map ABI");
static_assert(offsetof(struct bk_sparse_buffer_map_header, page_count) ==
                 AGX_SPARSE_BUFFER_MAP_PAGE_COUNT_OFFSET,
              "sparse buffer map ABI");
static_assert(sizeof(struct bk_sparse_buffer_map_header) ==
                 AGX_SPARSE_BUFFER_MAP_BITMAP_OFFSET,
              "sparse buffer map ABI");

VK_DEFINE_NONDISP_HANDLE_CASTS(bk_buffer, vk.base, VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)

uint64_t bk_buffer_address_sparse_ro(const struct bk_buffer *buffer,
                                     uint64_t offset);

/* Called for every vertex, index and descriptor buffer binding. Only sparse
 * resident buffers (buffer->va != NULL) read through the read-only shadow
 * mapping; keep the common case inline. */
static inline uint64_t
bk_buffer_address(const struct bk_buffer *buffer, uint64_t offset,
                  bool read_only)
{
   if (read_only && buffer->va)
      return bk_buffer_address_sparse_ro(buffer, offset);

   return vk_buffer_address(&buffer->vk, offset);
}

static inline uint64_t
bk_buffer_address_rw(const struct bk_buffer *buffer, uint64_t offset)
{
   return bk_buffer_address(buffer, offset, false);
}

static inline uint64_t
bk_buffer_address_ro(const struct bk_buffer *buffer, uint64_t offset)
{
   return bk_buffer_address(buffer, offset, true);
}

static inline struct bk_addr_range
bk_buffer_addr_range(const struct bk_buffer *buffer, uint64_t offset,
                     uint64_t range, bool read_only)
{
   /* If range == 0, return a pointer to the zero page. That allows
    * eliding robustness2 bounds checks for index = 0, even without soft fault.
    */
   if (buffer == NULL || range == 0) {
      return (struct bk_addr_range){.addr = AGX_ZERO_PAGE_ADDRESS, .range = 0};
   }

   return (struct bk_addr_range){
      .addr = bk_buffer_address(buffer, offset, read_only),
      .range = vk_buffer_range(&buffer->vk, offset, range),
   };
}

VkResult bk_reserve_sparse_scratch(struct bk_device *dev, uint64_t size_B);
void bk_release_sparse_scratch(struct bk_device *dev, uint64_t size_B);

/* On M5, the writable alias of an unbound sparse buffer page is left
 * unmapped: soft faults read it as zero and drop stores. The shared scratch
 * page would instead return whatever any other unbound page received. The
 * read-only alias still maps the zero page. */
static inline bool
bk_sparse_buffer_unmapped(const struct agx_device *dev)
{
   return agx_chip_g17_m5(dev->chip) && agx_has_soft_fault(dev);
}

/* G13 and G14 build a texel buffer as a plain linear texture, which has no
 * residency to report, so a sparse texel fetch reads the device-wide map of
 * non-resident pages (bk_device::physical_sparse_map) instead. */
static inline bool
bk_sparse_texel_residency_from_map(const struct agx_device *dev)
{
   return dev->chip == AGX_CHIP_G13G || dev->chip == AGX_CHIP_G13X ||
          dev->chip == AGX_CHIP_G14G || dev->chip == AGX_CHIP_G14X;
}

/* Whether the buffer's unbound pages are published to the device-wide map. */
static inline bool
bk_buffer_in_physical_sparse_map(const struct bk_device *dev,
                                 const struct bk_buffer *buffer)
{
   return dev->physical_sparse_map && buffer->va &&
          (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT);
}

VkResult bk_bind_scratch(struct bk_device *dev, struct agx_va *va,
                         unsigned offs_B, size_t size_B);
VkResult bk_bind_unbound_sparse_buffer(struct bk_device *dev,
                                       struct agx_va *va, size_t size_B);

/* The AGX_G17P_BUFFER_TEXTURE_SPARSE_OFFSET word of a texel view of buffer
 * with blocksize_B-byte texels, and of a view at address. */
uint32_t bk_buffer_sparse_texel_word(const struct bk_buffer *buffer,
                                     unsigned blocksize_B);
uint32_t bk_sparse_texel_word_at(struct bk_device *dev, uint64_t address,
                                 unsigned blocksize_B);

/* Publish a bound/unbound sparse buffer range to physical pointer reads. */
VkResult bk_update_physical_sparse_map(struct bk_device *dev, uint64_t address,
                                       uint64_t size, bool nonresident);
