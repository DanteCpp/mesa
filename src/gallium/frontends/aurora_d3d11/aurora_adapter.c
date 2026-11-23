/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Native D3D11 / WDDM 2.0 Adapter & Device lifecycle, Caps, DeviceFuncs
 * table installation, in-process Gallium Asahi runtime bridge, and J813
 * driver-key admission gate.
 */
#include "aurora_d3d11.h"
#include "compiler/glsl_types.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnDeallocate2Cb) == 46 * sizeof(void *),
               "D3DDDI_DEVICECALLBACKS layout must match agxw_ddi_devicecallbacks");
_Static_assert(sizeof(struct aurora_gpu_bridge_v4) == 64,
               "aurora_gpu_bridge_v4 size must match AuroraGpuBridge v4 (LLP64)");

/* =========================================================================
 * Driver-Key Admission Gate & Diagnostics Log
 * Mirrors core/agxw_agreement.h section 5 (Enum\ACPI\AURA8171\0 -> Class key).
 * ========================================================================= */
#define AURORA_DEVICE_KEY L"SYSTEM\\CurrentControlSet\\Enum\\ACPI\\AURA8171\\0"
#define AURORA_CLASS_ROOT L"SYSTEM\\CurrentControlSet\\Control\\Class\\"

static INIT_ONCE g_gate_once = INIT_ONCE_STATIC_INIT;
static bool g_gate_key_open = false;
static bool g_sw_umd = false;
static bool g_sw_test = false;
static bool g_sw_all = false;
static bool g_sw_present = false;
static WCHAR g_log_path[MAX_PATH];
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static volatile LONG g_log_lines = 0;
static volatile LONG g_err_log_lines = 0;
#define AURORA_LOG_LINE_CAP 4000
#define AURORA_ERR_LOG_CAP  500

static bool
read_reg_switch(HKEY key, const WCHAR *name)
{
   DWORD val = 0, type = 0, sz = sizeof(val);
   LONG st = RegQueryValueExW(key, name, NULL, &type, (BYTE *)&val, &sz);
   return (st == ERROR_SUCCESS && type == REG_DWORD && sz == 4u &&
           val == AURORA_AGXW_SWITCH_ON);
}

static BOOL CALLBACK
gate_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once; (void)param; (void)ctx;
   WCHAR drv[64] = {0}, path[160] = {0};
   DWORD sz = sizeof(drv);
   HKEY key = NULL;

   if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AURORA_DEVICE_KEY, 0,
                     KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
      return TRUE;
   LONG st = RegGetValueW(key, NULL, L"Driver", RRF_RT_REG_SZ, NULL, drv, &sz);
   RegCloseKey(key);
   if (st != ERROR_SUCCESS || drv[0] != L'{' || wcschr(drv, L'\\') == NULL ||
       wcslen(AURORA_CLASS_ROOT) + wcslen(drv) >= ARRAY_SIZE(path))
      return TRUE;

   wcscpy(path, AURORA_CLASS_ROOT);
   wcscat(path, drv);
   if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
      return TRUE;

   g_gate_key_open = true;
   g_sw_umd     = read_reg_switch(key, L"AgxwFirstDrawUmd");
   g_sw_test    = read_reg_switch(key, L"AgxwFirstDrawTest");
   g_sw_all     = read_reg_switch(key, L"AgxwFirstDrawAll");
   g_sw_present = read_reg_switch(key, L"AgxwFirstDrawPresent");

   DWORD lsz = sizeof(g_log_path);
   if (RegGetValueW(key, NULL, L"AgxwFirstDrawLog", RRF_RT_REG_SZ, NULL,
                    g_log_path, &lsz) != ERROR_SUCCESS)
      g_log_path[0] = L'\0';

   RegCloseKey(key);
   return TRUE;
}

static void
gate_ensure_loaded(void)
{
   InitOnceExecuteOnce(&g_gate_once, gate_init_once, NULL, NULL);
}

static bool
wstr_basename_ieq(const WCHAR *path, size_t len, const char *ascii_name)
{
   size_t start = 0;
   for (size_t i = 0; i < len; ++i) {
      if (path[i] == L'\\' || path[i] == L'/')
         start = i + 1;
   }
   size_t n = strlen(ascii_name);
   if (len - start != n)
      return false;
   for (size_t i = 0; i < n; ++i) {
      WCHAR a = path[start + i];
      WCHAR b = (WCHAR)(unsigned char)ascii_name[i];
      if (a >= L'A' && a <= L'Z') a = (WCHAR)(a + (L'a' - L'A'));
      if (b >= L'A' && b <= L'Z') b = (WCHAR)(b + (L'a' - L'A'));
      if (a != b)
         return false;
   }
   return true;
}

static bool
gate_admit_current_process(void)
{
   gate_ensure_loaded();
   if (!g_gate_key_open)
      return false;

   WCHAR img[MAX_PATH] = {0};
   DWORD len = GetModuleFileNameW(NULL, img, ARRAY_SIZE(img));
   if (len == 0 || len >= ARRAY_SIZE(img))
      return false;

   if (wstr_basename_ieq(img, len, "aurora-offscreen-test.exe") ||
       wstr_basename_ieq(img, len, "aurora-gears-test.exe") ||
       wstr_basename_ieq(img, len, "d2dtext-probe.exe")) {
      return g_sw_test || g_sw_all;
   }
   if (wstr_basename_ieq(img, len, "dwm.exe")) {
      return g_sw_umd;
   }
   return g_sw_all;
}

bool
aurora_gate_present_real(void)
{
   gate_ensure_loaded();
   return g_sw_present;
}

static void
aurora_log_v(bool is_err, const char *fmt, va_list ap)
{
   gate_ensure_loaded();
   if (g_log_path[0] == L'\0')
      return;
   if (is_err) {
      if (InterlockedIncrement(&g_err_log_lines) > AURORA_ERR_LOG_CAP)
         return;
   } else {
      if (InterlockedIncrement(&g_log_lines) > AURORA_LOG_LINE_CAP)
         return;
   }

   char buf[1024];
   DWORD pid = GetCurrentProcessId();
   DWORD tid = GetCurrentThreadId();
   int prefix = snprintf(buf, sizeof(buf), "[native-d3d11 pid=%lu tid=%lu] ",
                         (unsigned long)pid, (unsigned long)tid);
   if (prefix < 0 || (size_t)prefix >= sizeof(buf))
      return;

   int n = vsnprintf(buf + prefix, sizeof(buf) - (size_t)prefix - 2u, fmt, ap);
   if (n < 0)
      return;
   size_t total = (size_t)prefix + (size_t)n;
   if (total >= sizeof(buf) - 2u)
      total = sizeof(buf) - 2u;
   buf[total++] = '\r';
   buf[total++] = '\n';

   AcquireSRWLockExclusive(&g_log_lock);
   HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (h != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(h, buf, (DWORD)total, &written, NULL);
      CloseHandle(h);
   }
   ReleaseSRWLockExclusive(&g_log_lock);
}

