/* WDDM ("hardware") mode of the D3D10/11 UMD. SPDX-License-Identifier: MIT */
#pragma once

#include "DriverIncludes.h"

struct Adapter;
struct Device;
struct Resource;
struct pipe_resource;

bool WddmQueryAdapter(const D3D10DDIARG_OPENADAPTER *pOpenData, Adapter *pAdapter);
HRESULT WddmCreateDevice(Device *pDevice);
void WddmDestroyDevice(Device *pDevice);
bool WddmNeedsAllocation(const D3D10DDIARG_CREATERESOURCE *pCreate);
bool WddmCreateResource(Device *pDevice, const D3D10DDIARG_CREATERESOURCE *pCreate,
                        D3D10DDI_HRTRESOURCE hRTResource, const struct pipe_resource *templ,
                        Resource *pResource);
void WddmOpenResource(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_OPENRESOURCE *pOpen,
                      D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRTResource);
void WddmDestroyResource(Device *pDevice, Resource *pResource);
bool WddmRotateStorage(Device *pDevice, Resource *const *r, UINT n);
HRESULT WddmPresent(Device *pDevice, DXGI_DDI_ARG_PRESENT *pPresentData);
bool WddmUmdFlip(Device *pDevice, Resource *pSrc, UINT flags = 0, UINT64 tag = 0);
UINT64 WddmPerfNow(void);
bool WddmDeferFlush(Device *pDevice);
bool WddmUmdBlt(Device *pDevice, Resource *pDst, Resource *pSrc);
HRESULT WddmSetDisplayMode(Device *pDevice, DXGI_DDI_ARG_SETDISPLAYMODE *pData);
void WddmSyncShared(Device *pDevice);
void WddmFlushShadowed(Device *pDevice);
void WddmBumpShared(Device *pDevice);
void WddmMarkDirty(Device *pDevice, struct pipe_resource *res);
void WddmMarkBoundDirty(Device *pDevice);
void WddmSyncSharedResource(Device *pDevice, Resource *pResource);
void WddmCopyOut(Device *pDevice, Resource *pResource);
void WddmCopyIn(Device *pDevice, Resource *pResource);
HRESULT ReadbackPresent(Device *pDevice, Resource *pSrc, void *pDXGIContext);
