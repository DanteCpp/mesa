/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * The private AGXKMD backend's entry points, for Windows builds without it
 * (no ntagx_kmd_uapi_core cross-file property, see src/asahi/lib/meson.build;
 * agx_device_win.c needs kmd_uapi.h from a separate drivers tree). Opening a
 * device through it presets no operations, so agx_open_device() refuses the
 * device and an NTAGX_DEVICE_PATH physical device fails to initialize.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "agx_device.h"
#include "agx_device_win.h"

bool
agx_win_open_device(struct agx_device *dev, void *hdev)
{
   /* No operations: agx_open_device() refuses the device. The caller still
    * closes hdev. */
   dev->hdev = hdev;
   return false;
}

int
ntagx_ioctl(void *hdev, uint32_t drm_cmd, void *req)
{
   (void)hdev, (void)drm_cmd, (void)req;
   errno = ENODEV;
   return -1;
}

struct util_sync_provider *
util_sync_provider_ntagx(void *hdev)
{
   (void)hdev;
   return NULL;
}
