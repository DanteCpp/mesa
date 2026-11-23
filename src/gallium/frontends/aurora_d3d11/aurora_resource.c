/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Native D3D11 Resource management directly over Gallium Asahi + D3DKMT
 * (including zero-copy direct-scanout primaries and linear shared surfaces).
 */
#include "aurora_d3d11.h"
#include "aurora_format.h"

#include <stdlib.h>
#include <string.h>

static bool
is_direct_scanout_candidate(const D3D11DDIARG_CREATERESOURCE *args,
                            UINT width, UINT height)
{
   if (!args || !args->pPrimaryDesc)
      return false;
   const DXGI_DDI_PRIMARY_DESC *p = args->pPrimaryDesc;
   if ((args->BindFlags & (D3D10_DDI_BIND_PRESENT | D3D10_DDI_BIND_RENDER_TARGET)) !=
          (D3D10_DDI_BIND_PRESENT | D3D10_DDI_BIND_RENDER_TARGET) ||
       args->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       args->pInitialDataUP != NULL || args->MipLevels != 1u ||
       args->ArraySize != 1u || args->SampleDesc.Count > 1u ||
       args->Usage != D3D10_DDI_USAGE_DEFAULT ||
       width == 0u || height == 0u ||
       width != p->ModeDesc.Width || height != p->ModeDesc.Height ||
       args->Format != p->ModeDesc.Format ||
       p->VidPnSourceId == D3DDDI_ID_UNINITIALIZED)
      return false;

   return args->Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
          args->Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
          args->Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
          args->Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

static HRESULT
allocate_runtime_surface(struct aurora_device *dev,
                         D3D10DDI_HRTRESOURCE hRTResource,
                         uint64_t alloc_size, uint32_t alloc_flags,
                         uint64_t standard_desc, bool is_primary,
                         UINT vidpn_source_id,
                         D3DKMT_HANDLE *out_km_res,
                         D3DKMT_HANDLE *out_km_alloc)
{
   if (!dev || !hRTResource.handle || !dev->kt_callbacks.pfnAllocateCb ||
       !dev->kt_callbacks.pfnDeallocateCb || alloc_size == 0u)
      return E_INVALIDARG;

   struct aurora_agxw_resource_private res_priv;
   memset(&res_priv, 0, sizeof(res_priv));
   res_priv.version = AURORA_AGXW_RESOURCE_PRIVATE_VERSION;
   res_priv.struct_size = sizeof(res_priv);
   res_priv.allocation_count = 1u;

   struct aurora_agxw_alloc_private alloc_priv;
   memset(&alloc_priv, 0, sizeof(alloc_priv));
   alloc_priv.version = AURORA_AGXW_ALLOC_PRIVATE_VERSION;
   alloc_priv.struct_size = sizeof(alloc_priv);
   alloc_priv.size = alloc_size;
   alloc_priv.flags = alloc_flags;
   alloc_priv.align_log2 = AURORA_AGXW_WDDM_LARGE_PAGE_BITS;
   alloc_priv.reserved = standard_desc;

   D3DDDI_ALLOCATIONINFO2 info;
   memset(&info, 0, sizeof(info));
   info.pPrivateDriverData = &alloc_priv;
   info.PrivateDriverDataSize = sizeof(alloc_priv);
   if (is_primary) {
      info.Flags.Primary = 1u;
      info.VidPnSourceId = vidpn_source_id;
   }

   D3DDDICB_ALLOCATE alloc;
   memset(&alloc, 0, sizeof(alloc));
   alloc.pPrivateDriverData = &res_priv;
   alloc.PrivateDriverDataSize = sizeof(res_priv);
   alloc.hResource = hRTResource.handle;
   alloc.NumAllocations = 1u;
   alloc.pAllocationInfo2 = &info;

   HRESULT hr = dev->kt_callbacks.pfnAllocateCb(dev->hRTDevice.handle, &alloc);
   if (FAILED(hr) || info.hAllocation == 0u) {
      if (SUCCEEDED(hr)) {
         D3DDDICB_DEALLOCATE dealloc;
         memset(&dealloc, 0, sizeof(dealloc));
         dealloc.hResource = hRTResource.handle;
         dev->kt_callbacks.pfnDeallocateCb(dev->hRTDevice.handle, &dealloc);
      }
      return FAILED(hr) ? hr : E_FAIL;
   }
   *out_km_res = alloc.hKMResource;
   *out_km_alloc = info.hAllocation;
   return S_OK;
}

static void
build_pipe_template(const D3D11DDIARG_CREATERESOURCE *args,
                    struct pipe_resource *templ)
{
   memset(templ, 0, sizeof(*templ));
   UINT w = args->pMipInfoList ? args->pMipInfoList[0].TexelWidth : 1u;
   UINT h = args->pMipInfoList ? args->pMipInfoList[0].TexelHeight : 1u;
   UINT d = args->pMipInfoList ? args->pMipInfoList[0].TexelDepth : 1u;
   if (w == 0u) w = 1u;
   if (h == 0u) h = 1u;
   if (d == 0u) d = 1u;

   switch (args->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      templ->target = PIPE_BUFFER;
      templ->format = PIPE_FORMAT_R8_UNORM;
      templ->width0 = w;
      templ->height0 = 1;
      templ->depth0 = 1;
      templ->array_size = 1;
      templ->last_level = 0;
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      templ->target = (args->ArraySize > 1u) ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
      templ->format = aurora_dxgi_to_pipe_format(args->Format);
      templ->width0 = w;
      templ->height0 = 1;
      templ->depth0 = 1;
      templ->array_size = MAX2(args->ArraySize, 1u);
      templ->last_level = args->MipLevels > 0u ? args->MipLevels - 1u : 0u;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      templ->target = (args->ArraySize > 1u) ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
      templ->format = aurora_dxgi_to_pipe_format(args->Format);
      templ->width0 = w;
      templ->height0 = h;
      templ->depth0 = 1;
      templ->array_size = MAX2(args->ArraySize, 1u);
      templ->last_level = args->MipLevels > 0u ? args->MipLevels - 1u : 0u;
      templ->nr_samples = args->SampleDesc.Count > 1u ? args->SampleDesc.Count : 0u;
      templ->nr_storage_samples = templ->nr_samples;
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      templ->target = (args->ArraySize > 6u) ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
      templ->format = aurora_dxgi_to_pipe_format(args->Format);
      templ->width0 = w;
      templ->height0 = h;
      templ->depth0 = 1;
      templ->array_size = MAX2(args->ArraySize, 6u);
      templ->last_level = args->MipLevels > 0u ? args->MipLevels - 1u : 0u;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      templ->target = PIPE_TEXTURE_3D;
      templ->format = aurora_dxgi_to_pipe_format(args->Format);
      templ->width0 = w;
      templ->height0 = h;
      templ->depth0 = d;
      templ->array_size = 1;
      templ->last_level = args->MipLevels > 0u ? args->MipLevels - 1u : 0u;
      break;
   default:
      templ->target = PIPE_BUFFER;
      templ->format = PIPE_FORMAT_R8_UNORM;
      templ->width0 = w;
      templ->height0 = 1;
      templ->depth0 = 1;
      templ->array_size = 1;
      break;
   }

   /* Depth/stencil typeless parent needs depth format when BIND_DEPTH_STENCIL is set. */
   if (args->BindFlags & D3D10_DDI_BIND_DEPTH_STENCIL) {
      templ->format = aurora_dxgi_to_pipe_format_dsv(args->Format);
      templ->bind |= PIPE_BIND_DEPTH_STENCIL;
   }
   if (args->BindFlags & D3D10_DDI_BIND_RENDER_TARGET)
      templ->bind |= PIPE_BIND_RENDER_TARGET;
   if (args->BindFlags & D3D10_DDI_BIND_SHADER_RESOURCE)
      templ->bind |= PIPE_BIND_SAMPLER_VIEW;
   if (args->BindFlags & D3D11_DDI_BIND_UNORDERED_ACCESS)
      templ->bind |= PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SHADER_BUFFER;
   if (args->BindFlags & D3D10_DDI_BIND_VERTEX_BUFFER)
      templ->bind |= PIPE_BIND_VERTEX_BUFFER;
   if (args->BindFlags & D3D10_DDI_BIND_INDEX_BUFFER)
      templ->bind |= PIPE_BIND_INDEX_BUFFER;
   if (args->BindFlags & D3D10_DDI_BIND_CONSTANT_BUFFER)
      templ->bind |= PIPE_BIND_CONSTANT_BUFFER;
   if (args->BindFlags & D3D10_DDI_BIND_STREAM_OUTPUT)
      templ->bind |= PIPE_BIND_STREAM_OUTPUT;
   if (args->BindFlags & D3D10_DDI_BIND_PRESENT)
      templ->bind |= PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET;

   if (args->Usage == D3D10_DDI_USAGE_STAGING) {
      templ->usage = PIPE_USAGE_STAGING;
      templ->bind |= PIPE_BIND_LINEAR;
      if (args->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D &&
          templ->format != PIPE_FORMAT_NONE &&
          !util_format_is_depth_or_stencil(templ->format) &&
          !util_format_is_compressed(templ->format)) {
         /* Allow GPU-side blit into/from 2D staging textures when copying
          * from non-CPU-visible shared or direct-scanout allocations. */
         templ->bind |= PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
      }
   } else if (args->Usage == D3D10_DDI_USAGE_DYNAMIC) {
      templ->usage = PIPE_USAGE_STREAM;
   } else if (args->Usage == D3D10_DDI_USAGE_IMMUTABLE) {
      templ->usage = PIPE_USAGE_IMMUTABLE;
   } else {
      templ->usage = PIPE_USAGE_DEFAULT;
   }
}

static void
upload_initial_data(struct aurora_device *dev, struct aurora_resource *r,
                    const D3D10_DDIARG_SUBRESOURCE_UP *init)
{
   if (!init || !r->prsrc)
      return;
   if (r->dim == D3D10DDIRESOURCE_BUFFER) {
      if (init[0].pSysMem) {
         dev->pctx->buffer_subdata(dev->pctx, r->prsrc, PIPE_MAP_WRITE,
                                   0, r->width, init[0].pSysMem);
      }
      return;
   }
   UINT mips = MAX2(r->mip_levels, 1u);
   UINT slices = (r->dim == D3D10DDIRESOURCE_TEXTURE3D) ? 1u : MAX2(r->array_size, 1u);
   for (UINT s = 0; s < slices; ++s) {
      for (UINT m = 0; m < mips; ++m) {
         UINT sub = m + s * mips;
         if (!init[sub].pSysMem)
            continue;
         struct pipe_box box = {
            .x = 0, .y = 0,
            .z = (r->dim == D3D10DDIRESOURCE_TEXTURE3D) ? 0 : (int)s,
            .width = (int)u_minify(r->width, m),
            .height = (int)u_minify(r->height, m),
            .depth = (r->dim == D3D10DDIRESOURCE_TEXTURE3D)
                        ? (int)u_minify(r->depth, m) : 1,
         };
         dev->pctx->texture_subdata(dev->pctx, r->prsrc, m, PIPE_MAP_WRITE,
                                    &box, init[sub].pSysMem,
                                    init[sub].SysMemPitch,
                                    init[sub].SysMemSlicePitch);
      }
   }
}

SIZE_T APIENTRY
aurora_CalcPrivateResourceSize(D3D10DDI_HDEVICE hDevice,
                               const D3D11DDIARG_CREATERESOURCE *pArgs)
{
   (void)hDevice; (void)pArgs;
   return sizeof(struct aurora_resource);
}

SIZE_T APIENTRY
aurora_CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE hDevice,
                                     const D3D10DDIARG_OPENRESOURCE *pArgs)
{
   (void)hDevice; (void)pArgs;
   return sizeof(struct aurora_resource);
}

