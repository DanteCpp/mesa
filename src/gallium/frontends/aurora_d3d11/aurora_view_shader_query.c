/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Native D3D11 Views (RTV/DSV/SRV/UAV), Clears, GenMips, SM4/5 Shaders ->
 * NIR -> Gallium Asahi, Queries, Predication, and Flush.
 */
#include "aurora_d3d11.h"
#include "aurora_format.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Render Target Views (10 & WDDM 2.0)
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateRTVSize(D3D10DDI_HDEVICE d, const D3D10DDIARG_CREATERENDERTARGETVIEW *p)
{ (void)d; (void)p; return sizeof(struct aurora_rtv); }

SIZE_T APIENTRY
aurora_CalcPrivateRTVSize_WDDM2_0(D3D10DDI_HDEVICE d, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *p)
{ (void)d; (void)p; return sizeof(struct aurora_rtv); }

static void
create_rtv_common(struct aurora_device *dev, struct aurora_rtv *rtv,
                  struct aurora_resource *res, DXGI_FORMAT fmt,
                  D3D10DDIRESOURCE_TYPE dim, UINT mip, UINT first_layer,
                  UINT array_size)
{
   memset(rtv, 0, sizeof(*rtv));
   if (!res || !res->prsrc) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }
   enum pipe_format pf = aurora_dxgi_to_pipe_format(fmt);
   if (pf == PIPE_FORMAT_NONE)
      pf = res->prsrc->format;

   rtv->res = res;
   rtv->dxgi_format = fmt;
   rtv->dim = dim;
   rtv->surf.format = pf;
   rtv->surf.level = mip;
   rtv->surf.first_layer = first_layer;
   rtv->surf.last_layer =
      (array_size == (UINT)-1 || array_size == 0u)
         ? (MAX2(res->array_size, 1u) - 1u)
         : (first_layer + array_size - 1u);
   rtv->surf.texture = res->prsrc;
}

void APIENTRY
aurora_CreateRTV(D3D10DDI_HDEVICE hDevice,
                 const D3D10DDIARG_CREATERENDERTARGETVIEW *pArgs,
                 D3D10DDI_HRENDERTARGETVIEW hRTV,
                 D3D10DDI_HRTRENDERTARGETVIEW hRTRTV)
{
   (void)hRTRTV;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rtv *rtv = (struct aurora_rtv *)hRTV.pDrvPrivate;
   if (!dev || !pArgs || !rtv)
      return;
   struct aurora_resource *res =
      (struct aurora_resource *)pArgs->hDrvResource.pDrvPrivate;
   UINT mip = 0, first = 0, count = 1;
   switch (pArgs->ResourceDimension) {
   case D3D10DDIRESOURCE_TEXTURE1D:
      mip = pArgs->Tex1D.MipSlice;
      first = pArgs->Tex1D.FirstArraySlice;
      count = pArgs->Tex1D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      mip = pArgs->Tex2D.MipSlice;
      first = pArgs->Tex2D.FirstArraySlice;
      count = pArgs->Tex2D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      mip = pArgs->Tex3D.MipSlice;
      first = pArgs->Tex3D.FirstW;
      count = pArgs->Tex3D.WSize;
      break;
   default:
      break;
   }
   create_rtv_common(dev, rtv, res, pArgs->Format, pArgs->ResourceDimension,
                     mip, first, count);
}

void APIENTRY
aurora_CreateRTV_WDDM2_0(D3D10DDI_HDEVICE hDevice,
                         const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *pArgs,
                         D3D10DDI_HRENDERTARGETVIEW hRTV,
                         D3D10DDI_HRTRENDERTARGETVIEW hRTRTV)
{
   (void)hRTRTV;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rtv *rtv = (struct aurora_rtv *)hRTV.pDrvPrivate;
   if (!dev || !pArgs || !rtv)
      return;
   struct aurora_resource *res =
      (struct aurora_resource *)pArgs->hDrvResource.pDrvPrivate;
   UINT mip = 0, first = 0, count = 1;
   switch (pArgs->ResourceDimension) {
   case D3D10DDIRESOURCE_TEXTURE1D:
      mip = pArgs->Tex1D.MipSlice;
      first = pArgs->Tex1D.FirstArraySlice;
      count = pArgs->Tex1D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      mip = pArgs->Tex2D.MipSlice;
      first = pArgs->Tex2D.FirstArraySlice;
      count = pArgs->Tex2D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      mip = pArgs->Tex3D.MipSlice;
      first = pArgs->Tex3D.FirstW;
      count = pArgs->Tex3D.WSize;
      break;
   default:
      break;
   }
   create_rtv_common(dev, rtv, res, pArgs->Format, pArgs->ResourceDimension,
                     mip, first, count);
}

void APIENTRY
aurora_DestroyRTV(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRENDERTARGETVIEW hRTV)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rtv *rtv = (struct aurora_rtv *)hRTV.pDrvPrivate;
   if (!dev || !rtv)
      return;
   aurora_lock(dev);
   for (UINT i = 0; i < AURORA_MAX_RTVS; ++i) {
      if (dev->rtvs[i] == rtv) {
         dev->rtvs[i] = NULL;
         dev->fb_dirty = true;
      }
   }
   rtv->res = NULL;
   rtv->surf.texture = NULL;
   aurora_unlock(dev);
}

bool
aurora_rtv_fill_surface(struct aurora_rtv *rtv, struct pipe_surface *out)
{
   if (!rtv || !rtv->res || !rtv->res->prsrc || !out)
      return false;
   *out = rtv->surf;
   /* Always pull current prsrc so RotateResourceIdentities works transparently! */
   out->texture = rtv->res->prsrc;
   return true;
}

