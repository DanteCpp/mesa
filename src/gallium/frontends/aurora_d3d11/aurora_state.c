/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Native D3D11 Pipeline State (Blend, Depth/Stencil, Rasterizer, Sampler,
 * InputLayout, Viewports, Scissors, Resource Bindings, Draw & Dispatch)
 * directly driving Gallium Asahi on Apple Silicon M5 (G17G).
 */
#include "aurora_d3d11.h"
#include "aurora_format.h"

#include <string.h>

/* =========================================================================
 * Enum Conversions (D3D11 DDI -> Gallium)
 * ========================================================================= */
static unsigned
d3d_blend_factor(D3D10_DDI_BLEND f)
{
   switch (f) {
   case D3D10_DDI_BLEND_ZERO:             return PIPE_BLENDFACTOR_ZERO;
   case D3D10_DDI_BLEND_ONE:              return PIPE_BLENDFACTOR_ONE;
   case D3D10_DDI_BLEND_SRC_COLOR:        return PIPE_BLENDFACTOR_SRC_COLOR;
   case D3D10_DDI_BLEND_INV_SRC_COLOR:    return PIPE_BLENDFACTOR_INV_SRC_COLOR;
   case D3D10_DDI_BLEND_SRC_ALPHA:        return PIPE_BLENDFACTOR_SRC_ALPHA;
   case D3D10_DDI_BLEND_INV_SRC_ALPHA:    return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   case D3D10_DDI_BLEND_DEST_ALPHA:       return PIPE_BLENDFACTOR_DST_ALPHA;
   case D3D10_DDI_BLEND_INV_DEST_ALPHA:   return PIPE_BLENDFACTOR_INV_DST_ALPHA;
   case D3D10_DDI_BLEND_DEST_COLOR:       return PIPE_BLENDFACTOR_DST_COLOR;
   case D3D10_DDI_BLEND_INV_DEST_COLOR:   return PIPE_BLENDFACTOR_INV_DST_COLOR;
   case D3D10_DDI_BLEND_SRC_ALPHASAT:     return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
   case D3D10_DDI_BLEND_BLEND_FACTOR:     return PIPE_BLENDFACTOR_CONST_COLOR;
   case D3D10_DDI_BLEND_INVBLEND_FACTOR:  return PIPE_BLENDFACTOR_INV_CONST_COLOR;
   case D3D10_DDI_BLEND_SRC1_COLOR:       return PIPE_BLENDFACTOR_SRC1_COLOR;
   case D3D10_DDI_BLEND_INV_SRC1_COLOR:   return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
   case D3D10_DDI_BLEND_SRC1_ALPHA:       return PIPE_BLENDFACTOR_SRC1_ALPHA;
   case D3D10_DDI_BLEND_INV_SRC1_ALPHA:   return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
   default:                               return PIPE_BLENDFACTOR_ONE;
   }
}

static unsigned
d3d_blend_alpha_factor(D3D10_DDI_BLEND f)
{
   if (f == D3D10_DDI_BLEND_BLEND_FACTOR)
      return PIPE_BLENDFACTOR_CONST_ALPHA;
   if (f == D3D10_DDI_BLEND_INVBLEND_FACTOR)
      return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
   return d3d_blend_factor(f);
}

static unsigned
d3d_blend_op(D3D10_DDI_BLEND_OP op)
{
   switch (op) {
   case D3D10_DDI_BLEND_OP_ADD:          return PIPE_BLEND_ADD;
   case D3D10_DDI_BLEND_OP_SUBTRACT:     return PIPE_BLEND_SUBTRACT;
   case D3D10_DDI_BLEND_OP_REV_SUBTRACT: return PIPE_BLEND_REVERSE_SUBTRACT;
   case D3D10_DDI_BLEND_OP_MIN:          return PIPE_BLEND_MIN;
   case D3D10_DDI_BLEND_OP_MAX:          return PIPE_BLEND_MAX;
   default:                              return PIPE_BLEND_ADD;
   }
}

static unsigned
d3d_logic_op(D3D11_1_DDI_LOGIC_OP op)
{
   switch (op) {
   case D3D11_1_DDI_LOGIC_OP_CLEAR:         return PIPE_LOGICOP_CLEAR;
   case D3D11_1_DDI_LOGIC_OP_SET:           return PIPE_LOGICOP_SET;
   case D3D11_1_DDI_LOGIC_OP_COPY:          return PIPE_LOGICOP_COPY;
   case D3D11_1_DDI_LOGIC_OP_COPY_INVERTED: return PIPE_LOGICOP_COPY_INVERTED;
   case D3D11_1_DDI_LOGIC_OP_NOOP:          return PIPE_LOGICOP_NOOP;
   case D3D11_1_DDI_LOGIC_OP_INVERT:        return PIPE_LOGICOP_INVERT;
   case D3D11_1_DDI_LOGIC_OP_AND:           return PIPE_LOGICOP_AND;
   case D3D11_1_DDI_LOGIC_OP_NAND:          return PIPE_LOGICOP_NAND;
   case D3D11_1_DDI_LOGIC_OP_OR:            return PIPE_LOGICOP_OR;
   case D3D11_1_DDI_LOGIC_OP_NOR:           return PIPE_LOGICOP_NOR;
   case D3D11_1_DDI_LOGIC_OP_XOR:           return PIPE_LOGICOP_XOR;
   case D3D11_1_DDI_LOGIC_OP_EQUIV:         return PIPE_LOGICOP_EQUIV;
   case D3D11_1_DDI_LOGIC_OP_AND_REVERSE:   return PIPE_LOGICOP_AND_REVERSE;
   case D3D11_1_DDI_LOGIC_OP_AND_INVERTED:  return PIPE_LOGICOP_AND_INVERTED;
   case D3D11_1_DDI_LOGIC_OP_OR_REVERSE:    return PIPE_LOGICOP_OR_REVERSE;
   case D3D11_1_DDI_LOGIC_OP_OR_INVERTED:   return PIPE_LOGICOP_OR_INVERTED;
   default:                                 return PIPE_LOGICOP_COPY;
   }
}

static unsigned
d3d_cmp_func(D3D10_DDI_COMPARISON_FUNC f)
{
   switch (f) {
   case D3D10_DDI_COMPARISON_NEVER:         return PIPE_FUNC_NEVER;
   case D3D10_DDI_COMPARISON_LESS:          return PIPE_FUNC_LESS;
   case D3D10_DDI_COMPARISON_EQUAL:         return PIPE_FUNC_EQUAL;
   case D3D10_DDI_COMPARISON_LESS_EQUAL:    return PIPE_FUNC_LEQUAL;
   case D3D10_DDI_COMPARISON_GREATER:       return PIPE_FUNC_GREATER;
   case D3D10_DDI_COMPARISON_NOT_EQUAL:     return PIPE_FUNC_NOTEQUAL;
   case D3D10_DDI_COMPARISON_GREATER_EQUAL: return PIPE_FUNC_GEQUAL;
   case D3D10_DDI_COMPARISON_ALWAYS:        return PIPE_FUNC_ALWAYS;
   default:                                 return PIPE_FUNC_ALWAYS;
   }
}

static unsigned
d3d_stencil_op(D3D10_DDI_STENCIL_OP op)
{
   switch (op) {
   case D3D10_DDI_STENCIL_OP_KEEP:     return PIPE_STENCIL_OP_KEEP;
   case D3D10_DDI_STENCIL_OP_ZERO:     return PIPE_STENCIL_OP_ZERO;
   case D3D10_DDI_STENCIL_OP_REPLACE:  return PIPE_STENCIL_OP_REPLACE;
   case D3D10_DDI_STENCIL_OP_INCR_SAT: return PIPE_STENCIL_OP_INCR;
   case D3D10_DDI_STENCIL_OP_DECR_SAT: return PIPE_STENCIL_OP_DECR;
   case D3D10_DDI_STENCIL_OP_INVERT:   return PIPE_STENCIL_OP_INVERT;
   case D3D10_DDI_STENCIL_OP_INCR:     return PIPE_STENCIL_OP_INCR_WRAP;
   case D3D10_DDI_STENCIL_OP_DECR:     return PIPE_STENCIL_OP_DECR_WRAP;
   default:                            return PIPE_STENCIL_OP_KEEP;
   }
}

