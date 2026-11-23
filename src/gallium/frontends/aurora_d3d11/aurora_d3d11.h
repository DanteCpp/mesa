/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Native Direct3D 11 / WDDM 2.0 / DXGI 1.5 User-Mode Display Driver
 * directly backed by Mesa Gallium Asahi (Apple Silicon G17G / M5).
 */
#pragma once

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

#define __D3D10EFFECT_H__
#include <d3d10umddi.h>
#include <dxgiddi.h>

/* Avoid Win32 macro clashes with Mesa/Gallium headers. */
#undef min
#undef max
#undef near
#undef far
#undef ERROR
#undef Yield

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "asahi/lib/agx_d3dkmt.h"
#include "gallium/drivers/asahi/agx_public.h"

extern bool agx_d3dkmt_enumerate_adapters(struct agx_kmt_luid *out,
                                          uint32_t capacity, uint32_t *count);

#include "sm5_to_nir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * AppleAgxWddm KMD ABI constants (exact mirror of core/agxw_contract.h &
 * core/agxw_agreement.h; verified by _Static_assert).
 * ========================================================================= */
#define AURORA_AGXW_ADAPTER_IDENTITY_MAGIC    0x57584741u /* 'AGXW' */
#define AURORA_AGXW_ADAPTER_IDENTITY_VERSION  1u
#define AURORA_AGXW_RESOURCE_PRIVATE_VERSION  1u
#define AURORA_AGXW_ALLOC_PRIVATE_VERSION     1u
#define AURORA_AGXW_CONTEXT_PRIVATE_VERSION   1u

#define AURORA_AGXW_WDDM_LARGE_PAGE_BITS      16u
#define AURORA_AGXW_WDDM_LARGE_PAGE_SIZE      (1ull << 16)
#define AURORA_AGXW_WDDM_LARGE_PAGE_MASK      (AURORA_AGXW_WDDM_LARGE_PAGE_SIZE - 1ull)

#define AURORA_AGXW_ALLOC_WRITEBACK           (1u << 0)
#define AURORA_AGXW_ALLOC_VM_PRIVATE          (1u << 1)
#define AURORA_AGXW_ALLOC_SHAREABLE           (1u << 2)
#define AURORA_AGXW_ALLOC_NO_CPU_MAP          (1u << 4)
#define AURORA_AGXW_ALLOC_PRIMARY             (1u << 5)
#define AURORA_AGXW_ALLOC_PITCH_ALIGN128      (1u << 6)
#define AURORA_AGXW_ALLOC_DIRECT_SCANOUT      (1u << 9)
#define AURORA_AGXW_ALLOC_LINEAR_TIGHT        (1u << 10)

#define AURORA_AGXW_STANDARD_DESC_TAG         UINT64_C(0xa7)
#define AURORA_AGXW_STANDARD_DESC_DIM_MASK    UINT32_C(0x000fffff)
#define AURORA_AGXW_STANDARD_DESC_FORMAT_MASK UINT32_C(0x0000ffff)

#define AURORA_AGXW_CONTEXT_FLAG_VA_INTERLOCK (1u << 0)
#define AURORA_AGXW_PRIORITY_MEDIUM           1u
#define AURORA_AGXW_VM_USER_START             UINT64_C(0x0000000100000000)
#define AURORA_AGXW_J813_USC_EXEC_BASE        UINT64_C(0x0000004c00000000)

#define AURORA_AGXW_SWITCH_ON                 0x4631334Au /* 'J13F' */

struct aurora_agxw_adapter_identity {
   uint32_t magic;
   uint32_t version;
   uint32_t struct_size;
   uint32_t chip_id;
   uint32_t gpu_generation;
   uint32_t gpu_variant;
   uint32_t gpu_revision;
   uint32_t contract_version;
   uint64_t features;
   uint32_t params_blob_size;
   uint32_t submit_private_max_version;
};
_Static_assert(sizeof(struct aurora_agxw_adapter_identity) == 48, "identity size");

struct aurora_agxw_alloc_private {
   uint32_t version;
   uint32_t struct_size;
   uint64_t size;
   uint32_t flags;
   uint32_t align_log2;
   uint64_t reserved;
};
_Static_assert(sizeof(struct aurora_agxw_alloc_private) == 32, "alloc_private size");

struct aurora_agxw_resource_private {
   uint32_t version;
   uint32_t struct_size;
   uint32_t allocation_count;
   uint32_t flags;
};
_Static_assert(sizeof(struct aurora_agxw_resource_private) == 16, "resource_private size");

struct aurora_agxw_context_private {
   uint32_t version;
   uint32_t struct_size;
   uint32_t priority;
   uint32_t flags;
   uint64_t usc_exec_base;
};
_Static_assert(sizeof(struct aurora_agxw_context_private) == 24, "context_private size");

/* In-process bridge struct passed to agx_screen_create_d3dkmt(); matches
 * agx_d3dkmt_bridge.c::AuroraGpuBridge ABI v4 bit-for-bit. */