/* =========================================================================
 * Depth/Stencil Views
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateDSVSize(D3D10DDI_HDEVICE d, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *p)
{ (void)d; (void)p; return sizeof(struct aurora_dsv); }

void APIENTRY
aurora_CreateDSV(D3D10DDI_HDEVICE hDevice,
                 const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pArgs,
                 D3D10DDI_HDEPTHSTENCILVIEW hDSV,
                 D3D10DDI_HRTDEPTHSTENCILVIEW hRTDSV)
{
   (void)hRTDSV;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_dsv *dsv = (struct aurora_dsv *)hDSV.pDrvPrivate;
   if (!dev || !pArgs || !dsv)
      return;
   memset(dsv, 0, sizeof(*dsv));

   struct aurora_resource *res =
      (struct aurora_resource *)pArgs->hDrvResource.pDrvPrivate;
   if (!res || !res->prsrc) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }

   enum pipe_format pf = aurora_dxgi_to_pipe_format_dsv(pArgs->Format);
   if (pf == PIPE_FORMAT_NONE)
      pf = res->prsrc->format;

   UINT mip = 0, first = 0, count = 1;
   if (pArgs->ResourceDimension == D3D10DDIRESOURCE_TEXTURE1D) {
      mip = pArgs->Tex1D.MipSlice;
      first = pArgs->Tex1D.FirstArraySlice;
      count = pArgs->Tex1D.ArraySize;
   } else if (pArgs->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D) {
      mip = pArgs->Tex2D.MipSlice;
      first = pArgs->Tex2D.FirstArraySlice;
      count = pArgs->Tex2D.ArraySize;
   }

   dsv->res = res;
   dsv->dxgi_format = pArgs->Format;
   dsv->flags = pArgs->Flags;
   dsv->surf.format = pf;
   dsv->surf.level = mip;
   dsv->surf.first_layer = first;
   dsv->surf.last_layer =
      (count == (UINT)-1 || count == 0u)
         ? (MAX2(res->array_size, 1u) - 1u)
         : (first + count - 1u);
   dsv->surf.texture = res->prsrc;
}

void APIENTRY
aurora_DestroyDSV(D3D10DDI_HDEVICE hDevice, D3D10DDI_HDEPTHSTENCILVIEW hDSV)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_dsv *dsv = (struct aurora_dsv *)hDSV.pDrvPrivate;
   if (!dev || !dsv)
      return;
   aurora_lock(dev);
   if (dev->dsv == dsv) {
      dev->dsv = NULL;
      dev->fb_dirty = true;
   }
   dsv->res = NULL;
   dsv->surf.texture = NULL;
   aurora_unlock(dev);
}

bool
aurora_dsv_fill_surface(struct aurora_dsv *dsv, struct pipe_surface *out)
{
   if (!dsv || !dsv->res || !dsv->res->prsrc || !out)
      return false;
   *out = dsv->surf;
   out->texture = dsv->res->prsrc;
   return true;
}

/* =========================================================================
 * Shader Resource Views (11 & WDDM 2.0)
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateSRVSize(D3D10DDI_HDEVICE d, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *p)
{ (void)d; (void)p; return sizeof(struct aurora_srv); }

SIZE_T APIENTRY
aurora_CalcPrivateSRVSize_WDDM2_0(D3D10DDI_HDEVICE d, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *p)
{ (void)d; (void)p; return sizeof(struct aurora_srv); }

static void
create_srv_common(struct aurora_device *dev, struct aurora_srv *srv,
                  struct aurora_resource *res, DXGI_FORMAT fmt,
                  D3D10DDIRESOURCE_TYPE dim,
                  UINT first_elem, UINT num_elems, UINT buf_flags,
                  UINT most_detailed_mip, UINT mip_levels,
                  UINT first_array, UINT array_size)
{
   memset(srv, 0, sizeof(*srv));
   if (!dev || !dev->pctx || !res || !res->prsrc) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }

   srv->res = res;
   srv->res_gen = res->gen;
   srv->dxgi_format = fmt;
   srv->dim = dim;

   /* Raw or Structured Buffer SRV -> Gallium SSBO slot (AURORA_SSBO_SRV_BASE + t#) */
   if (dim == D3D11DDIRESOURCE_BUFFEREX &&
       (buf_flags & D3D11_DDI_BUFFEREX_SRV_FLAG_RAW)) {
      srv->is_raw_or_struct = true;
      srv->buf_offset = first_elem * 4u;
      srv->buf_size = num_elems * 4u;
      return;
   }
   if ((dim == D3D10DDIRESOURCE_BUFFER || dim == D3D11DDIRESOURCE_BUFFEREX) &&
       res->structure_stride > 0u && fmt == DXGI_FORMAT_UNKNOWN) {
      srv->is_raw_or_struct = true;
      srv->buf_offset = first_elem * res->structure_stride;
      srv->buf_size = num_elems * res->structure_stride;
      return;
   }

   enum pipe_format pf = aurora_dxgi_to_pipe_format_srv(fmt);
   if (pf == PIPE_FORMAT_NONE)
      pf = res->prsrc->format;

   struct pipe_sampler_view *t = &srv->templ;
   memset(t, 0, sizeof(*t));
   t->format = pf;
   t->swizzle_r = PIPE_SWIZZLE_X;
   t->swizzle_g = PIPE_SWIZZLE_Y;
   t->swizzle_b = PIPE_SWIZZLE_Z;
   t->swizzle_a = PIPE_SWIZZLE_W;

   /* D3D11 depth/stencil view formats replicate depth/stencil into .r / .g properly. */
   if (fmt == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT ||
       fmt == DXGI_FORMAT_X24_TYPELESS_G8_UINT) {
      t->swizzle_r = PIPE_SWIZZLE_0;
      t->swizzle_g = PIPE_SWIZZLE_X;
      t->swizzle_b = PIPE_SWIZZLE_0;
      t->swizzle_a = PIPE_SWIZZLE_1;
   } else if (fmt == DXGI_FORMAT_B8G8R8X8_UNORM ||
              fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) {
      t->swizzle_a = PIPE_SWIZZLE_1;
   }

   if (dim == D3D10DDIRESOURCE_BUFFER || dim == D3D11DDIRESOURCE_BUFFEREX) {
      unsigned elem_size = util_format_get_blocksize(pf);
      t->target = PIPE_BUFFER;
      t->u.buf.offset = first_elem * elem_size;
      t->u.buf.size = num_elems * elem_size;
   } else {
      switch (dim) {
      case D3D10DDIRESOURCE_TEXTURE1D:
         t->target = (res->array_size > 1u) ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
         break;
      case D3D10DDIRESOURCE_TEXTURE2D:
         t->target = (res->array_size > 1u) ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
         break;
      case D3D10DDIRESOURCE_TEXTURE3D:
         t->target = PIPE_TEXTURE_3D;
         break;
      case D3D10DDIRESOURCE_TEXTURECUBE:
         t->target = (array_size > 6u || res->array_size > 6u)
                        ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
         break;
      default:
         t->target = res->prsrc->target;
         break;
      }
      t->u.tex.first_level = most_detailed_mip;
      UINT max_lvl = res->mip_levels > 0u ? (res->mip_levels - 1u) : 0u;
      t->u.tex.last_level =
         (mip_levels == (UINT)-1 || mip_levels == 0u)
            ? max_lvl
            : MIN2(most_detailed_mip + mip_levels - 1u, max_lvl);
      t->u.tex.first_layer = first_array;
      UINT max_lyr = res->array_size > 0u ? (res->array_size - 1u) : 0u;
      if (dim == D3D10DDIRESOURCE_TEXTURECUBE) {
         UINT cubes = (array_size == (UINT)-1 || array_size == 0u) ? 1u : array_size;
         t->u.tex.last_layer = MIN2(first_array + cubes * 6u - 1u, max_lyr);
      } else if (dim == D3D10DDIRESOURCE_TEXTURE3D) {
         t->u.tex.first_layer = 0;
         t->u.tex.last_layer = 0;
      } else {
         t->u.tex.last_layer =
            (array_size == (UINT)-1 || array_size == 0u)
               ? max_lyr
               : MIN2(first_array + array_size - 1u, max_lyr);
      }
   }

   aurora_lock(dev);
   srv->view = dev->pctx->create_sampler_view(dev->pctx, res->prsrc, &srv->templ);
   aurora_unlock(dev);
}

