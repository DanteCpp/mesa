/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * The D3DKMT winsys entry points, for Windows builds without that winsys (no
 * ntagx_kmd_uapi_core / ntagx_wddm_contract_core cross-file property, see
 * src/asahi/lib/meson.build). Enumeration finds no adapter, so no device of
 * this backend ever exists. agx_device.c, agx_va.c and hk still call the
 * runtime-device bridge helpers on every Windows device; with no bridge arena
 * registered each one takes the path the real implementation takes for a
 * device of another backend.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "agx_bridge_vma.h"
#include "agx_d3dkmt_bridge.h"
#include "agx_device.h"
#include "agx_device_d3dkmt.h"

const agx_device_ops_t agx_device_d3dkmt_ops = {0};

bool
agx_d3dkmt_enumerate_adapters(struct agx_kmt_luid *out, uint32_t capacity,
                              uint32_t *count)
{
   (void)out, (void)capacity;
   *count = 0;
   return false;
}

bool
agx_d3dkmt_open_device(struct agx_device *dev, const struct agx_kmt_luid *luid)
{
   (void)dev, (void)luid;
   errno = ENODEV;
   return false;
}

void
agx_d3dkmt_close_device(struct agx_device *dev)
{
   (void)dev;
}

struct agx_kmt_luid
agx_d3dkmt_device_luid(const struct agx_device *dev)
{
   (void)dev;
   return (struct agx_kmt_luid){0};
}

struct agx_bo *
agx_d3dkmt_bo_create_external(struct agx_device *dev, size_t size,
                              unsigned align, enum agx_bo_flags flags,
                              enum agx_d3dkmt_external_memory_type external_type,
                              const char *label)
{
   (void)dev, (void)size, (void)align, (void)flags, (void)external_type,
      (void)label;
   errno = EOPNOTSUPP;
   return NULL;
}

struct agx_bo *
agx_d3dkmt_bo_import_external(struct agx_device *dev, uintptr_t handle,
                              enum agx_d3dkmt_external_memory_type external_type,
                              const char *label)
{
   (void)dev, (void)handle, (void)external_type, (void)label;
   errno = EOPNOTSUPP;
   return NULL;
}

struct agx_bo *
agx_d3dkmt_bo_import_local(struct agx_device *dev, agx_kmt_handle allocation,
                           size_t size, const char *label)
{
   (void)dev, (void)allocation, (void)size, (void)label;
   errno = EOPNOTSUPP;
   return NULL;
}

int
agx_d3dkmt_bo_export_external(struct agx_device *dev, struct agx_bo *bo,
                              enum agx_d3dkmt_external_memory_type external_type,
                              uintptr_t *handle)
{
   (void)dev, (void)bo, (void)external_type, (void)handle;
   errno = EOPNOTSUPP;
   return -1;
}

struct util_sync_provider *
util_sync_provider_d3dkmt(struct agx_device *dev)
{
   (void)dev;
   return NULL;
}

bool
agx_d3dkmt_bridge_enabled(void)
{
   return false;
}

bool
agx_bridge_valog_enabled(void)
{
   return false;
}

void
agx_bridge_valog(const char *fmt, ...)
{
   (void)fmt;
}

bool
agx_bridge_vma_register(struct agx_device *dev, uint64_t main_start,
                        uint64_t main_size, uint64_t usc_start,
                        uint64_t usc_size)
{
   (void)dev, (void)main_start, (void)main_size, (void)usc_start,
      (void)usc_size;
   return true;
}

void
agx_bridge_vma_unregister(struct agx_device *dev)
{
   (void)dev;
}

bool
agx_bridge_vma_registered(const struct agx_device *dev)
{
   (void)dev;
   return false;
}

uint64_t
agx_bridge_vma_alloc(struct agx_device *dev, uint64_t size_B,
                     uint64_t align_B, unsigned flags, uint64_t fixed_va)
{
   (void)dev, (void)size_B, (void)align_B, (void)flags, (void)fixed_va;
   return 0;
}

void
agx_bridge_vma_free(struct agx_device *dev, uint64_t addr, uint64_t size_B,
                    unsigned flags)
{
   (void)dev, (void)addr, (void)size_B, (void)flags;
}

int
agx_bridge_vma_bootstrap_bind(struct agx_device *dev, struct agx_bo *bo,
                              uint64_t addr, uint64_t size_B,
                              uint64_t bo_offset, unsigned bind_flags,
                              const char *label)
{
   (void)label;
   return agx_bo_bind(dev, bo, addr, size_B, bo_offset, bind_flags);
}

void
agx_bridge_vma_bootstrap_unbind_all(struct agx_device *dev)
{
   (void)dev;
}

void
agx_bridge_vma_wait_teardown(struct agx_device *dev)
{
   (void)dev;
}

bool
agx_bridge_vma_winsys_healthy(const struct agx_d3dkmt_winsys *ws)
{
   (void)ws;
   return false;
}