#define AURORA_GPU_BRIDGE_ABI_VERSION 4u
struct aurora_gpu_bridge_v4 {
   unsigned int struct_size;
   unsigned int abi_version;
   const void *callbacks;
   void *hRTDevice;
   const volatile long *alive;
   unsigned long alive_value;
   volatile long *in_flight;
   void *hRTAdapter;
   const void *query_adapter_info;
};

static inline uint32_t
aurora_agxw_alloc_pitch(uint32_t width, uint32_t flags)
{
   const uint32_t pitch = width * 4u;
   return (flags & AURORA_AGXW_ALLOC_PITCH_ALIGN128)
             ? ((pitch + 127u) & ~127u)
             : pitch;
}

static inline int
aurora_agxw_standard_desc_pack(uint32_t width, uint32_t height,
                               uint32_t d3d_fmt, uint64_t *out)
{
   if (!out || width == 0 || height == 0 ||
       width > AURORA_AGXW_STANDARD_DESC_DIM_MASK ||
       height > AURORA_AGXW_STANDARD_DESC_DIM_MASK ||
       d3d_fmt == 0 || d3d_fmt > AURORA_AGXW_STANDARD_DESC_FORMAT_MASK)
      return -1;
   *out = (AURORA_AGXW_STANDARD_DESC_TAG << 56) |
          ((uint64_t)(d3d_fmt & AURORA_AGXW_STANDARD_DESC_FORMAT_MASK) << 40) |
          ((uint64_t)(height & AURORA_AGXW_STANDARD_DESC_DIM_MASK) << 20) |
          (uint64_t)(width & AURORA_AGXW_STANDARD_DESC_DIM_MASK);
   return 0;
}

static inline int
aurora_agxw_standard_desc_unpack(uint64_t desc, uint32_t *width,
                                 uint32_t *height, uint32_t *d3d_fmt)
{
   if ((desc >> 56) != AURORA_AGXW_STANDARD_DESC_TAG)
      return -1;
   uint32_t w = (uint32_t)(desc & AURORA_AGXW_STANDARD_DESC_DIM_MASK);
   uint32_t h = (uint32_t)((desc >> 20) & AURORA_AGXW_STANDARD_DESC_DIM_MASK);
   uint32_t f = (uint32_t)((desc >> 40) & AURORA_AGXW_STANDARD_DESC_FORMAT_MASK);
   if (w == 0 || h == 0 || f == 0)
      return -1;
   if (width) *width = w;
   if (height) *height = h;
   if (d3d_fmt) *d3d_fmt = f;
   return 0;
}

/* =========================================================================
 * Driver Object Types
 * ========================================================================= */
#define AURORA_MAX_RTVS        8u
#define AURORA_MAX_SRVS        128u
#define AURORA_MAX_SAMPLERS    16u
#define AURORA_MAX_CBS         14u
#define AURORA_MAX_VBS         32u
#define AURORA_MAX_VIEWPORTS   16u
#define AURORA_MAX_SCISSORS    16u
#define AURORA_MAX_UAVS        8u
#define AURORA_STAGE_COUNT     6u /* VS, PS, GS, HS, DS, CS */

enum aurora_stage {
   AURORA_STAGE_VS = 0,
   AURORA_STAGE_PS = 1,
   AURORA_STAGE_GS = 2,
   AURORA_STAGE_HS = 3,
   AURORA_STAGE_DS = 4,
   AURORA_STAGE_CS = 5,
};

struct aurora_adapter {
   D3D10DDI_HRTADAPTER hRTAdapter;
   PFND3DDDI_QUERYADAPTERINFOCB query_adapter_info;
   struct aurora_agxw_adapter_identity identity;
   bool identity_valid;
};

struct aurora_resource {
   struct pipe_resource *prsrc;
   uint64_t gen; /* bumped on RotateResourceIdentities */

   D3D10DDIRESOURCE_TYPE dim;
   DXGI_FORMAT dxgi_format;
   enum pipe_format pipe_format;
   UINT width;
   UINT height;
   UINT depth;
   UINT mip_levels;
   UINT array_size;
   UINT sample_count;
   UINT bind_flags;
   UINT misc_flags;
   UINT usage;
   UINT map_flags;
   UINT structure_stride;

   D3D10DDI_HRTRESOURCE hRTResource;
   D3DKMT_HANDLE hKMResource;
   D3DKMT_HANDLE hKMAllocation;
   uint64_t alloc_size;
   uint32_t row_pitch;
   bool is_primary;
   bool is_direct_scanout;
   bool is_shared;
   bool owns_km_resource;

   /* Active CPU Map transfers, indexed by subresource. */
   struct pipe_transfer **transfers;
   UINT num_subresources;