void APIENTRY
aurora_CreateResource(D3D10DDI_HDEVICE hDevice,
                      const D3D11DDIARG_CREATERESOURCE *pArgs,
                      D3D10DDI_HRESOURCE hResource,
                      D3D10DDI_HRTRESOURCE hRTResource)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *r = (struct aurora_resource *)hResource.pDrvPrivate;
   if (!dev || !dev->screen || !dev->pctx || !pArgs || !r) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }
   memset(r, 0, sizeof(*r));

   struct pipe_resource templ;
   build_pipe_template(pArgs, &templ);
   if (templ.format == PIPE_FORMAT_NONE) {
      aurora_log("CreateResource: unsupported DXGI format %u dim=%u",
                 (unsigned)pArgs->Format, (unsigned)pArgs->ResourceDimension);
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }

   r->dim = pArgs->ResourceDimension;
   r->dxgi_format = pArgs->Format;
   r->pipe_format = templ.format;
   r->width = templ.width0;
   r->height = templ.height0;
   r->depth = templ.depth0;
   r->mip_levels = MAX2(pArgs->MipLevels, 1u);
   r->array_size = MAX2(pArgs->ArraySize, 1u);
   r->sample_count = MAX2(pArgs->SampleDesc.Count, 1u);
   r->bind_flags = pArgs->BindFlags;
   r->misc_flags = pArgs->MiscFlags;
   r->usage = pArgs->Usage;
   r->map_flags = pArgs->MapFlags;
   r->structure_stride = pArgs->ByteStride;
   r->hRTResource = hRTResource;
   r->num_subresources =
      (r->dim == D3D10DDIRESOURCE_TEXTURE3D) ? r->mip_levels
                                             : (r->mip_levels * r->array_size);

   bool is_shared = (pArgs->MiscFlags &
                     (D3D10_DDI_RESOURCE_MISC_SHARED | 0x00000100u | 0x00000800u)) != 0u;
   bool is_primary = (pArgs->pPrimaryDesc != NULL);
   bool direct_scanout = is_direct_scanout_candidate(pArgs, r->width, r->height);
   uint32_t d3d_fmt = aurora_dxgi_to_d3dddi_format(pArgs->Format);

   aurora_lock(dev);

   if ((is_primary || is_shared) &&
       r->dim == D3D10DDIRESOURCE_TEXTURE2D &&
       r->mip_levels == 1u && r->array_size == 1u && r->sample_count <= 1u &&
       d3d_fmt != 0u && hRTResource.handle != NULL) {
      uint64_t std_desc = 0u;
      uint32_t bpp = util_format_get_blocksize(templ.format);
      uint32_t alloc_flags = AURORA_AGXW_ALLOC_SHAREABLE;
      if (direct_scanout) {
         /* agxw_contract.h: DIRECT_SCANOUT requires PRIMARY|SHAREABLE and
          * tight pitch; NO_CPU_MAP, WRITEBACK, PITCH_ALIGN128 are refused! */
         alloc_flags |= AURORA_AGXW_ALLOC_PRIMARY |
                        AURORA_AGXW_ALLOC_DIRECT_SCANOUT;
      } else if (is_primary) {
         alloc_flags |= AURORA_AGXW_ALLOC_PRIMARY;
      } else if (bpp == 4u && ((r->width * 4u) & 15u) != 0u) {
         /* AGX linear texture strides must be 16-byte aligned (ail_initialize_linear). */
         alloc_flags |= AURORA_AGXW_ALLOC_PITCH_ALIGN128;
      }
      uint32_t row_bytes = (bpp == 4u)
                              ? aurora_agxw_alloc_pitch(r->width, alloc_flags)
                              : (r->width * bpp);
      uint64_t used_bytes = (uint64_t)row_bytes * (uint64_t)r->height;
      uint64_t alloc_size = (used_bytes + AURORA_AGXW_WDDM_LARGE_PAGE_MASK) &
                            ~AURORA_AGXW_WDDM_LARGE_PAGE_MASK;
      if (alloc_size == 0u)
         alloc_size = AURORA_AGXW_WDDM_LARGE_PAGE_SIZE;

      if ((row_bytes & 15u) == 0u &&
          aurora_agxw_standard_desc_pack(r->width, r->height, d3d_fmt, &std_desc) == 0) {
         D3DKMT_HANDLE km_res = 0u, km_alloc = 0u;
         UINT vidpn = is_primary ? pArgs->pPrimaryDesc->VidPnSourceId : 0u;
         HRESULT hr = allocate_runtime_surface(
            dev, hRTResource, alloc_size, alloc_flags, std_desc,
            is_primary, vidpn, &km_res, &km_alloc);
         if (SUCCEEDED(hr) && km_alloc != 0u) {
            r->prsrc = agx_resource_from_d3dkmt_allocation(
               dev->screen, &templ, km_alloc, (size_t)alloc_size,
               row_bytes, true /* linear */, is_primary);
            if (!r->prsrc) {
               D3DDDICB_DEALLOCATE dealloc;
               memset(&dealloc, 0, sizeof(dealloc));
               dealloc.hResource = hRTResource.handle;
               dev->kt_callbacks.pfnDeallocateCb(dev->hRTDevice.handle, &dealloc);
               aurora_unlock(dev);
               aurora_log("CreateResource: agx_resource_from_d3dkmt_allocation FAILED alloc=%u %ux%u",
                          (unsigned)km_alloc, (unsigned)r->width, (unsigned)r->height);
               aurora_set_error(dev, E_OUTOFMEMORY);
               return;
            }
            r->hKMResource = km_res;
            r->hKMAllocation = km_alloc;
            r->alloc_size = alloc_size;
            r->row_pitch = row_bytes;
            r->is_primary = is_primary;
            r->is_direct_scanout = direct_scanout;
            r->is_shared = is_shared || is_primary;
            r->owns_km_resource = true;
            aurora_log("CreateResource: %s %ux%u fmt=%u alloc=%u pitch=%u size=%llu",
                       direct_scanout ? "DIRECT-PRIMARY"
                                      : (is_primary ? "PRIMARY" : "SHARED"),
                       (unsigned)r->width, (unsigned)r->height,
                       (unsigned)r->dxgi_format, (unsigned)km_alloc,
                       (unsigned)row_bytes, (unsigned long long)alloc_size);
         } else {
            aurora_unlock(dev);
            aurora_log("CreateResource: runtime AllocateCb FAILED hr=0x%08lx %ux%u primary=%u",
                       (unsigned long)hr, (unsigned)r->width, (unsigned)r->height,
                       is_primary ? 1u : 0u);
            aurora_set_error(dev, FAILED(hr) ? hr : E_OUTOFMEMORY);
            return;
         }
      }
   }

   if (!r->prsrc) {
      r->prsrc = dev->screen->resource_create(dev->screen, &templ);
      if (!r->prsrc) {
         aurora_unlock(dev);
         aurora_log("CreateResource: screen->resource_create FAILED dim=%u %ux%u fmt=%u",
                    (unsigned)r->dim, (unsigned)r->width, (unsigned)r->height,
                    (unsigned)r->dxgi_format);
         aurora_set_error(dev, E_OUTOFMEMORY);
         return;
      }
   }

   if (pArgs->pInitialDataUP)
      upload_initial_data(dev, r, pArgs->pInitialDataUP);

   aurora_unlock(dev);
}

