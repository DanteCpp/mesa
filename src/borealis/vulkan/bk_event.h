/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "bk_private.h"

#include "vk_object.h"

struct bk_event {
   struct vk_object_base base;
   struct agx_bo *bo;

   uint64_t addr;
   VkResult *status;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(bk_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)