   /* CPU-shadowed fast path for small pure constant buffers (uploaded into
    * Gallium's persistent const_uploader ring on Draw/Dispatch instead of
    * allocating/shadowing per-update 16 KiB D3DKMT GPU BOs). */
   uint8_t *cb_cpu;
   uint64_t cb_ver;
   struct pipe_resource *cb_gpu_buf;
   unsigned cb_gpu_off;
   uint64_t cb_gpu_ver;
   bool cb_mapped_write;
};

struct aurora_rtv {
   struct aurora_resource *res;
   struct pipe_surface surf;
   DXGI_FORMAT dxgi_format;
   D3D10DDIRESOURCE_TYPE dim;
};

struct aurora_dsv {
   struct aurora_resource *res;
   struct pipe_surface surf;
   DXGI_FORMAT dxgi_format;
   UINT flags;
};

struct aurora_srv {
   struct aurora_resource *res;
   uint64_t res_gen;
   DXGI_FORMAT dxgi_format;
   D3D10DDIRESOURCE_TYPE dim;
   bool is_raw_or_struct;
   uint32_t buf_offset;
   uint32_t buf_size;
   struct pipe_sampler_view templ;
   struct pipe_sampler_view *view;
};

struct aurora_uav {
   struct aurora_resource *res;
   uint64_t res_gen;
   DXGI_FORMAT dxgi_format;
   D3D10DDIRESOURCE_TYPE dim;
   bool is_buffer;
   bool is_raw_or_struct;
   uint32_t buf_offset;
   uint32_t buf_size;
   struct pipe_image_view img_templ;
};

struct aurora_shader {
   mesa_shader_stage stage;
   void *cso;
   struct aurora_sm5_info info;
};

struct aurora_blend {
   void *cso;
   struct pipe_blend_state state;
};

struct aurora_dsa {
   void *cso;
   struct pipe_depth_stencil_alpha_state state;
};

struct aurora_rast {
   void *cso;
   struct pipe_rasterizer_state state;
   bool scissor_enable;
};

struct aurora_sampler {
   void *cso;
   struct pipe_sampler_state state;
};

struct aurora_velems {
   unsigned count;
   struct pipe_vertex_element elems[PIPE_MAX_ATTRIBS];
};

struct aurora_query {
   D3D10DDI_QUERY ddi_type;
   UINT misc_flags;
   struct pipe_query *pq;
   struct pipe_fence_handle *event_fence;
   bool begun;
   bool ended;
};

/* Per-device stable bridge storage (lives in process static pool so
 * agx_d3dkmt_bridge's alive/in_flight pointers remain valid forever). */
struct aurora_bridge_slot {
   D3DDDI_DEVICECALLBACKS callbacks_copy;
   volatile LONG alive;
   volatile LONG in_flight;
   struct aurora_gpu_bridge_v4 bridge;
   bool in_use;
};

struct aurora_device {
   struct aurora_adapter *adapter;
   D3D10DDI_HRTDEVICE hRTDevice;
   D3D10DDI_HRTCORELAYER hRTCoreLayer;
   UINT interface_version;
   D3DDDI_DEVICECALLBACKS kt_callbacks;
   const D3D11DDI_CORELAYER_DEVICECALLBACKS *um_callbacks;

   /* DXGI present callbacks captured in CreateDevice. */
   PFNDDXGIDDI_PRESENTCB pfnPresentCb;
   PFNDDXGIDDI_PRESENT_MULTIPLANE_OVERLAYCB pfnPresentMpoCb;

   /* Lazily created VM-only kernel context for PresentCb. */
   HANDLE hKMContext;

   CRITICAL_SECTION lock;
   CRITICAL_SECTION km_ctx_lock;
   bool locks_init;
   volatile LONG present_in_flight;

   /* Direct Gallium Asahi backend (zero DXVK, zero Vulkan). */
   struct aurora_bridge_slot *bridge_slot;
   struct pipe_screen *screen;
   struct pipe_context *pctx;

   /* Bound pipeline state */
   struct aurora_shader *vs;
   struct aurora_shader *ps;
   struct aurora_shader *gs;
   struct aurora_shader *hs;
   struct aurora_shader *ds;
   struct aurora_shader *cs;
   struct aurora_velems *velems;
   struct aurora_blend *blend;
   struct aurora_dsa *dsa;
   struct aurora_rast *rast;

   struct pipe_blend_color blend_color;
   UINT sample_mask;
   UINT stencil_ref;

   struct aurora_rtv *rtvs[AURORA_MAX_RTVS];
   UINT num_rtvs;
   struct aurora_dsv *dsv;
   bool fb_dirty;

   struct aurora_uav *om_uavs[AURORA_MAX_UAVS];
   UINT om_uav_start;
   UINT om_uav_count;
   struct aurora_uav *cs_uavs[AURORA_MAX_UAVS];

   struct aurora_srv *srvs[AURORA_STAGE_COUNT][AURORA_MAX_SRVS];
   UINT max_srv_slot[AURORA_STAGE_COUNT];
   bool srv_dirty[AURORA_STAGE_COUNT];
   bool uav_dirty[AURORA_STAGE_COUNT];