static unsigned
d3d_tex_wrap(D3D10_DDI_TEXTURE_ADDRESS_MODE m)
{
   switch (m) {
   case D3D10_DDI_TEXTURE_ADDRESS_WRAP:       return PIPE_TEX_WRAP_REPEAT;
   case D3D10_DDI_TEXTURE_ADDRESS_MIRROR:     return PIPE_TEX_WRAP_MIRROR_REPEAT;
   case D3D10_DDI_TEXTURE_ADDRESS_CLAMP:      return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   case D3D10_DDI_TEXTURE_ADDRESS_BORDER:     return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
   case D3D10_DDI_TEXTURE_ADDRESS_MIRRORONCE: return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
   default:                                   return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   }
}

static enum mesa_prim
d3d_topology(D3D10_DDI_PRIMITIVE_TOPOLOGY t)
{
   switch (t) {
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_POINTLIST:         return MESA_PRIM_POINTS;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINELIST:          return MESA_PRIM_LINES;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINESTRIP:         return MESA_PRIM_LINE_STRIP;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST:      return MESA_PRIM_TRIANGLES;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:     return MESA_PRIM_TRIANGLE_STRIP;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:      return MESA_PRIM_LINES_ADJACENCY;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:     return MESA_PRIM_LINE_STRIP_ADJACENCY;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:  return MESA_PRIM_TRIANGLES_ADJACENCY;
   case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return MESA_PRIM_TRIANGLE_STRIP_ADJACENCY;
   default:                                             return MESA_PRIM_TRIANGLES;
   }
}