void APIENTRY
aurora_OpenResource(D3D10DDI_HDEVICE hDevice,
                    const D3D10DDIARG_OPENRESOURCE *pArgs,
                    D3D10DDI_HRESOURCE hResource,
                    D3D10DDI_HRTRESOURCE hRTResource)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *r = (struct aurora_resource *)hResource.pDrvPrivate;
   if (!dev || !dev->screen || !pArgs || !r ||
       pArgs->NumAllocations != 1u || !pArgs->pOpenAllocationInfo2) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }
   memset(r, 0, sizeof(*r));

   const D3DDDI_OPENALLOCATIONINFO2 *oi = &pArgs->pOpenAllocationInfo2[0];
   const struct aurora_agxw_alloc_private *priv =
      (const struct aurora_agxw_alloc_private *)oi->pPrivateDriverData;

   uint32_t width = 0, height = 0, d3d_fmt = 0;
   if (oi->hAllocation == 0u || !priv ||
       oi->PrivateDriverDataSize < sizeof(*priv) ||
       priv->version != AURORA_AGXW_ALLOC_PRIVATE_VERSION ||
       priv->struct_size < sizeof(*priv) ||
       (priv->flags & AURORA_AGXW_ALLOC_SHAREABLE) == 0u ||
       aurora_agxw_standard_desc_unpack(priv->reserved, &width, &height, &d3d_fmt) != 0) {
      aurora_set_error(dev, DXGI_ERROR_UNSUPPORTED);
      return;
   }

   DXGI_FORMAT dxgi_fmt = aurora_d3dddi_to_dxgi_format(d3d_fmt);
   enum pipe_format pf = aurora_dxgi_to_pipe_format(dxgi_fmt);
   if (pf == PIPE_FORMAT_NONE) {
      aurora_set_error(dev, DXGI_ERROR_UNSUPPORTED);
      return;
   }

   bool is_primary =
      (priv->flags & (AURORA_AGXW_ALLOC_PRIMARY | AURORA_AGXW_ALLOC_DIRECT_SCANOUT)) != 0u;
   bool is_direct = (priv->flags & AURORA_AGXW_ALLOC_DIRECT_SCANOUT) != 0u;
   uint32_t row_pitch = aurora_agxw_alloc_pitch(width, priv->flags);

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = pf;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHARED;
   if (is_primary)
      templ.bind |= PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET;

   aurora_lock(dev);
   r->prsrc = agx_resource_from_d3dkmt_allocation(
      dev->screen, &templ, oi->hAllocation, (size_t)priv->size,
      row_pitch, true /* linear */, is_primary);
   aurora_unlock(dev);

   if (!r->prsrc) {
      aurora_log("OpenResource: import FAILED alloc=%u %ux%u",
                 (unsigned)oi->hAllocation, (unsigned)width, (unsigned)height);
      aurora_set_error(dev, E_OUTOFMEMORY);
      return;
   }

   r->dim = D3D10DDIRESOURCE_TEXTURE2D;
   r->dxgi_format = dxgi_fmt;
   r->pipe_format = pf;
   r->width = width;
   r->height = height;
   r->depth = 1u;
   r->mip_levels = 1u;
   r->array_size = 1u;
   r->sample_count = 1u;
   r->bind_flags = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_SHADER_RESOURCE;
   r->misc_flags = D3D10_DDI_RESOURCE_MISC_SHARED;
   r->hRTResource = hRTResource;
   r->hKMResource = pArgs->hKMResource.handle;
   r->hKMAllocation = oi->hAllocation;
   r->alloc_size = priv->size;
   r->row_pitch = row_pitch;
   r->is_primary = is_primary;
   r->is_direct_scanout = is_direct;
   r->is_shared = true;
   r->owns_km_resource = false;
   r->num_subresources = 1u;

   aurora_log("OpenResource: OK alloc=%u %ux%u fmt=%u primary=%u direct=%u",
              (unsigned)oi->hAllocation, (unsigned)width, (unsigned)height,
              (unsigned)dxgi_fmt, is_primary ? 1u : 0u, is_direct ? 1u : 0u);
}

