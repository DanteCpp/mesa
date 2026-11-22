/*
 * D3D11 DDI (D3D11_0_DDI_INTERFACE_VERSION) on top of the D3D10 frontend.
 *
 * The device reports 3D pipeline levels 10_0 and 10_1 (D3D11DDICAPS_3DPIPELINESUPPORT), so the D3D11
 * runtime creates feature level 10_x devices through the D3D11 DDI: HS/DS/CS, UAVs, indirect
 * draws and command lists are never used at that level and are accepted as no-ops (the
 * runtime emulates deferred contexts when D3D11DDICAPS_THREADING reports 0).
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
#include "Perf.h"

static void
ToD3D10(const D3D11DDIARG_CREATERESOURCE *in, D3D10DDIARG_CREATERESOURCE *out)
{
   out->pMipInfoList = in->pMipInfoList;
   out->pInitialDataUP = in->pInitialDataUP;
   out->ResourceDimension = in->ResourceDimension;
   out->Usage = in->Usage;
   out->BindFlags = in->BindFlags;
   out->MapFlags = in->MapFlags;
   out->MiscFlags = in->MiscFlags;
   out->Format = in->Format;
   out->SampleDesc = in->SampleDesc;
   out->MipLevels = in->MipLevels;
   out->ArraySize = in->ArraySize;
   out->pPrimaryDesc = in->pPrimaryDesc;
}

static SIZE_T APIENTRY
CalcPrivateResourceSize11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATERESOURCE *pCreate)
{
   D3D10DDIARG_CREATERESOURCE c;
   ToD3D10(pCreate, &c);
   return CalcPrivateResourceSize(hDevice, &c);
}

static void APIENTRY
CreateResource11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATERESOURCE *pCreate,
                 D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRTResource)
{
   LOG_ENTRYPOINT();
   D3D10DDIARG_CREATERESOURCE c;
   ToD3D10(pCreate, &c);
   CreateResource(hDevice, &c, hResource, hRTResource);
}

static SIZE_T APIENTRY
CalcPrivateShaderResourceViewSize11(D3D10DDI_HDEVICE hDevice,
                                    const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pCreate)
{
   return CalcPrivateShaderResourceViewSize1(hDevice, (const D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *)pCreate);
}

static void APIENTRY
CreateShaderResourceView11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pCreate,
                           D3D10DDI_HSHADERRESOURCEVIEW hView, D3D10DDI_HRTSHADERRESOURCEVIEW hRTView)
{
   LOG_ENTRYPOINT();
   D3D10_1DDIARG_CREATESHADERRESOURCEVIEW c;
   memset(&c, 0, sizeof c);
   c.hDrvResource = pCreate->hDrvResource;
   c.Format = pCreate->Format;
   c.ResourceDimension = pCreate->ResourceDimension;
   switch (pCreate->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      c.Buffer = pCreate->Buffer;
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      c.Tex1D = pCreate->Tex1D;
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      c.Tex2D = pCreate->Tex2D;
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      c.Tex3D = pCreate->Tex3D;
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      c.TexCube = pCreate->TexCube;
      break;
   case D3D11DDIRESOURCE_BUFFEREX:
      c.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
      c.Buffer.FirstElement = pCreate->BufferEx.FirstElement;
      c.Buffer.NumElements = pCreate->BufferEx.NumElements;
      break;
   default:
      break;
   }
   CreateShaderResourceView1(hDevice, &c, hView, hRTView);
}

static void
ToD3D10(const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *in, D3D10DDIARG_CREATEDEPTHSTENCILVIEW *out)
{
   memset(out, 0, sizeof *out);
   out->hDrvResource = in->hDrvResource;
   out->Format = in->Format;
   out->ResourceDimension = in->ResourceDimension;
   out->Tex2D = in->Tex2D; /* largest member of the union */
}

static SIZE_T APIENTRY
CalcPrivateDepthStencilViewSize11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pCreate)
{
   D3D10DDIARG_CREATEDEPTHSTENCILVIEW c;
   ToD3D10(pCreate, &c);
   return CalcPrivateDepthStencilViewSize(hDevice, &c);
}

static void APIENTRY
CreateDepthStencilView11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pCreate,
                         D3D10DDI_HDEPTHSTENCILVIEW hView, D3D10DDI_HRTDEPTHSTENCILVIEW hRTView)
{
   LOG_ENTRYPOINT();
   D3D10DDIARG_CREATEDEPTHSTENCILVIEW c;
   ToD3D10(pCreate, &c);
   CreateDepthStencilView(hDevice, &c, hView, hRTView);
}