   struct aurora_sampler *samplers[AURORA_STAGE_COUNT][AURORA_MAX_SAMPLERS];
   UINT max_sampler_slot[AURORA_STAGE_COUNT];

   struct pipe_constant_buffer cbs[AURORA_STAGE_COUNT][AURORA_MAX_CBS];
   struct aurora_resource *cb_res[AURORA_STAGE_COUNT][AURORA_MAX_CBS];
   UINT cb_off[AURORA_STAGE_COUNT][AURORA_MAX_CBS];
   UINT cb_sz[AURORA_STAGE_COUNT][AURORA_MAX_CBS];
   uint64_t cb_bound_ver[AURORA_STAGE_COUNT][AURORA_MAX_CBS];
   bool cb_dirty[AURORA_STAGE_COUNT];

   struct pipe_vertex_buffer vbs[AURORA_MAX_VBS];
   uint32_t vb_strides[AURORA_MAX_VBS];
   UINT max_vb_slot;
   bool vb_dirty;
   void *active_velems_cso;
   bool velems_dirty;

   struct aurora_resource *ib_res;
   DXGI_FORMAT ib_format;
   UINT ib_offset;

   D3D10_DDI_PRIMITIVE_TOPOLOGY topology;
   enum mesa_prim pipe_prim;
   uint8_t patch_vertices;

   struct pipe_viewport_state viewports[AURORA_MAX_VIEWPORTS];
   UINT num_viewports;
   struct pipe_scissor_state scissors[AURORA_MAX_SCISSORS];
   UINT num_scissors;
   bool scissor_dirty;

   /* Cached default & internal clear CSOs. */
   void *default_blend_cso;
   void *default_dsa_cso;
   void *no_depth_dsa_cso;
   void *default_rast_cso;
   void *default_sampler_cso;
   void *empty_velems_cso;
   void *clear_vs_cso;
   void *clear_fs_cso;
   void *clear_blend_cso;
   void *clear_dsa_cso;
   void *clear_rast_cso;

   /* Cached per-format CheckFormatSupport results (fast path for D2D/DWM). */
   UINT fmt_caps_cache[192];
   volatile LONG fmt_caps_valid[192];
};

/* =========================================================================
 * Diagnostics & Driver-Key Gate (aurora_adapter.c)
 * ========================================================================= */
void aurora_log(const char *fmt, ...);
void aurora_log_err(const char *fmt, ...);
bool aurora_gate_present_real(void);
void aurora_set_error_at(struct aurora_device *dev, HRESULT hr,
                         const char *func, int line);
#define aurora_set_error(dev, hr) aurora_set_error_at((dev), (hr), __func__, __LINE__)
void aurora_flush_and_wait(struct aurora_device *dev);
void aurora_ensure_present_context(struct aurora_device *dev);
HRESULT APIENTRY aurora_RetrieveSubObject(
   D3D10DDI_HDEVICE hDevice, UINT32 SubDeviceID, SIZE_T ParamSize,
   void *pParams, SIZE_T OutputParamSize, void *pOutputParamsBuffer);

static inline void
aurora_lock(struct aurora_device *dev)
{
   if (dev && dev->locks_init)
      EnterCriticalSection(&dev->lock);
}

static inline void
aurora_unlock(struct aurora_device *dev)
{
   if (dev && dev->locks_init)
      LeaveCriticalSection(&dev->lock);
}

static inline mesa_shader_stage
aurora_stage_to_mesa(enum aurora_stage st)
{
   switch (st) {
   case AURORA_STAGE_VS: return MESA_SHADER_VERTEX;
   case AURORA_STAGE_PS: return MESA_SHADER_FRAGMENT;
   case AURORA_STAGE_GS: return MESA_SHADER_GEOMETRY;
   case AURORA_STAGE_HS: return MESA_SHADER_TESS_CTRL;
   case AURORA_STAGE_DS: return MESA_SHADER_TESS_EVAL;
   case AURORA_STAGE_CS: return MESA_SHADER_COMPUTE;
   default:              return MESA_SHADER_FRAGMENT;
   }
}

/* View refresh helpers (handle RotateResourceIdentities seamlessly). */
bool aurora_rtv_fill_surface(struct aurora_rtv *rtv, struct pipe_surface *out);
bool aurora_dsv_fill_surface(struct aurora_dsv *dsv, struct pipe_surface *out);
struct pipe_sampler_view *aurora_srv_get_view(struct aurora_device *dev,
                                              struct aurora_srv *srv);
void aurora_sync_framebuffer(struct aurora_device *dev);
void aurora_sync_stage_resources(struct aurora_device *dev,
                                 enum aurora_stage stage);
void aurora_sync_draw_state(struct aurora_device *dev);

/* =========================================================================
 * DDI Entrypoints across modules
 * ========================================================================= */