void APIENTRY
aurora_CreateSRV(D3D10DDI_HDEVICE hDevice,
                 const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pArgs,
                 D3D10DDI_HSHADERRESOURCEVIEW hSRV,
                 D3D10DDI_HRTSHADERRESOURCEVIEW hRTSRV)
{
   (void)hRTSRV;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_srv *srv = (struct aurora_srv *)hSRV.pDrvPrivate;
   if (!dev || !pArgs || !srv)
      return;
   struct aurora_resource *res =
      (struct aurora_resource *)pArgs->hDrvResource.pDrvPrivate;
   UINT fe = 0, ne = 0, bf = 0, mdm = 0, ml = 1, fa = 0, as = 1;
   switch (pArgs->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      fe = pArgs->Buffer.FirstElement;
      ne = pArgs->Buffer.NumElements;
      break;
   case D3D11DDIRESOURCE_BUFFEREX:
      fe = pArgs->BufferEx.FirstElement;
      ne = pArgs->BufferEx.NumElements;
      bf = pArgs->BufferEx.Flags;
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      mdm = pArgs->Tex1D.MostDetailedMip;
      ml = pArgs->Tex1D.MipLevels;
      fa = pArgs->Tex1D.FirstArraySlice;
      as = pArgs->Tex1D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      mdm = pArgs->Tex2D.MostDetailedMip;
      ml = pArgs->Tex2D.MipLevels;
      fa = pArgs->Tex2D.FirstArraySlice;
      as = pArgs->Tex2D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      mdm = pArgs->Tex3D.MostDetailedMip;
      ml = pArgs->Tex3D.MipLevels;
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      mdm = pArgs->TexCube.MostDetailedMip;
      ml = pArgs->TexCube.MipLevels;
      fa = pArgs->TexCube.First2DArrayFace;
      as = pArgs->TexCube.NumCubes;
      break;
   default:
      break;
   }
   create_srv_common(dev, srv, res, pArgs->Format, pArgs->ResourceDimension,
                     fe, ne, bf, mdm, ml, fa, as);
}

void APIENTRY
aurora_CreateSRV_WDDM2_0(D3D10DDI_HDEVICE hDevice,
                         const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *pArgs,
                         D3D10DDI_HSHADERRESOURCEVIEW hSRV,
                         D3D10DDI_HRTSHADERRESOURCEVIEW hRTSRV)
{
   (void)hRTSRV;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_srv *srv = (struct aurora_srv *)hSRV.pDrvPrivate;
   if (!dev || !pArgs || !srv)
      return;
   struct aurora_resource *res =
      (struct aurora_resource *)pArgs->hDrvResource.pDrvPrivate;
   UINT fe = 0, ne = 0, bf = 0, mdm = 0, ml = 1, fa = 0, as = 1;
   switch (pArgs->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      fe = pArgs->Buffer.FirstElement;
      ne = pArgs->Buffer.NumElements;
      break;
   case D3D11DDIRESOURCE_BUFFEREX:
      fe = pArgs->BufferEx.FirstElement;
      ne = pArgs->BufferEx.NumElements;
      bf = pArgs->BufferEx.Flags;
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      mdm = pArgs->Tex1D.MostDetailedMip;
      ml = pArgs->Tex1D.MipLevels;
      fa = pArgs->Tex1D.FirstArraySlice;
      as = pArgs->Tex1D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      mdm = pArgs->Tex2D.MostDetailedMip;
      ml = pArgs->Tex2D.MipLevels;
      fa = pArgs->Tex2D.FirstArraySlice;
      as = pArgs->Tex2D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      mdm = pArgs->Tex3D.MostDetailedMip;
      ml = pArgs->Tex3D.MipLevels;
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      mdm = pArgs->TexCube.MostDetailedMip;
      ml = pArgs->TexCube.MipLevels;
      fa = pArgs->TexCube.First2DArrayFace;
      as = pArgs->TexCube.NumCubes;
      break;
   default:
      break;
   }
   create_srv_common(dev, srv, res, pArgs->Format, pArgs->ResourceDimension,
                     fe, ne, bf, mdm, ml, fa, as);
}

void APIENTRY
aurora_DestroySRV(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADERRESOURCEVIEW hSRV)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_srv *srv = (struct aurora_srv *)hSRV.pDrvPrivate;
   if (!dev || !srv)
      return;
   aurora_lock(dev);
   for (unsigned st = 0; st < AURORA_STAGE_COUNT; ++st) {
      for (UINT i = 0; i < AURORA_MAX_SRVS; ++i) {
         if (dev->srvs[st][i] == srv) {
            dev->srvs[st][i] = NULL;
            dev->srv_dirty[st] = true;
         }
      }
   }
   if (srv->view && dev->pctx) {
      pipe_sampler_view_reference(&srv->view, NULL);
   }
   srv->res = NULL;
   aurora_unlock(dev);
}

struct pipe_sampler_view *
aurora_srv_get_view(struct aurora_device *dev, struct aurora_srv *srv)
{
   if (!dev || !dev->pctx || !srv || !srv->res || !srv->res->prsrc)
      return NULL;
   if (srv->res_gen != srv->res->gen || !srv->view) {
      pipe_sampler_view_reference(&srv->view, NULL);
      srv->view = dev->pctx->create_sampler_view(dev->pctx, srv->res->prsrc,
                                                 &srv->templ);
      srv->res_gen = srv->res->gen;
   }
   return srv->view;
}

