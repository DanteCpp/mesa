/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#ifndef HK_ACCELERATION_STRUCTURE_H
#define HK_ACCELERATION_STRUCTURE_H

#include "hk_agx3_as_format.h"
#include "hk_bvh.h"

#include "vk_acceleration_structure.h"

struct hk_device;
struct hk_cmd_buffer;
struct agx_scs_layout_gen2;
struct vk_buffer;
bool hk_gen1_publish_buffer_as_addresses(struct hk_device *dev,
                                         const struct vk_buffer *buffer);
uint64_t hk_gen1_upload_as_table(struct hk_cmd_buffer *cmd);
uint64_t hk_gen1_as_metadata_addr(struct hk_cmd_buffer *cmd,
                                  struct vk_acceleration_structure *as);
#include "bvh/hk_build_interface.h"

struct hk_agx3_build_item {
   struct hk_agx3_plan_args plan;
   VkDeviceAddress node_status;
   VkDeviceAddress envelope_status;
   uint32_t slot_bias;
};

void hk_gen1_rq_finish(struct hk_device *dev);
VkResult hk_device_init_accel_struct_state(struct hk_device *device);
void hk_device_finish_accel_struct_state(struct hk_device *device);
uint64_t hk_m5_upload_as_table(struct hk_cmd_buffer *cmd);
uint32_t hk_m5_as_id(struct hk_device *dev, struct vk_acceleration_structure *as);
VkResult hk_cmd_build_agx3_batch_from_ir(VkCommandBuffer command_buffer,
                                         const struct hk_agx3_build_item *items,
                                         uint32_t count);
void hk_agx3_refit_save_batch(VkCommandBuffer commandBuffer,
                              const struct vk_acceleration_structure_build_state *states,
                              uint32_t build_count);
VkResult hk_cmd_refit_agx3(VkCommandBuffer commandBuffer, struct vk_device *device,
                           struct vk_meta_device *meta,
                           const struct vk_acceleration_structure_build_args *build_args,
                           const struct vk_acceleration_structure_build_state *states,
                           uint32_t build_count);
bool hk_agx3_refit_skips_leaves(const struct hk_device *dev,
                                const struct vk_acceleration_structure_build_state *state);
void hk_m5_as_buffer_bound(struct hk_device *dev, struct vk_buffer *buffer);

/* Plans, encodes and finalizes one triangle AS from common build IR, with
 * internal phase barriers and a GPU-written indirect node count. */
VkResult hk_cmd_build_agx3_from_ir(VkCommandBuffer command_buffer,
                                 const struct hk_agx3_plan_args *args,
                                 VkDeviceAddress node_status,
                                 VkDeviceAddress envelope_status,
                                 uint32_t slot_bias);

/* The SPIR-V that the node encoding and refit pipelines of each native AS
 * format are created from; hk_agx3_as_format_for_chip() selects the format.
 * GEN1 also has a triangle-pair encoder, M5 a batched one. */
const uint32_t *hk_agx3_node_encoder_spirv(enum agx_chip chip, size_t *size_B);
const uint32_t *hk_agx3_refit_spirv(enum agx_chip chip, size_t *size_B);
const uint32_t *hk_gen1_agx3_node_encoder_spirv(bool pairs, size_t *size_B);
const uint32_t *hk_gen1_agx3_refit_spirv(size_t *size_B);
const uint32_t *hk_m5_agx3_node_encoder_spirv(size_t *size_B);
const uint32_t *hk_m5_agx3_refit_spirv(size_t *size_B);

uint64_t hk_gen1_ray_query_scs_layout(const struct hk_device *dev);
uint64_t hk_gen1_query_state(struct hk_cmd_buffer *cmd,
                            const struct agx_scs_layout_gen2 *scs);

#endif /* HK_ACCELERATION_STRUCTURE_H */
