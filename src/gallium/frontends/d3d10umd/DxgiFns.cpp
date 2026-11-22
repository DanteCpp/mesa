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
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "DxgiFns.h"
#include "Format.h"
#include "State.h"

#include "Debug.h"
#include "Wddm.h"

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/box.h"
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
 * View tracking (R8) --
 *
 *    Each RTV, DSV and SRV is linked to the resource handle it was
 *    created on, so that RotateResourceIdentities can move the views
 *    along with the storage.
 *
 * ----------------------------------------------------------------------
 */

void
ViewTrack(struct ViewLink *l, Resource *pResource, int kind)
{
   l->res = pResource;
   l->kind = kind;
   l->prev = NULL;
   l->next = NULL;
   if (!pResource)
      return;
   l->next = pResource->views;
   if (l->next)
      l->next->prev = l;
   pResource->views = l;
}

void
ViewUntrack(struct ViewLink *l)
{
   if (!l->res)
      return;
   if (l->prev)
      l->prev->next = l->next;
   else
      l->res->views = l->next;
   if (l->next)
      l->next->prev = l->prev;
   l->res = NULL;
   l->next = l->prev = NULL;
}

/* R8 diagnostics (trace): which rotated handle (index in the last rotation) a bind/copy names, for the first
 * DDI calls after rotations start; -1 = not one of them */
static int g_rotDiagOn = -1;   /* C:\\g6w\\rotdiag.on with the UMD trace */

void
RotDiag(struct Device *pDev, const char *what, int slot, Resource *a, Resource *b)
{
   if (g_rotDiagOn < 0)
      g_rotDiagOn = UmdTrace() && GetFileAttributesA("C:\\g6w\\rotdiag.on") != INVALID_FILE_ATTRIBUTES;
   if (!g_rotDiagOn || !pDev || !pDev->rotN || pDev->rotDiag >= 3000)
      return;
   if (!strcmp(what, "Present")) {
      /* one line per present: where the frame's draws went (h0 is right) */
      pDev->rotDiag++;
      int ia = -1;
      for (UINT i = 0; i < pDev->rotN; i++)
         if (pDev->rotSet[i] == a)
            ia = i;
      /* software screens: a content hash of each rotated handle's storage (every 16th row) */
      UINT32 sum[4] = { 0, 0, 0, 0 };
      if (pDev->dt_capable) {
         for (UINT i = 0; i < pDev->rotN; i++) {
            struct pipe_resource *t = pDev->rotSet[i]->resource;
            struct pipe_transfer *tr;
            const uint8_t *m = t ? (const uint8_t *)pipe_texture_map(pDev->pipe, t, 0, 0, PIPE_MAP_READ, 0, 0, t->width0,
                                                                    t->height0, &tr) : NULL;
            if (!m)
               continue;
            UINT32 h = 2166136261u;
            for (UINT y = 0; y < t->height0; y += 16) {
               const UINT32 *row = (const UINT32 *)(m + (size_t)y * tr->stride);
               for (UINT x = 0; x < t->width0; x += 4)
                  h = (h ^ row[x]) * 16777619u;
            }
            sum[i] = h;
            pipe_texture_unmap(pDev->pipe, tr);
         }
      }
      DebugPrintf("rotdiag call %u: Present flags 0x%x dst 0x%x src 0x%x h%d: draws h0 %u h1 %u h2 %u h3 %u unbound %u other %u sums %08x %08x %08x\n",
                  pDev->rotCalls, slot, b ? b->hAllocation : 0, a ? a->hAllocation : 0, ia, pDev->rotDraws[0],
                  pDev->rotDraws[1], pDev->rotDraws[2], pDev->rotDraws[3],
                  pDev->rotDraws[4], pDev->rotDraws[5], sum[0], sum[1], sum[2]);
      memset(pDev->rotDraws, 0, sizeof pDev->rotDraws);
      return;
   }
   int ia = -1, ib = -1;
   for (UINT i = 0; i < pDev->rotN; i++) {
      if (a && pDev->rotSet[i] == a)
         ia = i;
      if (b && pDev->rotSet[i] == b)
         ib = i;
   }
   if (ia < 0 && ib < 0)
      return;
   pDev->rotDiag++;
   DebugPrintf("rotdiag call %u: %s slot %d: h%d%s h%d\n", pDev->rotCalls, what, slot, ia, b ? " <-" : "", ib);
}