/* =========================================================================
 * Unordered Access Views (11 & WDDM 2.0)
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateUAVSize(D3D10DDI_HDEVICE d, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *p)
{ (void)d; (void)p; return sizeof(struct aurora_uav); }

SIZE_T APIENTRY
aurora_CalcPrivateUAVSize_WDDM2_0(D3D10DDI_HDEVICE d, const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *p)
{ (void)d; (void)p; return sizeof(struct aurora_uav); }

static void
create_uav_common(struct aurora_device *dev, struct aurora_uav *uav,
                  struct aurora_resource *res, DXGI_FORMAT fmt,
                  D3D10DDIRESOURCE_TYPE dim,
                  UINT first_elem, UINT num_elems, UINT buf_flags,
                  UINT mip, UINT first_layer, UINT array_size)
{
   memset(uav, 0, sizeof(*uav));
   if (!res || !res->prsrc) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }
   uav->res = res;
   uav->res_gen = res->gen;
   uav->dxgi_format = fmt;
   uav->dim = dim;

   if (dim == D3D10DDIRESOURCE_BUFFER) {
      uav->is_buffer = true;
      if (buf_flags & D3D11_DDI_BUFFER_UAV_FLAG_RAW) {
         uav->is_raw_or_struct = true;
         uav->buf_offset = first_elem * 4u;
         uav->buf_size = num_elems * 4u;
         return;
      }
      if (res->structure_stride > 0u && fmt == DXGI_FORMAT_UNKNOWN) {
         uav->is_raw_or_struct = true;
         uav->buf_offset = first_elem * res->structure_stride;
         uav->buf_size = num_elems * res->structure_stride;
         return;
      }
      enum pipe_format pf = aurora_dxgi_to_pipe_format(fmt);
      unsigned elem_sz = util_format_get_blocksize(pf);
      uav->img_templ.resource = res->prsrc;
      uav->img_templ.format = pf;
      uav->img_templ.access = PIPE_IMAGE_ACCESS_READ_WRITE;
      uav->img_templ.shader_access = PIPE_IMAGE_ACCESS_READ_WRITE;
      uav->img_templ.u.buf.offset = first_elem * elem_sz;
      uav->img_templ.u.buf.size = num_elems * elem_sz;
      return;
   }

   enum pipe_format pf = aurora_dxgi_to_pipe_format(fmt);
   if (pf == PIPE_FORMAT_NONE)
      pf = res->prsrc->format;
   uav->img_templ.resource = res->prsrc;
   uav->img_templ.format = pf;
   uav->img_templ.access = PIPE_IMAGE_ACCESS_READ_WRITE;
   uav->img_templ.shader_access = PIPE_IMAGE_ACCESS_READ_WRITE;
   uav->img_templ.u.tex.level = mip;
   uav->img_templ.u.tex.first_layer = first_layer;
   UINT max_lyr = (dim == D3D10DDIRESOURCE_TEXTURE3D)
                     ? (u_minify(res->depth, mip) - 1u)
                     : (MAX2(res->array_size, 1u) - 1u);
   uav->img_templ.u.tex.last_layer =
      (array_size == (UINT)-1 || array_size == 0u)
         ? max_lyr
         : MIN2(first_layer + array_size - 1u, max_lyr);
}

void APIENTRY
aurora_CreateUAV(D3D10DDI_HDEVICE hDevice,
                 const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *pArgs,
                 D3D11DDI_HUNORDEREDACCESSVIEW hUAV,
                 D3D11DDI_HRTUNORDEREDACCESSVIEW hRTUAV)
{
   (void)hRTUAV;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_uav *uav = (struct aurora_uav *)hUAV.pDrvPrivate;
   if (!dev || !pArgs || !uav)
      return;
   struct aurora_resource *res =
      (struct aurora_resource *)pArgs->hDrvResource.pDrvPrivate;
   UINT fe = 0, ne = 0, bf = 0, mip = 0, fl = 0, as = 1;
   switch (pArgs->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      fe = pArgs->Buffer.FirstElement;
      ne = pArgs->Buffer.NumElements;
      bf = pArgs->Buffer.Flags;
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      mip = pArgs->Tex1D.MipSlice;
      fl = pArgs->Tex1D.FirstArraySlice;
      as = pArgs->Tex1D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      mip = pArgs->Tex2D.MipSlice;
      fl = pArgs->Tex2D.FirstArraySlice;
      as = pArgs->Tex2D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      mip = pArgs->Tex3D.MipSlice;
      fl = pArgs->Tex3D.FirstW;
      as = pArgs->Tex3D.WSize;
      break;
   default:
      break;
   }
   create_uav_common(dev, uav, res, pArgs->Format, pArgs->ResourceDimension,
                     fe, ne, bf, mip, fl, as);
}

void APIENTRY
aurora_CreateUAV_WDDM2_0(D3D10DDI_HDEVICE hDevice,
                         const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *pArgs,
                         D3D11DDI_HUNORDEREDACCESSVIEW hUAV,
                         D3D11DDI_HRTUNORDEREDACCESSVIEW hRTUAV)
{
   (void)hRTUAV;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_uav *uav = (struct aurora_uav *)hUAV.pDrvPrivate;
   if (!dev || !pArgs || !uav)
      return;
   struct aurora_resource *res =
      (struct aurora_resource *)pArgs->hDrvResource.pDrvPrivate;
   UINT fe = 0, ne = 0, bf = 0, mip = 0, fl = 0, as = 1;
   switch (pArgs->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      fe = pArgs->Buffer.FirstElement;
      ne = pArgs->Buffer.NumElements;
      bf = pArgs->Buffer.Flags;
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      mip = pArgs->Tex1D.MipSlice;
      fl = pArgs->Tex1D.FirstArraySlice;
      as = pArgs->Tex1D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      mip = pArgs->Tex2D.MipSlice;
      fl = pArgs->Tex2D.FirstArraySlice;
      as = pArgs->Tex2D.ArraySize;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      mip = pArgs->Tex3D.MipSlice;
      fl = pArgs->Tex3D.FirstW;
      as = pArgs->Tex3D.WSize;
      break;
   default:
      break;
   }
   create_uav_common(dev, uav, res, pArgs->Format, pArgs->ResourceDimension,
                     fe, ne, bf, mip, fl, as);
}

void APIENTRY
aurora_DestroyUAV(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hUAV)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_uav *uav = (struct aurora_uav *)hUAV.pDrvPrivate;
   if (!dev || !uav)
      return;
   aurora_lock(dev);
   for (UINT i = 0; i < AURORA_MAX_UAVS; ++i) {
      if (dev->cs_uavs[i] == uav) {
         dev->cs_uavs[i] = NULL;
         dev->uav_dirty[AURORA_STAGE_CS] = true;
      }
      if (dev->om_uavs[i] == uav) {
         dev->om_uavs[i] = NULL;
         dev->uav_dirty[AURORA_STAGE_PS] = true;
      }
   }
   uav->res = NULL;
   aurora_unlock(dev);
}

/* =========================================================================
 * Clears, Discard, GenerateMips
 * ========================================================================= */