/* =========================================================================
 * Blend State (10.1 & 11.1, including Dual-Source ClearType & LogicOp)
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateBlendStateSize_10(D3D10DDI_HDEVICE d, const D3D10_1_DDI_BLEND_DESC *desc)
{ (void)d; (void)desc; return sizeof(struct aurora_blend); }

SIZE_T APIENTRY
aurora_CalcPrivateBlendStateSize_11_1(D3D10DDI_HDEVICE d, const D3D11_1_DDI_BLEND_DESC *desc)
{ (void)d; (void)desc; return sizeof(struct aurora_blend); }

void APIENTRY
aurora_CreateBlendState_11_1(D3D10DDI_HDEVICE hDevice,
                             const D3D11_1_DDI_BLEND_DESC *pDesc,
                             D3D10DDI_HBLENDSTATE hState,
                             D3D10DDI_HRTBLENDSTATE hRTState)
{
   (void)hRTState;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_blend *st = (struct aurora_blend *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !pDesc || !st)
      return;
   memset(st, 0, sizeof(*st));

   struct pipe_blend_state *b = &st->state;
   b->independent_blend_enable = pDesc->IndependentBlendEnable ? 1 : 0;
   b->alpha_to_coverage = pDesc->AlphaToCoverageEnable ? 1 : 0;
   b->max_rt = AURORA_MAX_RTVS - 1u;

   if (pDesc->RenderTarget[0].LogicOpEnable) {
      b->logicop_enable = 1;
      b->logicop_func = d3d_logic_op(pDesc->RenderTarget[0].LogicOp);
   }

   unsigned count = pDesc->IndependentBlendEnable ? AURORA_MAX_RTVS : 1u;
   for (unsigned i = 0; i < count; ++i) {
      const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC *rt = &pDesc->RenderTarget[i];
      b->rt[i].blend_enable = rt->BlendEnable ? 1 : 0;
      b->rt[i].rgb_func = d3d_blend_op(rt->BlendOp);
      b->rt[i].rgb_src_factor = d3d_blend_factor(rt->SrcBlend);
      b->rt[i].rgb_dst_factor = d3d_blend_factor(rt->DestBlend);
      b->rt[i].alpha_func = d3d_blend_op(rt->BlendOpAlpha);
      b->rt[i].alpha_src_factor = d3d_blend_alpha_factor(rt->SrcBlendAlpha);
      b->rt[i].alpha_dst_factor = d3d_blend_alpha_factor(rt->DestBlendAlpha);
      b->rt[i].colormask = rt->RenderTargetWriteMask & 0xfu;
   }

   aurora_lock(dev);
   st->cso = dev->pctx->create_blend_state(dev->pctx, b);
   aurora_unlock(dev);
}

void APIENTRY
aurora_CreateBlendState_10(D3D10DDI_HDEVICE hDevice,
                           const D3D10_1_DDI_BLEND_DESC *pDesc,
                           D3D10DDI_HBLENDSTATE hState,
                           D3D10DDI_HRTBLENDSTATE hRTState)
{
   if (!pDesc)
      return;
   D3D11_1_DDI_BLEND_DESC d11;
   memset(&d11, 0, sizeof(d11));
   d11.AlphaToCoverageEnable = pDesc->AlphaToCoverageEnable;
   d11.IndependentBlendEnable = pDesc->IndependentBlendEnable;
   for (unsigned i = 0; i < 8; ++i) {
      d11.RenderTarget[i].BlendEnable = pDesc->RenderTarget[i].BlendEnable;
      d11.RenderTarget[i].SrcBlend = pDesc->RenderTarget[i].SrcBlend;
      d11.RenderTarget[i].DestBlend = pDesc->RenderTarget[i].DestBlend;
      d11.RenderTarget[i].BlendOp = pDesc->RenderTarget[i].BlendOp;
      d11.RenderTarget[i].SrcBlendAlpha = pDesc->RenderTarget[i].SrcBlendAlpha;
      d11.RenderTarget[i].DestBlendAlpha = pDesc->RenderTarget[i].DestBlendAlpha;
      d11.RenderTarget[i].BlendOpAlpha = pDesc->RenderTarget[i].BlendOpAlpha;
      d11.RenderTarget[i].RenderTargetWriteMask =
         pDesc->RenderTarget[i].RenderTargetWriteMask;
   }
   aurora_CreateBlendState_11_1(hDevice, &d11, hState, hRTState);
}

void APIENTRY
aurora_DestroyBlendState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HBLENDSTATE hState)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_blend *st = (struct aurora_blend *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !st)
      return;
   aurora_lock(dev);
   if (dev->blend == st) {
      dev->blend = NULL;
      if (dev->default_blend_cso)
         dev->pctx->bind_blend_state(dev->pctx, dev->default_blend_cso);
   }
   if (st->cso) {
      dev->pctx->delete_blend_state(dev->pctx, st->cso);
      st->cso = NULL;
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_SetBlendState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HBLENDSTATE hState,
                     const FLOAT BlendFactor[4], UINT SampleMask)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_blend *st = (struct aurora_blend *)hState.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;

   aurora_lock(dev);
   dev->blend = st;
   if (BlendFactor) {
      memcpy(dev->blend_color.color, BlendFactor, sizeof(float) * 4u);
      dev->pctx->set_blend_color(dev->pctx, &dev->blend_color);
   }
   dev->sample_mask = SampleMask;
   dev->pctx->set_sample_mask(dev->pctx, SampleMask);

   if (st && st->cso) {
      dev->pctx->bind_blend_state(dev->pctx, st->cso);
   } else {
      if (!dev->default_blend_cso) {
         struct pipe_blend_state def;
         memset(&def, 0, sizeof(def));
         def.rt[0].colormask = PIPE_MASK_RGBA;
         dev->default_blend_cso = dev->pctx->create_blend_state(dev->pctx, &def);
      }
      dev->pctx->bind_blend_state(dev->pctx, dev->default_blend_cso);
   }
   aurora_unlock(dev);
}

/* =========================================================================
 * Depth/Stencil State
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateDepthStencilStateSize(D3D10DDI_HDEVICE d,
                                        const D3D10_DDI_DEPTH_STENCIL_DESC *desc)
{ (void)d; (void)desc; return sizeof(struct aurora_dsa); }

void APIENTRY
aurora_CreateDepthStencilState(D3D10DDI_HDEVICE hDevice,
                               const D3D10_DDI_DEPTH_STENCIL_DESC *pDesc,
                               D3D10DDI_HDEPTHSTENCILSTATE hState,
                               D3D10DDI_HRTDEPTHSTENCILSTATE hRTState)
{
   (void)hRTState;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_dsa *st = (struct aurora_dsa *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !pDesc || !st)
      return;
   memset(st, 0, sizeof(*st));

   struct pipe_depth_stencil_alpha_state *d = &st->state;
   d->depth_enabled = pDesc->DepthEnable ? 1 : 0;
   d->depth_writemask =
      (pDesc->DepthEnable && pDesc->DepthWriteMask == D3D10_DDI_DEPTH_WRITE_MASK_ALL) ? 1 : 0;
   d->depth_func = d3d_cmp_func(pDesc->DepthFunc);

   if (pDesc->StencilEnable) {
      d->stencil[0].enabled = 1;
      d->stencil[0].func = d3d_cmp_func(pDesc->FrontFace.StencilFunc);
      d->stencil[0].fail_op = d3d_stencil_op(pDesc->FrontFace.StencilFailOp);
      d->stencil[0].zfail_op = d3d_stencil_op(pDesc->FrontFace.StencilDepthFailOp);
      d->stencil[0].zpass_op = d3d_stencil_op(pDesc->FrontFace.StencilPassOp);
      d->stencil[0].valuemask = pDesc->StencilReadMask;
      d->stencil[0].writemask = pDesc->StencilWriteMask;

      d->stencil[1].enabled = 1;
      d->stencil[1].func = d3d_cmp_func(pDesc->BackFace.StencilFunc);
      d->stencil[1].fail_op = d3d_stencil_op(pDesc->BackFace.StencilFailOp);
      d->stencil[1].zfail_op = d3d_stencil_op(pDesc->BackFace.StencilDepthFailOp);
      d->stencil[1].zpass_op = d3d_stencil_op(pDesc->BackFace.StencilPassOp);
      d->stencil[1].valuemask = pDesc->StencilReadMask;
      d->stencil[1].writemask = pDesc->StencilWriteMask;
   }

   aurora_lock(dev);
   st->cso = dev->pctx->create_depth_stencil_alpha_state(dev->pctx, d);
   aurora_unlock(dev);
}

void APIENTRY
aurora_DestroyDepthStencilState(D3D10DDI_HDEVICE hDevice,
                                D3D10DDI_HDEPTHSTENCILSTATE hState)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_dsa *st = (struct aurora_dsa *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !st)
      return;
   aurora_lock(dev);
   if (dev->dsa == st) {
      dev->dsa = NULL;
      if (dev->default_dsa_cso)
         dev->pctx->bind_depth_stencil_alpha_state(dev->pctx, dev->default_dsa_cso);
   }
   if (st->cso) {
      dev->pctx->delete_depth_stencil_alpha_state(dev->pctx, st->cso);
      st->cso = NULL;
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_SetDepthStencilState(D3D10DDI_HDEVICE hDevice,
                            D3D10DDI_HDEPTHSTENCILSTATE hState, UINT StencilRef)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_dsa *st = (struct aurora_dsa *)hState.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;

   aurora_lock(dev);
   dev->dsa = st;
   dev->stencil_ref = StencilRef;
   struct pipe_stencil_ref sr = { .ref_value = { (uint8_t)StencilRef, (uint8_t)StencilRef } };
   dev->pctx->set_stencil_ref(dev->pctx, sr);

   if (st && st->cso) {
      dev->pctx->bind_depth_stencil_alpha_state(dev->pctx, st->cso);
   } else {
      if (!dev->default_dsa_cso) {
         struct pipe_depth_stencil_alpha_state def;
         memset(&def, 0, sizeof(def));
         def.depth_enabled = 1;
         def.depth_writemask = 1;
         def.depth_func = PIPE_FUNC_LESS;
         dev->default_dsa_cso =
            dev->pctx->create_depth_stencil_alpha_state(dev->pctx, &def);
      }
      dev->pctx->bind_depth_stencil_alpha_state(dev->pctx, dev->default_dsa_cso);
   }
   aurora_unlock(dev);
}

/* =========================================================================
 * Rasterizer State (10 / 11.1 / WDDM 2.0) — G17G depth_clamp=0 contract!
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateRasterizerStateSize_10(D3D10DDI_HDEVICE d, const D3D10_DDI_RASTERIZER_DESC *p)
{ (void)d; (void)p; return sizeof(struct aurora_rast); }
SIZE_T APIENTRY
aurora_CalcPrivateRasterizerStateSize_11_1(D3D10DDI_HDEVICE d, const D3D11_1_DDI_RASTERIZER_DESC *p)
{ (void)d; (void)p; return sizeof(struct aurora_rast); }
SIZE_T APIENTRY
aurora_CalcPrivateRasterizerStateSize_WDDM2_0(D3D10DDI_HDEVICE d, const D3DWDDM2_0DDI_RASTERIZER_DESC *p)
{ (void)d; (void)p; return sizeof(struct aurora_rast); }

static void
create_rast_common(struct aurora_device *dev, struct aurora_rast *st,
                   D3D10_DDI_FILL_MODE fill, D3D10_DDI_CULL_MODE cull,
                   BOOL front_ccw, INT depth_bias, FLOAT depth_bias_clamp,
                   FLOAT slope_scaled_bias, BOOL scissor_enable,
                   BOOL multisample_enable, BOOL antialiased_line)
{
   memset(st, 0, sizeof(*st));
   struct pipe_rasterizer_state *r = &st->state;

   r->flatshade_first = 1;
   r->front_ccw = front_ccw ? 1 : 0;
   r->clip_halfz = 1;
   r->half_pixel_center = 1;
   r->bottom_edge_rule = 0;

   /* G17G requires depth_clamp == 0 and depth_clip_near == depth_clip_far == 1
    * (agx_state.c:11428-11430); never set depth_clamp = 1! */
   r->depth_clamp = 0;
   r->depth_clip_near = 1;
   r->depth_clip_far = 1;

   unsigned poly = (fill == D3D10_DDI_FILL_WIREFRAME) ? PIPE_POLYGON_MODE_LINE
                                                      : PIPE_POLYGON_MODE_FILL;
   r->fill_front = poly;
   r->fill_back = poly;

   switch (cull) {
   case D3D10_DDI_CULL_FRONT: r->cull_face = PIPE_FACE_FRONT; break;
   case D3D10_DDI_CULL_BACK:  r->cull_face = PIPE_FACE_BACK; break;
   default:                   r->cull_face = PIPE_FACE_NONE; break;
   }

   r->scissor = scissor_enable ? 1 : 0;
   st->scissor_enable = scissor_enable != FALSE;
   r->multisample = multisample_enable ? 1 : 0;
   r->line_smooth = antialiased_line ? 1 : 0;
   r->line_width = 1.0f;
   r->point_size = 1.0f;

   if (depth_bias != 0 || slope_scaled_bias != 0.0f) {
      r->offset_tri = 1;
      r->offset_line = 1;
      r->offset_point = 1;
      r->offset_units = (float)depth_bias;
      r->offset_scale = slope_scaled_bias;
      r->offset_clamp = depth_bias_clamp;
      r->offset_units_unscaled = 0;
   }

   aurora_lock(dev);
   st->cso = dev->pctx->create_rasterizer_state(dev->pctx, r);
   aurora_unlock(dev);
}