void
aurora_log(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   aurora_log_v(false, fmt, ap);
   va_end(ap);
}

void
aurora_log_err(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   aurora_log_v(true, fmt, ap);
   va_end(ap);
}

void
aurora_set_error_at(struct aurora_device *dev, HRESULT hr,
                    const char *func, int line)
{
   if (hr != DXGI_DDI_ERR_WASSTILLDRAWING) {
      aurora_log_err("SetError(%s:%d) hr=0x%08lx",
                     func ? func : "?", line, (unsigned long)hr);
   }
   if (dev && dev->um_callbacks && dev->um_callbacks->pfnSetErrorCb)
      dev->um_callbacks->pfnSetErrorCb(dev->hRTCoreLayer, hr);
}

void
aurora_flush_and_wait(struct aurora_device *dev)
{
   if (!dev || !dev->pctx || !dev->screen)
      return;
   struct pipe_fence_handle *fence = NULL;
   dev->pctx->flush(dev->pctx, &fence, 0);
   if (fence) {
      dev->screen->fence_finish(dev->screen, NULL, fence, 5000000000ULL);
      dev->screen->fence_reference(dev->screen, &fence, NULL);
   }
}

/* =========================================================================
 * Stable In-Process Bridge Pool (process-lifetime storage for alive/in_flight)
 * ========================================================================= */
#define AURORA_MAX_BRIDGE_SLOTS 32
static SRWLOCK g_bridge_pool_lock = SRWLOCK_INIT;
static struct aurora_bridge_slot g_bridge_pool[AURORA_MAX_BRIDGE_SLOTS];

static struct aurora_bridge_slot *
aurora_bridge_slot_alloc(struct aurora_device *dev)
{
   AcquireSRWLockExclusive(&g_bridge_pool_lock);
   struct aurora_bridge_slot *slot = NULL;
   for (unsigned i = 0; i < AURORA_MAX_BRIDGE_SLOTS; ++i) {
      if (!g_bridge_pool[i].in_use) {
         slot = &g_bridge_pool[i];
         slot->in_use = true;
         LONG gen = InterlockedIncrement(&slot->alive);
         slot->in_flight = 0;
         slot->callbacks_copy = dev->kt_callbacks;
         slot->bridge.struct_size = sizeof(slot->bridge);
         slot->bridge.abi_version = AURORA_GPU_BRIDGE_ABI_VERSION;
         slot->bridge.callbacks = &slot->callbacks_copy;
         slot->bridge.hRTDevice = dev->hRTDevice.handle;
         slot->bridge.alive = &slot->alive;
         slot->bridge.alive_value = (unsigned long)gen;
         slot->bridge.in_flight = &slot->in_flight;
         slot->bridge.hRTAdapter = dev->adapter->hRTAdapter.handle;
         slot->bridge.query_adapter_info = (const void *)dev->adapter->query_adapter_info;
         break;
      }
   }
   ReleaseSRWLockExclusive(&g_bridge_pool_lock);
   return slot;
}

static void
aurora_bridge_slot_release(struct aurora_bridge_slot *slot)
{
   if (!slot)
      return;
   /* Revoke first, then wait for any in-flight callback to drain. */
   InterlockedIncrement(&slot->alive);
   for (unsigned spin = 0; spin < 5000u; ++spin) {
      if (InterlockedCompareExchange(&slot->in_flight, 0, 0) == 0)
         break;
      Sleep(1);
   }
   AcquireSRWLockExclusive(&g_bridge_pool_lock);
   slot->in_use = false;
   ReleaseSRWLockExclusive(&g_bridge_pool_lock);
}

/* =========================================================================
 * Lazy VM-Only Present Context (mirrors compat/tritonPresent.c)
 * ========================================================================= */
void
aurora_ensure_present_context(struct aurora_device *dev)
{
   if (!dev || dev->hKMContext != NULL ||
       !dev->kt_callbacks.pfnCreateContextVirtualCb)
      return;

   if (dev->locks_init)
      EnterCriticalSection(&dev->km_ctx_lock);

   if (dev->hKMContext == NULL) {
      struct aurora_agxw_context_private priv;
      memset(&priv, 0, sizeof(priv));
      priv.version = AURORA_AGXW_CONTEXT_PRIVATE_VERSION;
      priv.struct_size = sizeof(priv);
      priv.priority = AURORA_AGXW_PRIORITY_MEDIUM;
      priv.flags = AURORA_AGXW_CONTEXT_FLAG_VA_INTERLOCK;
      priv.usc_exec_base =
         (dev->adapter && dev->adapter->identity_valid &&
          dev->adapter->identity.gpu_generation == 17u)
            ? AURORA_AGXW_J813_USC_EXEC_BASE
            : AURORA_AGXW_VM_USER_START;

      D3DDDICB_CREATECONTEXTVIRTUAL cc;
      memset(&cc, 0, sizeof(cc));
      cc.NodeOrdinal = 0u;
      cc.EngineAffinity = 1u;
      cc.pPrivateDriverData = &priv;
      cc.PrivateDriverDataSize = sizeof(priv);

      HRESULT hr = dev->kt_callbacks.pfnCreateContextVirtualCb(
         dev->hRTDevice.handle, &cc);
      aurora_log("present-context: usc_base=0x%llx hr=0x%08lx hCtx=%p",
                 (unsigned long long)priv.usc_exec_base, (unsigned long)hr,
                 SUCCEEDED(hr) ? (void *)cc.hContext : NULL);
      if (SUCCEEDED(hr))
         dev->hKMContext = cc.hContext;
   }

   if (dev->locks_init)
      LeaveCriticalSection(&dev->km_ctx_lock);
}

/* =========================================================================
 * Diagnostic / No-Op DDI Helpers
 * ========================================================================= */
static void APIENTRY
aurora_SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height)
{
   (void)hDevice; (void)Width; (void)Height;
}

static void APIENTRY
aurora_SetMarker(D3D10DDI_HDEVICE hDevice)
{
   (void)hDevice;
}

static void APIENTRY
aurora_SetMarkerMode(D3D10DDI_HDEVICE hDevice, D3DWDDM1_3DDI_MARKER_TYPE Type,
                     UINT Flags)
{
   (void)hDevice; (void)Type; (void)Flags;
}

static void APIENTRY
aurora_SetHardwareProtection(D3D10DDI_HDEVICE hDevice,
                             D3D10DDI_HRESOURCE hRes, BOOL Protected)
{
   (void)hDevice; (void)hRes; (void)Protected;
}

static void APIENTRY
aurora_SetHardwareProtectionState(D3D10DDI_HDEVICE hDevice, BOOL Protected)
{
   (void)hDevice; (void)Protected;
}

static void APIENTRY
aurora_AcquireResource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hRes,
                       HANDLE hSync)
{
   (void)hDevice; (void)hRes; (void)hSync;
}