void APIENTRY
aurora_ClearRenderTargetView(D3D10DDI_HDEVICE hDevice,
                             D3D10DDI_HRENDERTARGETVIEW hRTV, FLOAT Color[4])
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rtv *rtv = (struct aurora_rtv *)hRTV.pDrvPrivate;
   if (!dev || !dev->pctx || !rtv || !Color)
      return;

   aurora_lock(dev);
   struct pipe_surface surf;
   if (aurora_rtv_fill_surface(rtv, &surf)) {
      union pipe_color_union c;
      memcpy(c.f, Color, sizeof(float) * 4u);
      UINT w = u_minify(surf.texture->width0, surf.level);
      UINT h = u_minify(surf.texture->height0, surf.level);
      dev->pctx->clear_render_target(dev->pctx, &surf, &c, 0, 0, w, h, false);
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_ClearDepthStencilView(D3D10DDI_HDEVICE hDevice,
                             D3D10DDI_HDEPTHSTENCILVIEW hDSV,
                             UINT Flags, FLOAT Depth, UINT8 Stencil)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_dsv *dsv = (struct aurora_dsv *)hDSV.pDrvPrivate;
   if (!dev || !dev->pctx || !dsv)
      return;

   unsigned clear_flags = 0u;
   if (Flags & D3D10_DDI_CLEAR_DEPTH)
      clear_flags |= PIPE_CLEAR_DEPTH;
   if (Flags & D3D10_DDI_CLEAR_STENCIL)
      clear_flags |= PIPE_CLEAR_STENCIL;
   if (!clear_flags)
      return;

   aurora_lock(dev);
   struct pipe_surface surf;
   if (aurora_dsv_fill_surface(dsv, &surf)) {
      UINT w = u_minify(surf.texture->width0, surf.level);
      UINT h = u_minify(surf.texture->height0, surf.level);
      dev->pctx->clear_depth_stencil(dev->pctx, &surf, clear_flags,
                                     (double)Depth, Stencil, 0, 0, w, h, false);
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_ClearView(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE ViewType,
                 VOID *hView, const FLOAT Color[4],
                 const D3D10_DDI_RECT *pRects, UINT NumRects)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx || !hView || !Color)
      return;

   aurora_lock(dev);
   if (ViewType == D3D10DDI_HT_RENDERTARGETVIEW) {
      struct aurora_rtv *rtv = (struct aurora_rtv *)hView;
      struct pipe_surface surf;
      if (aurora_rtv_fill_surface(rtv, &surf)) {
         union pipe_color_union c;
         memcpy(c.f, Color, sizeof(float) * 4u);
         UINT sw = u_minify(surf.texture->width0, surf.level);
         UINT sh = u_minify(surf.texture->height0, surf.level);
         if (!pRects || NumRects == 0u) {
            dev->pctx->clear_render_target(dev->pctx, &surf, &c, 0, 0, sw, sh, false);
         } else {
            for (UINT i = 0; i < NumRects; ++i) {
               LONG l = CLAMP(pRects[i].left, 0, (LONG)sw);
               LONG t = CLAMP(pRects[i].top, 0, (LONG)sh);
               LONG r = CLAMP(pRects[i].right, 0, (LONG)sw);
               LONG b = CLAMP(pRects[i].bottom, 0, (LONG)sh);
               if (r > l && b > t) {
                  dev->pctx->clear_render_target(
                     dev->pctx, &surf, &c, (unsigned)l, (unsigned)t,
                     (unsigned)(r - l), (unsigned)(b - t), false);
               }
            }
         }
      }
   } else if (ViewType == D3D10DDI_HT_DEPTHSTENCILVIEW) {
      struct aurora_dsv *dsv = (struct aurora_dsv *)hView;
      struct pipe_surface surf;
      if (aurora_dsv_fill_surface(dsv, &surf)) {
         UINT sw = u_minify(surf.texture->width0, surf.level);
         UINT sh = u_minify(surf.texture->height0, surf.level);
         if (!pRects || NumRects == 0u) {
            dev->pctx->clear_depth_stencil(dev->pctx, &surf, PIPE_CLEAR_DEPTH,
                                           (double)Color[0], 0, 0, 0, sw, sh, false);
         } else {
            for (UINT i = 0; i < NumRects; ++i) {
               LONG l = CLAMP(pRects[i].left, 0, (LONG)sw);
               LONG t = CLAMP(pRects[i].top, 0, (LONG)sh);
               LONG r = CLAMP(pRects[i].right, 0, (LONG)sw);
               LONG b = CLAMP(pRects[i].bottom, 0, (LONG)sh);
               if (r > l && b > t) {
                  dev->pctx->clear_depth_stencil(
                     dev->pctx, &surf, PIPE_CLEAR_DEPTH, (double)Color[0], 0,
                     (unsigned)l, (unsigned)t, (unsigned)(r - l), (unsigned)(b - t), false);
               }
            }
         }
      }
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_ClearUAVUint(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hUAV,
                    const UINT Values[4])
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_uav *uav = (struct aurora_uav *)hUAV.pDrvPrivate;
   if (!dev || !dev->pctx || !uav || !uav->res || !uav->res->prsrc || !Values)
      return;

   aurora_lock(dev);
   if (uav->is_buffer) {
      UINT off = uav->is_raw_or_struct ? uav->buf_offset : uav->img_templ.u.buf.offset;
      UINT sz = uav->is_raw_or_struct ? uav->buf_size : uav->img_templ.u.buf.size;
      if (dev->pctx->clear_buffer && sz > 0u) {
         dev->pctx->clear_buffer(dev->pctx, uav->res->prsrc, off, sz,
                                 &Values[0], sizeof(uint32_t));
      }
   } else if (dev->pctx->clear_texture) {
      UINT lvl = uav->img_templ.u.tex.level;
      struct pipe_box box = {
         .x = 0, .y = 0, .z = (int)uav->img_templ.u.tex.first_layer,
         .width = (int)u_minify(uav->res->width, lvl),
         .height = (int)u_minify(uav->res->height, lvl),
         .depth = (int)(uav->img_templ.u.tex.last_layer -
                        uav->img_templ.u.tex.first_layer + 1u),
      };
      dev->pctx->clear_texture(dev->pctx, uav->res->prsrc, lvl, &box, Values);
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_ClearUAVFloat(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hUAV,
                     const FLOAT Values[4])
{
   UINT u[4];
   if (!Values)
      return;
   memcpy(u, Values, sizeof(u));
   aurora_ClearUAVUint(hDevice, hUAV, u);
}

void APIENTRY
aurora_CopyStructureCount(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst,
                          UINT DstOffset, D3D11DDI_HUNORDEREDACCESSVIEW hUAV)
{ (void)hDevice; (void)hDst; (void)DstOffset; (void)hUAV; }

void APIENTRY
aurora_Discard(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE ResourceType,
               VOID *hResourceOrView, const D3D10_DDI_RECT *pRects, UINT NumRects)
{ (void)hDevice; (void)ResourceType; (void)hResourceOrView; (void)pRects; (void)NumRects; }

void APIENTRY
aurora_GenerateMips(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADERRESOURCEVIEW hSRV)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_srv *srv = (struct aurora_srv *)hSRV.pDrvPrivate;
   if (!dev || !dev->pctx || !srv || !srv->res || !srv->res->prsrc)
      return;

   aurora_lock(dev);
   struct pipe_resource *res = srv->res->prsrc;
   unsigned first_lvl = srv->templ.u.tex.first_level;
   unsigned last_lvl = srv->templ.u.tex.last_level;
   unsigned first_lyr = srv->templ.u.tex.first_layer;
   unsigned last_lyr = srv->templ.u.tex.last_layer;

   for (unsigned lvl = first_lvl; lvl < last_lvl; ++lvl) {
      struct pipe_blit_info info;
      memset(&info, 0, sizeof(info));
      info.src.resource = res;
      info.src.level = lvl;
      info.src.format = srv->templ.format;
      info.src.box.z = (int)first_lyr;
      info.src.box.width = (int)u_minify(res->width0, lvl);
      info.src.box.height = (int)u_minify(res->height0, lvl);
      info.src.box.depth = (int)(last_lyr - first_lyr + 1u);

      info.dst.resource = res;
      info.dst.level = lvl + 1u;
      info.dst.format = srv->templ.format;
      info.dst.box.z = (int)first_lyr;
      info.dst.box.width = (int)u_minify(res->width0, lvl + 1u);
      info.dst.box.height = (int)u_minify(res->height0, lvl + 1u);
      info.dst.box.depth = (int)(last_lyr - first_lyr + 1u);

      info.mask = PIPE_MASK_RGBA;
      info.filter = PIPE_TEX_FILTER_LINEAR;
      dev->pctx->blit(dev->pctx, &info);
   }
   aurora_unlock(dev);
}

/* =========================================================================
 * Shaders: SM4.0/SM4.1/SM5.0 DDI Token Stream -> NIR -> Gallium Asahi
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateShaderSize(D3D10DDI_HDEVICE d, const UINT *code, const VOID *sigs)
{ (void)d; (void)code; (void)sigs; return sizeof(struct aurora_shader); }

SIZE_T APIENTRY
aurora_CalcPrivateTessShaderSize(D3D10DDI_HDEVICE d, const UINT *code, const VOID *sigs)
{ (void)d; (void)code; (void)sigs; return sizeof(struct aurora_shader); }

SIZE_T APIENTRY
aurora_CalcPrivateGSWithSOSize(D3D10DDI_HDEVICE d, const VOID *p)
{ (void)d; (void)p; return sizeof(struct aurora_shader); }

static void
extract_signatures(const struct aurora_device *dev, const VOID *pSigs,
                   struct aurora_sm5_sig_elem *in_elems, unsigned *num_in,
                   struct aurora_sm5_sig_elem *out_elems, unsigned *num_out)
{
   *num_in = 0;
   *num_out = 0;
   if (!pSigs)
      return;

   if (dev->interface_version >= D3D11_1_DDI_INTERFACE_VERSION) {
      const D3D11_1DDIARG_STAGE_IO_SIGNATURES *s =
         (const D3D11_1DDIARG_STAGE_IO_SIGNATURES *)pSigs;
      UINT ni = MIN2(s->NumInputSignatureEntries, 32u);
      for (UINT i = 0; i < ni; ++i) {
         in_elems[i].reg = s->pInputSignature[i].Register;
         in_elems[i].sysval = (uint32_t)s->pInputSignature[i].SystemValue;
         in_elems[i].mask = s->pInputSignature[i].Mask;
      }
      *num_in = ni;
      UINT no = MIN2(s->NumOutputSignatureEntries, 32u);
      for (UINT i = 0; i < no; ++i) {
         out_elems[i].reg = s->pOutputSignature[i].Register;
         out_elems[i].sysval = (uint32_t)s->pOutputSignature[i].SystemValue;
         out_elems[i].mask = s->pOutputSignature[i].Mask;
      }
      *num_out = no;
   } else {
      const D3D10DDIARG_STAGE_IO_SIGNATURES *s =
         (const D3D10DDIARG_STAGE_IO_SIGNATURES *)pSigs;
      UINT ni = MIN2(s->NumInputSignatureEntries, 32u);
      for (UINT i = 0; i < ni; ++i) {
         in_elems[i].reg = s->pInputSignature[i].Register;
         in_elems[i].sysval = (uint32_t)s->pInputSignature[i].SystemValue;
         in_elems[i].mask = s->pInputSignature[i].Mask;
      }
      *num_in = ni;
      UINT no = MIN2(s->NumOutputSignatureEntries, 32u);
      for (UINT i = 0; i < no; ++i) {
         out_elems[i].reg = s->pOutputSignature[i].Register;
         out_elems[i].sysval = (uint32_t)s->pOutputSignature[i].SystemValue;
         out_elems[i].mask = s->pOutputSignature[i].Mask;
      }
      *num_out = no;
   }
}

static void
create_shader_common(struct aurora_device *dev, mesa_shader_stage stage,
                     const UINT *pCode, struct aurora_shader *sh,
                     const VOID *pSigs)
{
   memset(sh, 0, sizeof(*sh));
   sh->stage = stage;
   if (!dev || !dev->screen || !dev->pctx || !pCode) {
      aurora_set_error(dev, E_INVALIDARG);
      return;
   }

   struct aurora_sm5_sig_elem in_sig[32], out_sig[32];
   unsigned num_in = 0, num_out = 0;
   extract_signatures(dev, pSigs, in_sig, &num_in, out_sig, &num_out);

   const nir_shader_compiler_options *opts =
      dev->screen->nir_options[stage];
   if (!opts) {
      aurora_log("CreateShader(stage=%d): missing nir_options", (int)stage);
      aurora_set_error(dev, E_FAIL);
      return;
   }

   /* DDI shader token streams start with [VerTok, LenTok, ...]. */
   size_t num_dwords = pCode[1];
   nir_shader *nir = aurora_sm5_to_nir(pCode, num_dwords, in_sig, num_in,
                                       out_sig, num_out, opts, &sh->info);
   if (!nir) {
      aurora_log("CreateShader(stage=%d): SM5->NIR failed ver=0x%08x len=%u",
                 (int)stage, pCode[0], pCode[1]);
      aurora_set_error(dev, E_FAIL);
      return;
   }

   aurora_lock(dev);
   if (stage == MESA_SHADER_COMPUTE) {
      struct pipe_compute_state cso;
      memset(&cso, 0, sizeof(cso));
      cso.ir_type = PIPE_SHADER_IR_NIR;
      cso.prog = nir;
      cso.static_shared_mem = nir->info.shared_size;
      sh->cso = dev->pctx->create_compute_state(dev->pctx, &cso);
   } else {
      struct pipe_shader_state cso;
      memset(&cso, 0, sizeof(cso));
      cso.type = PIPE_SHADER_IR_NIR;
      cso.ir.nir = nir;
      if (stage == MESA_SHADER_VERTEX)
         sh->cso = dev->pctx->create_vs_state(dev->pctx, &cso);
      else if (stage == MESA_SHADER_FRAGMENT)
         sh->cso = dev->pctx->create_fs_state(dev->pctx, &cso);
   }
   aurora_unlock(dev);

   if (!sh->cso) {
      aurora_log("CreateShader(stage=%d): Gallium Asahi create state FAILED", (int)stage);
      aurora_set_error(dev, E_FAIL);
   }
}

void APIENTRY
aurora_CreateVertexShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                          D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader,
                          const VOID *pSigs)
{
   (void)hRTShader;
   create_shader_common((struct aurora_device *)hDevice.pDrvPrivate,
                        MESA_SHADER_VERTEX, pCode,
                        (struct aurora_shader *)hShader.pDrvPrivate, pSigs);
}