/* aurora_format.c */
void APIENTRY aurora_CheckFormatSupport(D3D10DDI_HDEVICE hDevice,
                                        DXGI_FORMAT Format,
                                        UINT *pFormatCaps);
void APIENTRY aurora_CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                                   DXGI_FORMAT Format,
                                                   UINT SampleCount,
                                                   UINT *pNumQualityLevels);
void APIENTRY aurora_CheckMultisampleQualityLevels_1_3(D3D10DDI_HDEVICE hDevice,
                                                       DXGI_FORMAT Format,
                                                       UINT SampleCount,
                                                       UINT Flags,
                                                       UINT *pNumQualityLevels);

/* aurora_resource.c */
SIZE_T APIENTRY aurora_CalcPrivateResourceSize(D3D10DDI_HDEVICE,
                                               const D3D11DDIARG_CREATERESOURCE *);
SIZE_T APIENTRY aurora_CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE,
                                                     const D3D10DDIARG_OPENRESOURCE *);
void APIENTRY aurora_CreateResource(D3D10DDI_HDEVICE,
                                    const D3D11DDIARG_CREATERESOURCE *,
                                    D3D10DDI_HRESOURCE, D3D10DDI_HRTRESOURCE);
void APIENTRY aurora_OpenResource(D3D10DDI_HDEVICE,
                                  const D3D10DDIARG_OPENRESOURCE *,
                                  D3D10DDI_HRESOURCE, D3D10DDI_HRTRESOURCE);
void APIENTRY aurora_DestroyResource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE);
void APIENTRY aurora_ResourceMap(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                 D3D10_DDI_MAP, UINT, D3D10DDI_MAPPED_SUBRESOURCE *);
void APIENTRY aurora_ResourceUnmap(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);
BOOL APIENTRY aurora_ResourceIsStagingBusy(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE);
void APIENTRY aurora_ResourceUpdateSubresourceUP(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                                 UINT, const D3D10_DDI_BOX *,
                                                 const VOID *, UINT, UINT);
void APIENTRY aurora_ResourceUpdateSubresourceUP_11_1(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                                      UINT, const D3D10_DDI_BOX *,
                                                      const VOID *, UINT, UINT, UINT);
void APIENTRY aurora_ResourceCopy(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, D3D10DDI_HRESOURCE);
void APIENTRY aurora_ResourceCopyRegion(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                        UINT, UINT, UINT, D3D10DDI_HRESOURCE, UINT,
                                        const D3D10_DDI_BOX *);
void APIENTRY aurora_ResourceCopyRegion_11_1(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                             UINT, UINT, UINT, D3D10DDI_HRESOURCE, UINT,
                                             const D3D10_DDI_BOX *, UINT);
void APIENTRY aurora_ResourceResolveSubresource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                                D3D10DDI_HRESOURCE, UINT, DXGI_FORMAT);
void APIENTRY aurora_ResourceConvert(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, D3D10DDI_HRESOURCE);
void APIENTRY aurora_ResourceConvertRegion(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                           UINT, UINT, UINT, D3D10DDI_HRESOURCE, UINT,
                                           const D3D10_DDI_BOX *);
void APIENTRY aurora_ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE);
void APIENTRY aurora_SRVReadAfterWriteHazard(D3D10DDI_HDEVICE, D3D10DDI_HSHADERRESOURCEVIEW,
                                             D3D10DDI_HRESOURCE);
void APIENTRY aurora_SetResourceMinLOD(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, FLOAT);
void APIENTRY aurora_GetResourceLayout(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                       D3DKMT_HANDLE *, D3DWDDM2_0DDI_TEXTURE_LAYOUT *,
                                       UINT *, D3DWDDM2_0DDI_SUBRESOURCE_LAYOUT *);
void APIENTRY aurora_GetMipPacking(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT *, UINT *);
void APIENTRY aurora_UpdateTileMappings(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                        const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                        const D3DWDDM1_3DDI_TILE_REGION_SIZE *,
                                        D3D10DDI_HRESOURCE, UINT, const UINT *,
                                        const UINT *, const UINT *, UINT);
void APIENTRY aurora_CopyTileMappings(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                      const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                      D3D10DDI_HRESOURCE,
                                      const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                      const D3DWDDM1_3DDI_TILE_REGION_SIZE *, UINT);
void APIENTRY aurora_CopyTiles(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                               const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                               const D3DWDDM1_3DDI_TILE_REGION_SIZE *,
                               D3D10DDI_HRESOURCE, UINT64, UINT);
void APIENTRY aurora_UpdateTiles(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                 const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                 const D3DWDDM1_3DDI_TILE_REGION_SIZE *,
                                 const VOID *, UINT);
void APIENTRY aurora_TiledResourceBarrier(D3D10DDI_HDEVICE, D3D11DDI_HANDLETYPE, VOID *,
                                          D3D11DDI_HANDLETYPE, VOID *);
void APIENTRY aurora_ResizeTilePool(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT64);