/* a destroyed resource leaves the rotation diagnostics and the copy-forward chain (whose pointers would dangle) */
void
RotForget(struct Device *pDev, Resource *pResource)
{
   if (!pDev)
      return;
   for (UINT i = 0; i < pDev->rotN; i++)
      if (pDev->rotSet[i] == pResource) {
         pDev->rotN = 0;
         break;
      }
   bool cf = pResource->resource && pResource->resource == pDev->cfTarget;
   for (UINT i = 0; i < pDev->cfSetN; i++)
      cf = cf || pDev->cfSet[i] == pResource;
   if (cf) {
      pDev->cfSetN = 0;
      pDev->cfHistN = 0;
      pDev->cfTarget = NULL;
      memset(pDev->cfCur, 0, sizeof pDev->cfCur);
   }
}

void
RotDiagDraw(struct Device *pDev, struct pipe_resource *target)
{
   if (g_rotDiagOn <= 0 || !pDev || !pDev->rotN || pDev->rotDiag >= 3000)
      return;
   if (!target) {
      pDev->rotDraws[4]++;
      return;
   }
   for (UINT i = 0; i < pDev->rotN; i++)
      if (pDev->rotSet[i]->resource == target) {
         pDev->rotDraws[i]++;
         return;
      }
   pDev->rotDraws[5]++;
}

void
ViewsDetach(Resource *pResource)
{
   while (pResource->views)
      ViewUntrack(pResource->views);
}


/*
 * ----------------------------------------------------------------------
 *
 * Copy-forward damage (R8) --
 *
 *    DXGI presents DWM's swap chain by copy (its primaries are NO_SCANOUT
 *    here) and DWM then draws only what changed since the frame it just
 *    presented: the back buffer must hold the latest frame. After an
 *    identity rotation the new back buffer holds the frame from n-1
 *    presents ago; RotateIdentities copies into it, from the buffer just
 *    presented, the regions drawn in the frames it missed. Damage is
 *    tracked conservatively (bounding boxes; draws: viewport, clipped by
 *    the scissor when the test is on): a larger region only costs a
 *    larger copy, the source holds the complete latest frame everywhere.
 *
 * ----------------------------------------------------------------------
 */

static void
BoxUnion(int *b, int x0, int y0, int x1, int y1)
{
   if (x1 <= x0 || y1 <= y0)
      return;
   if (b[2] <= b[0] || b[3] <= b[1]) {
      b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1;
      return;
   }
   b[0] = MIN2(b[0], x0); b[1] = MIN2(b[1], y0);
   b[2] = MAX2(b[2], x1); b[3] = MAX2(b[3], y1);
}

void
CfDamage(struct Device *pDev, struct pipe_resource *tex, int x0, int y0, int x1, int y1)
{
   if (!pDev || !tex || tex != pDev->cfTarget)
      return;
   x0 = MAX2(x0, 0);
   y0 = MAX2(y0, 0);
   x1 = MIN2(x1, (int)tex->width0);
   y1 = MIN2(y1, (int)tex->height0);
   BoxUnion(pDev->cfCur, x0, y0, x1, y1);
}