void APIENTRY
aurora_CreateRasterizerState_10(D3D10DDI_HDEVICE hDevice,
                                const D3D10_DDI_RASTERIZER_DESC *pDesc,
                                D3D10DDI_HRASTERIZERSTATE hState,
                                D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
   (void)hRTState;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rast *st = (struct aurora_rast *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !pDesc || !st)
      return;
   create_rast_common(dev, st, pDesc->FillMode, pDesc->CullMode,
                      pDesc->FrontCounterClockwise, pDesc->DepthBias,
                      pDesc->DepthBiasClamp, pDesc->SlopeScaledDepthBias,
                      pDesc->ScissorEnable, pDesc->MultisampleEnable,
                      pDesc->AntialiasedLineEnable);
}

void APIENTRY
aurora_CreateRasterizerState_11_1(D3D10DDI_HDEVICE hDevice,
                                  const D3D11_1_DDI_RASTERIZER_DESC *pDesc,
                                  D3D10DDI_HRASTERIZERSTATE hState,
                                  D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
   (void)hRTState;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rast *st = (struct aurora_rast *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !pDesc || !st)
      return;
   create_rast_common(dev, st, pDesc->FillMode, pDesc->CullMode,
                      pDesc->FrontCounterClockwise, pDesc->DepthBias,
                      pDesc->DepthBiasClamp, pDesc->SlopeScaledDepthBias,
                      pDesc->ScissorEnable, pDesc->MultisampleEnable,
                      pDesc->AntialiasedLineEnable);
}

void APIENTRY
aurora_CreateRasterizerState_WDDM2_0(D3D10DDI_HDEVICE hDevice,
                                     const D3DWDDM2_0DDI_RASTERIZER_DESC *pDesc,
                                     D3D10DDI_HRASTERIZERSTATE hState,
                                     D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
   (void)hRTState;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rast *st = (struct aurora_rast *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !pDesc || !st)
      return;
   create_rast_common(dev, st, pDesc->FillMode, pDesc->CullMode,
                      pDesc->FrontCounterClockwise, pDesc->DepthBias,
                      pDesc->DepthBiasClamp, pDesc->SlopeScaledDepthBias,
                      pDesc->ScissorEnable, pDesc->MultisampleEnable,
                      pDesc->AntialiasedLineEnable);
}

void APIENTRY
aurora_DestroyRasterizerState(D3D10DDI_HDEVICE hDevice,
                              D3D10DDI_HRASTERIZERSTATE hState)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rast *st = (struct aurora_rast *)hState.pDrvPrivate;
   if (!dev || !dev->pctx || !st)
      return;
   aurora_lock(dev);
   if (dev->rast == st) {
      dev->rast = NULL;
      if (dev->default_rast_cso)
         dev->pctx->bind_rasterizer_state(dev->pctx, dev->default_rast_cso);
   }
   if (st->cso) {
      dev->pctx->delete_rasterizer_state(dev->pctx, st->cso);
      st->cso = NULL;
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_SetRasterizerState(D3D10DDI_HDEVICE hDevice,
                          D3D10DDI_HRASTERIZERSTATE hState)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_rast *st = (struct aurora_rast *)hState.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;

   aurora_lock(dev);
   dev->rast = st;
   if (st && st->cso) {
      dev->pctx->bind_rasterizer_state(dev->pctx, st->cso);
   } else {
      if (!dev->default_rast_cso) {
         struct pipe_rasterizer_state def;
         memset(&def, 0, sizeof(def));
         def.flatshade_first = 1;
         def.clip_halfz = 1;
         def.half_pixel_center = 1;
         def.depth_clip_near = 1;
         def.depth_clip_far = 1;
         def.cull_face = PIPE_FACE_BACK;
         def.fill_front = PIPE_POLYGON_MODE_FILL;
         def.fill_back = PIPE_POLYGON_MODE_FILL;
         def.line_width = 1.0f;
         def.point_size = 1.0f;
         dev->default_rast_cso = dev->pctx->create_rasterizer_state(dev->pctx, &def);
      }
      dev->pctx->bind_rasterizer_state(dev->pctx, dev->default_rast_cso);
   }
   aurora_unlock(dev);
}

/* =========================================================================
 * Sampler State
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateSamplerSize(D3D10DDI_HDEVICE d, const D3D10_DDI_SAMPLER_DESC *p)
{ (void)d; (void)p; return sizeof(struct aurora_sampler); }

void APIENTRY
aurora_CreateSampler(D3D10DDI_HDEVICE hDevice,
                     const D3D10_DDI_SAMPLER_DESC *pDesc,
                     D3D10DDI_HSAMPLER hSampler,
                     D3D10DDI_HRTSAMPLER hRTSampler)
{
   (void)hRTSampler;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_sampler *st = (struct aurora_sampler *)hSampler.pDrvPrivate;
   if (!dev || !dev->pctx || !pDesc || !st)
      return;
   memset(st, 0, sizeof(*st));

   struct pipe_sampler_state *s = &st->state;
   D3D10_DDI_FILTER f = pDesc->Filter;
   bool aniso = D3D10_DDI_DECODE_IS_ANISOTROPIC_FILTER(f);
   bool cmp = D3D10_DDI_DECODE_IS_COMPARISON_FILTER(f);

   s->min_img_filter =
      (aniso || D3D10_DDI_DECODE_MIN_FILTER(f) == D3D10_DDI_FILTER_TYPE_LINEAR)
         ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   s->mag_img_filter =
      (aniso || D3D10_DDI_DECODE_MAG_FILTER(f) == D3D10_DDI_FILTER_TYPE_LINEAR)
         ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   s->min_mip_filter =
      (aniso || D3D10_DDI_DECODE_MIP_FILTER(f) == D3D10_DDI_FILTER_TYPE_LINEAR)
         ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST;

   s->max_anisotropy = aniso ? MIN2(pDesc->MaxAnisotropy, 16u) : 1u;
   s->wrap_s = d3d_tex_wrap(pDesc->AddressU);
   s->wrap_t = d3d_tex_wrap(pDesc->AddressV);
   s->wrap_r = d3d_tex_wrap(pDesc->AddressW);
   s->lod_bias = pDesc->MipLODBias;
   s->min_lod = MAX2(pDesc->MinLOD, 0.0f);
   s->max_lod = MAX2(pDesc->MaxLOD, s->min_lod);
   if (cmp) {
      s->compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
      s->compare_func = d3d_cmp_func(pDesc->ComparisonFunc);
   }
   memcpy(s->border_color.f, pDesc->BorderColor, sizeof(float) * 4u);

   aurora_lock(dev);
   st->cso = dev->pctx->create_sampler_state(dev->pctx, s);
   aurora_unlock(dev);
}

void APIENTRY
aurora_DestroySampler(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSAMPLER hSampler)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_sampler *st = (struct aurora_sampler *)hSampler.pDrvPrivate;
   if (!dev || !dev->pctx || !st)
      return;
   aurora_lock(dev);
   for (unsigned stage = 0; stage < AURORA_STAGE_COUNT; ++stage) {
      for (unsigned i = 0; i < AURORA_MAX_SAMPLERS; ++i) {
         if (dev->samplers[stage][i] == st)
            dev->samplers[stage][i] = NULL;
      }
   }
   if (st->cso) {
      dev->pctx->delete_sampler_state(dev->pctx, st->cso);
      st->cso = NULL;
   }
   aurora_unlock(dev);
}

/* =========================================================================
 * Input Layout (deferred CSO creation so VB strides apply via src_stride!)
 * ========================================================================= */
SIZE_T APIENTRY
aurora_CalcPrivateElementLayoutSize(D3D10DDI_HDEVICE d,
                                    const D3D10DDIARG_CREATEELEMENTLAYOUT *p)
{ (void)d; (void)p; return sizeof(struct aurora_velems); }