void APIENTRY
aurora_CreatePixelShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                         D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader,
                         const VOID *pSigs)
{
   (void)hRTShader;
   create_shader_common((struct aurora_device *)hDevice.pDrvPrivate,
                        MESA_SHADER_FRAGMENT, pCode,
                        (struct aurora_shader *)hShader.pDrvPrivate, pSigs);
}

void APIENTRY
aurora_CreateComputeShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                           D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader)
{
   (void)hRTShader;
   create_shader_common((struct aurora_device *)hDevice.pDrvPrivate,
                        MESA_SHADER_COMPUTE, pCode,
                        (struct aurora_shader *)hShader.pDrvPrivate, NULL);
}

void APIENTRY
aurora_CreateGeometryShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                            D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRT,
                            const VOID *pSigs)
{
   (void)pCode; (void)hRT; (void)pSigs;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (sh) { memset(sh, 0, sizeof(*sh)); sh->stage = MESA_SHADER_GEOMETRY; }
   aurora_set_error((struct aurora_device *)hDevice.pDrvPrivate, E_NOTIMPL);
}

void APIENTRY
aurora_CreateGSWithSO(D3D10DDI_HDEVICE hDevice,
                      const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pArgs,
                      D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRT,
                      const VOID *pSigs)
{
   (void)pArgs; (void)hRT; (void)pSigs;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (sh) { memset(sh, 0, sizeof(*sh)); sh->stage = MESA_SHADER_GEOMETRY; }
   aurora_set_error((struct aurora_device *)hDevice.pDrvPrivate, E_NOTIMPL);
}

