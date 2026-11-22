/*
 * D3D11.1 DDI (D3D11_1_DDI_INTERFACE_VERSION) on top of the D3D11.0 table. The 11.1 device
 * table has the 11.0 layout with some entries retyped and five appended; the retyped ones get
 * adapters here, D3D11_1DDICAPS_D3D11_OPTIONS stays all-zero (no constant buffer offsets, no
 * logic ops, no discard semantics required).
 *
 * SPDX-License-Identifier: MIT
 */

#include "Draw.h"
#include "DxgiFns.h"
#include "InputAssembly.h"
#include "OutputMerger.h"
#include "Query.h"
#include "Rasterizer.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"
#include "Format.h"
#include "D3D11.h"
#include "Device.h"

#include "Debug.h"

#include "util/u_inlines.h"
#include "Perf.h"

static BOOL APIENTRY
Flush11_1(D3D10DDI_HDEVICE hDevice, UINT FlushFlags)
{
   Flush(hDevice);
   return TRUE;
}

static void APIENTRY
RelocateDeviceFuncs11_1(D3D10DDI_HDEVICE hDevice, struct D3D11_1DDI_DEVICEFUNCS *pFuncs)
{
}

static void
ToD3D10_1(const D3D11_1_DDI_BLEND_DESC *in, D3D10_1_DDI_BLEND_DESC *out)
{
   memset(out, 0, sizeof *out);
   out->AlphaToCoverageEnable = in->AlphaToCoverageEnable;
   out->IndependentBlendEnable = in->IndependentBlendEnable;
   for (unsigned i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
      const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC *s = &in->RenderTarget[i];
      D3D10_DDI_RENDER_TARGET_BLEND_DESC1 *d = &out->RenderTarget[i];
      d->BlendEnable = s->BlendEnable;
      d->SrcBlend = s->SrcBlend;
      d->DestBlend = s->DestBlend;
      d->BlendOp = s->BlendOp;
      d->SrcBlendAlpha = s->SrcBlendAlpha;
      d->DestBlendAlpha = s->DestBlendAlpha;
      d->BlendOpAlpha = s->BlendOpAlpha;
      d->RenderTargetWriteMask = s->RenderTargetWriteMask;
   }
}

static SIZE_T APIENTRY
CalcPrivateBlendStateSize11_1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_BLEND_DESC *pDesc)
{
   D3D10_1_DDI_BLEND_DESC d;
   ToD3D10_1(pDesc, &d);
   return CalcPrivateBlendStateSize1(hDevice, &d);
}

static void APIENTRY
CreateBlendState11_1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_BLEND_DESC *pDesc,
                     D3D10DDI_HBLENDSTATE hState, D3D10DDI_HRTBLENDSTATE hRTState)
{
   D3D10_1_DDI_BLEND_DESC d;
   ToD3D10_1(pDesc, &d);
   CreateBlendState1(hDevice, &d, hState, hRTState);
}

static void
ToD3D10(const D3D11_1_DDI_RASTERIZER_DESC *in, D3D10_DDI_RASTERIZER_DESC *out)
{
   out->FillMode = in->FillMode;
   out->CullMode = in->CullMode;
   out->FrontCounterClockwise = in->FrontCounterClockwise;
   out->DepthBias = in->DepthBias;
   out->DepthBiasClamp = in->DepthBiasClamp;
   out->SlopeScaledDepthBias = in->SlopeScaledDepthBias;
   out->DepthClipEnable = in->DepthClipEnable;
   out->ScissorEnable = in->ScissorEnable;
   out->MultisampleEnable = in->MultisampleEnable;
   out->AntialiasedLineEnable = in->AntialiasedLineEnable;
}

static SIZE_T APIENTRY
CalcPrivateRasterizerStateSize11_1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_RASTERIZER_DESC *pDesc)
{
   D3D10_DDI_RASTERIZER_DESC d;
   ToD3D10(pDesc, &d);
   return CalcPrivateRasterizerStateSize(hDevice, &d);
}

static void APIENTRY
CreateRasterizerState11_1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_RASTERIZER_DESC *pDesc,
                          D3D10DDI_HRASTERIZERSTATE hState, D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
   D3D10_DDI_RASTERIZER_DESC d;
   ToD3D10(pDesc, &d);
   CreateRasterizerState(hDevice, &d, hState, hRTState);
}