void APIENTRY
aurora_DestroyResource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *r = (struct aurora_resource *)hResource.pDrvPrivate;
   if (!dev || !r)
      return;

   aurora_lock(dev);
   if (r->transfers && dev->pctx) {
      for (UINT i = 0; i < r->num_subresources; ++i) {
         if (r->transfers[i]) {
            if (r->dim == D3D10DDIRESOURCE_BUFFER)
               dev->pctx->buffer_unmap(dev->pctx, r->transfers[i]);
            else
               dev->pctx->texture_unmap(dev->pctx, r->transfers[i]);
            r->transfers[i] = NULL;
         }
      }
   }
   free(r->transfers);
   r->transfers = NULL;

   /* Unbind if currently referenced as index buffer or vertex buffer or CB. */
   if (dev->ib_res == r)
      dev->ib_res = NULL;

   /* Drop Gallium pipe_resource FIRST (unmaps GPU VA + evicts residency),
    * THEN deallocate the runtime allocation if we own it. */
   pipe_resource_reference(&r->prsrc, NULL);
   aurora_unlock(dev);

   if (r->owns_km_resource && dev->kt_callbacks.pfnDeallocateCb) {
      D3DDDICB_DEALLOCATE dealloc;
      memset(&dealloc, 0, sizeof(dealloc));
      if (r->hRTResource.handle) {
         dealloc.hResource = r->hRTResource.handle;
      } else if (r->hKMAllocation != 0u) {
         dealloc.NumAllocations = 1u;
         dealloc.HandleList = &r->hKMAllocation;
      }
      dev->kt_callbacks.pfnDeallocateCb(dev->hRTDevice.handle, &dealloc);
      r->owns_km_resource = false;
      r->hKMAllocation = 0u;
      r->hKMResource = 0u;
   }
}

