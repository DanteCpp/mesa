/*
 * Copyright 2010 Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pipe_screen;
struct pipe_screen_config;
struct pipe_resource;
struct renderonly;

struct pipe_screen *agx_screen_create(int fd, struct renderonly *ro,
                                      const struct pipe_screen_config *config);

#ifdef _WIN32
struct agx_kmt_luid;

struct pipe_screen *agx_screen_create_d3dkmt(const struct agx_kmt_luid *luid,
                                             const void *rt_bridge);

struct pipe_resource *agx_resource_from_d3dkmt_allocation(
   struct pipe_screen *pscreen, const struct pipe_resource *templ,
   uint32_t allocation, size_t size, uint32_t linear_stride_B, bool linear,
   bool is_primary);

uint32_t agx_resource_get_d3dkmt_allocation(struct pipe_resource *prsrc);
#endif

#ifdef __cplusplus
}
#endif