#define MAX_SO_DECL 128

static bool
ToD3D10(const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *in,
        D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *out,
        D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *decl)
{
   UINT n = 0;
   for (UINT i = 0; i < in->NumEntries && n < MAX_SO_DECL; i++) {
      if (in->pOutputStreamDecl[i].Stream != 0)
         continue;
      decl[n].OutputSlot = in->pOutputStreamDecl[i].OutputSlot;
      decl[n].RegisterIndex = in->pOutputStreamDecl[i].RegisterIndex;
      decl[n].RegisterMask = in->pOutputStreamDecl[i].RegisterMask;
      n++;
   }
   out->pShaderCode = in->pShaderCode;
   out->pOutputStreamDecl = decl;
   out->NumEntries = n;
   out->StreamOutputStrideInBytes = in->NumStrides ? in->BufferStridesInBytes[0] : 0;
   return true;
}

static SIZE_T APIENTRY
CalcPrivateGeometryShaderWithStreamOutput11(D3D10DDI_HDEVICE hDevice,
                                            const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pCreate,
                                            const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
   D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY decl[MAX_SO_DECL];
   D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT c;
   ToD3D10(pCreate, &c, decl);
   return CalcPrivateGeometryShaderWithStreamOutput(hDevice, &c, pSignatures);
}

static void APIENTRY
CreateGeometryShaderWithStreamOutput11(D3D10DDI_HDEVICE hDevice,
                                       const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pCreate,
                                       D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader,
                                       const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
   LOG_ENTRYPOINT();
   D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY decl[MAX_SO_DECL];
   D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT c;
   ToD3D10(pCreate, &c, decl);
   CreateGeometryShaderWithStreamOutput(hDevice, &c, hShader, hRTShader, pSignatures);
}

static void APIENTRY
SetRenderTargets11(D3D10DDI_HDEVICE hDevice, const D3D10DDI_HRENDERTARGETVIEW *phRTV, UINT NumRTVs,
                   UINT ClearSlots, D3D10DDI_HDEPTHSTENCILVIEW hDSV,
                   const D3D11DDI_HUNORDEREDACCESSVIEW *phUAV, const UINT *pUAVInitialCounts,
                   UINT UAVStartSlot, UINT NumUAVs, UINT UAVRangeStart, UINT UAVRangeSize)
{
   LOG_ENTRYPOINT();
   SetRenderTargets(hDevice, phRTV, NumRTVs, ClearSlots, hDSV);
}

static void APIENTRY
RelocateDeviceFuncs11(D3D10DDI_HDEVICE hDevice, struct D3D11DDI_DEVICEFUNCS *pDeviceFunctions)
{
}

/* Feature level 10_0: the runtime never reaches the stages below; keep them harmless. */

static SIZE_T APIENTRY
CalcPrivateTessellationShaderSize11(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode,
                                    const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)
{
   return sizeof(Shader);
}

static void
NullShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
   Shader *pShader = CastShader(hShader);
   memset(pShader, 0, sizeof *pShader);
   SetError(hDevice, E_NOTIMPL);
}

static void APIENTRY
CreateHullShader11(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode, D3D10DDI_HSHADER hShader,
                   D3D10DDI_HRTSHADER hRTShader, const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   NullShader(hDevice, hShader);
}

static void APIENTRY
CreateDomainShader11(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode, D3D10DDI_HSHADER hShader,
                     D3D10DDI_HRTSHADER hRTShader, const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   NullShader(hDevice, hShader);
}

static void APIENTRY
CreateComputeShader11(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode, D3D10DDI_HSHADER hShader,
                      D3D10DDI_HRTSHADER hRTShader)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   NullShader(hDevice, hShader);
}

static void APIENTRY
SetShaderNop(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
}

static void APIENTRY
SetShaderResourcesNop(D3D10DDI_HDEVICE hDevice, UINT Offset, UINT NumViews,
                      const D3D10DDI_HSHADERRESOURCEVIEW *phViews)
{
}

static void APIENTRY
SetSamplersNop(D3D10DDI_HDEVICE hDevice, UINT Offset, UINT NumSamplers, const D3D10DDI_HSAMPLER *phSamplers)
{
}