static void APIENTRY
aurora_ReleaseResource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hRes,
                       HANDLE hSync)
{
   (void)hDevice; (void)hRes; (void)hSync;
}

#define AURORA_DEFINE_SHADER_WITH_IFACES(ST, BIND)                        \
static void APIENTRY aurora_##ST##SetShaderWithIfaces(                    \
   D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader,                    \
   UINT NumClassInstances, const UINT *pInterfaceIds,                     \
   const D3D11DDIARG_POINTERDATA *pPointerData)                           \
{                                                                         \
   (void)pInterfaceIds; (void)pPointerData;                               \
   if (NumClassInstances == 0u) {                                         \
      BIND(hDevice, hShader);                                             \
      return;                                                             \
   }                                                                      \
   aurora_set_error((struct aurora_device *)hDevice.pDrvPrivate,          \
                    E_NOTIMPL);                                           \
}
AURORA_DEFINE_SHADER_WITH_IFACES(Ps, aurora_PsSetShader)
AURORA_DEFINE_SHADER_WITH_IFACES(Vs, aurora_VsSetShader)
AURORA_DEFINE_SHADER_WITH_IFACES(Gs, aurora_GsSetShader)
AURORA_DEFINE_SHADER_WITH_IFACES(Hs, aurora_HsSetShader)
AURORA_DEFINE_SHADER_WITH_IFACES(Ds, aurora_DsSetShader)
AURORA_DEFINE_SHADER_WITH_IFACES(Cs, aurora_CsSetShader)
#undef AURORA_DEFINE_SHADER_WITH_IFACES

static void APIENTRY
aurora_RelocateD3D11(D3D10DDI_HDEVICE hDevice, D3D11DDI_DEVICEFUNCS *pFuncs)
{ (void)hDevice; (void)pFuncs; }
static void APIENTRY
aurora_RelocateD3D11_1(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_DEVICEFUNCS *pFuncs)
{ (void)hDevice; (void)pFuncs; }
static void APIENTRY
aurora_RelocateWDDM1_3(D3D10DDI_HDEVICE hDevice, D3DWDDM1_3DDI_DEVICEFUNCS *pFuncs)
{ (void)hDevice; (void)pFuncs; }
static void APIENTRY
aurora_RelocateWDDM2_0(D3D10DDI_HDEVICE hDevice, D3DWDDM2_0DDI_DEVICEFUNCS *pFuncs)
{ (void)hDevice; (void)pFuncs; }
static void APIENTRY
aurora_RelocateWDDM2_1(D3D10DDI_HDEVICE hDevice, D3DWDDM2_1DDI_DEVICEFUNCS *pFuncs)
{ (void)hDevice; (void)pFuncs; }

/* aurora_RetrieveSubObject is implemented in aurora_video.c */

/* =========================================================================
 * Device Teardown
 * ========================================================================= */
static void APIENTRY
aurora_DestroyDevice(D3D10DDI_HDEVICE hDevice)
{
   struct aurora_device *dev = (struct aurora_device *)hDevice.pDrvPrivate;
   if (!dev)
      return;

   aurora_log("DestroyDevice begin dev=%p", (void *)dev);

   aurora_lock(dev);
   if (dev->pctx) {
      aurora_flush_and_wait(dev);
      /* Release any bound constant buffers / vertex buffers. */
      for (unsigned st = 0; st < AURORA_STAGE_COUNT; ++st) {
         for (unsigned i = 0; i < AURORA_MAX_CBS; ++i)
            pipe_resource_reference(&dev->cbs[st][i].buffer, NULL);
      }
      for (unsigned i = 0; i < AURORA_MAX_VBS; ++i)
         pipe_resource_reference(&dev->vbs[i].buffer.resource, NULL);

      if (dev->active_velems_cso)
         dev->pctx->delete_vertex_elements_state(dev->pctx, dev->active_velems_cso);
      if (dev->empty_velems_cso)
         dev->pctx->delete_vertex_elements_state(dev->pctx, dev->empty_velems_cso);
      if (dev->default_blend_cso)
         dev->pctx->delete_blend_state(dev->pctx, dev->default_blend_cso);
      if (dev->default_dsa_cso)
         dev->pctx->delete_depth_stencil_alpha_state(dev->pctx, dev->default_dsa_cso);
      if (dev->no_depth_dsa_cso)
         dev->pctx->delete_depth_stencil_alpha_state(dev->pctx, dev->no_depth_dsa_cso);
      if (dev->default_rast_cso)
         dev->pctx->delete_rasterizer_state(dev->pctx, dev->default_rast_cso);
      if (dev->default_sampler_cso)
         dev->pctx->delete_sampler_state(dev->pctx, dev->default_sampler_cso);
      if (dev->clear_vs_cso)
         dev->pctx->delete_vs_state(dev->pctx, dev->clear_vs_cso);
      if (dev->clear_fs_cso)
         dev->pctx->delete_fs_state(dev->pctx, dev->clear_fs_cso);
      if (dev->clear_blend_cso)
         dev->pctx->delete_blend_state(dev->pctx, dev->clear_blend_cso);
      if (dev->clear_dsa_cso)
         dev->pctx->delete_depth_stencil_alpha_state(dev->pctx, dev->clear_dsa_cso);
      if (dev->clear_rast_cso)
         dev->pctx->delete_rasterizer_state(dev->pctx, dev->clear_rast_cso);

      dev->pctx->destroy(dev->pctx);
      dev->pctx = NULL;
   }

   /* Tear down the lazy present kernel context before destroying screen/bridge. */
   if (dev->hKMContext && dev->kt_callbacks.pfnDestroyContextCb) {
      D3DDDICB_DESTROYCONTEXT dc;
      memset(&dc, 0, sizeof(dc));
      dc.hContext = dev->hKMContext;
      if (dev->locks_init)
         EnterCriticalSection(&dev->km_ctx_lock);
      dev->kt_callbacks.pfnDestroyContextCb(dev->hRTDevice.handle, &dc);
      dev->hKMContext = NULL;
      if (dev->locks_init)
         LeaveCriticalSection(&dev->km_ctx_lock);
   }

   /* Destroy Gallium Asahi screen BEFORE retiring bridge so D3DKMT frees
    * (BOs, syncobjs, GPU VA leases) succeed cleanly! */
   if (dev->screen) {
      dev->screen->destroy(dev->screen);
      dev->screen = NULL;
   }

   if (dev->bridge_slot) {
      aurora_bridge_slot_release(dev->bridge_slot);
      dev->bridge_slot = NULL;
   }

   aurora_unlock(dev);
   if (dev->locks_init) {
      DeleteCriticalSection(&dev->lock);
      DeleteCriticalSection(&dev->km_ctx_lock);
      dev->locks_init = false;
   }
   dev->adapter = NULL;
   glsl_type_singleton_decref();
   aurora_log("DestroyDevice done");
}

/* =========================================================================
 * Device Function Table Population (D3D11_0 -> 11_1 -> WDDM1_3 -> WDDM2_0/2_1)
 * ========================================================================= */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