void APIENTRY
aurora_ResourceMap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                   UINT Subresource, D3D10_DDI_MAP DDIMap, UINT Flags,
                   D3D10DDI_MAPPED_SUBRESOURCE *pMapped)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *r = (struct aurora_resource *)hResource.pDrvPrivate;
   if (!dev || !dev->pctx || !r || !r->prsrc || !pMapped) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }
   memset(pMapped, 0, sizeof(*pMapped));
   if (Subresource >= MAX2(r->num_subresources, 1u)) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }

   unsigned usage = 0u;
   switch (DDIMap) {
   case D3D10_DDI_MAP_READ:
      usage = PIPE_MAP_READ;
      break;
   case D3D10_DDI_MAP_WRITE:
      usage = PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_READWRITE:
      usage = PIPE_MAP_READ | PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_WRITE_DISCARD:
      usage = PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE;
      break;
   case D3D10_DDI_MAP_WRITE_NOOVERWRITE:
      usage = PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED;
      break;
   default:
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }
   if (Flags & D3D10_DDI_MAP_FLAG_DONOTWAIT)
      usage |= PIPE_MAP_DONTBLOCK;

   aurora_lock(dev);
   if (!r->transfers) {
      r->transfers = (struct pipe_transfer **)calloc(
         MAX2(r->num_subresources, 1u), sizeof(struct pipe_transfer *));
      if (!r->transfers) {
         aurora_unlock(dev);
         aurora_set_error(dev, E_OUTOFMEMORY);
         return;
      }
   }

   void *ptr = NULL;
   struct pipe_transfer *xfer = NULL;
   if (r->dim == D3D10DDIRESOURCE_BUFFER) {
      struct pipe_box box = { .x = 0, .y = 0, .z = 0,
                              .width = (int)r->width, .height = 1, .depth = 1 };
      ptr = dev->pctx->buffer_map(dev->pctx, r->prsrc, 0, usage, &box, &xfer);
   } else {
      UINT mips = MAX2(r->mip_levels, 1u);
      UINT level = Subresource % mips;
      UINT slice = Subresource / mips;
      struct pipe_box box = {
         .x = 0, .y = 0,
         .z = (r->dim == D3D10DDIRESOURCE_TEXTURE3D) ? 0 : (int)slice,
         .width = (int)u_minify(r->width, level),
         .height = (int)u_minify(r->height, level),
         .depth = (r->dim == D3D10DDIRESOURCE_TEXTURE3D)
                     ? (int)u_minify(r->depth, level) : 1,
      };
      ptr = dev->pctx->texture_map(dev->pctx, r->prsrc, level, usage, &box, &xfer);
   }

   if (!ptr || !xfer) {
      aurora_unlock(dev);
      aurora_set_error(dev, (Flags & D3D10_DDI_MAP_FLAG_DONOTWAIT)
                               ? DXGI_DDI_ERR_WASSTILLDRAWING
                               : E_FAIL);
      return;
   }

   r->transfers[Subresource] = xfer;
   pMapped->pData = ptr;
   pMapped->RowPitch = xfer->stride;
   pMapped->DepthPitch = xfer->layer_stride;
   aurora_unlock(dev);
}

void APIENTRY
aurora_ResourceUnmap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                     UINT Subresource)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *r = (struct aurora_resource *)hResource.pDrvPrivate;
   if (!dev || !dev->pctx || !r || !r->transfers ||
       Subresource >= MAX2(r->num_subresources, 1u))
      return;

   aurora_lock(dev);
   struct pipe_transfer *xfer = r->transfers[Subresource];
   if (xfer) {
      r->transfers[Subresource] = NULL;
      if (r->dim == D3D10DDIRESOURCE_BUFFER)
         dev->pctx->buffer_unmap(dev->pctx, xfer);
      else
         dev->pctx->texture_unmap(dev->pctx, xfer);
   }
   aurora_unlock(dev);
}