void
CfDamageDraw(struct Device *pDev)
{
   for (unsigned i = 0; i < pDev->fb.nr_cbufs; i++) {
      if (pDev->fb.cbufs[i].texture != pDev->cfTarget)
         continue;
      int x0 = 0, y0 = 0, x1 = 1 << 30, y1 = 1 << 30;
      if (pDev->vp[2] > 0.0f && pDev->vp[3] > 0.0f) {
         x0 = (int)floorf(pDev->vp[0]);
         y0 = (int)floorf(pDev->vp[1]);
         x1 = (int)ceilf(pDev->vp[0] + pDev->vp[2]);
         y1 = (int)ceilf(pDev->vp[1] + pDev->vp[3]);
      }
      if (pDev->rsScissor) {
         x0 = MAX2(x0, pDev->scissor0[0]);
         y0 = MAX2(y0, pDev->scissor0[1]);
         x1 = MIN2(x1, pDev->scissor0[2]);
         y1 = MIN2(y1, pDev->scissor0[3]);
      }
      CfDamage(pDev, pDev->cfTarget, x0, y0, x1, y1);
   }
}

/* after the storage rotation: bring the new back buffer r[0] up to the frame just presented (now r[n-1]) */
static void
CopyForward(Device *pDev, Resource *const *r, struct pipe_resource *const *old, UINT n)
{
   struct pipe_context *pipe = pDev->pipe;
   bool same = pDev->cfSetN == n && pDev->cfTarget == old[0];
   for (UINT i = 0; same && i < n; i++)
      same = pDev->cfSet[i] == r[i];
   if (!same) {
      pDev->cfHistN = 0;
      pDev->cfSetN = MIN2(n, 4);
      for (UINT i = 0; i < pDev->cfSetN; i++)
         pDev->cfSet[i] = r[i];
   } else {
      /* the frame just presented: what was drawn into old[0] */
      memmove(pDev->cfHist[1], pDev->cfHist[0], sizeof(pDev->cfHist[0]) * 3);
      memcpy(pDev->cfHist[0], pDev->cfCur, sizeof pDev->cfCur);
      pDev->cfHistN = MIN2(pDev->cfHistN + 1, 4);
   }
   struct pipe_resource *dst = r[0]->resource, *src = r[n - 1]->resource;
   int box[4] = { 0, 0, 0, 0 };
   bool full = !same || n - 1 > pDev->cfHistN || n > 4;
   if (full) {
      box[2] = dst->width0;
      box[3] = dst->height0;
      pDev->cfFull++;
   } else {
      for (UINT k = 0; k < n - 1; k++)
         BoxUnion(box, pDev->cfHist[k][0], pDev->cfHist[k][1], pDev->cfHist[k][2], pDev->cfHist[k][3]);
   }
   if (box[2] > box[0] && box[3] > box[1]) {
      struct pipe_box b;
      u_box_2d(box[0], box[1], box[2] - box[0], box[3] - box[1], &b);
      UINT64 t0 = PerfQpc();
      pipe->resource_copy_region(pipe, dst, 0, box[0], box[1], 0, src, 0, &b);
      PerfAdd(PERF_R_COPY, PerfQpc() - t0);
      pDev->cfPixels += (UINT64)(box[2] - box[0]) * (box[3] - box[1]);
   }
   pDev->cfFrames++;
   static unsigned logged;
   if (UmdTrace() && (logged++ < 8 || pDev->cfFrames % 600 == 0))
      DebugPrintf("rotate: copy-forward %s %d,%d-%d,%d into the new back buffer (frames %llu, full %llu, %.1f Mpx copied)\n",
                  full ? "FULL" : "damage", box[0], box[1], box[2], box[3], (unsigned long long)pDev->cfFrames,
                  (unsigned long long)pDev->cfFull, pDev->cfPixels / 1e6);
   pDev->cfTarget = dst;
   memset(pDev->cfCur, 0, sizeof pDev->cfCur);
}


/*
 * ----------------------------------------------------------------------
 *
 * RotateIdentities (R8) --
 *
 *    Rotate the storage of the swap-chain buffers without copies: handle
 *    i takes over what handle i+1 held (handle n-1 what handle 0 held).
 *    The storage (pipe resource, KMD allocation, zero-copy import, shadow
 *    state) moves in WddmRotateStorage; here the views of each handle are
 *    retargeted to the handle's new storage, and the bound render targets
 *    and shader resources are rebound so that bound state follows too.
 *
 * ----------------------------------------------------------------------
 */

