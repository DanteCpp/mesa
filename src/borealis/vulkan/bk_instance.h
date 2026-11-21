/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "bk_drirc.h"
#include "vk_instance.h"

struct bk_instance {
   struct vk_instance vk;

   struct bk_drirc drirc;

   uint8_t driver_build_sha[BLAKE3_KEY_LEN];
};

VK_DEFINE_HANDLE_CASTS(bk_instance, vk.base, VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)