static void
fill_d3d11_funcs(D3D11DDI_DEVICEFUNCS *p)
{
   p->pfnDefaultConstantBufferUpdateSubresourceUP = aurora_ResourceUpdateSubresourceUP;
   p->pfnVsSetConstantBuffers                     = aurora_VsSetConstantBuffers10;
   p->pfnPsSetShaderResources                     = aurora_PsSetShaderResources;
   p->pfnPsSetShader                              = aurora_PsSetShader;
   p->pfnPsSetSamplers                            = aurora_PsSetSamplers;
   p->pfnVsSetShader                              = aurora_VsSetShader;
   p->pfnDrawIndexed                              = aurora_DrawIndexed;
   p->pfnDraw                                     = aurora_Draw;
   p->pfnDynamicIABufferMapNoOverwrite            = aurora_ResourceMap;
   p->pfnDynamicIABufferUnmap                     = aurora_ResourceUnmap;
   p->pfnDynamicConstantBufferMapDiscard          = aurora_ResourceMap;
   p->pfnDynamicIABufferMapDiscard                = aurora_ResourceMap;
   p->pfnDynamicConstantBufferUnmap               = aurora_ResourceUnmap;
   p->pfnPsSetConstantBuffers                     = aurora_PsSetConstantBuffers10;
   p->pfnIaSetInputLayout                         = aurora_IaSetInputLayout;
   p->pfnIaSetVertexBuffers                       = aurora_IaSetVertexBuffers;
   p->pfnIaSetIndexBuffer                         = aurora_IaSetIndexBuffer;

   p->pfnDrawIndexedInstanced                     = aurora_DrawIndexedInstanced;
   p->pfnDrawInstanced                            = aurora_DrawInstanced;
   p->pfnDynamicResourceMapDiscard                = aurora_ResourceMap;
   p->pfnDynamicResourceUnmap                     = aurora_ResourceUnmap;
   p->pfnGsSetConstantBuffers                     = aurora_GsSetConstantBuffers10;
   p->pfnGsSetShader                              = aurora_GsSetShader;
   p->pfnIaSetTopology                            = aurora_IaSetTopology;
   p->pfnStagingResourceMap                       = aurora_ResourceMap;
   p->pfnStagingResourceUnmap                     = aurora_ResourceUnmap;
   p->pfnVsSetShaderResources                     = aurora_VsSetShaderResources;
   p->pfnVsSetSamplers                            = aurora_VsSetSamplers;
   p->pfnGsSetShaderResources                     = aurora_GsSetShaderResources;
   p->pfnGsSetSamplers                            = aurora_GsSetSamplers;
   p->pfnSetRenderTargets                         = aurora_SetRenderTargets;
   p->pfnShaderResourceViewReadAfterWriteHazard   = aurora_SRVReadAfterWriteHazard;
   p->pfnResourceReadAfterWriteHazard             = aurora_ResourceReadAfterWriteHazard;
   p->pfnSetBlendState                            = aurora_SetBlendState;
   p->pfnSetDepthStencilState                     = aurora_SetDepthStencilState;
   p->pfnSetRasterizerState                       = aurora_SetRasterizerState;
   p->pfnQueryEnd                                 = aurora_QueryEnd;
   p->pfnQueryBegin                               = aurora_QueryBegin;
   p->pfnResourceCopyRegion                       = aurora_ResourceCopyRegion;
   p->pfnResourceUpdateSubresourceUP              = aurora_ResourceUpdateSubresourceUP;
   p->pfnSoSetTargets                             = aurora_SoSetTargets;
   p->pfnDrawAuto                                 = aurora_DrawAuto;
   p->pfnSetViewports                             = aurora_SetViewports;
   p->pfnSetScissorRects                          = aurora_SetScissorRects;
   p->pfnClearRenderTargetView                    = aurora_ClearRenderTargetView;
   p->pfnClearDepthStencilView                    = aurora_ClearDepthStencilView;
   p->pfnSetPredication                           = aurora_SetPredication;
   p->pfnQueryGetData                             = aurora_QueryGetData;
   p->pfnFlush                                    = aurora_Flush;
   p->pfnGenMips                                  = aurora_GenerateMips;
   p->pfnResourceCopy                             = aurora_ResourceCopy;
   p->pfnResourceResolveSubresource               = aurora_ResourceResolveSubresource;

   p->pfnResourceMap                              = aurora_ResourceMap;
   p->pfnResourceUnmap                            = aurora_ResourceUnmap;
   p->pfnResourceIsStagingBusy                    = aurora_ResourceIsStagingBusy;
   p->pfnRelocateDeviceFuncs                      = aurora_RelocateD3D11;
   p->pfnCalcPrivateResourceSize                  = aurora_CalcPrivateResourceSize;
   p->pfnCalcPrivateOpenedResourceSize            = aurora_CalcPrivateOpenedResourceSize;
   p->pfnCreateResource                           = aurora_CreateResource;
   p->pfnOpenResource                             = aurora_OpenResource;
   p->pfnDestroyResource                          = aurora_DestroyResource;
   p->pfnCalcPrivateShaderResourceViewSize        = aurora_CalcPrivateSRVSize;
   p->pfnCreateShaderResourceView                 = aurora_CreateSRV;
   p->pfnDestroyShaderResourceView                = aurora_DestroySRV;
   p->pfnCalcPrivateRenderTargetViewSize          = aurora_CalcPrivateRTVSize;
   p->pfnCreateRenderTargetView                   = aurora_CreateRTV;
   p->pfnDestroyRenderTargetView                  = aurora_DestroyRTV;
   p->pfnCalcPrivateDepthStencilViewSize          = aurora_CalcPrivateDSVSize;
   p->pfnCreateDepthStencilView                   = aurora_CreateDSV;
   p->pfnDestroyDepthStencilView                  = aurora_DestroyDSV;
   p->pfnCalcPrivateElementLayoutSize             = aurora_CalcPrivateElementLayoutSize;
   p->pfnCreateElementLayout                      = aurora_CreateElementLayout;
   p->pfnDestroyElementLayout                     = aurora_DestroyElementLayout;
   p->pfnCalcPrivateBlendStateSize                = aurora_CalcPrivateBlendStateSize_10;
   p->pfnCreateBlendState                         = aurora_CreateBlendState_10;
   p->pfnDestroyBlendState                        = aurora_DestroyBlendState;
   p->pfnCalcPrivateDepthStencilStateSize         = aurora_CalcPrivateDepthStencilStateSize;
   p->pfnCreateDepthStencilState                  = aurora_CreateDepthStencilState;
   p->pfnDestroyDepthStencilState                 = aurora_DestroyDepthStencilState;
   p->pfnCalcPrivateRasterizerStateSize           = aurora_CalcPrivateRasterizerStateSize_10;
   p->pfnCreateRasterizerState                    = aurora_CreateRasterizerState_10;
   p->pfnDestroyRasterizerState                   = aurora_DestroyRasterizerState;
   p->pfnCalcPrivateShaderSize                    = (PFND3D10DDI_CALCPRIVATESHADERSIZE)aurora_CalcPrivateShaderSize;
   p->pfnCreateVertexShader                       = (PFND3D10DDI_CREATEVERTEXSHADER)aurora_CreateVertexShader;
   p->pfnCreateGeometryShader                     = (PFND3D10DDI_CREATEGEOMETRYSHADER)aurora_CreateGeometryShader;
   p->pfnCreatePixelShader                        = (PFND3D10DDI_CREATEPIXELSHADER)aurora_CreatePixelShader;
   p->pfnCalcPrivateGeometryShaderWithStreamOutput = (PFND3D11DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT)aurora_CalcPrivateGSWithSOSize;
   p->pfnCreateGeometryShaderWithStreamOutput     = (PFND3D11DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT)aurora_CreateGSWithSO;
   p->pfnDestroyShader                            = aurora_DestroyShader;
   p->pfnCalcPrivateSamplerSize                   = aurora_CalcPrivateSamplerSize;
   p->pfnCreateSampler                            = aurora_CreateSampler;
   p->pfnDestroySampler                           = aurora_DestroySampler;
   p->pfnCalcPrivateQuerySize                     = aurora_CalcPrivateQuerySize;
   p->pfnCreateQuery                              = aurora_CreateQuery;
   p->pfnDestroyQuery                             = aurora_DestroyQuery;

   p->pfnCheckFormatSupport                       = aurora_CheckFormatSupport;
   p->pfnCheckMultisampleQualityLevels            = aurora_CheckMultisampleQualityLevels;
   p->pfnCheckCounterInfo                         = aurora_CheckCounterInfo;
   p->pfnCheckCounter                             = aurora_CheckCounter;
   p->pfnDestroyDevice                            = aurora_DestroyDevice;
   p->pfnSetTextFilterSize                        = aurora_SetTextFilterSize;
   p->pfnResourceConvert                          = aurora_ResourceConvert;
   p->pfnResourceConvertRegion                    = aurora_ResourceConvertRegion;

   p->pfnDrawIndexedInstancedIndirect             = aurora_DrawIndexedInstancedIndirect;
   p->pfnDrawInstancedIndirect                    = aurora_DrawInstancedIndirect;
   p->pfnHsSetShaderResources                     = aurora_HsSetShaderResources;
   p->pfnHsSetShader                              = aurora_HsSetShader;
   p->pfnHsSetSamplers                            = aurora_HsSetSamplers;
   p->pfnHsSetConstantBuffers                     = aurora_HsSetConstantBuffers10;
   p->pfnDsSetShaderResources                     = aurora_DsSetShaderResources;
   p->pfnDsSetShader                              = aurora_DsSetShader;
   p->pfnDsSetSamplers                            = aurora_DsSetSamplers;
   p->pfnDsSetConstantBuffers                     = aurora_DsSetConstantBuffers10;
   p->pfnCreateHullShader                         = (PFND3D11DDI_CREATEHULLSHADER)aurora_CreateHullShader;
   p->pfnCreateDomainShader                       = (PFND3D11DDI_CREATEDOMAINSHADER)aurora_CreateDomainShader;
   p->pfnCalcPrivateTessellationShaderSize        = (PFND3D11DDI_CALCPRIVATETESSELLATIONSHADERSIZE)aurora_CalcPrivateTessShaderSize;
   p->pfnPsSetShaderWithIfaces                    = aurora_PsSetShaderWithIfaces;
   p->pfnVsSetShaderWithIfaces                    = aurora_VsSetShaderWithIfaces;
   p->pfnGsSetShaderWithIfaces                    = aurora_GsSetShaderWithIfaces;
   p->pfnHsSetShaderWithIfaces                    = aurora_HsSetShaderWithIfaces;
   p->pfnDsSetShaderWithIfaces                    = aurora_DsSetShaderWithIfaces;
   p->pfnCsSetShaderWithIfaces                    = aurora_CsSetShaderWithIfaces;
   p->pfnCreateComputeShader                      = aurora_CreateComputeShader;
   p->pfnCsSetShader                              = aurora_CsSetShader;
   p->pfnCsSetShaderResources                     = aurora_CsSetShaderResources;
   p->pfnCsSetSamplers                            = aurora_CsSetSamplers;
   p->pfnCsSetConstantBuffers                     = aurora_CsSetConstantBuffers10;
   p->pfnCalcPrivateUnorderedAccessViewSize       = aurora_CalcPrivateUAVSize;
   p->pfnCreateUnorderedAccessView                = aurora_CreateUAV;
   p->pfnDestroyUnorderedAccessView               = aurora_DestroyUAV;
   p->pfnClearUnorderedAccessViewUint             = aurora_ClearUAVUint;
   p->pfnClearUnorderedAccessViewFloat            = aurora_ClearUAVFloat;
   p->pfnCsSetUnorderedAccessViews                = aurora_CsSetUAVs;
   p->pfnDispatch                                 = aurora_Dispatch;
   p->pfnDispatchIndirect                         = aurora_DispatchIndirect;
   p->pfnSetResourceMinLOD                        = aurora_SetResourceMinLOD;
   p->pfnCopyStructureCount                       = aurora_CopyStructureCount;
}

