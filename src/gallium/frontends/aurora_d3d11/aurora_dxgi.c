/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Native DXGI 1.0..1.5 DDI callbacks (Present, Present1, MPO, SetDisplayMode,
 * RotateResourceIdentities, Blt/Blt1, ResolveSharedResource, Offer/Reclaim).
 */
#include "aurora_d3d11.h"

#include <string.h>

/* =========================================================================
 * Direct-Scanout & Standard Present Submission
 * ========================================================================= */
static HRESULT
submit_present(struct aurora_device *dev, struct aurora_resource *src,
               struct aurora_resource *dst, void *dxgi_ctx, const char *tag)
{
   static volatile LONG log_count = 0;
   if (!dev || !src)
      return E_INVALIDARG;
   if (!aurora_gate_present_real())
      return DXGI_ERROR_UNSUPPORTED;

   InterlockedIncrement(&dev->present_in_flight);
   if (!dev->hKMContext)
      aurora_ensure_present_context(dev);
   if (!dev->hKMContext || !dev->pfnPresentCb || src->hKMAllocation == 0u) {
      InterlockedDecrement(&dev->present_in_flight);
      aurora_log("present(%s): refused ctx=%p cb=%p alloc=%u", tag,
                 dev->hKMContext, (void *)dev->pfnPresentCb,
                 (unsigned)src->hKMAllocation);
      return DXGI_ERROR_UNSUPPORTED;
   }

   /* Flush pending GPU commands targeting the primary.
    * For direct-scanout primaries, flush() synchronously calls D3DKMTSubmitCommand
    * (with primary_writes marking the BO gpu_dirty in KMD) WITHOUT blocking the
    * compositor thread on GPU completion; KMD's hardware IRQ wakes the display
    * worker on completion. For non-direct primaries, wait for completion first. */
   aurora_lock(dev);
   if (dev->pctx) {
      if (src->is_direct_scanout)
         dev->pctx->flush(dev->pctx, NULL, 0);
      else
         aurora_flush_and_wait(dev);
   }
   aurora_unlock(dev);

   DXGIDDICB_PRESENT cb;
   memset(&cb, 0, sizeof(cb));
   cb.hSrcAllocation = src->hKMAllocation;
   cb.hDstAllocation = dst ? dst->hKMAllocation : 0u;
   cb.pDXGIContext = dxgi_ctx;
   cb.hContext = dev->hKMContext;

   if (dev->locks_init)
      EnterCriticalSection(&dev->km_ctx_lock);
   HRESULT hr = dev->pfnPresentCb(dev->hRTDevice.handle, &cb);
   if (dev->locks_init)
      LeaveCriticalSection(&dev->km_ctx_lock);

   InterlockedDecrement(&dev->present_in_flight);
   LONG n = InterlockedIncrement(&log_count);
   if (n <= 8 || FAILED(hr)) {
      aurora_log("present(%s) #%ld alloc=%u %ux%u direct=%u hr=0x%08lx", tag,
                 (long)n, (unsigned)src->hKMAllocation, (unsigned)src->width,
                 (unsigned)src->height, src->is_direct_scanout ? 1u : 0u,
                 (unsigned long)hr);
   }
   return hr;
}

static HRESULT APIENTRY
aurora_DxgiPresent(DXGI_DDI_ARG_PRESENT *args)
{
   if (!args)
      return E_INVALIDARG;
   return submit_present((struct aurora_device *)(uintptr_t)args->hDevice,
                         (struct aurora_resource *)(uintptr_t)args->hSurfaceToPresent,
                         (struct aurora_resource *)(uintptr_t)args->hDstResource,
                         args->pDXGIContext, "Present");
}

static HRESULT APIENTRY
aurora_DxgiPresent1(DXGI_DDI_ARG_PRESENT1 *args)
{
   if (!args || !args->phSurfacesToPresent || args->SurfacesToPresent == 0u)
      return E_INVALIDARG;
   return submit_present(
      (struct aurora_device *)(uintptr_t)args->hDevice,
      (struct aurora_resource *)(uintptr_t)args->phSurfacesToPresent[0].hSurface,
      (struct aurora_resource *)(uintptr_t)args->hDstResource,
      args->pDXGIContext, "Present1");
}

