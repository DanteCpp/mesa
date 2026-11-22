/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../agx_bo.h"
#include "../agx_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The G18 render-node backend (agx_device_g18.c): the asahi uAPI to the
 * g18win driver's render node (\\.\G18Win0, win/g18_uapi.h) through
 * DeviceIoControl, with user-mode sync objects.
 */
extern const agx_device_ops_t agx_g18_device_ops;

/* Whether dev was opened by agx_g18_open_device(). */
static inline bool
agx_device_is_g18(const struct agx_device *dev)
{
   return dev->ops.simple_ioctl != NULL &&
          dev->ops.simple_ioctl == agx_g18_device_ops.simple_ioctl;
}

/*
 * Open the Windows render node and preset dev->ops/dev->fd, so that a
 * following agx_open_device() skips the DRM probe and goes straight to
 * GET_PARAMS. Returns false (leaving dev untouched) if there is no node, it
 * is not the asahi uAPI, or it is not live yet.
 */
bool agx_g18_open_device(struct agx_device *dev);

/* Release a pseudo-fd set by agx_g18_open_device(); never close() it. */
void agx_g18_close_fd(int fd);

/*
 * agx_bo_import_host_ptr() on this backend: the render node's GEM_USERPTR
 * over [ptr, ptr + size) of this process (a locked WDDM allocation of the
 * D3D UMD). size is rounded up to the node's 16 KiB page.
 */
struct agx_bo *agx_g18_bo_import_host_ptr(struct agx_device *dev, void *ptr,
                                          size_t size,
                                          enum agx_host_ptr_access access);

struct util_sync_provider;

/* User-mode sync objects with libdrm syncobj semantics; the SUBMIT op waits
 * for and signals them around the kernel SUBMIT. One process-wide handle
 * space; fd is unused. */
struct util_sync_provider *agx_g18_sync_provider_create(int fd);

#ifdef __cplusplus
}
#endif