static void
fill_d3d11_1_funcs(D3D11_1DDI_DEVICEFUNCS *p)
{
   fill_d3d11_funcs((D3D11DDI_DEVICEFUNCS *)p);
   p->pfnDefaultConstantBufferUpdateSubresourceUP = aurora_ResourceUpdateSubresourceUP_11_1;
   p->pfnVsSetConstantBuffers                     = aurora_VsSetConstantBuffers11_1;
   p->pfnPsSetConstantBuffers                     = aurora_PsSetConstantBuffers11_1;
   p->pfnGsSetConstantBuffers                     = aurora_GsSetConstantBuffers11_1;
   p->pfnHsSetConstantBuffers                     = aurora_HsSetConstantBuffers11_1;
   p->pfnDsSetConstantBuffers                     = aurora_DsSetConstantBuffers11_1;
   p->pfnCsSetConstantBuffers                     = aurora_CsSetConstantBuffers11_1;
   p->pfnResourceCopyRegion                       = aurora_ResourceCopyRegion_11_1;
   p->pfnResourceUpdateSubresourceUP              = aurora_ResourceUpdateSubresourceUP_11_1;
   p->pfnFlush                                    = aurora_Flush11_1;
   p->pfnCalcPrivateBlendStateSize                = aurora_CalcPrivateBlendStateSize_11_1;
   p->pfnCreateBlendState                         = aurora_CreateBlendState_11_1;
   p->pfnCalcPrivateRasterizerStateSize           = aurora_CalcPrivateRasterizerStateSize_11_1;
   p->pfnCreateRasterizerState                    = aurora_CreateRasterizerState_11_1;
   p->pfnCalcPrivateShaderSize                    = (PFND3D11_1DDI_CALCPRIVATESHADERSIZE)aurora_CalcPrivateShaderSize;
   p->pfnCreateVertexShader                       = (PFND3D11_1DDI_CREATEVERTEXSHADER)aurora_CreateVertexShader;
   p->pfnCreateGeometryShader                     = (PFND3D11_1DDI_CREATEGEOMETRYSHADER)aurora_CreateGeometryShader;
   p->pfnCreatePixelShader                        = (PFND3D11_1DDI_CREATEPIXELSHADER)aurora_CreatePixelShader;
   p->pfnCalcPrivateGeometryShaderWithStreamOutput = (PFND3D11_1DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT)aurora_CalcPrivateGSWithSOSize;
   p->pfnCreateGeometryShaderWithStreamOutput     = (PFND3D11_1DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT)aurora_CreateGSWithSO;
   p->pfnCreateHullShader                         = (PFND3D11_1DDI_CREATEHULLSHADER)aurora_CreateHullShader;
   p->pfnCreateDomainShader                       = (PFND3D11_1DDI_CREATEDOMAINSHADER)aurora_CreateDomainShader;
   p->pfnCalcPrivateTessellationShaderSize        = (PFND3D11_1DDI_CALCPRIVATETESSELLATIONSHADERSIZE)aurora_CalcPrivateTessShaderSize;
   p->pfnDiscard                                  = aurora_Discard;
   p->pfnAssignDebugBinary                        = aurora_AssignDebugBinary;
   p->pfnDynamicConstantBufferMapNoOverwrite      = aurora_ResourceMap;
   p->pfnRelocateDeviceFuncs                      = aurora_RelocateD3D11_1;
   p->pfnCheckDirectFlipSupport                   = aurora_CheckDirectFlipSupport;
   p->pfnClearView                                = aurora_ClearView;
}