BOOL APIENTRY
aurora_ResourceIsStagingBusy(D3D10DDI_HDEVICE hDevice,
                             D3D10DDI_HRESOURCE hResource)
{
   (void)hDevice; (void)hResource;
   return FALSE;
}

static void
update_subresource_common(struct aurora_device *dev, struct aurora_resource *r,
                          UINT Subresource, const D3D10_DDI_BOX *pDstBox,
                          const VOID *pSysMemUP, UINT RowPitch, UINT DepthPitch,
                          UINT CopyFlags)
{
   if (!dev || !dev->pctx || !r || !r->prsrc || !pSysMemUP)
      return;

   unsigned map_flags = PIPE_MAP_WRITE;
   if (CopyFlags & D3D11_1DDI_COPY_DISCARD)
      map_flags |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
   else if (CopyFlags & D3D11_1DDI_COPY_NO_OVERWRITE)
      map_flags |= PIPE_MAP_UNSYNCHRONIZED;

   aurora_lock(dev);
   if (r->dim == D3D10DDIRESOURCE_BUFFER) {
      UINT offset = pDstBox ? pDstBox->left : 0u;
      UINT size = pDstBox ? (pDstBox->right - pDstBox->left) : r->width;
      if (size > 0u) {
         dev->pctx->buffer_subdata(dev->pctx, r->prsrc, map_flags,
                                   offset, size, pSysMemUP);
      }
   } else {
      UINT mips = MAX2(r->mip_levels, 1u);
      UINT level = Subresource % mips;
      UINT slice = Subresource / mips;
      struct pipe_box box;
      if (pDstBox) {
         box.x = (int)pDstBox->left;
         box.y = (int)pDstBox->top;
         box.z = (r->dim == D3D10DDIRESOURCE_TEXTURE3D)
                    ? (int)pDstBox->front : (int)slice;
         box.width = (int)(pDstBox->right - pDstBox->left);
         box.height = (int)(pDstBox->bottom - pDstBox->top);
         box.depth = (r->dim == D3D10DDIRESOURCE_TEXTURE3D)
                        ? (int)(pDstBox->back - pDstBox->front) : 1;
      } else {
         box.x = 0; box.y = 0;
         box.z = (r->dim == D3D10DDIRESOURCE_TEXTURE3D) ? 0 : (int)slice;
         box.width = (int)u_minify(r->width, level);
         box.height = (int)u_minify(r->height, level);
         box.depth = (r->dim == D3D10DDIRESOURCE_TEXTURE3D)
                        ? (int)u_minify(r->depth, level) : 1;
      }
      if (box.width > 0 && box.height > 0 && box.depth > 0) {
         dev->pctx->texture_subdata(dev->pctx, r->prsrc, level, map_flags,
                                    &box, pSysMemUP, RowPitch, DepthPitch);
      }
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_ResourceUpdateSubresourceUP(D3D10DDI_HDEVICE hDevice,
                                   D3D10DDI_HRESOURCE hDstResource,
                                   UINT DstSubresource,
                                   const D3D10_DDI_BOX *pDstBox,
                                   const VOID *pSysMemUP,
                                   UINT RowPitch, UINT DepthPitch)
{
   update_subresource_common((struct aurora_device *)hDevice.pDrvPrivate,
                             (struct aurora_resource *)hDstResource.pDrvPrivate,
                             DstSubresource, pDstBox, pSysMemUP,
                             RowPitch, DepthPitch, 0u);
}

void APIENTRY
aurora_ResourceUpdateSubresourceUP_11_1(D3D10DDI_HDEVICE hDevice,
                                        D3D10DDI_HRESOURCE hDstResource,
                                        UINT DstSubresource,
                                        const D3D10_DDI_BOX *pDstBox,
                                        const VOID *pSysMemUP,
                                        UINT RowPitch, UINT DepthPitch,
                                        UINT CopyFlags)
{
   update_subresource_common((struct aurora_device *)hDevice.pDrvPrivate,
                             (struct aurora_resource *)hDstResource.pDrvPrivate,
                             DstSubresource, pDstBox, pSysMemUP,
                             RowPitch, DepthPitch, CopyFlags);
}

static bool
should_gpu_blit_copy(const struct aurora_resource *dst,
                     const struct aurora_resource *src)
{
   if (!dst || !src || !dst->prsrc || !src->prsrc)
      return false;
   if (dst->dim != D3D10DDIRESOURCE_TEXTURE2D ||
       src->dim != D3D10DDIRESOURCE_TEXTURE2D)
      return false;
   if (!(src->is_shared || dst->is_shared ||
         src->is_direct_scanout || dst->is_direct_scanout))
      return false;
   if (util_format_is_depth_or_stencil(dst->prsrc->format) ||
       util_format_is_depth_or_stencil(src->prsrc->format) ||
       util_format_is_compressed(dst->prsrc->format) ||
       util_format_is_compressed(src->prsrc->format))
      return false;
   return true;
}

static void
gpu_blit_2d_locked(struct aurora_device *dev, struct pipe_resource *dst,
                   unsigned dst_lvl, unsigned dstx, unsigned dsty, unsigned dstz,
                   struct pipe_resource *src, unsigned src_lvl,
                   const struct pipe_box *box)
{
   struct pipe_blit_info info;
   memset(&info, 0, sizeof(info));
   info.dst.resource = dst;
   info.dst.level = dst_lvl;
   info.dst.format = dst->format;
   info.dst.box.x = (int)dstx;
   info.dst.box.y = (int)dsty;
   info.dst.box.z = (int)dstz;
   info.dst.box.width = box->width;
   info.dst.box.height = box->height;
   info.dst.box.depth = box->depth;

   info.src.resource = src;
   info.src.level = src_lvl;
   info.src.format = src->format;
   info.src.box = *box;

   info.mask = PIPE_MASK_RGBA;
   info.filter = PIPE_TEX_FILTER_NEAREST;
   dev->pctx->blit(dev->pctx, &info);
}

void APIENTRY
aurora_ResourceCopy(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                    D3D10DDI_HRESOURCE hSrc)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *dst = (struct aurora_resource *)hDst.pDrvPrivate;
   struct aurora_resource *src = (struct aurora_resource *)hSrc.pDrvPrivate;
   if (!dev || !dev->pctx || !dst || !src || !dst->prsrc || !src->prsrc)
      return;

   aurora_lock(dev);
   if (dst->dim == D3D10DDIRESOURCE_BUFFER) {
      struct pipe_box box = { .x = 0, .y = 0, .z = 0,
                              .width = (int)MIN2(dst->width, src->width),
                              .height = 1, .depth = 1 };
      dev->pctx->resource_copy_region(dev->pctx, dst->prsrc, 0, 0, 0, 0,
                                      src->prsrc, 0, &box);
   } else {
      bool use_blit = should_gpu_blit_copy(dst, src);
      UINT mips = MIN2(MAX2(dst->mip_levels, 1u), MAX2(src->mip_levels, 1u));
      UINT layers = (dst->dim == D3D10DDIRESOURCE_TEXTURE3D)
                       ? 1u : MIN2(MAX2(dst->array_size, 1u), MAX2(src->array_size, 1u));
      for (UINT m = 0; m < mips; ++m) {
         struct pipe_box box = {
            .x = 0, .y = 0, .z = 0,
            .width = (int)u_minify(src->width, m),
            .height = (int)u_minify(src->height, m),
            .depth = (dst->dim == D3D10DDIRESOURCE_TEXTURE3D)
                        ? (int)u_minify(src->depth, m) : (int)layers,
         };
         if (use_blit)
            gpu_blit_2d_locked(dev, dst->prsrc, m, 0, 0, 0, src->prsrc, m, &box);
         else
            dev->pctx->resource_copy_region(dev->pctx, dst->prsrc, m, 0, 0, 0,
                                            src->prsrc, m, &box);
      }
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_ResourceCopyRegion(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                          UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ,
                          D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                          const D3D10_DDI_BOX *pSrcBox)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *dst = (struct aurora_resource *)hDst.pDrvPrivate;
   struct aurora_resource *src = (struct aurora_resource *)hSrc.pDrvPrivate;
   if (!dev || !dev->pctx || !dst || !src || !dst->prsrc || !src->prsrc)
      return;

   UINT dst_mips = MAX2(dst->mip_levels, 1u);
   UINT src_mips = MAX2(src->mip_levels, 1u);
   UINT dst_lvl = (dst->dim == D3D10DDIRESOURCE_BUFFER) ? 0u : (DstSubresource % dst_mips);
   UINT dst_lyr = (dst->dim == D3D10DDIRESOURCE_BUFFER || dst->dim == D3D10DDIRESOURCE_TEXTURE3D)
                     ? DstZ : (DstSubresource / dst_mips);
   UINT src_lvl = (src->dim == D3D10DDIRESOURCE_BUFFER) ? 0u : (SrcSubresource % src_mips);
   UINT src_lyr = (src->dim == D3D10DDIRESOURCE_BUFFER || src->dim == D3D10DDIRESOURCE_TEXTURE3D)
                     ? 0u : (SrcSubresource / src_mips);

   struct pipe_box box;
   if (pSrcBox) {
      box.x = (int)pSrcBox->left;
      box.y = (int)pSrcBox->top;
      box.z = (src->dim == D3D10DDIRESOURCE_TEXTURE3D)
                 ? (int)pSrcBox->front : (int)src_lyr;
      box.width = (int)(pSrcBox->right - pSrcBox->left);
      box.height = (int)(pSrcBox->bottom - pSrcBox->top);
      box.depth = (src->dim == D3D10DDIRESOURCE_TEXTURE3D)
                     ? (int)(pSrcBox->back - pSrcBox->front) : 1;
   } else {
      box.x = 0; box.y = 0; box.z = (int)src_lyr;
      box.width = (int)u_minify(src->width, src_lvl);
      box.height = (int)u_minify(src->height, src_lvl);
      box.depth = (src->dim == D3D10DDIRESOURCE_TEXTURE3D)
                     ? (int)u_minify(src->depth, src_lvl) : 1;
   }
   if (box.width <= 0 || box.height <= 0 || box.depth <= 0)
      return;

   aurora_lock(dev);
   if (should_gpu_blit_copy(dst, src))
      gpu_blit_2d_locked(dev, dst->prsrc, dst_lvl, DstX, DstY, dst_lyr,
                         src->prsrc, src_lvl, &box);
   else
      dev->pctx->resource_copy_region(dev->pctx, dst->prsrc, dst_lvl,
                                      DstX, DstY, dst_lyr,
                                      src->prsrc, src_lvl, &box);
   aurora_unlock(dev);
}

void APIENTRY
aurora_ResourceCopyRegion_11_1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                               UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ,
                               D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                               const D3D10_DDI_BOX *pSrcBox, UINT CopyFlags)
{
   (void)CopyFlags;
   aurora_ResourceCopyRegion(hDevice, hDst, DstSubresource, DstX, DstY, DstZ,
                             hSrc, SrcSubresource, pSrcBox);
}

void APIENTRY
aurora_ResourceResolveSubresource(D3D10DDI_HDEVICE hDevice,
                                  D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                                  D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                                  DXGI_FORMAT ResolveFormat)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *dst = (struct aurora_resource *)hDst.pDrvPrivate;
   struct aurora_resource *src = (struct aurora_resource *)hSrc.pDrvPrivate;
   if (!dev || !dev->pctx || !dst || !src || !dst->prsrc || !src->prsrc)
      return;

   enum pipe_format pf = aurora_dxgi_to_pipe_format(ResolveFormat);
   if (pf == PIPE_FORMAT_NONE)
      pf = dst->prsrc->format;

   UINT dst_mips = MAX2(dst->mip_levels, 1u);
   UINT src_mips = MAX2(src->mip_levels, 1u);
   UINT dst_lvl = DstSubresource % dst_mips;
   UINT dst_lyr = DstSubresource / dst_mips;
   UINT src_lvl = SrcSubresource % src_mips;
   UINT src_lyr = SrcSubresource / src_mips;

   struct pipe_blit_info info;
   memset(&info, 0, sizeof(info));
   info.dst.resource = dst->prsrc;
   info.dst.level = dst_lvl;
   info.dst.format = pf;
   info.dst.box.z = (int)dst_lyr;
   info.dst.box.width = (int)u_minify(dst->width, dst_lvl);
   info.dst.box.height = (int)u_minify(dst->height, dst_lvl);
   info.dst.box.depth = 1;

   info.src.resource = src->prsrc;
   info.src.level = src_lvl;
   info.src.format = pf;
   info.src.box.z = (int)src_lyr;
   info.src.box.width = (int)u_minify(src->width, src_lvl);
   info.src.box.height = (int)u_minify(src->height, src_lvl);
   info.src.box.depth = 1;

   info.mask = PIPE_MASK_RGBA;
   info.filter = PIPE_TEX_FILTER_NEAREST;

   aurora_lock(dev);
   dev->pctx->blit(dev->pctx, &info);
   aurora_unlock(dev);
}