static void APIENTRY
ResourceCopyRegion11_1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst, UINT DstSub, UINT x, UINT y,
                       UINT z, D3D10DDI_HRESOURCE hSrc, UINT SrcSub, const D3D10_DDI_BOX *pBox,
                       UINT CopyFlags)
{
   ResourceCopyRegion(hDevice, hDst, DstSub, x, y, z, hSrc, SrcSub, pBox);
}

static void APIENTRY
ResourceUpdateSubresourceUP11_1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst, UINT DstSub,
                                const D3D10_DDI_BOX *pBox, const VOID *pData, UINT RowPitch,
                                UINT DepthPitch, UINT CopyFlags)
{
   ResourceUpdateSubResourceUP(hDevice, hDst, DstSub, pBox, pData, RowPitch, DepthPitch);
}

#define CB_ADAPTER(Stage)                                                                     \
static void APIENTRY                                                                          \
Stage##SetConstantBuffers11_1(D3D10DDI_HDEVICE hDevice, UINT StartSlot, UINT NumBuffers,     \
                              const D3D10DDI_HRESOURCE *phBuffers, const UINT *pFirst,        \
                              const UINT *pNum)                                               \
{                                                                                             \
   Stage##SetConstantBuffers(hDevice, StartSlot, NumBuffers, phBuffers);                      \
}
CB_ADAPTER(Vs)
CB_ADAPTER(Ps)
CB_ADAPTER(Gs)

static void APIENTRY
NopSetConstantBuffers11_1(D3D10DDI_HDEVICE hDevice, UINT StartSlot, UINT NumBuffers,
                          const D3D10DDI_HRESOURCE *phBuffers, const UINT *pFirst, const UINT *pNum)
{
}

/* The frontend's shader creation does not read the I/O signatures. */
static SIZE_T APIENTRY
CalcPrivateShaderSize11_1(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                          const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSig)
{
   return CalcPrivateShaderSize(hDevice, pCode, NULL);
}

static void APIENTRY
CreateVertexShader11_1(D3D10DDI_HDEVICE hDevice, const UINT *pCode, D3D10DDI_HSHADER hShader,
                       D3D10DDI_HRTSHADER hRTShader, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSig)
{
   CreateVertexShader(hDevice, pCode, hShader, hRTShader, NULL);
}

static void APIENTRY
CreateGeometryShader11_1(D3D10DDI_HDEVICE hDevice, const UINT *pCode, D3D10DDI_HSHADER hShader,
                         D3D10DDI_HRTSHADER hRTShader, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSig)
{
   CreateGeometryShader(hDevice, pCode, hShader, hRTShader, NULL);
}

static void APIENTRY
CreatePixelShader11_1(D3D10DDI_HDEVICE hDevice, const UINT *pCode, D3D10DDI_HSHADER hShader,
                      D3D10DDI_HRTSHADER hRTShader, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSig)
{
   CreatePixelShader(hDevice, pCode, hShader, hRTShader, NULL);
}

static SIZE_T APIENTRY
CalcPrivateGeometryShaderWithStreamOutput11_1(D3D10DDI_HDEVICE hDevice,
                                              const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *p,
                                              const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSig)
{
   return CalcPrivateShaderSize(hDevice, p->pShaderCode, NULL);
}

static void APIENTRY
CreateGeometryShaderWithStreamOutput11_1(D3D10DDI_HDEVICE hDevice,
                                         const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *p,
                                         D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader,
                                         const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSig)
{
   D3D11_1CreateGeometryShaderWithStreamOutput(hDevice, p, hShader, hRTShader);
}

static SIZE_T APIENTRY
CalcPrivateTessellationShaderSize11_1(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                                      const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *pSig)
{
   return sizeof(Shader);
}

static void APIENTRY
CreateTessellationShader11_1(D3D10DDI_HDEVICE hDevice, const UINT *pCode, D3D10DDI_HSHADER hShader,
                             D3D10DDI_HRTSHADER hRTShader,
                             const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *pSig)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   memset(hShader.pDrvPrivate, 0, sizeof(Shader));
   SetError(hDevice, E_NOTIMPL);
}

static void APIENTRY
Discard11_1(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE HandleType, VOID *hResourceOrView,
            const D3D10_DDI_RECT *pRects, UINT NumRects)
{
}

static void APIENTRY
AssignDebugBinary11_1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, UINT uBinarySize,
                      const VOID *pBinary)
{
}

static void APIENTRY
CheckDirectFlipSupport11_1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                           D3D10DDI_HRESOURCE hResourceDWM, UINT Flags, BOOL *pSupported)
{
   *pSupported = FALSE;
}