void APIENTRY
aurora_CreateHullShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                        D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRT,
                        const VOID *pSigs)
{
   (void)pCode; (void)hRT; (void)pSigs;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (sh) { memset(sh, 0, sizeof(*sh)); sh->stage = MESA_SHADER_TESS_CTRL; }
   aurora_set_error((struct aurora_device *)hDevice.pDrvPrivate, E_NOTIMPL);
}

void APIENTRY
aurora_CreateDomainShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                          D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRT,
                          const VOID *pSigs)
{
   (void)pCode; (void)hRT; (void)pSigs;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (sh) { memset(sh, 0, sizeof(*sh)); sh->stage = MESA_SHADER_TESS_EVAL; }
   aurora_set_error((struct aurora_device *)hDevice.pDrvPrivate, E_NOTIMPL);
}

void APIENTRY
aurora_DestroyShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (!dev || !dev->pctx || !sh)
      return;

   aurora_lock(dev);
   if (sh->cso) {
      if (sh->stage == MESA_SHADER_VERTEX) {
         if (dev->vs == sh) {
            dev->pctx->bind_vs_state(dev->pctx, NULL);
            dev->vs = NULL;
         }
         dev->pctx->delete_vs_state(dev->pctx, sh->cso);
      } else if (sh->stage == MESA_SHADER_FRAGMENT) {
         if (dev->ps == sh) {
            dev->pctx->bind_fs_state(dev->pctx, NULL);
            dev->ps = NULL;
         }
         dev->pctx->delete_fs_state(dev->pctx, sh->cso);
      } else if (sh->stage == MESA_SHADER_COMPUTE) {
         if (dev->cs == sh) {
            dev->pctx->bind_compute_state(dev->pctx, NULL);
            dev->cs = NULL;
         }
         dev->pctx->delete_compute_state(dev->pctx, sh->cso);
      }
      sh->cso = NULL;
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_VsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   dev->vs = sh;
   dev->pctx->bind_vs_state(dev->pctx, sh ? sh->cso : NULL);
   aurora_unlock(dev);
}

void APIENTRY
aurora_PsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   dev->ps = sh;
   dev->pctx->bind_fs_state(dev->pctx, sh ? sh->cso : NULL);
   aurora_unlock(dev);
}

void APIENTRY
aurora_CsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_shader *sh = (struct aurora_shader *)hShader.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   dev->cs = sh;
   dev->pctx->bind_compute_state(dev->pctx, sh ? sh->cso : NULL);
   aurora_unlock(dev);
}

void APIENTRY aurora_GsSetShader(D3D10DDI_HDEVICE d, D3D10DDI_HSHADER s) { (void)d; (void)s; }
void APIENTRY aurora_HsSetShader(D3D10DDI_HDEVICE d, D3D10DDI_HSHADER s) { (void)d; (void)s; }
void APIENTRY aurora_DsSetShader(D3D10DDI_HDEVICE d, D3D10DDI_HSHADER s) { (void)d; (void)s; }

HRESULT APIENTRY
aurora_RetrieveShaderComment(D3D10DDI_HDEVICE d, D3D10DDI_HSHADER s,
                             WCHAR *buf, SIZE_T *sz)
{
   (void)d; (void)s; (void)buf;
   if (sz) *sz = 0;
   return S_OK;
}

void APIENTRY
aurora_AssignDebugBinary(D3D10DDI_HDEVICE d, D3D10DDI_HSHADER s,
                         UINT sz, const VOID *data)
{ (void)d; (void)s; (void)sz; (void)data; }