static void
fill_wddm1_3_funcs(D3DWDDM1_3DDI_DEVICEFUNCS *p)
{
   fill_d3d11_1_funcs((D3D11_1DDI_DEVICEFUNCS *)p);
   p->pfnRelocateDeviceFuncs           = aurora_RelocateWDDM1_3;
   p->pfnCheckMultisampleQualityLevels = aurora_CheckMultisampleQualityLevels_1_3;
   p->pfnUpdateTileMappings            = aurora_UpdateTileMappings;
   p->pfnCopyTileMappings              = aurora_CopyTileMappings;
   p->pfnCopyTiles                     = aurora_CopyTiles;
   p->pfnUpdateTiles                   = aurora_UpdateTiles;
   p->pfnTiledResourceBarrier          = aurora_TiledResourceBarrier;
   p->pfnGetMipPacking                 = aurora_GetMipPacking;
   p->pfnResizeTilePool                = aurora_ResizeTilePool;
   p->pfnSetMarker                     = aurora_SetMarker;
   p->pfnSetMarkerMode                 = aurora_SetMarkerMode;
}

static void
fill_wddm2_0_funcs(D3DWDDM2_0DDI_DEVICEFUNCS *p)
{
   fill_wddm1_3_funcs((D3DWDDM1_3DDI_DEVICEFUNCS *)p);
   p->pfnRelocateDeviceFuncs                = aurora_RelocateWDDM2_0;
   p->pfnCalcPrivateShaderResourceViewSize  = aurora_CalcPrivateSRVSize_WDDM2_0;
   p->pfnCreateShaderResourceView           = aurora_CreateSRV_WDDM2_0;
   p->pfnCalcPrivateRenderTargetViewSize    = aurora_CalcPrivateRTVSize_WDDM2_0;
   p->pfnCreateRenderTargetView             = aurora_CreateRTV_WDDM2_0;
   p->pfnCalcPrivateRasterizerStateSize     = aurora_CalcPrivateRasterizerStateSize_WDDM2_0;
   p->pfnCreateRasterizerState              = aurora_CreateRasterizerState_WDDM2_0;
   p->pfnCalcPrivateQuerySize               = aurora_CalcPrivateQuerySize_WDDM2_0;
   p->pfnCreateQuery                        = aurora_CreateQuery_WDDM2_0;
   p->pfnCalcPrivateUnorderedAccessViewSize = aurora_CalcPrivateUAVSize_WDDM2_0;
   p->pfnCreateUnorderedAccessView          = aurora_CreateUAV_WDDM2_0;
   p->pfnFlush                              = aurora_Flush_WDDM2_0;
   p->pfnSetHardwareProtection              = aurora_SetHardwareProtection;
   p->pfnGetResourceLayout                  = aurora_GetResourceLayout;
   p->pfnRetrieveShaderComment              = aurora_RetrieveShaderComment;
   p->pfnSetHardwareProtectionState         = aurora_SetHardwareProtectionState;
}

static void
fill_wddm2_1_funcs(D3DWDDM2_1DDI_DEVICEFUNCS *p)
{
   fill_wddm2_0_funcs((D3DWDDM2_0DDI_DEVICEFUNCS *)p);
   p->pfnRelocateDeviceFuncs = aurora_RelocateWDDM2_1;
   p->pfnAcquireResource     = (PFND3DWDDM2_1DDI_SYNC_TOKEN)aurora_AcquireResource;
   p->pfnReleaseResource     = (PFND3DWDDM2_1DDI_SYNC_TOKEN)aurora_ReleaseResource;
}

#pragma GCC diagnostic pop

/* =========================================================================
 * Adapter DDI Callbacks & OpenAdapter10 / OpenAdapter10_2 Exports
 * ========================================================================= */
static SIZE_T APIENTRY
aurora_CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,
                             const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pArgs)
{
   (void)hAdapter; (void)pArgs;
   return sizeof(struct aurora_device);
}