void APIENTRY
aurora_CreateElementLayout(D3D10DDI_HDEVICE hDevice,
                           const D3D10DDIARG_CREATEELEMENTLAYOUT *pArgs,
                           D3D10DDI_HELEMENTLAYOUT hLayout,
                           D3D10DDI_HRTELEMENTLAYOUT hRTLayout)
{
   (void)hDevice; (void)hRTLayout;
   struct aurora_velems *ve = (struct aurora_velems *)hLayout.pDrvPrivate;
   if (!pArgs || !ve)
      return;
   memset(ve, 0, sizeof(*ve));

   unsigned max_reg = 0;
   bool used[PIPE_MAX_ATTRIBS] = {false};
   for (UINT i = 0; i < pArgs->NumElements; ++i) {
      const D3D10DDIARG_INPUT_ELEMENT_DESC *e = &pArgs->pVertexElements[i];
      if (e->InputRegister >= PIPE_MAX_ATTRIBS)
         continue;
      unsigned reg = e->InputRegister;
      enum pipe_format pf = aurora_dxgi_to_pipe_format(e->Format);
      if (pf == PIPE_FORMAT_NONE)
         pf = PIPE_FORMAT_R32G32B32A32_FLOAT;
      ve->elems[reg].src_offset = (uint16_t)e->AlignedByteOffset;
      ve->elems[reg].vertex_buffer_index = (uint8_t)e->InputSlot;
      ve->elems[reg].src_format = (uint8_t)pf;
      ve->elems[reg].instance_divisor =
         (e->InputSlotClass == D3D10_DDI_INPUT_PER_INSTANCE_DATA)
            ? e->InstanceDataStepRate : 0u;
      used[reg] = true;
      if (reg + 1u > max_reg)
         max_reg = reg + 1u;
   }

   /* Fill any sparse gaps with a harmless zero-stride dummy element. */
   for (unsigned reg = 0; reg < max_reg; ++reg) {
      if (!used[reg]) {
         ve->elems[reg].src_offset = 0;
         ve->elems[reg].vertex_buffer_index = 0;
         ve->elems[reg].src_format = (uint8_t)PIPE_FORMAT_R32_FLOAT;
         ve->elems[reg].src_stride = 0;
         ve->elems[reg].instance_divisor = 0;
      }
   }
   ve->count = max_reg;
}

void APIENTRY
aurora_DestroyElementLayout(D3D10DDI_HDEVICE hDevice,
                            D3D10DDI_HELEMENTLAYOUT hLayout)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_velems *ve = (struct aurora_velems *)hLayout.pDrvPrivate;
   if (!dev || !ve)
      return;
   aurora_lock(dev);
   if (dev->velems == ve) {
      dev->velems = NULL;
      dev->velems_dirty = true;
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_IaSetInputLayout(D3D10DDI_HDEVICE hDevice, D3D10DDI_HELEMENTLAYOUT hLayout)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev)
      return;
   aurora_lock(dev);
   struct aurora_velems *ve = (struct aurora_velems *)hLayout.pDrvPrivate;
   if (dev->velems != ve) {
      dev->velems = ve;
      dev->velems_dirty = true;
   }
   aurora_unlock(dev);
}

/* =========================================================================
 * Viewports, Scissors, Topology, Vertex/Index Buffers
 * ========================================================================= */
void APIENTRY
aurora_SetViewports(D3D10DDI_HDEVICE hDevice, UINT NumViewports,
                    UINT ClearViewports, const D3D10_DDI_VIEWPORT *pViewports)
{
   (void)ClearViewports;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   UINT n = MIN2(NumViewports, AURORA_MAX_VIEWPORTS);
   for (UINT i = 0; i < n; ++i) {
      const D3D10_DDI_VIEWPORT *v = &pViewports[i];
      float half_w = 0.5f * v->Width;
      float half_h = 0.5f * v->Height;
      dev->viewports[i].scale[0] = half_w;
      dev->viewports[i].scale[1] = -half_h;
      dev->viewports[i].scale[2] = v->MaxDepth - v->MinDepth;
      dev->viewports[i].translate[0] = v->TopLeftX + half_w;
      dev->viewports[i].translate[1] = v->TopLeftY + half_h;
      dev->viewports[i].translate[2] = v->MinDepth;
      dev->viewports[i].swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
      dev->viewports[i].swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
      dev->viewports[i].swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
      dev->viewports[i].swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
   }
   dev->num_viewports = n;
   if (n > 0u)
      dev->pctx->set_viewport_states(dev->pctx, 0, n, dev->viewports);
   aurora_unlock(dev);
}

void APIENTRY
aurora_SetScissorRects(D3D10DDI_HDEVICE hDevice, UINT NumRects,
                       UINT ClearRects, const D3D10_DDI_RECT *pRects)
{
   (void)ClearRects;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   UINT n = MIN2(NumRects, AURORA_MAX_SCISSORS);
   for (UINT i = 0; i < n; ++i) {
      dev->scissors[i].minx = (unsigned)MAX2(pRects[i].left, 0);
      dev->scissors[i].miny = (unsigned)MAX2(pRects[i].top, 0);
      dev->scissors[i].maxx = (unsigned)MAX2(pRects[i].right, 0);
      dev->scissors[i].maxy = (unsigned)MAX2(pRects[i].bottom, 0);
   }
   dev->num_scissors = n;
   if (n > 0u)
      dev->pctx->set_scissor_states(dev->pctx, 0, n, dev->scissors);
   dev->scissor_dirty = false;
   aurora_unlock(dev);
}

void APIENTRY
aurora_IaSetTopology(D3D10DDI_HDEVICE hDevice, D3D10_DDI_PRIMITIVE_TOPOLOGY Topo)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev)
      return;
   aurora_lock(dev);
   dev->topology = Topo;
   dev->pipe_prim = d3d_topology(Topo);
   aurora_unlock(dev);
}

void APIENTRY
aurora_IaSetVertexBuffers(D3D10DDI_HDEVICE hDevice, UINT StartSlot,
                          UINT NumBuffers, const D3D10DDI_HRESOURCE *phBuffers,
                          const UINT *pStrides, const UINT *pOffsets)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx)
      return;
   aurora_lock(dev);
   for (UINT i = 0; i < NumBuffers; ++i) {
      UINT slot = StartSlot + i;
      if (slot >= AURORA_MAX_VBS)
         break;
      struct aurora_resource *r =
         phBuffers ? (struct aurora_resource *)phBuffers[i].pDrvPrivate : NULL;
      pipe_resource_reference(&dev->vbs[slot].buffer.resource, r ? r->prsrc : NULL);
      dev->vbs[slot].is_user_buffer = false;
      dev->vbs[slot].buffer_offset = pOffsets ? pOffsets[i] : 0u;
      uint32_t new_stride = pStrides ? pStrides[i] : 0u;
      if (dev->vb_strides[slot] != new_stride) {
         dev->vb_strides[slot] = new_stride;
         dev->velems_dirty = true;
      }
   }
   UINT max_slot = 0u;
   for (UINT i = 0; i < AURORA_MAX_VBS; ++i) {
      if (dev->vbs[i].buffer.resource)
         max_slot = i + 1u;
   }
   dev->max_vb_slot = max_slot;
   dev->vb_dirty = true;
   aurora_unlock(dev);
}

void APIENTRY
aurora_IaSetIndexBuffer(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hBuffer,
                        DXGI_FORMAT Format, UINT Offset)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev)
      return;
   aurora_lock(dev);
   dev->ib_res = (struct aurora_resource *)hBuffer.pDrvPrivate;
   dev->ib_format = Format;
   dev->ib_offset = Offset;
   aurora_unlock(dev);
}

void APIENTRY
aurora_SoSetTargets(D3D10DDI_HDEVICE hDevice, UINT SOTargets, UINT ClearTargets,
                    const D3D10DDI_HRESOURCE *phResource, const UINT *pOffsets)
{
   (void)hDevice; (void)SOTargets; (void)ClearTargets;
   (void)phResource; (void)pOffsets;
}

/* =========================================================================
 * Render Targets, Constant Buffers, SRVs, Samplers, UAVs
 * ========================================================================= */