/* aurora_state.c */
SIZE_T APIENTRY aurora_CalcPrivateBlendStateSize_10(D3D10DDI_HDEVICE, const D3D10_1_DDI_BLEND_DESC *);
void APIENTRY aurora_CreateBlendState_10(D3D10DDI_HDEVICE, const D3D10_1_DDI_BLEND_DESC *, D3D10DDI_HBLENDSTATE, D3D10DDI_HRTBLENDSTATE);
SIZE_T APIENTRY aurora_CalcPrivateBlendStateSize_11_1(D3D10DDI_HDEVICE, const D3D11_1_DDI_BLEND_DESC *);
void APIENTRY aurora_CreateBlendState_11_1(D3D10DDI_HDEVICE, const D3D11_1_DDI_BLEND_DESC *, D3D10DDI_HBLENDSTATE, D3D10DDI_HRTBLENDSTATE);
void APIENTRY aurora_DestroyBlendState(D3D10DDI_HDEVICE, D3D10DDI_HBLENDSTATE);
void APIENTRY aurora_SetBlendState(D3D10DDI_HDEVICE, D3D10DDI_HBLENDSTATE, const FLOAT[4], UINT);

SIZE_T APIENTRY aurora_CalcPrivateDepthStencilStateSize(D3D10DDI_HDEVICE, const D3D10_DDI_DEPTH_STENCIL_DESC *);
void APIENTRY aurora_CreateDepthStencilState(D3D10DDI_HDEVICE, const D3D10_DDI_DEPTH_STENCIL_DESC *, D3D10DDI_HDEPTHSTENCILSTATE, D3D10DDI_HRTDEPTHSTENCILSTATE);
void APIENTRY aurora_DestroyDepthStencilState(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILSTATE);
void APIENTRY aurora_SetDepthStencilState(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILSTATE, UINT);

SIZE_T APIENTRY aurora_CalcPrivateRasterizerStateSize_10(D3D10DDI_HDEVICE, const D3D10_DDI_RASTERIZER_DESC *);
void APIENTRY aurora_CreateRasterizerState_10(D3D10DDI_HDEVICE, const D3D10_DDI_RASTERIZER_DESC *, D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE);
SIZE_T APIENTRY aurora_CalcPrivateRasterizerStateSize_11_1(D3D10DDI_HDEVICE, const D3D11_1_DDI_RASTERIZER_DESC *);
void APIENTRY aurora_CreateRasterizerState_11_1(D3D10DDI_HDEVICE, const D3D11_1_DDI_RASTERIZER_DESC *, D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE);
SIZE_T APIENTRY aurora_CalcPrivateRasterizerStateSize_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDI_RASTERIZER_DESC *);
void APIENTRY aurora_CreateRasterizerState_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDI_RASTERIZER_DESC *, D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE);
void APIENTRY aurora_DestroyRasterizerState(D3D10DDI_HDEVICE, D3D10DDI_HRASTERIZERSTATE);
void APIENTRY aurora_SetRasterizerState(D3D10DDI_HDEVICE, D3D10DDI_HRASTERIZERSTATE);

SIZE_T APIENTRY aurora_CalcPrivateSamplerSize(D3D10DDI_HDEVICE, const D3D10_DDI_SAMPLER_DESC *);
void APIENTRY aurora_CreateSampler(D3D10DDI_HDEVICE, const D3D10_DDI_SAMPLER_DESC *, D3D10DDI_HSAMPLER, D3D10DDI_HRTSAMPLER);
void APIENTRY aurora_DestroySampler(D3D10DDI_HDEVICE, D3D10DDI_HSAMPLER);

SIZE_T APIENTRY aurora_CalcPrivateElementLayoutSize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEELEMENTLAYOUT *);
void APIENTRY aurora_CreateElementLayout(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEELEMENTLAYOUT *, D3D10DDI_HELEMENTLAYOUT, D3D10DDI_HRTELEMENTLAYOUT);
void APIENTRY aurora_DestroyElementLayout(D3D10DDI_HDEVICE, D3D10DDI_HELEMENTLAYOUT);
void APIENTRY aurora_IaSetInputLayout(D3D10DDI_HDEVICE, D3D10DDI_HELEMENTLAYOUT);

void APIENTRY aurora_SetViewports(D3D10DDI_HDEVICE, UINT, UINT, const D3D10_DDI_VIEWPORT *);
void APIENTRY aurora_SetScissorRects(D3D10DDI_HDEVICE, UINT, UINT, const D3D10_DDI_RECT *);
void APIENTRY aurora_IaSetTopology(D3D10DDI_HDEVICE, D3D10_DDI_PRIMITIVE_TOPOLOGY);
void APIENTRY aurora_IaSetVertexBuffers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HRESOURCE *, const UINT *, const UINT *);
void APIENTRY aurora_IaSetIndexBuffer(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, DXGI_FORMAT, UINT);
void APIENTRY aurora_SoSetTargets(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HRESOURCE *, const UINT *);
void APIENTRY aurora_SetRenderTargets(D3D10DDI_HDEVICE, const D3D10DDI_HRENDERTARGETVIEW *, UINT, UINT,
                                      D3D10DDI_HDEPTHSTENCILVIEW, const D3D11DDI_HUNORDEREDACCESSVIEW *,
                                      const UINT *, UINT, UINT, UINT, UINT);
