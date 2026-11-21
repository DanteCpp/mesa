/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <vulkan/vulkan_core.h>

struct bk_host_as_staging {
   uint64_t vertices, indices, transform, data;
   uint32_t vertex_count;
};


bool bk_host_as_stage_layout(const VkAccelerationStructureGeometryKHR *g,
   const VkAccelerationStructureBuildRangeInfoKHR *r,
   struct bk_host_as_staging *layout, uint64_t *offset);
void bk_host_as_stage_geometries(uint32_t count,
   VkAccelerationStructureGeometryKHR *geometries,
   VkAccelerationStructureBuildRangeInfoKHR *ranges,
   const struct bk_host_as_staging *layouts,
   uint8_t *map, uint64_t va, uint64_t size);