void APIENTRY
aurora_SetRenderTargets(D3D10DDI_HDEVICE hDevice,
                        const D3D10DDI_HRENDERTARGETVIEW *phRTVs,
                        UINT NumRTVs, UINT ClearSlots,
                        D3D10DDI_HDEPTHSTENCILVIEW hDSV,
                        const D3D11DDI_HUNORDEREDACCESSVIEW *phUAVs,
                        const UINT *pUAVInitialCounts,
                        UINT UAVIndex, UINT NumUAVs,
                        UINT UAVFirsttoSet, UINT UAVNumberUpdated)
{
   (void)ClearSlots; (void)pUAVInitialCounts; (void)UAVFirsttoSet; (void)UAVNumberUpdated;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev)
      return;

   aurora_lock(dev);
   UINT n = MIN2(NumRTVs, AURORA_MAX_RTVS);
   for (UINT i = 0; i < AURORA_MAX_RTVS; ++i) {
      dev->rtvs[i] = (i < n && phRTVs)
                        ? (struct aurora_rtv *)phRTVs[i].pDrvPrivate : NULL;
   }
   dev->num_rtvs = n;
   dev->dsv = (struct aurora_dsv *)hDSV.pDrvPrivate;
   dev->fb_dirty = true;

   if (NumUAVs > 0u && phUAVs) {
      for (UINT i = 0; i < NumUAVs; ++i) {
         UINT slot = UAVIndex + i;
         if (slot < AURORA_MAX_UAVS)
            dev->om_uavs[slot] = (struct aurora_uav *)phUAVs[i].pDrvPrivate;
      }
      dev->om_uav_start = UAVIndex;
      dev->om_uav_count = NumUAVs;
      dev->uav_dirty[AURORA_STAGE_PS] = true;
   }
   aurora_unlock(dev);
}

void APIENTRY
aurora_CsSetUAVs(D3D10DDI_HDEVICE hDevice, UINT Offset, UINT NumViews,
                 const D3D11DDI_HUNORDEREDACCESSVIEW *phUAVs,
                 const UINT *pUAVInitialCounts)
{
   (void)pUAVInitialCounts;
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev)
      return;
   aurora_lock(dev);
   for (UINT i = 0; i < NumViews; ++i) {
      UINT slot = Offset + i;
      if (slot < AURORA_MAX_UAVS)
         dev->cs_uavs[slot] =
            phUAVs ? (struct aurora_uav *)phUAVs[i].pDrvPrivate : NULL;
   }
   dev->uav_dirty[AURORA_STAGE_CS] = true;
   aurora_unlock(dev);
}

static void
set_constant_buffers_common(struct aurora_device *dev, enum aurora_stage stage,
                            UINT StartSlot, UINT NumBuffers,
                            const D3D10DDI_HRESOURCE *phBuffers,
                            const UINT *pFirstConstant, const UINT *pNumConstants)
{
   if (!dev || !dev->pctx)
      return;
   mesa_shader_stage mst = aurora_stage_to_mesa(stage);
   aurora_lock(dev);
   for (UINT i = 0; i < NumBuffers; ++i) {
      UINT slot = StartSlot + i;
      if (slot >= AURORA_MAX_CBS)
         break;
      struct aurora_resource *r =
         phBuffers ? (struct aurora_resource *)phBuffers[i].pDrvPrivate : NULL;
      struct pipe_constant_buffer *cb = &dev->cbs[stage][slot];
      if (r && r->prsrc) {
         UINT offset_bytes = pFirstConstant ? (pFirstConstant[i] * 16u) : 0u;
         UINT size_bytes = pNumConstants ? (pNumConstants[i] * 16u) : r->width;
         if (offset_bytes > r->width)
            offset_bytes = r->width;
         if (size_bytes > r->width - offset_bytes)
            size_bytes = r->width - offset_bytes;
         pipe_resource_reference(&cb->buffer, r->prsrc);
         cb->buffer_offset = offset_bytes;
         cb->buffer_size = size_bytes;
         cb->user_buffer = NULL;
         dev->pctx->set_constant_buffer(dev->pctx, mst, slot, cb);
      } else {
         pipe_resource_reference(&cb->buffer, NULL);
         cb->buffer_offset = 0;
         cb->buffer_size = 0;
         cb->user_buffer = NULL;
         dev->pctx->set_constant_buffer(dev->pctx, mst, slot, NULL);
      }
   }
   aurora_unlock(dev);
}

static void
set_srvs_common(struct aurora_device *dev, enum aurora_stage stage,
                UINT StartSlot, UINT NumViews,
                const D3D10DDI_HSHADERRESOURCEVIEW *phViews)
{
   if (!dev)
      return;
   aurora_lock(dev);
   for (UINT i = 0; i < NumViews; ++i) {
      UINT slot = StartSlot + i;
      if (slot >= AURORA_MAX_SRVS)
         break;
      dev->srvs[stage][slot] =
         phViews ? (struct aurora_srv *)phViews[i].pDrvPrivate : NULL;
   }
   UINT max_slot = 0u;
   for (UINT i = 0; i < AURORA_MAX_SRVS; ++i) {
      if (dev->srvs[stage][i])
         max_slot = i + 1u;
   }
   dev->max_srv_slot[stage] = max_slot;
   dev->srv_dirty[stage] = true;
   aurora_unlock(dev);
}

static void
set_samplers_common(struct aurora_device *dev, enum aurora_stage stage,
                    UINT StartSlot, UINT NumSamplers,
                    const D3D10DDI_HSAMPLER *phSamplers)
{
   if (!dev || !dev->pctx)
      return;
   mesa_shader_stage mst = aurora_stage_to_mesa(stage);
   aurora_lock(dev);
   for (UINT i = 0; i < NumSamplers; ++i) {
      UINT slot = StartSlot + i;
      if (slot >= AURORA_MAX_SAMPLERS)
         break;
      dev->samplers[stage][slot] =
         phSamplers ? (struct aurora_sampler *)phSamplers[i].pDrvPrivate : NULL;
   }
   void *csos[AURORA_MAX_SAMPLERS];
   UINT max_slot = 0u;
   for (UINT i = 0; i < AURORA_MAX_SAMPLERS; ++i) {
      csos[i] = dev->samplers[stage][i] ? dev->samplers[stage][i]->cso : NULL;
      if (csos[i])
         max_slot = i + 1u;
   }
   UINT bind_count = MAX2(max_slot, dev->max_sampler_slot[stage]);
   if (bind_count > 0u)
      dev->pctx->bind_sampler_states(dev->pctx, mst, 0, bind_count, csos);
   dev->max_sampler_slot[stage] = max_slot;
   aurora_unlock(dev);
}

#define AURORA_DEFINE_STAGE_BINDERS(ST, ENUM_ST)                                  \
void APIENTRY aurora_##ST##SetConstantBuffers10(                                  \
   D3D10DDI_HDEVICE d, UINT s, UINT n, const D3D10DDI_HRESOURCE *b)               \
{ set_constant_buffers_common((struct aurora_device *)d.pDrvPrivate, ENUM_ST,     \
                              s, n, b, NULL, NULL); }                             \
void APIENTRY aurora_##ST##SetConstantBuffers11_1(                                \
   D3D10DDI_HDEVICE d, UINT s, UINT n, const D3D10DDI_HRESOURCE *b,               \
   const UINT *fc, const UINT *nc)                                                \
{ set_constant_buffers_common((struct aurora_device *)d.pDrvPrivate, ENUM_ST,     \
                              s, n, b, fc, nc); }                                 \
void APIENTRY aurora_##ST##SetShaderResources(                                    \
   D3D10DDI_HDEVICE d, UINT s, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v)     \
{ set_srvs_common((struct aurora_device *)d.pDrvPrivate, ENUM_ST, s, n, v); }     \
void APIENTRY aurora_##ST##SetSamplers(                                           \
   D3D10DDI_HDEVICE d, UINT s, UINT n, const D3D10DDI_HSAMPLER *sm)               \
{ set_samplers_common((struct aurora_device *)d.pDrvPrivate, ENUM_ST, s, n, sm); }

