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
 * Device.cpp --
 *    Functions that provide the 3D device functionality.
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
#include "Wddm.h"

#include "Debug.h"

#include "util/u_sampler.h"
#include "util/u_framebuffer.h"
#include "Perf.h"


void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);
static void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions);
void APIENTRY Flush(D3D10DDI_HDEVICE hDevice);
void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                               __out UINT *pFormatCaps);
void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                          DXGI_FORMAT Format,
                                          UINT SampleCount,
                                          __out UINT *pNumQualityLevels);
void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height);


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateDeviceSize --
 *
 *    The CalcPrivateDeviceSize function determines the size of a memory
 *    region that the user-mode display driver requires from the Microsoft
 *    Direct3D runtime to store frequently-accessed data.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,                          // IN
                      __in const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData) // IN
{
   return sizeof(Device);
}

/*
 * ----------------------------------------------------------------------
 *
 * CreateDevice --
 *
 *    The CreateDevice function creates a graphics context that is
 *    referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
CreateDevice(D3D10DDI_HADAPTER hAdapter,                 // IN
             __in D3D10DDIARG_CREATEDEVICE *pCreateData) // IN
{
   LOG_ENTRYPOINT();

   if (UmdTrace()) {
      DebugPrintf("CreateDevice Interface 0x%08x Version 0x%08x Flags 0x%x\n",
                  pCreateData->Interface, pCreateData->Version, pCreateData->Flags);
      DebugPrintf("hAdapter = %p\n", hAdapter.pDrvPrivate);
      DebugPrintf("pKTCallbacks = %p\n", pCreateData->pKTCallbacks);
      DebugPrintf("pDeviceFuncs = %p\n", (void *)pCreateData->p10_1DeviceFuncs);
      DebugPrintf("hDrvDevice = %p\n", pCreateData->hDrvDevice.pDrvPrivate);
      DebugPrintf("DXGI base funcs = %p\n", (void *)pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions);
      DebugPrintf("hRTCoreLayer = %p\n", pCreateData->hRTCoreLayer.handle);
      DebugPrintf("pUMCallbacks = %p\n", (void *)pCreateData->pUMCallbacks);
   }

   switch (pCreateData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
#if SUPPORT_D3D11
   case D3D11_0_DDI_INTERFACE_VERSION:
   case D3D11_0_7_DDI_INTERFACE_VERSION:
   case D3D11_1_DDI_INTERFACE_VERSION:
#endif
      break;
   default:
      DebugPrintf("%s: unsupported interface version 0x%08x\n",
                  __func__, pCreateData->Interface);
      return E_FAIL;
   }

   Adapter *pAdapter = CastAdapter(hAdapter);

   Device *pDevice = CastDevice(pCreateData->hDrvDevice);
   memset(pDevice, 0, sizeof *pDevice);

   struct pipe_screen *screen = pAdapter->screen;
   struct pipe_context *pipe = NULL;
   for (unsigned attempt = 0; attempt < 3 && !pipe; attempt++) {
      pipe = screen->context_create(screen, NULL, 0);
      if (!pipe) {
         DebugPrintf("CreateDevice: context_create failed (attempt %u)\n", attempt);
         Sleep(20);
      }
   }
   if (!pipe)
      return E_OUTOFMEMORY;
   pDevice->pipe = pipe;
   /* softpipe/llvmpipe take user vertex buffers; GPU screens (zink) need u_vbuf */
   pDevice->cso = cso_create_context(pipe, pAdapter->dt_capable ? CSO_NO_VBUF : 0);
   if (!pDevice->cso) {
      DebugPrintf("CreateDevice: cso_create_context failed\n");
      pipe->destroy(pipe);
      return E_OUTOFMEMORY;
   }

   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);
   pDevice->empty_fs = CreateEmptyShader(pDevice, MESA_SHADER_FRAGMENT);

   pipe->bind_vs_state(pipe, pDevice->empty_vs);
   pipe->bind_fs_state(pipe, pDevice->empty_fs);

   pDevice->max_dual_source_render_targets =
         screen->caps.max_dual_source_render_targets;

   pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
   pDevice->hDevice = (HANDLE)pCreateData->hRTDevice.handle;
   pDevice->KTCallbacks = *pCreateData->pKTCallbacks;
   /* G6 r7: callback times for the per-frame profile (Perf.h) */
   PerfInit();
   if (g_perfOn) {
      CBW(1, pDevice->KTCallbacks, pfnAllocateCb);
      CBW(2, pDevice->KTCallbacks, pfnDeallocateCb);
      CBW(3, pDevice->KTCallbacks, pfnLockCb);
      CBW(4, pDevice->KTCallbacks, pfnUnlockCb);
      CBW(5, pDevice->KTCallbacks, pfnRenderCb);
      CBW(6, pDevice->KTCallbacks, pfnSetDisplayModeCb);
   }
   pDevice->UMCallbacks = *pCreateData->pUMCallbacks;
   pDevice->pDXGIBaseCallbacks = pCreateData->DXGIBaseDDI.pDXGIBaseCallbacks;
   pDevice->wddm = pAdapter->wddm;
   pDevice->dt_capable = pAdapter->dt_capable;
   pDevice->pitch_align = pAdapter->pitch_align;
   pDevice->zc = pAdapter->wddm && pAdapter->zc_kmd; /* WddmCreateDevice decides (software screens: zc.sim) */
   pDevice->scanW = pAdapter->scan_w;
   pDevice->scanH = pAdapter->scan_h;
   pDevice->segmentSize = pAdapter->segment_size;
   pDevice->hRTAdapter = pAdapter->hRTAdapter;
   if (pDevice->wddm) {
      HRESULT hr = WddmCreateDevice(pDevice);
      if (FAILED(hr))
         return hr;
   }

   pDevice->draw_so_target = NULL;

   if (0) {
      DebugPrintf("pDevice = %p\n", pDevice);
   }

   st_debug_parse();

   bool d3d11 = pCreateData->Interface == D3D11_0_DDI_INTERFACE_VERSION ||
                pCreateData->Interface == D3D11_0_7_DDI_INTERFACE_VERSION;
   bool d3d11_1 = pCreateData->Interface == D3D11_1_DDI_INTERFACE_VERSION;
   if (d3d11_1) {
      FillD3D11_1DeviceFuncs(pCreateData->p11_1DeviceFuncs);
   } else if (d3d11) {
      FillD3D11DeviceFuncs(pCreateData->p11DeviceFuncs);
   } else {
   /*
    * Fill in the D3D10 DDI functions
    */
   D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = DDIW(ResourceUpdateSubResourceUP);
   pDeviceFuncs->pfnVsSetConstantBuffers = DDIW(VsSetConstantBuffers);
   pDeviceFuncs->pfnPsSetShaderResources = DDIW(PsSetShaderResources);
   pDeviceFuncs->pfnPsSetShader = DDIW(PsSetShader);
   pDeviceFuncs->pfnPsSetSamplers = DDIW(PsSetSamplers);
   pDeviceFuncs->pfnVsSetShader = DDIW(VsSetShader);
   pDeviceFuncs->pfnDrawIndexed = DDIW(DrawIndexed);
   pDeviceFuncs->pfnDraw = DDIW(Draw);
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = DDIW(ResourceMap);
   pDeviceFuncs->pfnDynamicIABufferUnmap = DDIW(ResourceUnmap);
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = DDIW(ResourceMap);
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = DDIW(ResourceMap);
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = DDIW(ResourceUnmap);
   pDeviceFuncs->pfnPsSetConstantBuffers = DDIW(PsSetConstantBuffers);
   pDeviceFuncs->pfnIaSetInputLayout = DDIW(IaSetInputLayout);
   pDeviceFuncs->pfnIaSetVertexBuffers = DDIW(IaSetVertexBuffers);
   pDeviceFuncs->pfnIaSetIndexBuffer = DDIW(IaSetIndexBuffer);
   pDeviceFuncs->pfnDrawIndexedInstanced = DDIW(DrawIndexedInstanced);
   pDeviceFuncs->pfnDrawInstanced = DDIW(DrawInstanced);
   pDeviceFuncs->pfnDynamicResourceMapDiscard = DDIW(ResourceMap);
   pDeviceFuncs->pfnDynamicResourceUnmap = DDIW(ResourceUnmap);
   pDeviceFuncs->pfnGsSetConstantBuffers = DDIW(GsSetConstantBuffers);
   pDeviceFuncs->pfnGsSetShader = DDIW(GsSetShader);
   pDeviceFuncs->pfnIaSetTopology = DDIW(IaSetTopology);
   pDeviceFuncs->pfnStagingResourceMap = DDIW(ResourceMap);
   pDeviceFuncs->pfnStagingResourceUnmap = DDIW(ResourceUnmap);
   pDeviceFuncs->pfnVsSetShaderResources = DDIW(VsSetShaderResources);
   pDeviceFuncs->pfnVsSetSamplers = DDIW(VsSetSamplers);
   pDeviceFuncs->pfnGsSetShaderResources = DDIW(GsSetShaderResources);
   pDeviceFuncs->pfnGsSetSamplers = DDIW(GsSetSamplers);
   pDeviceFuncs->pfnSetRenderTargets = DDIW(SetRenderTargets);
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = DDIW(ShaderResourceViewReadAfterWriteHazard);
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = DDIW(ResourceReadAfterWriteHazard);
   pDeviceFuncs->pfnSetBlendState = DDIW(SetBlendState);
   pDeviceFuncs->pfnSetDepthStencilState = DDIW(SetDepthStencilState);
   pDeviceFuncs->pfnSetRasterizerState = DDIW(SetRasterizerState);
   pDeviceFuncs->pfnQueryEnd = DDIW(QueryEnd);
   pDeviceFuncs->pfnQueryBegin = DDIW(QueryBegin);
   pDeviceFuncs->pfnResourceCopyRegion = DDIW(ResourceCopyRegion);
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = DDIW(ResourceUpdateSubResourceUP);
   pDeviceFuncs->pfnSoSetTargets = DDIW(SoSetTargets);
   pDeviceFuncs->pfnDrawAuto = DDIW(DrawAuto);
   pDeviceFuncs->pfnSetViewports = DDIW(SetViewports);
   pDeviceFuncs->pfnSetScissorRects = DDIW(SetScissorRects);
   pDeviceFuncs->pfnClearRenderTargetView = DDIW(ClearRenderTargetView);
   pDeviceFuncs->pfnClearDepthStencilView = DDIW(ClearDepthStencilView);
   pDeviceFuncs->pfnSetPredication = DDIW(SetPredication);
   pDeviceFuncs->pfnQueryGetData = DDIW(QueryGetData);
   pDeviceFuncs->pfnFlush = DDIW(Flush);
   pDeviceFuncs->pfnGenMips = DDIW(GenMips);
   pDeviceFuncs->pfnResourceCopy = DDIW(ResourceCopy);
   pDeviceFuncs->pfnResourceResolveSubresource = DDIW(ResourceResolveSubResource);
   pDeviceFuncs->pfnResourceMap = DDIW(ResourceMap);
   pDeviceFuncs->pfnResourceUnmap = DDIW(ResourceUnmap);
   pDeviceFuncs->pfnResourceIsStagingBusy = DDIW(ResourceIsStagingBusy);
   pDeviceFuncs->pfnRelocateDeviceFuncs = DDIW(RelocateDeviceFuncs);
   pDeviceFuncs->pfnCalcPrivateResourceSize = DDIW(CalcPrivateResourceSize);
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = DDIW(CalcPrivateOpenedResourceSize);
   pDeviceFuncs->pfnCreateResource = DDIW(CreateResource);
   pDeviceFuncs->pfnOpenResource = DDIW(OpenResource);
   pDeviceFuncs->pfnDestroyResource = DDIW(DestroyResource);
   pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = DDIW(CalcPrivateShaderResourceViewSize);
   pDeviceFuncs->pfnCreateShaderResourceView = DDIW(CreateShaderResourceView);
   pDeviceFuncs->pfnDestroyShaderResourceView = DDIW(DestroyShaderResourceView);
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = DDIW(CalcPrivateRenderTargetViewSize);
   pDeviceFuncs->pfnCreateRenderTargetView = DDIW(CreateRenderTargetView);
   pDeviceFuncs->pfnDestroyRenderTargetView = DDIW(DestroyRenderTargetView);
   pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = DDIW(CalcPrivateDepthStencilViewSize);
   pDeviceFuncs->pfnCreateDepthStencilView = DDIW(CreateDepthStencilView);
   pDeviceFuncs->pfnDestroyDepthStencilView = DDIW(DestroyDepthStencilView);
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = DDIW(CalcPrivateElementLayoutSize);
   pDeviceFuncs->pfnCreateElementLayout = DDIW(CreateElementLayout);
   pDeviceFuncs->pfnDestroyElementLayout = DDIW(DestroyElementLayout);
   pDeviceFuncs->pfnCalcPrivateBlendStateSize = DDIW(CalcPrivateBlendStateSize);
   pDeviceFuncs->pfnCreateBlendState = DDIW(CreateBlendState);
   pDeviceFuncs->pfnDestroyBlendState = DDIW(DestroyBlendState);
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = DDIW(CalcPrivateDepthStencilStateSize);
   pDeviceFuncs->pfnCreateDepthStencilState = DDIW(CreateDepthStencilState);
   pDeviceFuncs->pfnDestroyDepthStencilState = DDIW(DestroyDepthStencilState);
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = DDIW(CalcPrivateRasterizerStateSize);
   pDeviceFuncs->pfnCreateRasterizerState = DDIW(CreateRasterizerState);
   pDeviceFuncs->pfnDestroyRasterizerState = DDIW(DestroyRasterizerState);
   pDeviceFuncs->pfnCalcPrivateShaderSize = DDIW(CalcPrivateShaderSize);
   pDeviceFuncs->pfnCreateVertexShader = DDIW(CreateVertexShader);
   pDeviceFuncs->pfnCreateGeometryShader = DDIW(CreateGeometryShader);
   pDeviceFuncs->pfnCreatePixelShader = DDIW(CreatePixelShader);
   pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = DDIW(CalcPrivateGeometryShaderWithStreamOutput);
   pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = DDIW(CreateGeometryShaderWithStreamOutput);
   pDeviceFuncs->pfnDestroyShader = DDIW(DestroyShader);
   pDeviceFuncs->pfnCalcPrivateSamplerSize = DDIW(CalcPrivateSamplerSize);
   pDeviceFuncs->pfnCreateSampler = DDIW(CreateSampler);
   pDeviceFuncs->pfnDestroySampler = DDIW(DestroySampler);
   pDeviceFuncs->pfnCalcPrivateQuerySize = DDIW(CalcPrivateQuerySize);
   pDeviceFuncs->pfnCreateQuery = DDIW(CreateQuery);
   pDeviceFuncs->pfnDestroyQuery = DDIW(DestroyQuery);
   pDeviceFuncs->pfnCheckFormatSupport = DDIW(CheckFormatSupport);
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = DDIW(CheckMultisampleQualityLevels);
   pDeviceFuncs->pfnCheckCounterInfo = DDIW(CheckCounterInfo);
   pDeviceFuncs->pfnCheckCounter = DDIW(CheckCounter);
   pDeviceFuncs->pfnDestroyDevice = DDIW(DestroyDevice);
   pDeviceFuncs->pfnSetTextFilterSize = DDIW(SetTextFilterSize);
   if (pCreateData->Interface == D3D10_1_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_x_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_7_DDI_INTERFACE_VERSION) {
      D3D10_1DDI_DEVICEFUNCS *p10_1DeviceFuncs = pCreateData->p10_1DeviceFuncs;
      p10_1DeviceFuncs->pfnRelocateDeviceFuncs = DDIW(RelocateDeviceFuncs1);
      p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize = DDIW(CalcPrivateShaderResourceViewSize1);
      p10_1DeviceFuncs->pfnCreateShaderResourceView = DDIW(CreateShaderResourceView1);
      p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize = DDIW(CalcPrivateBlendStateSize1);
      p10_1DeviceFuncs->pfnCreateBlendState = DDIW(CreateBlendState1);
      p10_1DeviceFuncs->pfnResourceConvert = DDIW(ResourceCopy);
      p10_1DeviceFuncs->pfnResourceConvertRegion = DDIW(ResourceCopyRegion);
   }
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      DDIW(_Present);
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      DDIW(_GetGammaCaps);
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      DDIW(_SetDisplayMode);
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      DDIW(_SetResourcePriority);
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      DDIW(_QueryResourceResidency);
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      DDIW(_RotateResourceIdentities);
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      DDIW(_Blt);
   if (d3d11 || d3d11_1) {
      /* DXGI 1.1 (D3D11.0) and DXGI 1.2 (D3D11.1) base tables extend the 1.0 one */
      pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnResolveSharedResource = DDIW(_ResolveSharedResource);
   }
   if (d3d11_1) {
      DXGI1_2_DDI_BASE_FUNCTIONS *f2 = pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions3;
      f2->pfnBlt1 = DDIW(_Blt1);
      f2->pfnOfferResources = DDIW(_OfferResources);
      f2->pfnReclaimResources = DDIW(_ReclaimResources);
   }

   if (pDevice->wddm) {
      /* WDDM mode: presents go through pfnPresentCb with KMD allocations (DWM redirection) */
      return S_OK;
   } else {
      // Tell DXGI to not use the shared resource presentation path when
      // communicating with DWM:
      // http://msdn.microsoft.com/en-us/library/windows/hardware/ff569887(v=vs.85).aspx
      return DXGI_STATUS_NO_REDIRECTION;
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyDevice --
 *
 *    The DestroyDevice function destroys a graphics context.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyDevice(D3D10DDI_HDEVICE hDevice)   // IN
{
   unsigned i;

   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   pipe->flush(pipe, NULL, 0);
   if (pDevice->wddm)
      WddmDestroyDevice(pDevice);

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      pipe_so_target_reference(&pDevice->so_targets[i], NULL);
   }
   if (pDevice->draw_so_target) {
      pipe_so_target_reference(&pDevice->draw_so_target, NULL);
   }

   pipe->bind_fs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, NULL);
   cso_unbind_context(pDevice->cso);
   cso_destroy_context(pDevice->cso);

   DeleteEmptyShader(pDevice, MESA_SHADER_FRAGMENT, pDevice->empty_fs);
   DeleteEmptyShader(pDevice, MESA_SHADER_VERTEX, pDevice->empty_vs);

   util_unreference_framebuffer_state(&pDevice->fb);

   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      if (!pDevice->vertex_buffers[i].is_user_buffer) {
         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);
      }
   }

   pipe_resource_reference(&pDevice->index_buffer, NULL);

   static struct pipe_sampler_view * sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   memset(sampler_views, 0, sizeof sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_VERTEX, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_GEOMETRY, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);

   pipe->destroy(pipe);
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs --
 *
 *    The RelocateDeviceFuncs function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs1 --
 *
 *    The RelocateDeviceFuncs1 function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * Flush --
 *
 *    The Flush function submits outstanding hardware commands that
 *    are in the hardware command buffer to the display miniport driver.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Flush(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);

   Device *pDevice = CastDevice(hDevice);
   if (pDevice->shadowHead)
      WddmFlushShadowed(pDevice);
   pDevice->perfFlushes++;
   if (pDevice->wddm && WddmDeferFlush(pDevice))
      return; /* G6 r7 (A): the panel owner's Present flushes the frame together with its scan-out copy */
   UINT64 t0 = pDevice->wddm ? WddmPerfNow() : 0;
   UINT64 tq0 = PerfQpc();
   pipe->flush(pipe, NULL, 0);
   PerfAdd(PERF_F_PIPE, PerfQpc() - tq0);
   if (pDevice->wddm)
      pDevice->perfFlushCpu += WddmPerfNow() - t0;
   if (pDevice->sharedHead)
      WddmBumpShared(pDevice);
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckFormatSupport --
 *
 *    The CheckFormatSupport function retrieves the capabilites that
 *    the device has with the specified format.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckFormatSupport(D3D10DDI_HDEVICE hDevice, // IN
                   DXGI_FORMAT Format,       // IN
                   __out UINT *pFormatCaps)  // OUT
{
   //LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   *pFormatCaps = 0;

   enum pipe_format format = FormatTranslate(Format, false);
   if (format == PIPE_FORMAT_NONE) {
      *pFormatCaps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
      return;
   }

   if (Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
      /*
       * We only need to support creation.
       * http://msdn.microsoft.com/en-us/library/windows/hardware/ff552818.aspx
       */
      return;
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_RENDER_TARGET)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
      }
#endif
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_SAMPLER_VIEW)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      }
#endif
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckMultisampleQualityLevels --
 *
 *    The CheckMultisampleQualityLevels function retrieves the number
 *    of quality levels that the device supports for the specified
 *    number of samples.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,        // IN
                              DXGI_FORMAT Format,              // IN
                              UINT SampleCount,                // IN
                              __out UINT *pNumQualityLevels)   // OUT
{
   //LOG_ENTRYPOINT();

   /* XXX: Disable MSAA */
   *pNumQualityLevels = 0;
}


/*
 * ----------------------------------------------------------------------
 *
 * SetTextFilterSize --
 *
 *    The SetTextFilterSize function sets the width and height
 *    of the monochrome convolution filter.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetTextFilterSize(D3D10DDI_HDEVICE hDevice,  // IN
                  UINT Width,                // IN
                  UINT Height)               // IN
{
   LOG_ENTRYPOINT();

   LOG_UNSUPPORTED(Width != 1 || Height != 1);
}
