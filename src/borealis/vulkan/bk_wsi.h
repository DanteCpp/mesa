/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "bk_physical_device.h"

VkResult bk_init_wsi(struct bk_physical_device *pdev);
void bk_finish_wsi(struct bk_physical_device *pdev);

VkResult bk_wsi_allocate_memory(VkDevice device, const VkMemoryAllocateInfo *info,
   const VkAllocationCallbacks *alloc, VkDeviceMemory *out);