AURORA_DEFINE_STAGE_BINDERS(Vs, AURORA_STAGE_VS)
AURORA_DEFINE_STAGE_BINDERS(Ps, AURORA_STAGE_PS)
AURORA_DEFINE_STAGE_BINDERS(Gs, AURORA_STAGE_GS)
AURORA_DEFINE_STAGE_BINDERS(Hs, AURORA_STAGE_HS)
AURORA_DEFINE_STAGE_BINDERS(Ds, AURORA_STAGE_DS)
AURORA_DEFINE_STAGE_BINDERS(Cs, AURORA_STAGE_CS)
#undef AURORA_DEFINE_STAGE_BINDERS

/* =========================================================================
 * Pre-Draw / Pre-Dispatch State Synchronization
 * ========================================================================= */
void
aurora_sync_framebuffer(struct aurora_device *dev)
{
   if (!dev->fb_dirty || !dev->pctx)
      return;

   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   UINT fb_w = 0, fb_h = 0;

   for (UINT i = 0; i < dev->num_rtvs; ++i) {
      if (dev->rtvs[i] && aurora_rtv_fill_surface(dev->rtvs[i], &fb.cbufs[i])) {
         struct pipe_resource *tex = fb.cbufs[i].texture;
         UINT w = u_minify(tex->width0, fb.cbufs[i].level);
         UINT h = u_minify(tex->height0, fb.cbufs[i].level);
         if (fb_w == 0 || w < fb_w) fb_w = w;
         if (fb_h == 0 || h < fb_h) fb_h = h;
      }
   }
   fb.nr_cbufs = (uint8_t)dev->num_rtvs;

   if (dev->dsv && aurora_dsv_fill_surface(dev->dsv, &fb.zsbuf)) {
      struct pipe_resource *tex = fb.zsbuf.texture;
      UINT w = u_minify(tex->width0, fb.zsbuf.level);
      UINT h = u_minify(tex->height0, fb.zsbuf.level);
      if (fb_w == 0 || w < fb_w) fb_w = w;
      if (fb_h == 0 || h < fb_h) fb_h = h;
   }

   fb.width = fb_w ? fb_w : 1u;
   fb.height = fb_h ? fb_h : 1u;
   dev->pctx->set_framebuffer_state(dev->pctx, &fb);
   dev->fb_dirty = false;
}

void
aurora_sync_stage_resources(struct aurora_device *dev, enum aurora_stage stage)
{
   mesa_shader_stage mst = aurora_stage_to_mesa(stage);
   if (dev->srv_dirty[stage]) {
      struct pipe_sampler_view *views[PIPE_MAX_SAMPLERS];
      memset(views, 0, sizeof(views));
      UINT max_tex = 0u;

      struct pipe_shader_buffer ssbos[32];
      memset(ssbos, 0, sizeof(ssbos));
      UINT max_srv_ssbo = 0u;

      UINT limit = MIN2(dev->max_srv_slot[stage], AURORA_MAX_SRVS);
      for (UINT i = 0; i < limit; ++i) {
         struct aurora_srv *srv = dev->srvs[stage][i];
         if (!srv || !srv->res || !srv->res->prsrc)
            continue;
         if (srv->is_raw_or_struct) {
            if (AURORA_SSBO_SRV_BASE + i < 32u) {
               UINT slot = AURORA_SSBO_SRV_BASE + i;
               ssbos[slot].buffer = srv->res->prsrc;
               ssbos[slot].buffer_offset = srv->buf_offset;
               ssbos[slot].buffer_size = srv->buf_size;
               if (slot + 1u > max_srv_ssbo)
                  max_srv_ssbo = slot + 1u;
            }
         } else if (i < PIPE_MAX_SAMPLERS) {
            views[i] = aurora_srv_get_view(dev, srv);
            if (views[i] && i + 1u > max_tex)
               max_tex = i + 1u;
         }
      }
      dev->pctx->set_sampler_views(dev->pctx, mst, 0, max_tex,
                                   PIPE_MAX_SAMPLERS - max_tex, views);
      if (max_srv_ssbo > AURORA_SSBO_SRV_BASE) {
         dev->pctx->set_shader_buffers(
            dev->pctx, mst, AURORA_SSBO_SRV_BASE,
            max_srv_ssbo - AURORA_SSBO_SRV_BASE,
            &ssbos[AURORA_SSBO_SRV_BASE], 0u);
      }
      dev->srv_dirty[stage] = false;
   }

   if (dev->uav_dirty[stage]) {
      struct aurora_uav **uavs =
         (stage == AURORA_STAGE_CS) ? dev->cs_uavs : dev->om_uavs;
      struct pipe_shader_buffer ssbos[AURORA_SSBO_UAV_COUNT];
      struct pipe_image_view imgs[AURORA_MAX_UAVS];
      memset(ssbos, 0, sizeof(ssbos));
      memset(imgs, 0, sizeof(imgs));
      unsigned writable_mask = 0u;
      UINT max_ssbo = 0u, max_img = 0u;

      for (UINT i = 0; i < AURORA_MAX_UAVS; ++i) {
         struct aurora_uav *u = uavs[i];
         if (!u || !u->res || !u->res->prsrc)
            continue;
         if (u->is_raw_or_struct) {
            ssbos[i].buffer = u->res->prsrc;
            ssbos[i].buffer_offset = u->buf_offset;
            ssbos[i].buffer_size = u->buf_size;
            writable_mask |= (1u << i);
            max_ssbo = i + 1u;
         } else {
            imgs[i] = u->img_templ;
            imgs[i].resource = u->res->prsrc;
            max_img = i + 1u;
         }
      }
      if (max_ssbo > 0u) {
         dev->pctx->set_shader_buffers(dev->pctx, mst, 0, max_ssbo,
                                       ssbos, writable_mask);
      }
      dev->pctx->set_shader_images(dev->pctx, mst, 0, max_img,
                                   AURORA_MAX_UAVS - max_img, imgs);
      dev->uav_dirty[stage] = false;
   }
}

void
aurora_sync_draw_state(struct aurora_device *dev)
{
   aurora_sync_framebuffer(dev);

   if (dev->vb_dirty) {
      dev->pctx->set_vertex_buffers(dev->pctx, dev->max_vb_slot, dev->vbs);
      dev->vb_dirty = false;
   }

   if (dev->velems_dirty) {
      if (dev->active_velems_cso) {
         if (dev->empty_velems_cso)
            dev->pctx->bind_vertex_elements_state(dev->pctx, dev->empty_velems_cso);
         dev->pctx->delete_vertex_elements_state(dev->pctx, dev->active_velems_cso);
         dev->active_velems_cso = NULL;
      }
      if (dev->velems && dev->velems->count > 0u) {
         struct pipe_vertex_element elems[PIPE_MAX_ATTRIBS];
         memcpy(elems, dev->velems->elems,
                dev->velems->count * sizeof(struct pipe_vertex_element));
         for (unsigned i = 0; i < dev->velems->count; ++i) {
            unsigned slot = elems[i].vertex_buffer_index;
            elems[i].src_stride =
               (slot < AURORA_MAX_VBS) ? dev->vb_strides[slot] : 0u;
         }
         dev->active_velems_cso = dev->pctx->create_vertex_elements_state(
            dev->pctx, dev->velems->count, elems);
         dev->pctx->bind_vertex_elements_state(dev->pctx, dev->active_velems_cso);
      } else if (dev->empty_velems_cso) {
         dev->pctx->bind_vertex_elements_state(dev->pctx, dev->empty_velems_cso);
      }
      dev->velems_dirty = false;
   }

   /* Ensure default blend / dsa / rast are bound if app hasn't set them. */
   if (!dev->blend && dev->default_blend_cso)
      dev->pctx->bind_blend_state(dev->pctx, dev->default_blend_cso);
   if (!dev->dsa && dev->default_dsa_cso)
      dev->pctx->bind_depth_stencil_alpha_state(dev->pctx, dev->default_dsa_cso);
   if (!dev->rast && dev->default_rast_cso)
      dev->pctx->bind_rasterizer_state(dev->pctx, dev->default_rast_cso);

   aurora_sync_stage_resources(dev, AURORA_STAGE_VS);
   aurora_sync_stage_resources(dev, AURORA_STAGE_PS);
}