static HRESULT APIENTRY
aurora_CreateDevice(D3D10DDI_HADAPTER hAdapter,
                    D3D10DDIARG_CREATEDEVICE *pArgs)
{
   if (!gate_admit_current_process())
      return DXGI_ERROR_UNSUPPORTED;

   struct aurora_adapter *adapter = (struct aurora_adapter *)hAdapter.pDrvPrivate;
   if (!adapter || !pArgs || !pArgs->hDrvDevice.pDrvPrivate || !pArgs->pKTCallbacks)
      return E_INVALIDARG;

   struct aurora_device *dev = (struct aurora_device *)pArgs->hDrvDevice.pDrvPrivate;
   memset(dev, 0, sizeof(*dev));

   aurora_log("CreateDevice: iface=0x%08x ver=0x%08x flags=0x%08x",
              pArgs->Interface, pArgs->Version, pArgs->Flags);

   switch (pArgs->Interface) {
   case D3D11_0_DDI_INTERFACE_VERSION:
      fill_d3d11_funcs(pArgs->p11DeviceFuncs);
      break;
   case D3D11_1_DDI_INTERFACE_VERSION:
      fill_d3d11_1_funcs(pArgs->p11_1DeviceFuncs);
      break;
   case D3DWDDM1_3_DDI_INTERFACE_VERSION:
      fill_wddm1_3_funcs(pArgs->pWDDM1_3DeviceFuncs);
      break;
   case D3DWDDM2_0_DDI_INTERFACE_VERSION:
      fill_wddm2_0_funcs(pArgs->pWDDM2_0DeviceFuncs);
      break;
   case D3DWDDM2_1_DDI_INTERFACE_VERSION:
      fill_wddm2_1_funcs(pArgs->pWDDM2_1DeviceFuncs);
      break;
   default:
      aurora_log("CreateDevice: unsupported interface 0x%08x", pArgs->Interface);
      return E_FAIL;
   }

   if (pArgs->ppfnRetrieveSubObject)
      *pArgs->ppfnRetrieveSubObject = aurora_RetrieveSubObject;

   dev->adapter = adapter;
   dev->hRTDevice = pArgs->hRTDevice;
   dev->hRTCoreLayer = pArgs->hRTCoreLayer;
   dev->interface_version = pArgs->Interface;
   dev->kt_callbacks = *pArgs->pKTCallbacks;
   dev->um_callbacks = pArgs->p11UMCallbacks;
   dev->sample_mask = 0xffffffffu;
   dev->pipe_prim = MESA_PRIM_TRIANGLES;

   /* Find the Apple AGX WDDM adapter LUID via D3DKMT. */
   struct agx_kmt_luid luids[4];
   uint32_t luid_count = 0;
   if (!agx_d3dkmt_enumerate_adapters(luids, ARRAY_SIZE(luids), &luid_count) ||
       luid_count == 0) {
      aurora_log("CreateDevice: agx_d3dkmt_enumerate_adapters found 0 adapters");
      return DXGI_ERROR_UNSUPPORTED;
   }

   /* Reserve stable process-lifetime bridge slot and open Gallium Asahi
    * directly on the runtime's own D3DKMT device (zero DXVK, zero Vulkan!). */
   glsl_type_singleton_init_or_ref();
   dev->bridge_slot = aurora_bridge_slot_alloc(dev);
   if (!dev->bridge_slot) {
      glsl_type_singleton_decref();
      aurora_log("CreateDevice: bridge slot pool exhausted");
      return E_OUTOFMEMORY;
   }

   dev->screen = agx_screen_create_d3dkmt(&luids[0], &dev->bridge_slot->bridge);
   if (!dev->screen) {
      aurora_log("CreateDevice: agx_screen_create_d3dkmt failed");
      aurora_bridge_slot_release(dev->bridge_slot);
      dev->bridge_slot = NULL;
      glsl_type_singleton_decref();
      return E_FAIL;
   }

   dev->pctx = dev->screen->context_create(dev->screen, NULL, 0);
   if (!dev->pctx) {
      aurora_log("CreateDevice: screen->context_create failed");
      dev->screen->destroy(dev->screen);
      dev->screen = NULL;
      aurora_bridge_slot_release(dev->bridge_slot);
      dev->bridge_slot = NULL;
      glsl_type_singleton_decref();
      return E_FAIL;
   }

   InitializeCriticalSection(&dev->lock);
   InitializeCriticalSection(&dev->km_ctx_lock);
   dev->locks_init = true;

   /* Eagerly create and bind D3D11 default CSOs so Gallium Asahi's ctx->blend,
    * ctx->zs, ctx->rast, and ctx->attributes are NEVER NULL at draw time. */
   {
      struct pipe_blend_state bdef;
      memset(&bdef, 0, sizeof(bdef));
      bdef.max_rt = AURORA_MAX_RTVS - 1u;
      bdef.rt[0].colormask = PIPE_MASK_RGBA;
      dev->default_blend_cso = dev->pctx->create_blend_state(dev->pctx, &bdef);
      dev->pctx->bind_blend_state(dev->pctx, dev->default_blend_cso);

      struct pipe_depth_stencil_alpha_state ddef;
      memset(&ddef, 0, sizeof(ddef));
      ddef.depth_enabled = 1;
      ddef.depth_writemask = 1;
      ddef.depth_func = PIPE_FUNC_LESS;
      dev->default_dsa_cso =
         dev->pctx->create_depth_stencil_alpha_state(dev->pctx, &ddef);

      struct pipe_depth_stencil_alpha_state dnone;
      memset(&dnone, 0, sizeof(dnone));
      dev->no_depth_dsa_cso =
         dev->pctx->create_depth_stencil_alpha_state(dev->pctx, &dnone);
      /* Start with depth disabled until a DSV is bound (G17G rejects
       * depth_enabled=1 when fb.zsbuf.texture == NULL). */
      dev->pctx->bind_depth_stencil_alpha_state(dev->pctx, dev->no_depth_dsa_cso);

      struct pipe_rasterizer_state rdef;
      memset(&rdef, 0, sizeof(rdef));
      rdef.flatshade_first = 1;
      rdef.clip_halfz = 1;
      rdef.half_pixel_center = 1;
      rdef.depth_clip_near = 1;
      rdef.depth_clip_far = 1;
      rdef.cull_face = PIPE_FACE_BACK;
      rdef.fill_front = PIPE_POLYGON_MODE_FILL;
      rdef.fill_back = PIPE_POLYGON_MODE_FILL;
      rdef.line_width = 1.0f;
      rdef.point_size = 1.0f;
      dev->default_rast_cso = dev->pctx->create_rasterizer_state(dev->pctx, &rdef);
      dev->pctx->bind_rasterizer_state(dev->pctx, dev->default_rast_cso);

      dev->empty_velems_cso = dev->pctx->create_vertex_elements_state(dev->pctx, 0, NULL);
      dev->pctx->bind_vertex_elements_state(dev->pctx, dev->empty_velems_cso);
      dev->pctx->set_sample_mask(dev->pctx, dev->sample_mask);
   }

   /* Wire DXGI 1.0..1.5 callbacks + capture pfnPresentCb / pfnPresentMpoCb. */
   aurora_install_dxgi_funcs(dev, pArgs);

   aurora_log("CreateDevice: SUCCESS native Gallium Asahi screen=%p pctx=%p (no DXVK/Vulkan)",
              (void *)dev->screen, (void *)dev->pctx);
   return S_OK;
}

static HRESULT APIENTRY
aurora_CloseAdapter(D3D10DDI_HADAPTER hAdapter)
{
   struct aurora_adapter *adapter = (struct aurora_adapter *)hAdapter.pDrvPrivate;
   if (adapter)
      HeapFree(GetProcessHeap(), 0, adapter);
   return S_OK;
}