void APIENTRY
aurora_ResourceConvert(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                       D3D10DDI_HRESOURCE hSrc)
{
   aurora_ResourceResolveSubresource(hDevice, hDst, 0, hSrc, 0, DXGI_FORMAT_UNKNOWN);
}

void APIENTRY
aurora_ResourceConvertRegion(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                             UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ,
                             D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                             const D3D10_DDI_BOX *pSrcBox)
{
   aurora_ResourceCopyRegion(hDevice, hDst, DstSubresource, DstX, DstY, DstZ,
                             hSrc, SrcSubresource, pSrcBox);
}

void APIENTRY aurora_ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE r)
{ (void)d; (void)r; }
void APIENTRY aurora_SRVReadAfterWriteHazard(D3D10DDI_HDEVICE d, D3D10DDI_HSHADERRESOURCEVIEW v, D3D10DDI_HRESOURCE r)
{ (void)d; (void)v; (void)r; }
void APIENTRY aurora_SetResourceMinLOD(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE r, FLOAT lod)
{ (void)d; (void)r; (void)lod; }
void APIENTRY aurora_GetResourceLayout(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE r, UINT count,
                                       D3DKMT_HANDLE *alloc, D3DWDDM2_0DDI_TEXTURE_LAYOUT *layout,
                                       UINT *trans, D3DWDDM2_0DDI_SUBRESOURCE_LAYOUT *subs)
{
   (void)d; (void)r;
   if (alloc) *alloc = 0;
   if (layout) *layout = D3DWDDM2_0DDI_TL_UNDEFINED;
   if (trans) *trans = 0;
   if (subs && count) memset(subs, 0, count * sizeof(*subs));
}
void APIENTRY aurora_GetMipPacking(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE r, UINT *p, UINT *t)
{ (void)d; (void)r; if (p) *p = 0; if (t) *t = 0; }
void APIENTRY aurora_UpdateTileMappings(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE r, UINT a,
                                        const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *b,
                                        const D3DWDDM1_3DDI_TILE_REGION_SIZE *c, D3D10DDI_HRESOURCE e,
                                        UINT f, const UINT *g, const UINT *h, const UINT *i, UINT j)
{ (void)d;(void)r;(void)a;(void)b;(void)c;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j; }
void APIENTRY aurora_CopyTileMappings(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE a,
                                      const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *b,
                                      D3D10DDI_HRESOURCE c,
                                      const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *e,
                                      const D3DWDDM1_3DDI_TILE_REGION_SIZE *f, UINT g)
{ (void)d;(void)a;(void)b;(void)c;(void)e;(void)f;(void)g; }
void APIENTRY aurora_CopyTiles(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE a,
                               const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *b,
                               const D3DWDDM1_3DDI_TILE_REGION_SIZE *c,
                               D3D10DDI_HRESOURCE e, UINT64 f, UINT g)
{ (void)d;(void)a;(void)b;(void)c;(void)e;(void)f;(void)g; }
void APIENTRY aurora_UpdateTiles(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE a,
                                 const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *b,
                                 const D3DWDDM1_3DDI_TILE_REGION_SIZE *c, const VOID *e, UINT f)
{ (void)d;(void)a;(void)b;(void)c;(void)e;(void)f; }
void APIENTRY aurora_TiledResourceBarrier(D3D10DDI_HDEVICE d, D3D11DDI_HANDLETYPE a, VOID *b,
                                          D3D11DDI_HANDLETYPE c, VOID *e)
{ (void)d;(void)a;(void)b;(void)c;(void)e; }
void APIENTRY aurora_ResizeTilePool(D3D10DDI_HDEVICE d, D3D10DDI_HRESOURCE r, UINT64 s)
{ (void)d; (void)r; (void)s; }