/* =========================================================================
 * Queries, Predication, Flush, Counters, Direct Flip Check
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateQuerySize(D3D10DDI_HDEVICE d, const D3D10DDIARG_CREATEQUERY *p)
{ (void)d; (void)p; return sizeof(struct aurora_query); }

SIZE_T APIENTRY
aurora_CalcPrivateQuerySize_WDDM2_0(D3D10DDI_HDEVICE d, const D3DWDDM2_0DDIARG_CREATEQUERY *p)
{ (void)d; (void)p; return sizeof(struct aurora_query); }

static void
create_query_common(struct aurora_device *dev, struct aurora_query *q,
                    D3D10DDI_QUERY ddi_type, UINT misc_flags)
{
   memset(q, 0, sizeof(*q));
   q->ddi_type = ddi_type;
   q->misc_flags = misc_flags;
   if (!dev || !dev->pctx)
      return;

   aurora_lock(dev);
   switch (ddi_type) {
   case D3D10DDI_QUERY_OCCLUSION:
      q->pq = dev->pctx->create_query(dev->pctx, PIPE_QUERY_OCCLUSION_COUNTER, 0);
      break;
   case D3D10DDI_QUERY_OCCLUSIONPREDICATE:
      q->pq = dev->pctx->create_query(dev->pctx, PIPE_QUERY_OCCLUSION_PREDICATE, 0);
      break;
   case D3D10DDI_QUERY_TIMESTAMP:
      q->pq = dev->pctx->create_query(dev->pctx, PIPE_QUERY_TIMESTAMP, 0);
      break;
   case D3D10DDI_QUERY_TIMESTAMPDISJOINT:
      q->pq = dev->pctx->create_query(dev->pctx, PIPE_QUERY_TIMESTAMP_DISJOINT, 0);
      break;
   case D3D10DDI_QUERY_PIPELINESTATS:
   case D3D11DDI_QUERY_PIPELINESTATS:
      q->pq = dev->pctx->create_query(dev->pctx, PIPE_QUERY_PIPELINE_STATISTICS, 0);
      break;
   default:
      break;
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_CreateQuery(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATEQUERY *pArgs,
                   D3D10DDI_HQUERY hQuery, D3D10DDI_HRTQUERY hRTQuery)
{
   (void)hRTQuery;
   if (!pArgs)
      return;
   create_query_common((struct aurora_device *)hDevice.pDrvPrivate,
                       (struct aurora_query *)hQuery.pDrvPrivate,
                       pArgs->Query, pArgs->MiscFlags);
}

void APIENTRY
aurora_CreateQuery_WDDM2_0(D3D10DDI_HDEVICE hDevice,
                           const D3DWDDM2_0DDIARG_CREATEQUERY *pArgs,
                           D3D10DDI_HQUERY hQuery, D3D10DDI_HRTQUERY hRTQuery)
{
   (void)hRTQuery;
   if (!pArgs)
      return;
   create_query_common((struct aurora_device *)hDevice.pDrvPrivate,
                       (struct aurora_query *)hQuery.pDrvPrivate,
                       pArgs->Query, pArgs->MiscFlags);
}

void APIENTRY
aurora_DestroyQuery(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hQuery)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_query *q = (struct aurora_query *)hQuery.pDrvPrivate;
   if (!dev || !q)
      return;
   aurora_lock(dev);
   if (q->pq && dev->pctx) {
      dev->pctx->destroy_query(dev->pctx, q->pq);
      q->pq = NULL;
   }
   if (q->event_fence && dev->screen)
      dev->screen->fence_reference(dev->screen, &q->event_fence, NULL);
   aurora_unlock(dev);
}

void APIENTRY
aurora_QueryBegin(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hQuery)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_query *q = (struct aurora_query *)hQuery.pDrvPrivate;
   if (!dev || !dev->pctx || !q)
      return;
   aurora_lock(dev);
   q->begun = true;
   q->ended = false;
   if (q->pq)
      dev->pctx->begin_query(dev->pctx, q->pq);
   aurora_unlock(dev);
}

void APIENTRY
aurora_QueryEnd(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hQuery)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_query *q = (struct aurora_query *)hQuery.pDrvPrivate;
   if (!dev || !dev->pctx || !q)
      return;
   aurora_lock(dev);
   q->ended = true;
   if (q->ddi_type == D3D10DDI_QUERY_EVENT) {
      dev->screen->fence_reference(dev->screen, &q->event_fence, NULL);
      dev->pctx->flush(dev->pctx, &q->event_fence, 0);
   } else if (q->pq) {
      dev->pctx->end_query(dev->pctx, q->pq);
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_QueryGetData(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hQuery,
                    VOID *pData, UINT DataSize, UINT Flags)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_query *q = (struct aurora_query *)hQuery.pDrvPrivate;
   if (!dev || !dev->pctx || !dev->screen || !q)
      return;

   bool dont_flush = (Flags & D3D10_DDI_GET_DATA_DO_NOT_FLUSH) != 0u;
   aurora_lock(dev);

   if (q->ddi_type == D3D10DDI_QUERY_EVENT) {
      bool done = true;
      if (q->event_fence) {
         done = dev->screen->fence_finish(dev->screen, dev->pctx,
                                          q->event_fence, 0);
         if (done)
            dev->screen->fence_reference(dev->screen, &q->event_fence, NULL);
      }
      aurora_unlock(dev);
      if (!done) {
         aurora_set_error(dev, DXGI_DDI_ERR_WASSTILLDRAWING);
         return;
      }
      if (pData && DataSize >= sizeof(BOOL))
         *(BOOL *)pData = TRUE;
      return;
   }

   if (q->pq) {
      union pipe_query_result res;
      memset(&res, 0, sizeof(res));
      if (!dont_flush)
         dev->pctx->flush(dev->pctx, NULL, 0);
      bool ready = dev->pctx->get_query_result(dev->pctx, q->pq, false, &res);
      aurora_unlock(dev);
      if (!ready) {
         aurora_set_error(dev, DXGI_DDI_ERR_WASSTILLDRAWING);
         return;
      }
      if (!pData)
         return;
      switch (q->ddi_type) {
      case D3D10DDI_QUERY_OCCLUSION:
      case D3D10DDI_QUERY_TIMESTAMP:
         if (DataSize >= sizeof(UINT64))
            *(UINT64 *)pData = res.u64;
         break;
      case D3D10DDI_QUERY_OCCLUSIONPREDICATE:
         if (DataSize >= sizeof(BOOL))
            *(BOOL *)pData = res.b ? TRUE : FALSE;
         break;
      case D3D10DDI_QUERY_TIMESTAMPDISJOINT:
         if (DataSize >= sizeof(D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT)) {
            D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT *td =
               (D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT *)pData;
            td->Frequency = res.timestamp_disjoint.frequency
                               ? res.timestamp_disjoint.frequency : 1000000000ULL;
            td->Disjoint = res.timestamp_disjoint.disjoint ? TRUE : FALSE;
         }
         break;
      default:
         memset(pData, 0, DataSize);
         break;
      }
      return;
   }

   aurora_unlock(dev);
   if (pData && DataSize)
      memset(pData, 0, DataSize);
}

void APIENTRY
aurora_SetPredication(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hPred,
                      BOOL PredicateValue)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_query *q = (struct aurora_query *)hPred.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   dev->pctx->render_condition(dev->pctx, q ? q->pq : NULL,
                               PredicateValue != FALSE,
                               PIPE_RENDER_COND_NO_WAIT);
   aurora_unlock(dev);
}

void APIENTRY
aurora_Flush(D3D10DDI_HDEVICE hDevice)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   dev->pctx->flush(dev->pctx, NULL, 0);
   aurora_unlock(dev);
}

BOOL APIENTRY
aurora_Flush11_1(D3D10DDI_HDEVICE hDevice, UINT FlushFlags)
{
   (void)FlushFlags;
   aurora_Flush(hDevice);
   return TRUE;
}

BOOL APIENTRY
aurora_Flush_WDDM2_0(D3D10DDI_HDEVICE hDevice, UINT FlushFlags, UINT ContextType)
{
   (void)FlushFlags; (void)ContextType;
   aurora_Flush(hDevice);
   return TRUE;
}

void APIENTRY
aurora_CheckCounterInfo(D3D10DDI_HDEVICE hDevice,
                        D3D10DDI_COUNTER_INFO *pCounterInfo)
{
   (void)hDevice;
   if (pCounterInfo)
      memset(pCounterInfo, 0, sizeof(*pCounterInfo));
}

void APIENTRY
aurora_CheckCounter(D3D10DDI_HDEVICE hDevice, D3D10DDI_QUERY Query,
                    D3D10DDI_COUNTER_TYPE *pCounterType,
                    UINT *pActiveCounters, LPSTR pName, UINT *pNameLength,
                    LPSTR pUnits, UINT *pUnitsLength,
                    LPSTR pDescription, UINT *pDescriptionLength)
{
   (void)Query; (void)pCounterType; (void)pActiveCounters;
   (void)pName; (void)pNameLength; (void)pUnits; (void)pUnitsLength;
   (void)pDescription; (void)pDescriptionLength;
   aurora_set_error((struct aurora_device *)hDevice.pDrvPrivate,
                    DXGI_ERROR_UNSUPPORTED);
}

void APIENTRY
aurora_CheckDirectFlipSupport(D3D10DDI_HDEVICE hDevice,
                              D3D10DDI_HRESOURCE hRes1,
                              D3D10DDI_HRESOURCE hRes2,
                              UINT CheckDirectFlipFlags,
                              BOOL *pSupported)
{
   (void)hDevice; (void)CheckDirectFlipFlags;
   if (!pSupported)
      return;
   struct aurora_resource *r1 = (struct aurora_resource *)hRes1.pDrvPrivate;
   struct aurora_resource *r2 = (struct aurora_resource *)hRes2.pDrvPrivate;
   *pSupported = (r1 && r2 && r1->is_direct_scanout && r2->is_direct_scanout &&
                  r1->width == r2->width && r1->height == r2->height &&
                  r1->dxgi_format == r2->dxgi_format)
                    ? TRUE : FALSE;
}