static HRESULT APIENTRY
aurora_GetSupportedVersions(D3D10DDI_HADAPTER hAdapter, UINT32 *puEntries,
                            UINT64 *pSupportedDDIInterfaceVersions)
{
   (void)hAdapter;
   static const UINT64 kVersions[] = {
      D3DWDDM2_0_DDI_SUPPORTED,
      D3D11_1_DDI_SUPPORTED,
      D3D11_0_DDI_SUPPORTED,
   };
   const UINT32 kCount = ARRAY_SIZE(kVersions);
   if (!puEntries)
      return E_INVALIDARG;
   if (!pSupportedDDIInterfaceVersions) {
      *puEntries = kCount;
      return S_OK;
   }
   if (*puEntries < kCount) {
      *puEntries = kCount;
      return E_OUTOFMEMORY;
   }
   for (UINT32 i = 0; i < kCount; ++i)
      pSupportedDDIInterfaceVersions[i] = kVersions[i];
   *puEntries = kCount;
   return S_OK;
}

static HRESULT APIENTRY
aurora_GetCaps(D3D10DDI_HADAPTER hAdapter, const D3D10_2DDIARG_GETCAPS *pArgs)
{
   (void)hAdapter;
   if (!pArgs || !pArgs->pData || !pArgs->DataSize)
      return S_OK;
   ZeroMemory(pArgs->pData, pArgs->DataSize);

   switch (pArgs->Type) {
   case D3D11DDICAPS_THREADING: {
      D3D11DDI_THREADING_CAPS *c = (D3D11DDI_THREADING_CAPS *)pArgs->pData;
      c->Caps = D3D11DDICAPS_FREETHREADED;
      break;
   }
   case D3D11DDICAPS_SHADER: {
      D3D11DDI_SHADER_CAPS *c = (D3D11DDI_SHADER_CAPS *)pArgs->pData;
      c->Caps = D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
      break;
   }
   case D3D11_1DDICAPS_D3D11_OPTIONS: {
      D3D11_1DDI_D3D11_OPTIONS_DATA *c = (D3D11_1DDI_D3D11_OPTIONS_DATA *)pArgs->pData;
      c->OutputMergerLogicOp = TRUE;
      c->AssignDebugBinarySupport = FALSE;
      break;
   }
   case D3D11_1DDICAPS_ARCHITECTURE_INFO: {
      D3D11_1DDI_ARCHITECTURE_INFO_DATA *c = (D3D11_1DDI_ARCHITECTURE_INFO_DATA *)pArgs->pData;
      c->TileBasedDeferredRenderer = FALSE;
      break;
   }
   case D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT: {
      D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA *c =
         (D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA *)pArgs->pData;
      c->PixelShaderMinPrecision = 0;
      c->AllOtherStagesMinPrecision = 0;
      break;
   }
   case D3D11DDICAPS_3DPIPELINESUPPORT: {
      D3D11DDI_3DPIPELINESUPPORT_CAPS *c = (D3D11DDI_3DPIPELINESUPPORT_CAPS *)pArgs->pData;
      c->Caps =
         D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0) |
         D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_1) |
         D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_11_0) |
         D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11_1DDI_3DPIPELINELEVEL_11_1);
      break;
   }
   case D3DWDDM2_0DDICAPS_MEMORY_ARCHITECTURE: {
      if (pArgs->DataSize >= sizeof(D3DWDDM2_0DDI_MEMORY_ARCHITECTURE_CAPS)) {
         D3DWDDM2_0DDI_MEMORY_ARCHITECTURE_CAPS *c =
            (D3DWDDM2_0DDI_MEMORY_ARCHITECTURE_CAPS *)pArgs->pData;
         c->UMA = TRUE;
         c->CacheCoherent = FALSE;
      }
      break;
   }
   case D3DWDDM2_0DDICAPS_GPUVA_CAPS: {
      if (pArgs->DataSize >= sizeof(D3DWDDM2_0DDI_GPUVA_CAPS_DATA)) {
         D3DWDDM2_0DDI_GPUVA_CAPS_DATA *c =
            (D3DWDDM2_0DDI_GPUVA_CAPS_DATA *)pArgs->pData;
         c->MaxGPUVirtualAddressBitsPerResource = 39u;
      }
      break;
   }
   default:
      break;
   }
   return S_OK;
}

static struct aurora_adapter *
adapter_create_common(D3D10DDIARG_OPENADAPTER *pOpenData)
{
   struct aurora_adapter *adapter = (struct aurora_adapter *)
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*adapter));
   if (!adapter)
      return NULL;

   adapter->hRTAdapter = pOpenData->hRTAdapter;
   adapter->query_adapter_info =
      pOpenData->pAdapterCallbacks ? pOpenData->pAdapterCallbacks->pfnQueryAdapterInfoCb : NULL;

   if (adapter->query_adapter_info && adapter->hRTAdapter.handle) {
      D3DDDICB_QUERYADAPTERINFO q;
      memset(&q, 0, sizeof(q));
      q.pPrivateDriverData = &adapter->identity;
      q.PrivateDriverDataSize = sizeof(adapter->identity);
      HRESULT hr = adapter->query_adapter_info(adapter->hRTAdapter.handle, &q);
      if (SUCCEEDED(hr) &&
          adapter->identity.magic == AURORA_AGXW_ADAPTER_IDENTITY_MAGIC &&
          adapter->identity.version == AURORA_AGXW_ADAPTER_IDENTITY_VERSION &&
          adapter->identity.struct_size == sizeof(adapter->identity)) {
         adapter->identity_valid = true;
      }
   }
   pOpenData->hAdapter.pDrvPrivate = adapter;
   return adapter;
}

__declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData);
__declspec(dllexport) HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *pOpenData);

__declspec(dllexport) HRESULT APIENTRY
OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData)
{
   if (!pOpenData || !pOpenData->pAdapterFuncs_2)
      return E_INVALIDARG;
   if (!adapter_create_common(pOpenData))
      return E_OUTOFMEMORY;

   D3D10_2DDI_ADAPTERFUNCS *p = pOpenData->pAdapterFuncs_2;
   p->pfnCalcPrivateDeviceSize = aurora_CalcPrivateDeviceSize;
   p->pfnCreateDevice          = aurora_CreateDevice;
   p->pfnCloseAdapter          = aurora_CloseAdapter;
   p->pfnGetSupportedVersions  = aurora_GetSupportedVersions;
   p->pfnGetCaps               = aurora_GetCaps;
   return S_OK;
}

__declspec(dllexport) HRESULT APIENTRY
OpenAdapter10(D3D10DDIARG_OPENADAPTER *pOpenData)
{
   if (!pOpenData || !pOpenData->pAdapterFuncs)
      return E_INVALIDARG;
   if (!adapter_create_common(pOpenData))
      return E_OUTOFMEMORY;

   D3D10DDI_ADAPTERFUNCS *p = pOpenData->pAdapterFuncs;
   p->pfnCalcPrivateDeviceSize = aurora_CalcPrivateDeviceSize;
   p->pfnCreateDevice          = aurora_CreateDevice;
   p->pfnCloseAdapter          = aurora_CloseAdapter;
   return S_OK;
}