static void APIENTRY
SetConstantBuffersNop(D3D10DDI_HDEVICE hDevice, UINT StartBuffer, UINT NumBuffers,
                      const D3D10DDI_HRESOURCE *phBuffers)
{
}

static void APIENTRY
VsSetShaderWithIfaces11(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, UINT NumClassInstances,
                        const UINT *pIfaces, const D3D11DDIARG_POINTERDATA *pPointerData)
{
   VsSetShader(hDevice, hShader);
}

static void APIENTRY
PsSetShaderWithIfaces11(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, UINT NumClassInstances,
                        const UINT *pIfaces, const D3D11DDIARG_POINTERDATA *pPointerData)
{
   PsSetShader(hDevice, hShader);
}

static void APIENTRY
GsSetShaderWithIfaces11(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, UINT NumClassInstances,
                        const UINT *pIfaces, const D3D11DDIARG_POINTERDATA *pPointerData)
{
   GsSetShader(hDevice, hShader);
}

static void APIENTRY
NopSetShaderWithIfaces11(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, UINT NumClassInstances,
                         const UINT *pIfaces, const D3D11DDIARG_POINTERDATA *pPointerData)
{
}

static void APIENTRY
DrawIndirectNop(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hBuffer, UINT Offset)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
}

static void APIENTRY
DispatchNop(D3D10DDI_HDEVICE hDevice, UINT x, UINT y, UINT z)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
}

static SIZE_T APIENTRY
CalcPrivateUnorderedAccessViewSize11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *p)
{
   return sizeof(void *);
}

static void APIENTRY
CreateUnorderedAccessView11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *p,
                            D3D11DDI_HUNORDEREDACCESSVIEW hView, D3D11DDI_HRTUNORDEREDACCESSVIEW hRTView)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   SetError(hDevice, E_NOTIMPL);
}

static void APIENTRY
DestroyUnorderedAccessView11(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView)
{
}

static void APIENTRY
ClearUnorderedAccessViewUint11(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView, const UINT v[4])
{
}

static void APIENTRY
ClearUnorderedAccessViewFloat11(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hView, const FLOAT v[4])
{
}

static void APIENTRY
CsSetUnorderedAccessViews11(D3D10DDI_HDEVICE hDevice, UINT StartSlot, UINT NumViews,
                            const D3D11DDI_HUNORDEREDACCESSVIEW *phViews, const UINT *pCounts)
{
}

static void APIENTRY
SetResourceMinLOD11(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource, FLOAT MinLOD)
{
}

static void APIENTRY
CopyStructureCount11(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst, UINT Offset,
                     D3D11DDI_HUNORDEREDACCESSVIEW hSrc)
{
}

static void APIENTRY
CheckDeferredContextHandleSizes11(D3D10DDI_HDEVICE hDevice, UINT *pHSizes, D3D11DDI_HANDLESIZE *pHandleSize)
{
   *pHSizes = 0;
}

static SIZE_T APIENTRY
CalcDeferredContextHandleSize11(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE HandleType, VOID *pHandle)
{
   return 0;
}

static SIZE_T APIENTRY
CalcPrivateDeferredContextSize11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *p)
{
   return sizeof(Device);
}

static void APIENTRY
CreateDeferredContext11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEDEFERREDCONTEXT *p)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   SetError(hDevice, E_NOTIMPL);
}

static HRESULT APIENTRY
RecycleCreateDeferredContext11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEDEFERREDCONTEXT *p)
{
   return E_NOTIMPL;
}

static void APIENTRY
AbandonCommandList11(D3D10DDI_HDEVICE hDevice)
{
}

static SIZE_T APIENTRY
CalcPrivateCommandListSize11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATECOMMANDLIST *p)
{
   return sizeof(void *);
}

static void APIENTRY
CreateCommandList11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATECOMMANDLIST *p,
                    D3D11DDI_HCOMMANDLIST hList, D3D11DDI_HRTCOMMANDLIST hRTList)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   SetError(hDevice, E_NOTIMPL);
}

static HRESULT APIENTRY
RecycleCreateCommandList11(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATECOMMANDLIST *p,
                           D3D11DDI_HCOMMANDLIST hList, D3D11DDI_HRTCOMMANDLIST hRTList)
{
   return E_NOTIMPL;
}

static void APIENTRY
CommandListNop11(D3D10DDI_HDEVICE hDevice, D3D11DDI_HCOMMANDLIST hList)
{
}