void APIENTRY aurora_CsSetUAVs(D3D10DDI_HDEVICE, UINT, UINT, const D3D11DDI_HUNORDEREDACCESSVIEW *, const UINT *);

#define AURORA_DECL_STAGE_BINDERS(ST) \
   void APIENTRY aurora_##ST##SetConstantBuffers10(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HRESOURCE *); \
   void APIENTRY aurora_##ST##SetConstantBuffers11_1(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HRESOURCE *, const UINT *, const UINT *); \
   void APIENTRY aurora_##ST##SetShaderResources(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSHADERRESOURCEVIEW *); \
   void APIENTRY aurora_##ST##SetSamplers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSAMPLER *);
AURORA_DECL_STAGE_BINDERS(Vs)
AURORA_DECL_STAGE_BINDERS(Ps)
AURORA_DECL_STAGE_BINDERS(Gs)
AURORA_DECL_STAGE_BINDERS(Hs)
AURORA_DECL_STAGE_BINDERS(Ds)
AURORA_DECL_STAGE_BINDERS(Cs)
#undef AURORA_DECL_STAGE_BINDERS

void APIENTRY aurora_Draw(D3D10DDI_HDEVICE, UINT, UINT);
void APIENTRY aurora_DrawIndexed(D3D10DDI_HDEVICE, UINT, UINT, INT);
void APIENTRY aurora_DrawInstanced(D3D10DDI_HDEVICE, UINT, UINT, UINT, UINT);
void APIENTRY aurora_DrawIndexedInstanced(D3D10DDI_HDEVICE, UINT, UINT, UINT, INT, UINT);
void APIENTRY aurora_DrawAuto(D3D10DDI_HDEVICE);
void APIENTRY aurora_DrawInstancedIndirect(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);
void APIENTRY aurora_DrawIndexedInstancedIndirect(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);
void APIENTRY aurora_Dispatch(D3D10DDI_HDEVICE, UINT, UINT, UINT);
void APIENTRY aurora_DispatchIndirect(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);

/* aurora_view_shader_query.c */
SIZE_T APIENTRY aurora_CalcPrivateRTVSize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATERENDERTARGETVIEW *);
void APIENTRY aurora_CreateRTV(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATERENDERTARGETVIEW *, D3D10DDI_HRENDERTARGETVIEW, D3D10DDI_HRTRENDERTARGETVIEW);
SIZE_T APIENTRY aurora_CalcPrivateRTVSize_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *);
void APIENTRY aurora_CreateRTV_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *, D3D10DDI_HRENDERTARGETVIEW, D3D10DDI_HRTRENDERTARGETVIEW);
void APIENTRY aurora_DestroyRTV(D3D10DDI_HDEVICE, D3D10DDI_HRENDERTARGETVIEW);

SIZE_T APIENTRY aurora_CalcPrivateDSVSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *);
void APIENTRY aurora_CreateDSV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *, D3D10DDI_HDEPTHSTENCILVIEW, D3D10DDI_HRTDEPTHSTENCILVIEW);
void APIENTRY aurora_DestroyDSV(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILVIEW);

SIZE_T APIENTRY aurora_CalcPrivateSRVSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *);
void APIENTRY aurora_CreateSRV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *, D3D10DDI_HSHADERRESOURCEVIEW, D3D10DDI_HRTSHADERRESOURCEVIEW);
SIZE_T APIENTRY aurora_CalcPrivateSRVSize_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *);
void APIENTRY aurora_CreateSRV_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *, D3D10DDI_HSHADERRESOURCEVIEW, D3D10DDI_HRTSHADERRESOURCEVIEW);
void APIENTRY aurora_DestroySRV(D3D10DDI_HDEVICE, D3D10DDI_HSHADERRESOURCEVIEW);

SIZE_T APIENTRY aurora_CalcPrivateUAVSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *);
void APIENTRY aurora_CreateUAV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *, D3D11DDI_HUNORDEREDACCESSVIEW, D3D11DDI_HRTUNORDEREDACCESSVIEW);
SIZE_T APIENTRY aurora_CalcPrivateUAVSize_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *);
void APIENTRY aurora_CreateUAV_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *, D3D11DDI_HUNORDEREDACCESSVIEW, D3D11DDI_HRTUNORDEREDACCESSVIEW);
void APIENTRY aurora_DestroyUAV(D3D10DDI_HDEVICE, D3D11DDI_HUNORDEREDACCESSVIEW);