static HRESULT APIENTRY
aurora_DxgiGetGammaCaps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *args)
{
   if (!args || !args->pGammaCapabilities)
      return E_INVALIDARG;
   memset(args->pGammaCapabilities, 0, sizeof(*args->pGammaCapabilities));
   args->pGammaCapabilities->MinConvertedValue = 0.0f;
   args->pGammaCapabilities->MaxConvertedValue = 1.0f;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiSetDisplayMode(DXGI_DDI_ARG_SETDISPLAYMODE *args)
{
   if (!args)
      return E_INVALIDARG;
   struct aurora_device *dev = (struct aurora_device *)(uintptr_t)args->hDevice;
   struct aurora_resource *res = (struct aurora_resource *)(uintptr_t)args->hResource;
   if (!dev || !res || res->hKMAllocation == 0u ||
       !dev->kt_callbacks.pfnSetDisplayModeCb)
      return DXGI_ERROR_UNSUPPORTED;

   aurora_lock(dev);
   aurora_flush_and_wait(dev);
   aurora_unlock(dev);

   D3DDDICB_SETDISPLAYMODE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hPrimaryAllocation = res->hKMAllocation;
   HRESULT hr = dev->kt_callbacks.pfnSetDisplayModeCb(dev->hRTDevice.handle, &cb);
   aurora_log("SetDisplayMode alloc=%u %ux%u hr=0x%08lx",
              (unsigned)res->hKMAllocation, (unsigned)res->width,
              (unsigned)res->height, (unsigned long)hr);
   return hr;
}

static HRESULT APIENTRY
aurora_DxgiSetResourcePriority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *args)
{
   (void)args;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiQueryResourceResidency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *args)
{
   if (!args || !args->pStatus)
      return E_INVALIDARG;
   for (SIZE_T i = 0; i < args->Resources; ++i)
      args->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiRotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   if (!args)
      return E_INVALIDARG;
   struct aurora_device *dev = (struct aurora_device *)(uintptr_t)args->hDevice;
   if (!dev || args->Resources < 2u || !args->pResources)
      return S_OK;

   aurora_lock(dev);
   for (UINT i = 0; i < args->Resources; ++i) {
      struct aurora_resource *r =
         (struct aurora_resource *)(uintptr_t)args->pResources[i];
      if (!r || !r->prsrc) {
         aurora_unlock(dev);
         return E_INVALIDARG;
      }
   }

   struct aurora_resource *first =
      (struct aurora_resource *)(uintptr_t)args->pResources[0];
   struct pipe_resource *saved_prsrc = first->prsrc;
   D3D10DDI_HRTRESOURCE saved_rt = first->hRTResource;
   D3DKMT_HANDLE saved_km_res = first->hKMResource;
   D3DKMT_HANDLE saved_km_alloc = first->hKMAllocation;
   uint64_t saved_alloc_size = first->alloc_size;
   uint32_t saved_row_pitch = first->row_pitch;
   bool saved_primary = first->is_primary;
   bool saved_direct = first->is_direct_scanout;
   bool saved_shared = first->is_shared;
   bool saved_owns_km = first->owns_km_resource;

   for (UINT i = 0; i + 1u < args->Resources; ++i) {
      struct aurora_resource *a =
         (struct aurora_resource *)(uintptr_t)args->pResources[i];
      struct aurora_resource *b =
         (struct aurora_resource *)(uintptr_t)args->pResources[i + 1u];
      a->prsrc = b->prsrc;
      a->hRTResource = b->hRTResource;
      a->hKMResource = b->hKMResource;
      a->hKMAllocation = b->hKMAllocation;
      a->alloc_size = b->alloc_size;
      a->row_pitch = b->row_pitch;
      a->is_primary = b->is_primary;
      a->is_direct_scanout = b->is_direct_scanout;
      a->is_shared = b->is_shared;
      a->owns_km_resource = b->owns_km_resource;
      a->gen++;
   }

   struct aurora_resource *last =
      (struct aurora_resource *)(uintptr_t)args->pResources[args->Resources - 1u];
   last->prsrc = saved_prsrc;
   last->hRTResource = saved_rt;
   last->hKMResource = saved_km_res;
   last->hKMAllocation = saved_km_alloc;
   last->alloc_size = saved_alloc_size;
   last->row_pitch = saved_row_pitch;
   last->is_primary = saved_primary;
   last->is_direct_scanout = saved_direct;
   last->is_shared = saved_shared;
   last->owns_km_resource = saved_owns_km;
   last->gen++;

   /* Mark framebuffer + SRVs dirty so views refresh lazily against new prsrc. */
   dev->fb_dirty = true;
   for (unsigned st = 0; st < AURORA_STAGE_COUNT; ++st)
      dev->srv_dirty[st] = true;

   aurora_unlock(dev);
   return S_OK;
}