static void APIENTRY
ClearView11_1(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE viewType, VOID *hView, const FLOAT Color[4],
              const D3D10_DDI_RECT *pRects, UINT NumRects)
{
   LOG_ENTRYPOINT();
   if (viewType != D3D10DDI_HT_RENDERTARGETVIEW) {
      LOG_UNSUPPORTED(viewType != D3D10DDI_HT_RENDERTARGETVIEW);
      return;
   }
   D3D10DDI_HRENDERTARGETVIEW hRTV;
   hRTV.pDrvPrivate = hView;
   if (NumRects == 0) {
      FLOAT c[4] = { Color[0], Color[1], Color[2], Color[3] };
      ClearRenderTargetView(hDevice, hRTV, c);
      return;
   }
   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_surface *surface = CastPipeRenderTargetView(hRTV);
   union pipe_color_union cc;
   memcpy(cc.f, Color, sizeof cc.f);
   for (UINT i = 0; i < NumRects; i++) {
      const D3D10_DDI_RECT *r = &pRects[i];
      if (r->right <= r->left || r->bottom <= r->top)
         continue;
      pipe->clear_render_target(pipe, surface, &cc, r->left, r->top, r->right - r->left,
                                r->bottom - r->top, false);
   }
}

void
FillD3D11_1DeviceFuncs(D3D11_1DDI_DEVICEFUNCS *f)
{
   /* same layout as the 11.0 table up to pfnRecycleDestroyCommandList */
   FillD3D11DeviceFuncs((D3D11DDI_DEVICEFUNCS *)f);

   f->pfnDefaultConstantBufferUpdateSubresourceUP = DDIW(ResourceUpdateSubresourceUP11_1);
   f->pfnResourceUpdateSubresourceUP = DDIW(ResourceUpdateSubresourceUP11_1);
   f->pfnVsSetConstantBuffers = DDIW(VsSetConstantBuffers11_1);
   f->pfnPsSetConstantBuffers = DDIW(PsSetConstantBuffers11_1);
   f->pfnGsSetConstantBuffers = DDIW(GsSetConstantBuffers11_1);
   f->pfnHsSetConstantBuffers = DDIW(NopSetConstantBuffers11_1);
   f->pfnDsSetConstantBuffers = DDIW(NopSetConstantBuffers11_1);
   f->pfnCsSetConstantBuffers = DDIW(NopSetConstantBuffers11_1);
   f->pfnResourceCopyRegion = DDIW(ResourceCopyRegion11_1);
   f->pfnResourceConvertRegion = DDIW(ResourceCopyRegion11_1);
   f->pfnFlush = DDIW(Flush11_1);
   f->pfnRelocateDeviceFuncs = DDIW(RelocateDeviceFuncs11_1);
   f->pfnCalcPrivateBlendStateSize = DDIW(CalcPrivateBlendStateSize11_1);
   f->pfnCreateBlendState = DDIW(CreateBlendState11_1);
   f->pfnCalcPrivateRasterizerStateSize = DDIW(CalcPrivateRasterizerStateSize11_1);
   f->pfnCreateRasterizerState = DDIW(CreateRasterizerState11_1);
   f->pfnCalcPrivateShaderSize = DDIW(CalcPrivateShaderSize11_1);
   f->pfnCreateVertexShader = DDIW(CreateVertexShader11_1);
   f->pfnCreateGeometryShader = DDIW(CreateGeometryShader11_1);
   f->pfnCreatePixelShader = DDIW(CreatePixelShader11_1);
   f->pfnCalcPrivateGeometryShaderWithStreamOutput = DDIW(CalcPrivateGeometryShaderWithStreamOutput11_1);
   f->pfnCreateGeometryShaderWithStreamOutput = DDIW(CreateGeometryShaderWithStreamOutput11_1);
   f->pfnCalcPrivateTessellationShaderSize = DDIW(CalcPrivateTessellationShaderSize11_1);
   f->pfnCreateHullShader = DDIW(CreateTessellationShader11_1);
   f->pfnCreateDomainShader = DDIW(CreateTessellationShader11_1);
   f->pfnDiscard = DDIW(Discard11_1);
   f->pfnAssignDebugBinary = DDIW(AssignDebugBinary11_1);
   f->pfnDynamicConstantBufferMapNoOverwrite = DDIW(ResourceMap);
   f->pfnCheckDirectFlipSupport = DDIW(CheckDirectFlipSupport11_1);
   f->pfnClearView = DDIW(ClearView11_1);
}