/* =========================================================================
 * Draw & Dispatch Entrypoints
 * ========================================================================= */
void APIENTRY
aurora_DrawInstanced(D3D10DDI_HDEVICE hDevice, UINT VertexCountPerInstance,
                     UINT InstanceCount, UINT StartVertexLocation,
                     UINT StartInstanceLocation)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx || VertexCountPerInstance == 0u || InstanceCount == 0u)
      return;

   aurora_lock(dev);
   aurora_sync_draw_state(dev);
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode = dev->pipe_prim;
   info.index_size = 0;
   info.start_instance = StartInstanceLocation;
   info.instance_count = InstanceCount;
   struct pipe_draw_start_count_bias draw = {
      .start = StartVertexLocation,
      .count = VertexCountPerInstance,
      .index_bias = 0,
   };
   dev->pctx->draw_vbo(dev->pctx, &info, 0, NULL, &draw, 1);
   aurora_unlock(dev);
}

void APIENTRY
aurora_Draw(D3D10DDI_HDEVICE hDevice, UINT VertexCount, UINT StartVertexLocation)
{
   aurora_DrawInstanced(hDevice, VertexCount, 1u, StartVertexLocation, 0u);
}

void APIENTRY
aurora_DrawIndexedInstanced(D3D10DDI_HDEVICE hDevice, UINT IndexCountPerInstance,
                            UINT InstanceCount, UINT StartIndexLocation,
                            INT BaseVertexLocation, UINT StartInstanceLocation)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx || !dev->ib_res || !dev->ib_res->prsrc ||
       IndexCountPerInstance == 0u || InstanceCount == 0u)
      return;

   unsigned idx_size = (dev->ib_format == DXGI_FORMAT_R32_UINT) ? 4u : 2u;

   aurora_lock(dev);
   aurora_sync_draw_state(dev);
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode = dev->pipe_prim;
   info.index_size = (uint16_t)idx_size;
   info.index.resource = dev->ib_res->prsrc;
   info.primitive_restart = true;
   info.restart_index = (idx_size == 4u) ? 0xffffffffu : 0xffffu;
   info.start_instance = StartInstanceLocation;
   info.instance_count = InstanceCount;
   struct pipe_draw_start_count_bias draw = {
      .start = StartIndexLocation + (dev->ib_offset / idx_size),
      .count = IndexCountPerInstance,
      .index_bias = BaseVertexLocation,
   };
   dev->pctx->draw_vbo(dev->pctx, &info, 0, NULL, &draw, 1);
   aurora_unlock(dev);
}

void APIENTRY
aurora_DrawIndexed(D3D10DDI_HDEVICE hDevice, UINT IndexCount,
                   UINT StartIndexLocation, INT BaseVertexLocation)
{
   aurora_DrawIndexedInstanced(hDevice, IndexCount, 1u, StartIndexLocation,
                               BaseVertexLocation, 0u);
}

void APIENTRY
aurora_DrawAuto(D3D10DDI_HDEVICE hDevice)
{
   (void)hDevice;
}

void APIENTRY
aurora_DrawInstancedIndirect(D3D10DDI_HDEVICE hDevice,
                             D3D10DDI_HRESOURCE hBufferForArgs,
                             UINT AlignedByteOffsetForArgs)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *arg = (struct aurora_resource *)hBufferForArgs.pDrvPrivate;
   if (!dev || !dev->pctx || !arg || !arg->prsrc)
      return;

   aurora_lock(dev);
   aurora_sync_draw_state(dev);
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode = dev->pipe_prim;
   info.instance_count = 1;
   struct pipe_draw_indirect_info ind;
   memset(&ind, 0, sizeof(ind));
   ind.buffer = arg->prsrc;
   ind.offset = AlignedByteOffsetForArgs;
   ind.draw_count = 1;
   struct pipe_draw_start_count_bias draw = {0};
   dev->pctx->draw_vbo(dev->pctx, &info, 0, &ind, &draw, 1);
   aurora_unlock(dev);
}

void APIENTRY
aurora_DrawIndexedInstancedIndirect(D3D10DDI_HDEVICE hDevice,
                                    D3D10DDI_HRESOURCE hBufferForArgs,
                                    UINT AlignedByteOffsetForArgs)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *arg = (struct aurora_resource *)hBufferForArgs.pDrvPrivate;
   if (!dev || !dev->pctx || !arg || !arg->prsrc ||
       !dev->ib_res || !dev->ib_res->prsrc)
      return;

   unsigned idx_size = (dev->ib_format == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
   aurora_lock(dev);
   aurora_sync_draw_state(dev);
   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode = dev->pipe_prim;
   info.index_size = (uint16_t)idx_size;
   info.index.resource = dev->ib_res->prsrc;
   info.primitive_restart = true;
   info.restart_index = (idx_size == 4u) ? 0xffffffffu : 0xffffu;
   info.instance_count = 1;
   struct pipe_draw_indirect_info ind;
   memset(&ind, 0, sizeof(ind));
   ind.buffer = arg->prsrc;
   ind.offset = AlignedByteOffsetForArgs;
   ind.draw_count = 1;
   struct pipe_draw_start_count_bias draw = {0};
   dev->pctx->draw_vbo(dev->pctx, &info, 0, &ind, &draw, 1);
   aurora_unlock(dev);
}

void APIENTRY
aurora_Dispatch(D3D10DDI_HDEVICE hDevice, UINT ThreadGroupCountX,
                UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev || !dev->pctx || !dev->cs ||
       ThreadGroupCountX == 0u || ThreadGroupCountY == 0u || ThreadGroupCountZ == 0u)
      return;

   aurora_lock(dev);
   aurora_sync_stage_resources(dev, AURORA_STAGE_CS);
   struct pipe_grid_info grid;
   memset(&grid, 0, sizeof(grid));
   grid.block[0] = MAX2(dev->cs->info.workgroup_size[0], 1u);
   grid.block[1] = MAX2(dev->cs->info.workgroup_size[1], 1u);
   grid.block[2] = MAX2(dev->cs->info.workgroup_size[2], 1u);
   grid.grid[0] = ThreadGroupCountX;
   grid.grid[1] = ThreadGroupCountY;
   grid.grid[2] = ThreadGroupCountZ;
   dev->pctx->launch_grid(dev->pctx, &grid);
   aurora_unlock(dev);
}

void APIENTRY
aurora_DispatchIndirect(D3D10DDI_HDEVICE hDevice,
                        D3D10DDI_HRESOURCE hBufferForArgs,
                        UINT AlignedByteOffsetForArgs)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   struct aurora_resource *arg = (struct aurora_resource *)hBufferForArgs.pDrvPrivate;
   if (!dev || !dev->pctx || !dev->cs || !arg || !arg->prsrc)
      return;

   aurora_lock(dev);
   aurora_sync_stage_resources(dev, AURORA_STAGE_CS);
   struct pipe_grid_info grid;
   memset(&grid, 0, sizeof(grid));
   grid.block[0] = MAX2(dev->cs->info.workgroup_size[0], 1u);
   grid.block[1] = MAX2(dev->cs->info.workgroup_size[1], 1u);
   grid.block[2] = MAX2(dev->cs->info.workgroup_size[2], 1u);
   grid.indirect = arg->prsrc;
   grid.indirect_offset = AlignedByteOffsetForArgs;
   dev->pctx->launch_grid(dev->pctx, &grid);
   aurora_unlock(dev);
}
