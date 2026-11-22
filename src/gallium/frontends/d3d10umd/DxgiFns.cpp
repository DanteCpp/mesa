/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdio.h>

#include "DxgiFns.h"
#include "Format.h"
#include "State.h"

#include "Debug.h"
#include "Wddm.h"

#include "util/format/u_format.h"
#include "Perf.h"


/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);

   if (device->wddm)
      return WddmPresent(device, pPresentData);

   device->pipe->flush(device->pipe, NULL, 0);
   if (!device->dt_capable)
      return ReadbackPresent(device, pSrcResource, pPresentData->pDXGIContext);
   device->pipe->screen->flush_frontbuffer(device->pipe->screen, device->pipe, 
      pSrcResource->resource, 0, 0, pPresentData->pDXGIContext, 0, NULL);

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   struct Device *device = CastDevice(SetDisplayMode->hDevice);
   if (device->wddm)
      return WddmSetDisplayMode(device, SetDisplayMode);
   LOG_UNSUPPORTED_ENTRYPOINT();

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_RotateResourceIdentities( DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *RotateResourceIdentities )
{
   LOG_ENTRYPOINT();

   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }

   struct pipe_context *pipe = CastPipeDevice(RotateResourceIdentities->hDevice);
   struct pipe_screen *screen = pipe->screen;

   struct pipe_resource *resource0 = CastPipeResource(RotateResourceIdentities->pResources[0]);

   assert(resource0);
   LOG_UNSUPPORTED(resource0->last_level);

   /*
    * XXX: Copying is not very efficient, but it is much simpler than the
    * alternative of recreating all views.
    */

   /* R7: C:\g6w\rotate.noop (checked at every call): no content rotation. The back buffer then keeps the newest
    * frame instead of the one two presents ago; DWM redraws its dirty regions over it (a superset of what changed
    * since), which stays correct for the back buffer; whoever reads the other buffers sees stale content. */
   {
      static unsigned noopLogged;
      if (GetFileAttributesA("C:\\g6w\\rotate.noop") != INVALID_FILE_ATTRIBUTES) {
         if (UmdTrace() && noopLogged++ < 2)
            DebugPrintf("rotate: %u resources %ux%u: no-op (C:\\g6w\\rotate.noop)\n", RotateResourceIdentities->Resources,
                        resource0->width0, resource0->height0);
         return S_OK;
      }
      if (UmdTrace() && noopLogged == 0 && (noopLogged = 1))
         DebugPrintf("rotate: %u resources %ux%u: copies\n", RotateResourceIdentities->Resources, resource0->width0,
                     resource0->height0);
   }
   /* R7: the temporary is kept for the next rotation of the same kind (on the G18 every create was a GEM_CREATE
    * plus a VM_BIND, ~18 ms per DWM frame in r7 run 20261121-020733) */
   Device *pDev = CastDevice(RotateResourceIdentities->hDevice);
   UINT64 tc0 = PerfQpc();
   struct pipe_resource *temp_resource = NULL;
   struct pipe_resource *keep = pDev->rotTemp;
   if (keep && keep->target == resource0->target && keep->format == resource0->format &&
       keep->width0 == resource0->width0 && keep->height0 == resource0->height0 &&
       keep->depth0 == resource0->depth0 && keep->array_size == resource0->array_size &&
       keep->last_level == resource0->last_level && keep->nr_samples == resource0->nr_samples) {
      pipe_resource_reference(&temp_resource, keep);
   } else {
      struct pipe_resource t = *resource0;
      t.flags = 0;
      t.bind &= PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
      temp_resource = screen->resource_create(screen, &t);
      if (!temp_resource)
         temp_resource = screen->resource_create(screen, resource0);
      pipe_resource_reference(&pDev->rotTemp, temp_resource);
   }
   assert(temp_resource);
   if (!temp_resource) {
      return E_OUTOFMEMORY;
   }
   UINT64 tc1 = PerfQpc();
   PerfAdd(PERF_R_CREATE, tc1 - tc0);

   struct pipe_box src_box;
   src_box.x = 0;
   src_box.y = 0;
   src_box.z = 0;
   src_box.width  = resource0->width0;
   src_box.height = resource0->height0;
   src_box.depth  = resource0->depth0;

   for (UINT i = 0; i < RotateResourceIdentities->Resources + 1; ++i) {
      struct pipe_resource *src_resource;
      struct pipe_resource *dst_resource;

      if (i < RotateResourceIdentities->Resources) {
         src_resource = CastPipeResource(RotateResourceIdentities->pResources[i]);
      } else {
         src_resource = temp_resource;
      }

      if (i > 0) {
         dst_resource = CastPipeResource(RotateResourceIdentities->pResources[i - 1]);
      } else {
         dst_resource = temp_resource;
      }

      assert(dst_resource);
      assert(src_resource);

      pipe->resource_copy_region(pipe,
                                 dst_resource,
                                 0, // dst_level
                                 0, 0, 0, // dst_x,y,z
                                 src_resource,
                                 0, // src_level
                                 &src_box);
   }

   pipe_resource_reference(&temp_resource, NULL);
   PerfAdd(PERF_R_COPY, PerfQpc() - tc1);

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

