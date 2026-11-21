/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#ifndef BK_ACCELERATION_STRUCTURE_H
#define BK_ACCELERATION_STRUCTURE_H

#include "bk_agx3_as_format.h"
#include "bk_bvh.h"

#include "vk_acceleration_structure.h"

struct bk_device;
struct bk_cmd_buffer;
struct agx_scs_layout_gen2;
struct vk_buffer;
bool bk_gen1_publish_buffer_as_addresses(struct bk_device *dev,
                                         const struct vk_buffer *buffer);
uint64_t bk_gen1_upload_as_table(struct bk_cmd_buffer *cmd);
uint64_t bk_gen1_as_metadata_addr(struct bk_cmd_buffer *cmd,
                                  struct vk_acceleration_structure *as);
struct bk_agx3_plan_args;

void bk_gen1_rq_finish(struct bk_device *dev);
VkResult bk_device_init_accel_struct_state(struct bk_device *device);
void bk_device_finish_accel_struct_state(struct bk_device *device);
void bk_m5_as_buffer_bound(struct bk_device *dev, struct vk_buffer *buffer);

/* Plans, encodes and finalizes one triangle AS from common build IR, with
 * internal phase barriers and a GPU-written indirect node count. */
VkResult bk_cmd_build_agx3_from_ir(VkCommandBuffer command_buffer,
                                 const struct bk_agx3_plan_args *args,
                                 VkDeviceAddress node_status,
                                 VkDeviceAddress envelope_status,
                                 uint32_t slot_bias);

/* The SPIR-V that the node encoding and refit pipelines of each native AS
 * format are created from; bk_agx3_as_format_for_chip() selects the format.
 * GEN1 also has a triangle-pair encoder, M5 a batched one. */
const uint32_t *bk_agx3_node_encoder_spirv(enum agx_chip chip, size_t *size_B);
const uint32_t *bk_agx3_refit_spirv(enum agx_chip chip, size_t *size_B);
const uint32_t *bk_gen1_agx3_node_encoder_spirv(bool pairs, size_t *size_B);
const uint32_t *bk_gen1_agx3_refit_spirv(size_t *size_B);
const uint32_t *bk_m5_agx3_node_encoder_spirv(size_t *size_B);
const uint32_t *bk_m5_agx3_refit_spirv(size_t *size_B);

uint64_t bk_gen1_ray_query_scs_layout(const struct bk_device *dev);
uint64_t bk_gen1_query_state(struct bk_cmd_buffer *cmd,
                            const struct agx_scs_layout_gen2 *scs);

#endif /* BK_ACCELERATION_STRUCTURE_H */