void APIENTRY aurora_ClearRenderTargetView(D3D10DDI_HDEVICE, D3D10DDI_HRENDERTARGETVIEW, FLOAT[4]);
void APIENTRY aurora_ClearDepthStencilView(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILVIEW, UINT, FLOAT, UINT8);
void APIENTRY aurora_ClearView(D3D10DDI_HDEVICE, D3D11DDI_HANDLETYPE, VOID *, const FLOAT[4], const D3D10_DDI_RECT *, UINT);
void APIENTRY aurora_ClearUAVUint(D3D10DDI_HDEVICE, D3D11DDI_HUNORDEREDACCESSVIEW, const UINT[4]);
void APIENTRY aurora_ClearUAVFloat(D3D10DDI_HDEVICE, D3D11DDI_HUNORDEREDACCESSVIEW, const FLOAT[4]);
void APIENTRY aurora_CopyStructureCount(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT, D3D11DDI_HUNORDEREDACCESSVIEW);
void APIENTRY aurora_Discard(D3D10DDI_HDEVICE, D3D11DDI_HANDLETYPE, VOID *, const D3D10_DDI_RECT *, UINT);
void APIENTRY aurora_GenerateMips(D3D10DDI_HDEVICE, D3D10DDI_HSHADERRESOURCEVIEW);

SIZE_T APIENTRY aurora_CalcPrivateShaderSize(D3D10DDI_HDEVICE, const UINT *, const VOID *);
SIZE_T APIENTRY aurora_CalcPrivateTessShaderSize(D3D10DDI_HDEVICE, const UINT *, const VOID *);
SIZE_T APIENTRY aurora_CalcPrivateGSWithSOSize(D3D10DDI_HDEVICE, const VOID *);
void APIENTRY aurora_CreateVertexShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void APIENTRY aurora_CreatePixelShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void APIENTRY aurora_CreateComputeShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER);
void APIENTRY aurora_CreateGeometryShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void APIENTRY aurora_CreateGSWithSO(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void APIENTRY aurora_CreateHullShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void APIENTRY aurora_CreateDomainShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void APIENTRY aurora_DestroyShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void APIENTRY aurora_VsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void APIENTRY aurora_PsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void APIENTRY aurora_CsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void APIENTRY aurora_GsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void APIENTRY aurora_HsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void APIENTRY aurora_DsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
HRESULT APIENTRY aurora_RetrieveShaderComment(D3D10DDI_HDEVICE, D3D10DDI_HSHADER, WCHAR *, SIZE_T *);
void APIENTRY aurora_AssignDebugBinary(D3D10DDI_HDEVICE, D3D10DDI_HSHADER, UINT, const VOID *);

SIZE_T APIENTRY aurora_CalcPrivateQuerySize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY *);
void APIENTRY aurora_CreateQuery(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY *, D3D10DDI_HQUERY, D3D10DDI_HRTQUERY);
SIZE_T APIENTRY aurora_CalcPrivateQuerySize_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATEQUERY *);
void APIENTRY aurora_CreateQuery_WDDM2_0(D3D10DDI_HDEVICE, const D3DWDDM2_0DDIARG_CREATEQUERY *, D3D10DDI_HQUERY, D3D10DDI_HRTQUERY);
void APIENTRY aurora_DestroyQuery(D3D10DDI_HDEVICE, D3D10DDI_HQUERY);
void APIENTRY aurora_QueryBegin(D3D10DDI_HDEVICE, D3D10DDI_HQUERY);
void APIENTRY aurora_QueryEnd(D3D10DDI_HDEVICE, D3D10DDI_HQUERY);
void APIENTRY aurora_QueryGetData(D3D10DDI_HDEVICE, D3D10DDI_HQUERY, VOID *, UINT, UINT);
void APIENTRY aurora_SetPredication(D3D10DDI_HDEVICE, D3D10DDI_HQUERY, BOOL);
void APIENTRY aurora_Flush(D3D10DDI_HDEVICE);
BOOL APIENTRY aurora_Flush11_1(D3D10DDI_HDEVICE, UINT);
BOOL APIENTRY aurora_Flush_WDDM2_0(D3D10DDI_HDEVICE, UINT, UINT);
void APIENTRY aurora_CheckCounterInfo(D3D10DDI_HDEVICE, D3D10DDI_COUNTER_INFO *);
void APIENTRY aurora_CheckCounter(D3D10DDI_HDEVICE, D3D10DDI_QUERY, D3D10DDI_COUNTER_TYPE *,
                                  UINT *, LPSTR, UINT *, LPSTR, UINT *, LPSTR, UINT *);
void APIENTRY aurora_CheckDirectFlipSupport(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                            D3D10DDI_HRESOURCE, UINT, BOOL *);

/* aurora_dxgi.c */
void aurora_install_dxgi_funcs(struct aurora_device *dev,
                               const D3D10DDIARG_CREATEDEVICE *args);

#ifdef __cplusplus
}
#endif