static bool
RotateIdentities(Device *pDev, DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *pRotate)
{
   struct pipe_context *pipe = pDev->pipe;
   UINT n = pRotate->Resources;
   if (n > 16)
      return false;
   Resource *r[16];
   struct pipe_resource *old[16];
   for (UINT i = 0; i < n; i++) {
      r[i] = CastResource(pRotate->pResources[i]);
      if (!r[i] || !r[i]->resource)
         return false;
      if (r[i]->transfers)
         for (UINT s = 0; s < r[i]->NumSubResources; s++)
            if (r[i]->transfers[s])
               return false;
      old[i] = r[i]->resource;
   }
   if (!WddmRotateStorage(pDev, r, n))
      return false;
   pDev->rotCalls++;
   pDev->rotN = MIN2(n, 4);
   for (UINT i = 0; i < pDev->rotN; i++)
      pDev->rotSet[i] = r[i];

   /* the views of each handle: RTV/DSV surfaces point at the new storage, SRVs are made anew on it */
   struct pipe_sampler_view *svOld[64], *svNew[64];
   UINT nsv = 0, nrt = 0, nds = 0;
   for (UINT i = 0; i < n; i++) {
      struct pipe_resource *now = r[i]->resource;
      for (struct ViewLink *l = r[i]->views; l; l = l->next) {
         if (l->kind == VIEW_RTV) {
            RenderTargetView *v = (RenderTargetView *)((char *)l - offsetof(RenderTargetView, link));
            if (v->surface.texture == old[i]) {
               pipe_resource_reference(&v->surface.texture, now);
               nrt++;
            }
         } else if (l->kind == VIEW_DSV) {
            DepthStencilView *v = (DepthStencilView *)((char *)l - offsetof(DepthStencilView, link));
            if (v->surface.texture == old[i]) {
               pipe_resource_reference(&v->surface.texture, now);
               nds++;
            }
         } else if (l->kind == VIEW_SRV) {
            ShaderResourceView *v = (ShaderResourceView *)((char *)l - offsetof(ShaderResourceView, link));
            if (!v->handle || v->handle->texture != old[i])
               continue;
            struct pipe_sampler_view *sv = pipe->create_sampler_view(pipe, now, &v->tmpl);
            if (!sv)
               continue; /* keeps the old one (stale, but valid) */
            if (nsv < ARRAY_SIZE(svOld)) {
               svOld[nsv] = v->handle;
               svNew[nsv] = sv;
               nsv++;
               v->handle = sv;
            } else {
               /* not tracked for rebinding: an unbound view can be swapped right away */
               bool bound = false;
               for (unsigned st = 0; st < MESA_SHADER_STAGES && !bound; st++)
                  for (unsigned k = 0; k < PIPE_MAX_SHADER_SAMPLER_VIEWS; k++)
                     if (pDev->sampler_views[st][k] == v->handle) {
                        bound = true;
                        break;
                     }
               if (bound) {
                  pipe->sampler_view_release(pipe, sv);
               } else {
                  pipe->sampler_view_release(pipe, v->handle);
                  v->handle = sv;
               }
            }
         }
      }
   }

   /* DWM's copy-model chain: the new back buffer gets the latest frame (C:\g6w\rotate.nocf: not) */
   bool cm = true;
   for (UINT i = 0; i < n; i++)
      cm = cm && r[i]->copyModel;
   if (cm && GetFileAttributesA("C:\\g6w\\rotate.nocf") == INVALID_FILE_ATTRIBUTES)
      CopyForward(pDev, r, old, n);
   else
      pDev->cfTarget = NULL;

   /* bound render targets: the storage of handle i was bound, so the new storage of handle i is */
   bool fbChanged = false;
   for (UINT c = 0; c <= PIPE_MAX_COLOR_BUFS; c++) {
      struct pipe_surface *ps = c < PIPE_MAX_COLOR_BUFS ? &pDev->fb.cbufs[c] : &pDev->fb.zsbuf;
      if (!ps->texture)
         continue;
      for (UINT i = 0; i < n; i++)
         if (ps->texture == old[i]) {
            pipe_resource_reference(&ps->texture, r[i]->resource);
            fbChanged = true;
            break;
         }
   }
   if (fbChanged)
      pipe->set_framebuffer_state(pipe, &pDev->fb);

   /* bound shader resources */
   unsigned svRebinds = 0;
   if (nsv) {
      for (unsigned st = 0; st < MESA_SHADER_STAGES; st++) {
         bool changed = false;
         for (unsigned k = 0; k < PIPE_MAX_SHADER_SAMPLER_VIEWS; k++) {
            struct pipe_sampler_view *b = pDev->sampler_views[st][k];
            if (!b)
               continue;
            for (UINT j = 0; j < nsv; j++)
               if (b == svOld[j]) {
                  pDev->sampler_views[st][k] = svNew[j];
                  changed = true;
                  svRebinds++;
                  break;
               }
         }
         if (changed)
            pipe->set_sampler_views(pipe, (mesa_shader_stage)st, 0, PIPE_MAX_SHADER_SAMPLER_VIEWS, 0,
                                    pDev->sampler_views[st]);
      }
      for (UINT j = 0; j < nsv; j++)
         pipe->sampler_view_release(pipe, svOld[j]);
   }

   static unsigned logged;
   if (UmdTrace() && (logged++ < 4 || logged % 3600 == 0))
      DebugPrintf("rotate: %u resources: views moved rtv %u dsv %u srv %u, framebuffer rebound %d, srv rebinds %u (call %u)\n",
                  n, nrt, nds, nsv, fbChanged, svRebinds, logged);
   return true;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate the identities of a list of resources (R8: their storage and
 *    views move, no copies). Debug switches (checked at every call):
 *    C:\g6w\rotate.noop skips the rotation (wrong: stale buffers),
 *    C:\g6w\rotate.copy rotates the contents by GPU copies (the old way).
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
   if (!resource0)
      return E_INVALIDARG;
   LOG_UNSUPPORTED(resource0->last_level);

   {
      static unsigned noopLogged, copyLogged;
      if (GetFileAttributesA("C:\\g6w\\rotate.noop") != INVALID_FILE_ATTRIBUTES) {
         if (UmdTrace() && noopLogged++ < 2)
            DebugPrintf("rotate: %u resources %ux%u: no-op (C:\\g6w\\rotate.noop)\n", RotateResourceIdentities->Resources,
                        resource0->width0, resource0->height0);
         return S_OK;
      }
      bool copy = GetFileAttributesA("C:\\g6w\\rotate.copy") != INVALID_FILE_ATTRIBUTES;
      if (!copy) {
         UINT64 t0 = PerfQpc();
         bool ok = RotateIdentities(CastDevice(RotateResourceIdentities->hDevice), RotateResourceIdentities);
         PerfAdd(PERF_R_CREATE, PerfQpc() - t0);
         if (ok)
            return S_OK;
      }
      {
         Device *pd = CastDevice(RotateResourceIdentities->hDevice);
         pd->rotCalls++;
         pd->rotN = MIN2(RotateResourceIdentities->Resources, 4);
         for (UINT i = 0; i < pd->rotN; i++)
            pd->rotSet[i] = CastResource(RotateResourceIdentities->pResources[i]);
      }
      if (UmdTrace() && copyLogged++ < 2)
         DebugPrintf("rotate: %u resources %ux%u: copies (%s)\n", RotateResourceIdentities->Resources, resource0->width0,
                     resource0->height0, copy ? "C:\\g6w\\rotate.copy" : "not rotatable by identity");
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
   RotDiag(CastDevice(hDevice), "Blt", -1, CastResource(hDst), CastResource(hSrc));
   CfDamage(CastDevice(hDevice), dst, 0, 0, 1 << 30, 1 << 30);
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