static HRESULT
DoBlt(DXGI_DDI_HDEVICE hDevice, DXGI_DDI_HRESOURCE hDst, UINT DstSub, const RECT *pDstRect,
      DXGI_DDI_HRESOURCE hSrc, UINT SrcSub, const RECT *pSrcRect, DXGI_DDI_MODE_ROTATION Rotate)
{
   struct pipe_context *pipe = CastPipeDevice(hDevice);
   struct pipe_resource *dst = CastPipeResource(hDst);
   struct pipe_resource *src = CastPipeResource(hSrc);
   if (!pipe || !dst || !src)
      return E_INVALIDARG;

   LOG_UNSUPPORTED(Rotate != DXGI_DDI_MODE_ROTATION_IDENTITY &&
                   Rotate != DXGI_DDI_MODE_ROTATION_UNSPECIFIED);

   unsigned src_level = SrcSub % (src->last_level + 1);
   unsigned src_layer = SrcSub / (src->last_level + 1);
   unsigned dst_level = DstSub % (dst->last_level + 1);
   unsigned dst_layer = DstSub / (dst->last_level + 1);

   struct pipe_blit_info info;
   memset(&info, 0, sizeof info);
   info.src.resource = src;
   info.src.level = src_level;
   info.src.format = src->format;
   info.src.box.z = src_layer;
   info.src.box.depth = 1;
   if (pSrcRect && pSrcRect->right > pSrcRect->left && pSrcRect->bottom > pSrcRect->top) {
      info.src.box.x = pSrcRect->left;
      info.src.box.y = pSrcRect->top;
      info.src.box.width = pSrcRect->right - pSrcRect->left;
      info.src.box.height = pSrcRect->bottom - pSrcRect->top;
   } else {
      info.src.box.width = u_minify(src->width0, src_level);
      info.src.box.height = u_minify(src->height0, src_level);
   }
   info.dst.resource = dst;
   info.dst.level = dst_level;
   info.dst.format = dst->format;
   info.dst.box.z = dst_layer;
   info.dst.box.depth = 1;
   if (pDstRect && pDstRect->right > pDstRect->left && pDstRect->bottom > pDstRect->top) {
      info.dst.box.x = pDstRect->left;
      info.dst.box.y = pDstRect->top;
      info.dst.box.width = pDstRect->right - pDstRect->left;
      info.dst.box.height = pDstRect->bottom - pDstRect->top;
   } else {
      info.dst.box.width = u_minify(dst->width0, dst_level);
      info.dst.box.height = u_minify(dst->height0, dst_level);
   }
   info.mask = PIPE_MASK_RGBA;
   info.filter = src->nr_samples > 1 ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
   pipe->blit(pipe, &info);
   return S_OK;
}

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   LOG_ENTRYPOINT();
   RECT d = { (LONG)Blt->DstLeft, (LONG)Blt->DstTop, (LONG)Blt->DstRight, (LONG)Blt->DstBottom };
   return DoBlt(Blt->hDevice, Blt->hDstResource, Blt->DstSubresource, &d, Blt->hSrcResource,
                Blt->SrcSubresource, NULL, Blt->Rotate);
}

HRESULT APIENTRY
_Blt1(DXGI_DDI_ARG_BLT1 *Blt)
{
   LOG_ENTRYPOINT();
   RECT d = { (LONG)Blt->DstLeft, (LONG)Blt->DstTop, (LONG)Blt->DstRight, (LONG)Blt->DstBottom };
   RECT s = { (LONG)Blt->SrcLeft, (LONG)Blt->SrcTop, (LONG)Blt->SrcRight, (LONG)Blt->SrcBottom };
   return DoBlt(Blt->hDevice, Blt->hDstResource, Blt->DstSubresource, &d, Blt->hSrcResource,
                Blt->SrcSubresource, &s, Blt->Rotate);
}

HRESULT APIENTRY
_ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *p)
{
   LOG_ENTRYPOINT();
   struct pipe_context *pipe = CastPipeDevice(p->hDevice);
   if (pipe)
      pipe->flush(pipe, NULL, 0);
   return S_OK;
}

HRESULT APIENTRY
_OfferResources(DXGI_DDI_ARG_OFFERRESOURCES *p)
{
   LOG_ENTRYPOINT();
   return S_OK;
}

HRESULT APIENTRY
_ReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *p)
{
   LOG_ENTRYPOINT();
   if (p->pDiscarded)
      for (UINT i = 0; i < p->Resources; i++)
         p->pDiscarded[i] = FALSE;
   return S_OK;
}