static HRESULT
dxgi_blt_common(DXGI_DDI_HDEVICE hDevice, DXGI_DDI_HRESOURCE hDst,
                UINT dstSub, UINT dstLeft, UINT dstTop, UINT dstRight,
                UINT dstBottom, DXGI_DDI_HRESOURCE hSrc, UINT srcSub,
                UINT srcLeft, UINT srcTop, UINT srcRight, UINT srcBottom,
                DXGI_DDI_ARG_BLT_FLAGS flags, DXGI_DDI_MODE_ROTATION rotate)
{
   struct aurora_device *dev = (struct aurora_device *)(uintptr_t)hDevice;
   struct aurora_resource *dst = (struct aurora_resource *)(uintptr_t)hDst;
   struct aurora_resource *src = (struct aurora_resource *)(uintptr_t)hSrc;
   if (!dev || !dev->pctx || !dst || !src || !dst->prsrc || !src->prsrc)
      return E_INVALIDARG;
   if (rotate != DXGI_DDI_MODE_ROTATION_IDENTITY)
      return DXGI_ERROR_UNSUPPORTED;
   if (srcRight <= srcLeft || srcBottom <= srcTop ||
       dstRight <= dstLeft || dstBottom <= dstTop)
      return E_INVALIDARG;

   UINT src_level = srcSub % MAX2(src->mip_levels, 1u);
   UINT src_layer = srcSub / MAX2(src->mip_levels, 1u);
   UINT dst_level = dstSub % MAX2(dst->mip_levels, 1u);
   UINT dst_layer = dstSub / MAX2(dst->mip_levels, 1u);

   aurora_lock(dev);
   bool same_size = (dstRight - dstLeft == srcRight - srcLeft) &&
                    (dstBottom - dstTop == srcBottom - srcTop);
   if (!flags.Resolve && !flags.Convert && !flags.Stretch && same_size &&
       dst->prsrc->format == src->prsrc->format &&
       dst->prsrc->nr_samples == src->prsrc->nr_samples) {
      struct pipe_box box = {
         .x = (int)srcLeft, .y = (int)srcTop, .z = (int)src_layer,
         .width = (int)(srcRight - srcLeft),
         .height = (int)(srcBottom - srcTop),
         .depth = 1,
      };
      dev->pctx->resource_copy_region(dev->pctx, dst->prsrc, dst_level,
                                      dstLeft, dstTop, dst_layer,
                                      src->prsrc, src_level, &box);
   } else {
      struct pipe_blit_info info;
      memset(&info, 0, sizeof(info));
      info.dst.resource = dst->prsrc;
      info.dst.level = dst_level;
      info.dst.format = dst->prsrc->format;
      info.dst.box.x = (int)dstLeft;
      info.dst.box.y = (int)dstTop;
      info.dst.box.z = (int)dst_layer;
      info.dst.box.width = (int)(dstRight - dstLeft);
      info.dst.box.height = (int)(dstBottom - dstTop);
      info.dst.box.depth = 1;

      info.src.resource = src->prsrc;
      info.src.level = src_level;
      info.src.format = src->prsrc->format;
      info.src.box.x = (int)srcLeft;
      info.src.box.y = (int)srcTop;
      info.src.box.z = (int)src_layer;
      info.src.box.width = (int)(srcRight - srcLeft);
      info.src.box.height = (int)(srcBottom - srcTop);
      info.src.box.depth = 1;

      info.mask = PIPE_MASK_RGBA;
      info.filter = flags.Stretch ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
      dev->pctx->blit(dev->pctx, &info);
   }
   if (flags.Present)
      dev->pctx->flush(dev->pctx, NULL, 0);
   aurora_unlock(dev);
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiBlt(DXGI_DDI_ARG_BLT *args)
{
   if (!args)
      return E_INVALIDARG;
   struct aurora_resource *src =
      (struct aurora_resource *)(uintptr_t)args->hSrcResource;
   if (!src)
      return E_INVALIDARG;
   return dxgi_blt_common(args->hDevice, args->hDstResource, args->DstSubresource,
                          args->DstLeft, args->DstTop, args->DstRight, args->DstBottom,
                          args->hSrcResource, args->SrcSubresource,
                          0u, 0u, src->width, src->height,
                          args->Flags, args->Rotate);
}

static HRESULT APIENTRY
aurora_DxgiBlt1(DXGI_DDI_ARG_BLT1 *args)
{
   if (!args)
      return E_INVALIDARG;
   return dxgi_blt_common(args->hDevice, args->hDstResource, args->DstSubresource,
                          args->DstLeft, args->DstTop, args->DstRight, args->DstBottom,
                          args->hSrcResource, args->SrcSubresource,
                          args->SrcLeft, args->SrcTop, args->SrcRight, args->SrcBottom,
                          args->Flags, args->Rotate);
}

static HRESULT APIENTRY
aurora_DxgiResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *args)
{
   if (!args)
      return E_INVALIDARG;
   struct aurora_device *dev = (struct aurora_device *)(uintptr_t)args->hDevice;
   if (!dev)
      return E_INVALIDARG;
   aurora_lock(dev);
   aurora_flush_and_wait(dev);
   aurora_unlock(dev);
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiOfferResources(DXGI_DDI_ARG_OFFERRESOURCES *args)
{
   (void)args;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiOfferResources1(DXGI_DDI_ARG_OFFERRESOURCES1 *args)
{
   (void)args;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *args)
{
   if (!args)
      return E_INVALIDARG;
   if (args->pDiscarded) {
      for (UINT i = 0; i < args->Resources; ++i)
         args->pDiscarded[i] = FALSE;
   }
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiReclaimResources1(DXGI_DDI_ARG_RECLAIMRESOURCES1 *args)
{
   if (!args)
      return E_INVALIDARG;
   if (args->pResults) {
      for (UINT i = 0; i < args->Resources; ++i)
         args->pResults[i] = D3DDDI_RECLAIM_RESULT_OK;
   }
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiCheckPresentDurationSupport(
   DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT *args)
{
   if (!args)
      return E_INVALIDARG;
   args->ClosestSmallerDuration = 0u;
   args->ClosestLargerDuration = 0u;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiTrimResidencySet(DXGI_DDI_ARG_TRIMRESIDENCYSET *args)
{
   (void)args;
   return S_OK;
}

/* =========================================================================
 * Multiplane Overlay (MPO) — Single Direct-Scanout RGB Plane
 * ========================================================================= */
static HRESULT APIENTRY
aurora_DxgiGetMultiplaneOverlayCaps(
   DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *args)
{
   if (!args || args->VidPnSourceId != 0u)
      return E_INVALIDARG;
   memset(&args->MultiplaneOverlayCaps, 0, sizeof(args->MultiplaneOverlayCaps));
   args->MultiplaneOverlayCaps.MaxPlanes = 1u;
   args->MultiplaneOverlayCaps.NumCapabilityGroups = 1u;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiGetMultiplaneOverlayGroupCaps(
   DXGI_DDI_ARG_GETMULTIPLANEOVERLAYGROUPCAPS *args)
{
   if (!args || args->VidPnSourceId != 0u || args->GroupIndex != 0u)
      return E_INVALIDARG;
   memset(&args->MultiplaneOverlayGroupCaps, 0,
          sizeof(args->MultiplaneOverlayGroupCaps));
   args->MultiplaneOverlayGroupCaps.NumPlanes = 1u;
   args->MultiplaneOverlayGroupCaps.MaxStretchFactor = 1.0f;
   args->MultiplaneOverlayGroupCaps.MaxShrinkFactor = 1.0f;
   args->MultiplaneOverlayGroupCaps.OverlayCaps =
      DXGI_DDI_MULTIPLANE_OVERLAY_FEATURE_CAPS_RGB;
   return S_OK;
}

static HRESULT APIENTRY
aurora_DxgiCheckMpoColorSpaceSupport(
   DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYCOLORSPACESUPPORT *args)
{
   if (!args || args->VidPnSourceId != 0u)
      return E_INVALIDARG;
   args->Supported =
      (args->Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
       args->Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
       args->Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
      args->ColorSpace == D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
   return S_OK;
}

static bool
mpo_rect_matches(const RECT *r, const struct aurora_resource *res)
{
   return r && res && r->left == 0 && r->top == 0 &&
          r->right == (LONG)res->width && r->bottom == (LONG)res->height;
}

static HRESULT
submit_mpo(struct aurora_device *dev, struct aurora_resource *res,
           UINT subresource, bool enabled, void *dxgi_ctx)
{
   if (!dev)
      return E_INVALIDARG;
   if (!aurora_gate_present_real())
      return DXGI_ERROR_UNSUPPORTED;

   InterlockedIncrement(&dev->present_in_flight);
   if (!dev->hKMContext)
      aurora_ensure_present_context(dev);
   if (!dev->hKMContext || !dev->pfnPresentMpoCb) {
      InterlockedDecrement(&dev->present_in_flight);
      return DXGI_ERROR_UNSUPPORTED;
   }

   if (enabled) {
      if (!res || !res->is_direct_scanout || res->hKMAllocation == 0u ||
          subresource != 0u) {
         InterlockedDecrement(&dev->present_in_flight);
         return E_INVALIDARG;
      }
      aurora_lock(dev);
      if (dev->pctx)
         dev->pctx->flush(dev->pctx, NULL, 0);
      aurora_unlock(dev);
   }

   DXGIDDICB_PRESENT_MULTIPLANE_OVERLAY cb;
   memset(&cb, 0, sizeof(cb));
   cb.pDXGIContext = dxgi_ctx;
   cb.hContext = dev->hKMContext;
   if (enabled) {
      cb.AllocationInfoCount = 1u;
      cb.AllocationInfo[0].PresentAllocation = res->hKMAllocation;
      cb.AllocationInfo[0].SubResourceIndex = subresource;
   }

   if (dev->locks_init)
      EnterCriticalSection(&dev->km_ctx_lock);
   HRESULT hr = dev->pfnPresentMpoCb(dev->hRTDevice.handle, &cb);
   if (dev->locks_init)
      LeaveCriticalSection(&dev->km_ctx_lock);

   InterlockedDecrement(&dev->present_in_flight);
   return hr;
}

static HRESULT APIENTRY
aurora_DxgiPresentMpo(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *args)
{
   if (!args || args->VidPnSourceId != 0u)
      return E_INVALIDARG;
   struct aurora_device *dev = (struct aurora_device *)(uintptr_t)args->hDevice;
   if (args->PresentPlaneCount == 0u)
      return submit_mpo(dev, NULL, 0u, false, args->pDXGIContext);
   if (args->PresentPlaneCount != 1u || !args->pPresentPlanes)
      return E_INVALIDARG;
   DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY *pl = &args->pPresentPlanes[0];
   if (pl->LayerIndex != 0u)
      return E_INVALIDARG;
   if (!pl->Enabled)
      return submit_mpo(dev, NULL, 0u, false, args->pDXGIContext);
   struct aurora_resource *res =
      (struct aurora_resource *)(uintptr_t)pl->hResource;
   if (!res || pl->PlaneAttributes.Flags != 0u ||
       pl->PlaneAttributes.Rotation != DXGI_DDI_MODE_ROTATION_IDENTITY ||
       pl->PlaneAttributes.Blend != DXGI_DDI_MULTIPLANE_OVERLAY_BLEND_OPAQUE ||
       !mpo_rect_matches(&pl->PlaneAttributes.SrcRect, res) ||
       !mpo_rect_matches(&pl->PlaneAttributes.DstRect, res) ||
       !mpo_rect_matches(&pl->PlaneAttributes.ClipRect, res))
      return E_INVALIDARG;
   return submit_mpo(dev, res, pl->SubResourceIndex, true, args->pDXGIContext);
}

static HRESULT APIENTRY
aurora_DxgiPresentMpo1(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY1 *args)
{
   if (!args || args->VidPnSourceId != 0u)
      return E_INVALIDARG;
   struct aurora_device *dev = (struct aurora_device *)(uintptr_t)args->hDevice;
   if (args->PresentPlaneCount == 0u)
      return submit_mpo(dev, NULL, 0u, false, args->pDXGIContext);
   if (args->PresentPlaneCount != 1u || !args->pPresentPlanes)
      return E_INVALIDARG;
   DXGI_DDI_PRESENT_MULTIPLANE_OVERLAY1 *pl = &args->pPresentPlanes[0];
   if (pl->LayerIndex != 0u)
      return E_INVALIDARG;
   if (!pl->Enabled)
      return submit_mpo(dev, NULL, 0u, false, args->pDXGIContext);
   struct aurora_resource *res =
      (struct aurora_resource *)(uintptr_t)pl->hResource;
   if (!res || pl->PlaneAttributes.Flags != 0u ||
       pl->PlaneAttributes.Rotation != DXGI_DDI_MODE_ROTATION_IDENTITY ||
       pl->PlaneAttributes.Blend != DXGI_DDI_MULTIPLANE_OVERLAY_BLEND_OPAQUE ||
       pl->PlaneAttributes.ColorSpace != D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
       !mpo_rect_matches(&pl->PlaneAttributes.SrcRect, res) ||
       !mpo_rect_matches(&pl->PlaneAttributes.DstRect, res) ||
       !mpo_rect_matches(&pl->PlaneAttributes.ClipRect, res))
      return E_INVALIDARG;
   return submit_mpo(dev, res, pl->SubResourceIndex, true, args->pDXGIContext);
}

/* =========================================================================
 * DXGI Table Installation (Base / 1.1 / 1.2 / 1.3 / 1.4 / 1.5)
 * ========================================================================= */
void
aurora_install_dxgi_funcs(struct aurora_device *dev,
                          const D3D10DDIARG_CREATEDEVICE *args)
{
   if (!args || !args->DXGIBaseDDI.pDXGIDDIBaseFunctions)
      return;
   if (dev && args->DXGIBaseDDI.pDXGIBaseCallbacks) {
      dev->pfnPresentCb = args->DXGIBaseDDI.pDXGIBaseCallbacks->pfnPresentCb;
      dev->pfnPresentMpoCb =
         args->DXGIBaseDDI.pDXGIBaseCallbacks->pfnPresentMultiplaneOverlayCb;
   }

   if (IS_DXGI1_5_BASE_FUNCTIONS(args->Interface, args->Version)) {
      DXGI1_5_DDI_BASE_FUNCTIONS *p = args->DXGIBaseDDI.pDXGIDDIBaseFunctions6;
      memset(p, 0, sizeof(*p));
      p->pfnPresent                              = aurora_DxgiPresent;
      p->pfnGetGammaCaps                         = aurora_DxgiGetGammaCaps;
      p->pfnSetDisplayMode                       = aurora_DxgiSetDisplayMode;
      p->pfnSetResourcePriority                  = aurora_DxgiSetResourcePriority;
      p->pfnQueryResourceResidency               = aurora_DxgiQueryResourceResidency;
      p->pfnRotateResourceIdentities             = aurora_DxgiRotateResourceIdentities;
      p->pfnBlt                                  = aurora_DxgiBlt;
      p->pfnResolveSharedResource                = aurora_DxgiResolveSharedResource;
      p->pfnBlt1                                 = aurora_DxgiBlt1;
      p->pfnOfferResources1                      = aurora_DxgiOfferResources1;
      p->pfnReclaimResources                     = aurora_DxgiReclaimResources;
      p->pfnGetMultiplaneOverlayCaps             = aurora_DxgiGetMultiplaneOverlayCaps;
      p->pfnGetMultiplaneOverlayGroupCaps        = aurora_DxgiGetMultiplaneOverlayGroupCaps;
      p->pfnPresentMultiplaneOverlay             = aurora_DxgiPresentMpo;
      p->pfnPresent1                             = aurora_DxgiPresent1;
      p->pfnCheckPresentDurationSupport          = aurora_DxgiCheckPresentDurationSupport;
      p->pfnTrimResidencySet                     = aurora_DxgiTrimResidencySet;
      p->pfnCheckMultiplaneOverlayColorSpaceSupport = aurora_DxgiCheckMpoColorSpaceSupport;
      p->pfnPresentMultiplaneOverlay1            = aurora_DxgiPresentMpo1;
      p->pfnReclaimResources1                    = aurora_DxgiReclaimResources1;
   } else if (IS_DXGI1_4_BASE_FUNCTIONS(args->Interface, args->Version)) {
      DXGI1_4_DDI_BASE_FUNCTIONS *p = args->DXGIBaseDDI.pDXGIDDIBaseFunctions5;
      memset(p, 0, sizeof(*p));
      p->pfnPresent                              = aurora_DxgiPresent;
      p->pfnGetGammaCaps                         = aurora_DxgiGetGammaCaps;
      p->pfnSetDisplayMode                       = aurora_DxgiSetDisplayMode;
      p->pfnSetResourcePriority                  = aurora_DxgiSetResourcePriority;
      p->pfnQueryResourceResidency               = aurora_DxgiQueryResourceResidency;
      p->pfnRotateResourceIdentities             = aurora_DxgiRotateResourceIdentities;
      p->pfnBlt                                  = aurora_DxgiBlt;
      p->pfnResolveSharedResource                = aurora_DxgiResolveSharedResource;
      p->pfnBlt1                                 = aurora_DxgiBlt1;
      p->pfnOfferResources                       = aurora_DxgiOfferResources;
      p->pfnReclaimResources                     = aurora_DxgiReclaimResources;
      p->pfnGetMultiplaneOverlayCaps             = aurora_DxgiGetMultiplaneOverlayCaps;
      p->pfnGetMultiplaneOverlayGroupCaps        = aurora_DxgiGetMultiplaneOverlayGroupCaps;
      p->pfnPresentMultiplaneOverlay             = aurora_DxgiPresentMpo;
      p->pfnPresent1                             = aurora_DxgiPresent1;
      p->pfnCheckPresentDurationSupport          = aurora_DxgiCheckPresentDurationSupport;
      p->pfnTrimResidencySet                     = aurora_DxgiTrimResidencySet;
      p->pfnCheckMultiplaneOverlayColorSpaceSupport = aurora_DxgiCheckMpoColorSpaceSupport;
      p->pfnPresentMultiplaneOverlay1            = aurora_DxgiPresentMpo1;
   } else if (IS_DXGI1_3_BASE_FUNCTIONS(args->Interface, args->Version)) {
      DXGI1_3_DDI_BASE_FUNCTIONS *p = args->DXGIBaseDDI.pDXGIDDIBaseFunctions4;
      memset(p, 0, sizeof(*p));
      p->pfnPresent                  = aurora_DxgiPresent;
      p->pfnGetGammaCaps             = aurora_DxgiGetGammaCaps;
      p->pfnSetDisplayMode           = aurora_DxgiSetDisplayMode;
      p->pfnSetResourcePriority      = aurora_DxgiSetResourcePriority;
      p->pfnQueryResourceResidency   = aurora_DxgiQueryResourceResidency;
      p->pfnRotateResourceIdentities = aurora_DxgiRotateResourceIdentities;
      p->pfnBlt                      = aurora_DxgiBlt;
      p->pfnResolveSharedResource    = aurora_DxgiResolveSharedResource;
      p->pfnBlt1                     = aurora_DxgiBlt1;
      p->pfnOfferResources           = aurora_DxgiOfferResources;
      p->pfnReclaimResources         = aurora_DxgiReclaimResources;
      p->pfnGetMultiplaneOverlayCaps = aurora_DxgiGetMultiplaneOverlayCaps;
      p->pfnGetMultiplaneOverlayGroupCaps = aurora_DxgiGetMultiplaneOverlayGroupCaps;
      p->pfnPresentMultiplaneOverlay = aurora_DxgiPresentMpo;
      p->pfnPresent1                 = aurora_DxgiPresent1;
      p->pfnCheckPresentDurationSupport = aurora_DxgiCheckPresentDurationSupport;
   } else if (IS_DXGI1_2_BASE_FUNCTIONS(args->Interface, args->Version)) {
      DXGI1_2_DDI_BASE_FUNCTIONS *p = args->DXGIBaseDDI.pDXGIDDIBaseFunctions3;
      memset(p, 0, sizeof(*p));
      p->pfnPresent                  = aurora_DxgiPresent;
      p->pfnGetGammaCaps             = aurora_DxgiGetGammaCaps;
      p->pfnSetDisplayMode           = aurora_DxgiSetDisplayMode;
      p->pfnSetResourcePriority      = aurora_DxgiSetResourcePriority;
      p->pfnQueryResourceResidency   = aurora_DxgiQueryResourceResidency;
      p->pfnRotateResourceIdentities = aurora_DxgiRotateResourceIdentities;
      p->pfnBlt                      = aurora_DxgiBlt;
      p->pfnResolveSharedResource    = aurora_DxgiResolveSharedResource;
      p->pfnBlt1                     = aurora_DxgiBlt1;
      p->pfnOfferResources           = aurora_DxgiOfferResources;
      p->pfnReclaimResources         = aurora_DxgiReclaimResources;
   } else if (IS_DXGI1_1_BASE_FUNCTIONS(args->Interface, args->Version)) {
      DXGI1_1_DDI_BASE_FUNCTIONS *p = args->DXGIBaseDDI.pDXGIDDIBaseFunctions2;
      memset(p, 0, sizeof(*p));
      p->pfnPresent                  = aurora_DxgiPresent;
      p->pfnGetGammaCaps             = aurora_DxgiGetGammaCaps;
      p->pfnSetDisplayMode           = aurora_DxgiSetDisplayMode;
      p->pfnSetResourcePriority      = aurora_DxgiSetResourcePriority;
      p->pfnQueryResourceResidency   = aurora_DxgiQueryResourceResidency;
      p->pfnRotateResourceIdentities = aurora_DxgiRotateResourceIdentities;
      p->pfnBlt                      = aurora_DxgiBlt;
      p->pfnResolveSharedResource    = aurora_DxgiResolveSharedResource;
   } else {
      DXGI_DDI_BASE_FUNCTIONS *p = args->DXGIBaseDDI.pDXGIDDIBaseFunctions;
      memset(p, 0, sizeof(*p));
      p->pfnPresent                  = aurora_DxgiPresent;
      p->pfnGetGammaCaps             = aurora_DxgiGetGammaCaps;
      p->pfnSetDisplayMode           = aurora_DxgiSetDisplayMode;
      p->pfnSetResourcePriority      = aurora_DxgiSetResourcePriority;
      p->pfnQueryResourceResidency   = aurora_DxgiQueryResourceResidency;
      p->pfnRotateResourceIdentities = aurora_DxgiRotateResourceIdentities;
      p->pfnBlt                      = aurora_DxgiBlt;
   }
}