void
D3D11_1CreateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE hDevice,
                                            const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pCreate,
                                            D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader)
{
   CreateGeometryShaderWithStreamOutput11(hDevice, pCreate, hShader, hRTShader, NULL);
}

void
FillD3D11DeviceFuncs(D3D11DDI_DEVICEFUNCS *f)
{
   f->pfnDefaultConstantBufferUpdateSubresourceUP = DDIW(ResourceUpdateSubResourceUP);
   f->pfnVsSetConstantBuffers = DDIW(VsSetConstantBuffers);
   f->pfnPsSetShaderResources = DDIW(PsSetShaderResources);
   f->pfnPsSetShader = DDIW(PsSetShader);
   f->pfnPsSetSamplers = DDIW(PsSetSamplers);
   f->pfnVsSetShader = DDIW(VsSetShader);
   f->pfnDrawIndexed = DDIW(DrawIndexed);
   f->pfnDraw = DDIW(Draw);
   f->pfnDynamicIABufferMapNoOverwrite = DDIW(ResourceMap);
   f->pfnDynamicIABufferUnmap = DDIW(ResourceUnmap);
   f->pfnDynamicConstantBufferMapDiscard = DDIW(ResourceMap);
   f->pfnDynamicIABufferMapDiscard = DDIW(ResourceMap);
   f->pfnDynamicConstantBufferUnmap = DDIW(ResourceUnmap);
   f->pfnPsSetConstantBuffers = DDIW(PsSetConstantBuffers);
   f->pfnIaSetInputLayout = DDIW(IaSetInputLayout);
   f->pfnIaSetVertexBuffers = DDIW(IaSetVertexBuffers);
   f->pfnIaSetIndexBuffer = DDIW(IaSetIndexBuffer);
   f->pfnDrawIndexedInstanced = DDIW(DrawIndexedInstanced);
   f->pfnDrawInstanced = DDIW(DrawInstanced);
   f->pfnDynamicResourceMapDiscard = DDIW(ResourceMap);
   f->pfnDynamicResourceUnmap = DDIW(ResourceUnmap);
   f->pfnGsSetConstantBuffers = DDIW(GsSetConstantBuffers);
   f->pfnGsSetShader = DDIW(GsSetShader);
   f->pfnIaSetTopology = DDIW(IaSetTopology);
   f->pfnStagingResourceMap = DDIW(ResourceMap);
   f->pfnStagingResourceUnmap = DDIW(ResourceUnmap);
   f->pfnVsSetShaderResources = DDIW(VsSetShaderResources);
   f->pfnVsSetSamplers = DDIW(VsSetSamplers);
   f->pfnGsSetShaderResources = DDIW(GsSetShaderResources);
   f->pfnGsSetSamplers = DDIW(GsSetSamplers);
   f->pfnShaderResourceViewReadAfterWriteHazard = DDIW(ShaderResourceViewReadAfterWriteHazard);
   f->pfnResourceReadAfterWriteHazard = DDIW(ResourceReadAfterWriteHazard);
   f->pfnSetBlendState = DDIW(SetBlendState);
   f->pfnSetDepthStencilState = DDIW(SetDepthStencilState);
   f->pfnSetRasterizerState = DDIW(SetRasterizerState);
   f->pfnQueryEnd = DDIW(QueryEnd);
   f->pfnQueryBegin = DDIW(QueryBegin);
   f->pfnResourceCopyRegion = DDIW(ResourceCopyRegion);
   f->pfnResourceUpdateSubresourceUP = DDIW(ResourceUpdateSubResourceUP);
   f->pfnSoSetTargets = DDIW(SoSetTargets);
   f->pfnDrawAuto = DDIW(DrawAuto);
   f->pfnSetViewports = DDIW(SetViewports);
   f->pfnSetScissorRects = DDIW(SetScissorRects);
   f->pfnClearRenderTargetView = DDIW(ClearRenderTargetView);
   f->pfnClearDepthStencilView = DDIW(ClearDepthStencilView);
   f->pfnSetPredication = DDIW(SetPredication);
   f->pfnQueryGetData = DDIW(QueryGetData);
   f->pfnFlush = DDIW(Flush);
   f->pfnGenMips = DDIW(GenMips);
   f->pfnResourceCopy = DDIW(ResourceCopy);
   f->pfnResourceResolveSubresource = DDIW(ResourceResolveSubResource);
   f->pfnResourceMap = DDIW(ResourceMap);
   f->pfnResourceUnmap = DDIW(ResourceUnmap);
   f->pfnResourceIsStagingBusy = DDIW(ResourceIsStagingBusy);
   f->pfnCalcPrivateOpenedResourceSize = DDIW(CalcPrivateOpenedResourceSize);
   f->pfnOpenResource = DDIW(OpenResource);
   f->pfnDestroyResource = DDIW(DestroyResource);
   f->pfnDestroyShaderResourceView = DDIW(DestroyShaderResourceView);
   f->pfnCalcPrivateRenderTargetViewSize = DDIW(CalcPrivateRenderTargetViewSize);
   f->pfnCreateRenderTargetView = DDIW(CreateRenderTargetView);
   f->pfnDestroyRenderTargetView = DDIW(DestroyRenderTargetView);
   f->pfnDestroyDepthStencilView = DDIW(DestroyDepthStencilView);
   f->pfnCalcPrivateElementLayoutSize = DDIW(CalcPrivateElementLayoutSize);
   f->pfnCreateElementLayout = DDIW(CreateElementLayout);
   f->pfnDestroyElementLayout = DDIW(DestroyElementLayout);
   f->pfnDestroyBlendState = DDIW(DestroyBlendState);
   f->pfnCalcPrivateDepthStencilStateSize = DDIW(CalcPrivateDepthStencilStateSize);
   f->pfnCreateDepthStencilState = DDIW(CreateDepthStencilState);
   f->pfnDestroyDepthStencilState = DDIW(DestroyDepthStencilState);
   f->pfnCalcPrivateRasterizerStateSize = DDIW(CalcPrivateRasterizerStateSize);
   f->pfnCreateRasterizerState = DDIW(CreateRasterizerState);
   f->pfnDestroyRasterizerState = DDIW(DestroyRasterizerState);
   f->pfnCalcPrivateShaderSize = DDIW(CalcPrivateShaderSize);
   f->pfnCreateVertexShader = DDIW(CreateVertexShader);
   f->pfnCreateGeometryShader = DDIW(CreateGeometryShader);
   f->pfnCreatePixelShader = DDIW(CreatePixelShader);
   f->pfnDestroyShader = DDIW(DestroyShader);
   f->pfnCalcPrivateSamplerSize = DDIW(CalcPrivateSamplerSize);
   f->pfnCreateSampler = DDIW(CreateSampler);
   f->pfnDestroySampler = DDIW(DestroySampler);
   f->pfnCalcPrivateQuerySize = DDIW(CalcPrivateQuerySize);
   f->pfnCreateQuery = DDIW(CreateQuery);
   f->pfnDestroyQuery = DDIW(DestroyQuery);
   f->pfnCheckFormatSupport = DDIW(CheckFormatSupport);
   f->pfnCheckMultisampleQualityLevels = DDIW(CheckMultisampleQualityLevels);
   f->pfnCheckCounterInfo = DDIW(CheckCounterInfo);
   f->pfnCheckCounter = DDIW(CheckCounter);
   f->pfnDestroyDevice = DDIW(DestroyDevice);
   f->pfnSetTextFilterSize = DDIW(SetTextFilterSize);
   f->pfnResourceConvert = DDIW(ResourceCopy);
   f->pfnResourceConvertRegion = DDIW(ResourceCopyRegion);
   f->pfnCalcPrivateBlendStateSize = DDIW(CalcPrivateBlendStateSize1);
   f->pfnCreateBlendState = DDIW(CreateBlendState1);
   f->pfnSetRenderTargets = DDIW(SetRenderTargets11);
   f->pfnRelocateDeviceFuncs = DDIW(RelocateDeviceFuncs11);
   f->pfnCalcPrivateResourceSize = DDIW(CalcPrivateResourceSize11);
   f->pfnCreateResource = DDIW(CreateResource11);
   f->pfnCalcPrivateShaderResourceViewSize = DDIW(CalcPrivateShaderResourceViewSize11);
   f->pfnCreateShaderResourceView = DDIW(CreateShaderResourceView11);
   f->pfnCalcPrivateDepthStencilViewSize = DDIW(CalcPrivateDepthStencilViewSize11);
   f->pfnCreateDepthStencilView = DDIW(CreateDepthStencilView11);
   f->pfnCalcPrivateGeometryShaderWithStreamOutput = DDIW(CalcPrivateGeometryShaderWithStreamOutput11);
   f->pfnCreateGeometryShaderWithStreamOutput = DDIW(CreateGeometryShaderWithStreamOutput11);
   f->pfnDrawIndexedInstancedIndirect = DDIW(DrawIndirectNop);
   f->pfnDrawInstancedIndirect = DDIW(DrawIndirectNop);
   f->pfnCommandListExecute = DDIW(CommandListNop11);
   f->pfnHsSetShaderResources = DDIW(SetShaderResourcesNop);
   f->pfnHsSetShader = DDIW(SetShaderNop);
   f->pfnHsSetSamplers = DDIW(SetSamplersNop);
   f->pfnHsSetConstantBuffers = DDIW(SetConstantBuffersNop);
   f->pfnDsSetShaderResources = DDIW(SetShaderResourcesNop);
   f->pfnDsSetShader = DDIW(SetShaderNop);
   f->pfnDsSetSamplers = DDIW(SetSamplersNop);
   f->pfnDsSetConstantBuffers = DDIW(SetConstantBuffersNop);
   f->pfnCsSetShaderResources = DDIW(SetShaderResourcesNop);
   f->pfnCsSetShader = DDIW(SetShaderNop);
   f->pfnCsSetSamplers = DDIW(SetSamplersNop);
   f->pfnCsSetConstantBuffers = DDIW(SetConstantBuffersNop);
   f->pfnCreateHullShader = DDIW(CreateHullShader11);
   f->pfnCreateDomainShader = DDIW(CreateDomainShader11);
   f->pfnCheckDeferredContextHandleSizes = DDIW(CheckDeferredContextHandleSizes11);
   f->pfnCalcDeferredContextHandleSize = DDIW(CalcDeferredContextHandleSize11);
   f->pfnCalcPrivateDeferredContextSize = DDIW(CalcPrivateDeferredContextSize11);
   f->pfnCreateDeferredContext = DDIW(CreateDeferredContext11);
   f->pfnAbandonCommandList = DDIW(AbandonCommandList11);
   f->pfnCalcPrivateCommandListSize = DDIW(CalcPrivateCommandListSize11);
   f->pfnCreateCommandList = DDIW(CreateCommandList11);
   f->pfnDestroyCommandList = DDIW(CommandListNop11);
   f->pfnCalcPrivateTessellationShaderSize = DDIW(CalcPrivateTessellationShaderSize11);
   f->pfnPsSetShaderWithIfaces = DDIW(PsSetShaderWithIfaces11);
   f->pfnVsSetShaderWithIfaces = DDIW(VsSetShaderWithIfaces11);
   f->pfnGsSetShaderWithIfaces = DDIW(GsSetShaderWithIfaces11);
   f->pfnHsSetShaderWithIfaces = DDIW(NopSetShaderWithIfaces11);
   f->pfnDsSetShaderWithIfaces = DDIW(NopSetShaderWithIfaces11);
   f->pfnCsSetShaderWithIfaces = DDIW(NopSetShaderWithIfaces11);
   f->pfnCreateComputeShader = DDIW(CreateComputeShader11);
   f->pfnCalcPrivateUnorderedAccessViewSize = DDIW(CalcPrivateUnorderedAccessViewSize11);
   f->pfnCreateUnorderedAccessView = DDIW(CreateUnorderedAccessView11);
   f->pfnDestroyUnorderedAccessView = DDIW(DestroyUnorderedAccessView11);
   f->pfnClearUnorderedAccessViewUint = DDIW(ClearUnorderedAccessViewUint11);
   f->pfnClearUnorderedAccessViewFloat = DDIW(ClearUnorderedAccessViewFloat11);
   f->pfnCsSetUnorderedAccessViews = DDIW(CsSetUnorderedAccessViews11);
   f->pfnDispatch = DDIW(DispatchNop);
   f->pfnDispatchIndirect = DDIW(DrawIndirectNop);
   f->pfnSetResourceMinLOD = DDIW(SetResourceMinLOD11);
   f->pfnCopyStructureCount = DDIW(CopyStructureCount11);
   f->pfnRecycleCommandList = DDIW(CommandListNop11);
   f->pfnRecycleCreateCommandList = DDIW(RecycleCreateCommandList11);
   f->pfnRecycleCreateDeferredContext = DDIW(RecycleCreateDeferredContext11);
   f->pfnRecycleDestroyCommandList = DDIW(CommandListNop11);
}
