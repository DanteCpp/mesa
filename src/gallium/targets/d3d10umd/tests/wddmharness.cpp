/*
 * wddmharness: drives g18d3d10.dll's WDDM mode without a kernel driver. It plays the D3D11.1
 * runtime and a g18wddm-like KMD (g18w_uapi.h): the adapter answers the G18W magic,
 * allocations are host memory, Lock returns it, PresentCb checks the presented pixels.
 *
 *   wddmharness [DLL]     lines "WDDMH ...", exit 0 = PASS
 *
 * Steps: open adapter (WDDM mode expected), create device (context), create a 256x256
 * B8G8R8A8 present buffer (KMD allocation), clear it red through an RTV, DXGI Present
 * (PresentCb sees red in the source allocation), open a KMD "GDI" allocation filled with
 * green, copy it into the present buffer, Present again (green), destroy everything.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include "winddk_compat.h"
#include <d3d10umddi.h>

#include "../../../frontends/d3d10umd/g18w_uapi.h"

#define LOG(...) do { printf("WDDMH " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

struct alloc {
   bool used;
   struct g18w_alloc_priv priv;
   void *mem;
   int locks;
};
static struct alloc allocs[64];
static HRESULT last_error;
static unsigned presents;
static uint32_t present_pixel;
static D3DKMT_HANDLE present_src;

static HRESULT APIENTRY
QueryAdapterInfoCb(HANDLE hAdapter, const D3DDDICB_QUERYADAPTERINFO *q)
{
   if (q->PrivateDriverDataSize < sizeof(struct g18w_adapter_info))
      return E_INVALIDARG;
   struct g18w_adapter_info *i = (struct g18w_adapter_info *)q->pPrivateDriverData;
   memset(i, 0, sizeof *i);
   i->magic = G18W_ALLOC_MAGIC;
   i->version = G18W_UAPI_VERSION;
   i->executor = 1;
   i->width = 800;
   i->height = 600;
   i->format = D3DDDIFMT_X8R8G8B8;
   i->segment_size = 256u << 20;
   return S_OK;
}

static D3DKMT_HANDLE
new_alloc(const struct g18w_alloc_priv *p)
{
   for (unsigned i = 1; i < 64; i++) {
      if (allocs[i].used)
         continue;
      allocs[i].used = true;
      allocs[i].priv = *p;
      uint64_t size = p->size ? p->size: (uint64_t)p->pitch * p->height;
      size = (size + 4095) & ~4095ull;
      allocs[i].priv.size = size;
      allocs[i].mem = _aligned_malloc((size_t)size, 4096);
      memset(allocs[i].mem, 0xcd, (size_t)size);
      return i;
   }
   return 0;
}

static HRESULT APIENTRY
AllocateCb(HANDLE hDevice, D3DDDICB_ALLOCATE *a)
{
   for (UINT i = 0; i < a->NumAllocations; i++) {
      D3DDDI_ALLOCATIONINFO *ai = &a->pAllocationInfo[i];
      const struct g18w_alloc_priv *p = (const struct g18w_alloc_priv *)ai->pPrivateDriverData;
      if (ai->PrivateDriverDataSize < sizeof *p || p->magic != G18W_ALLOC_MAGIC) {
         LOG("AllocateCb: bad private data (%u bytes)", ai->PrivateDriverDataSize);
         return E_INVALIDARG;
      }
      ai->hAllocation = new_alloc(p);
      LOG("AllocateCb: alloc %u kind %u %ux%u pitch %u fmt %u bpp %u primary %u", ai->hAllocation, p->kind,
          p->width, p->height, p->pitch, p->format, p->bpp, ai->Flags.Primary);
   }
   a->hKMResource = 0x100;
   return S_OK;
}

static HRESULT APIENTRY
DeallocateCb(HANDLE hDevice, const D3DDDICB_DEALLOCATE *d)
{
   for (UINT i = 0; i < d->NumAllocations; i++) {
      D3DKMT_HANDLE h = d->HandleList[i];
      if (h < 64 && allocs[h].used) {
         if (allocs[h].locks)
            LOG("DeallocateCb: alloc %u still locked %d", h, allocs[h].locks);
         _aligned_free(allocs[h].mem);
         allocs[h].used = false;
      }
   }
   return S_OK;
}

static HRESULT APIENTRY
LockCb(HANDLE hDevice, D3DDDICB_LOCK *l)
{
   if (l->hAllocation >= 64 || !allocs[l->hAllocation].used)
      return E_INVALIDARG;
   allocs[l->hAllocation].locks++;
   l->pData = allocs[l->hAllocation].mem;
   return S_OK;
}

static HRESULT APIENTRY
UnlockCb(HANDLE hDevice, const D3DDDICB_UNLOCK *u)
{
   for (UINT i = 0; i < u->NumAllocations; i++)
      if (u->phAllocations[i] < 64)
         allocs[u->phAllocations[i]].locks--;
   return S_OK;
}

static HRESULT APIENTRY
CreateContextCb(HANDLE hDevice, D3DDDICB_CREATECONTEXT *c)
{
   c->hContext = (HANDLE)0xc0;
   LOG("CreateContextCb: node %u", c->NodeOrdinal);
   return S_OK;
}

static HRESULT APIENTRY
DestroyContextCb(HANDLE hDevice, const D3DDDICB_DESTROYCONTEXT *c)
{
   return S_OK;
}

static HRESULT APIENTRY
SetDisplayModeCb(HANDLE hDevice, D3DDDICB_SETDISPLAYMODE *s)
{
   return S_OK;
}

static HRESULT APIENTRY
DxgiPresentCb(HANDLE hDevice, DXGIDDICB_PRESENT *p)
{
   presents++;
   present_src = p->hSrcAllocation;
   if (p->hSrcAllocation >= 64 || !allocs[p->hSrcAllocation].used) {
      LOG("PresentCb: bad source allocation %u", p->hSrcAllocation);
      return E_INVALIDARG;
   }
   const struct alloc *a = &allocs[p->hSrcAllocation];
   present_pixel = *(const uint32_t *)((const char *)a->mem + (a->priv.height / 2) * a->priv.pitch +
                                       (a->priv.width / 2) * 4);
   LOG("PresentCb: src %u dst %u ctx %p center 0x%08x", p->hSrcAllocation, p->hDstAllocation, p->hContext,
       present_pixel);
   return S_OK;
}

static void APIENTRY
SetErrorCb(D3D10DDI_HRTCORELAYER h, HRESULT hr)
{
   last_error = hr;
   LOG("SetErrorCb 0x%08lx", hr);
}

int
main(int argc, char **argv)
{
   const char *dll = argc > 1 ? argv[1]: "g18d3d10.dll";
   HMODULE m = LoadLibraryA(dll);
   if (!m) {
      LOG("LoadLibrary %s failed %lu", dll, GetLastError());
      return 2;
   }
   PFND3D10DDI_OPENADAPTER open2 = (PFND3D10DDI_OPENADAPTER)GetProcAddress(m, "OpenAdapter10_2");

   D3DDDI_ADAPTERCALLBACKS acb = {};
   acb.pfnQueryAdapterInfoCb = (PFND3DDDI_QUERYADAPTERINFOCB)QueryAdapterInfoCb;
   D3D10_2DDI_ADAPTERFUNCS af = {};
   D3D10DDIARG_OPENADAPTER oa = {};
   oa.hRTAdapter.handle = (HANDLE)0xada;
   oa.Interface = D3D11_1_DDI_INTERFACE_VERSION;
   oa.Version = D3D11_1_DDI_BUILD_VERSION;
   oa.pAdapterCallbacks = &acb;
   oa.pAdapterFuncs_2 = &af;
   HRESULT hr = open2(&oa);
   LOG("OpenAdapter10_2 hr=0x%08lx", hr);
   if (FAILED(hr))
      return 3;

   UINT64 versions[32];
   UINT n = 32;
   af.pfnGetSupportedVersions(oa.hAdapter, &n, versions);
   bool has11_1 = false;
   for (UINT i = 0; i < n; i++)
      has11_1 |= (versions[i] >> 32) == D3D11_1_DDI_INTERFACE_VERSION;
   LOG("GetSupportedVersions %u entries, 11.1 %s", n, has11_1 ? "yes": "no");

   D3D10DDIARG_CALCPRIVATEDEVICESIZE cs = { D3D11_1_DDI_INTERFACE_VERSION, D3D11_1_DDI_BUILD_VERSION, 0 };
   SIZE_T dsize = af.pfnCalcPrivateDeviceSize(oa.hAdapter, &cs);
   void *dmem = calloc(1, dsize);

   D3DDDI_DEVICECALLBACKS kt = {};
   kt.pfnAllocateCb = (PFND3DDDI_ALLOCATECB)AllocateCb;
   kt.pfnDeallocateCb = (PFND3DDDI_DEALLOCATECB)DeallocateCb;
   kt.pfnLockCb = (PFND3DDDI_LOCKCB)LockCb;
   kt.pfnUnlockCb = (PFND3DDDI_UNLOCKCB)UnlockCb;
   kt.pfnCreateContextCb = (PFND3DDDI_CREATECONTEXTCB)CreateContextCb;
   kt.pfnDestroyContextCb = (PFND3DDDI_DESTROYCONTEXTCB)DestroyContextCb;
   kt.pfnSetDisplayModeCb = (PFND3DDDI_SETDISPLAYMODECB)SetDisplayModeCb;
   D3D11DDI_CORELAYER_DEVICECALLBACKS um = {};
   um.pfnSetErrorCb = SetErrorCb;
   DXGI_DDI_BASE_CALLBACKS dcb = {};
   dcb.pfnPresentCb = (PFNDDXGIDDI_PRESENTCB)DxgiPresentCb;
   static D3D11_1DDI_DEVICEFUNCS f;
   static DXGI1_2_DDI_BASE_FUNCTIONS df;

   D3D10DDIARG_CREATEDEVICE cd = {};
   cd.hRTDevice.handle = (HANDLE)0xde7;
   cd.Interface = D3D11_1_DDI_INTERFACE_VERSION;
   cd.Version = D3D11_1_DDI_BUILD_VERSION;
   cd.pKTCallbacks = &kt;
   cd.p11_1DeviceFuncs = &f;
   cd.hDrvDevice.pDrvPrivate = dmem;
   cd.DXGIBaseDDI.pDXGIBaseCallbacks = &dcb;
   cd.DXGIBaseDDI.pDXGIDDIBaseFunctions3 = &df;
   cd.hRTCoreLayer.handle = (HANDLE)0xc1;
   cd.p11UMCallbacks = &um;
   hr = af.pfnCreateDevice(oa.hAdapter, &cd);
   LOG("CreateDevice hr=0x%08lx Present %p Blt1 %p ClearView %p", hr, (void *)df.pfnPresent, (void *)df.pfnBlt1,
       (void *)f.pfnClearView);
   if (FAILED(hr))
      return 4;
   D3D10DDI_HDEVICE hDev = cd.hDrvDevice;

   /* present buffer */
   D3D10DDI_MIPINFO mip = { 256, 256, 1, 256, 256, 1 };
   D3D11DDIARG_CREATERESOURCE cr = {};
   cr.pMipInfoList = &mip;
   cr.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   cr.Usage = D3D10_DDI_USAGE_DEFAULT;
   cr.BindFlags = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT | D3D10_DDI_BIND_SHADER_RESOURCE;
   cr.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   cr.SampleDesc.Count = 1;
   cr.MipLevels = 1;
   cr.ArraySize = 1;
   void *rmem = calloc(1, f.pfnCalcPrivateResourceSize(hDev, &cr));
   D3D10DDI_HRESOURCE hRes = { rmem };
   D3D10DDI_HRTRESOURCE hRTRes = { (HANDLE)0x5e5 };
   f.pfnCreateResource(hDev, &cr, hRes, hRTRes);
   LOG("CreateResource (present buffer) error 0x%08lx", last_error);
   if (FAILED(last_error))
      return 5;

   D3D10DDIARG_CREATERENDERTARGETVIEW rv = {};
   rv.hDrvResource = hRes;
   rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   rv.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
   rv.Tex2D.MipSlice = 0;
   rv.Tex2D.FirstArraySlice = 0;
   rv.Tex2D.ArraySize = 1;
   void *vmem = calloc(1, f.pfnCalcPrivateRenderTargetViewSize(hDev, &rv));
   D3D10DDI_HRENDERTARGETVIEW hRtv = { vmem };
   D3D10DDI_HRTRENDERTARGETVIEW hRTRtv = { (HANDLE)0x7f7 };
   f.pfnCreateRenderTargetView(hDev, &rv, hRtv, hRTRtv);
   FLOAT red[4] = { 1, 0, 0, 1 };
   f.pfnClearRenderTargetView(hDev, hRtv, red);

   DXGI_DDI_ARG_PRESENT pr = {};
   pr.hDevice = (DXGI_DDI_HDEVICE)dmem;
   pr.hSurfaceToPresent = (DXGI_DDI_HRESOURCE)rmem;
   pr.pDXGIContext = (void *)0xd1;
   hr = df.pfnPresent(&pr);
   bool ok1 = SUCCEEDED(hr) && presents == 1 && (present_pixel & 0xffffff) == 0xff0000;
   LOG("Present 1 hr=0x%08lx center 0x%08x -> %s", hr, present_pixel, ok1 ? "PASS": "FAIL");

   /* a KMD standard (GDI) allocation opened by the UMD, as DWM does */
   struct g18w_alloc_priv gp = {};
   gp.magic = G18W_ALLOC_MAGIC;
   gp.version = G18W_UAPI_VERSION;
   gp.kind = G18W_ALLOC_GDI;
   gp.width = 256;
   gp.height = 256;
   gp.pitch = 1024;
   gp.bpp = 4;
   gp.format = D3DDDIFMT_A8R8G8B8;
   D3DKMT_HANDLE ga = new_alloc(&gp);
   for (unsigned y = 0; y < 256; y++)
      for (unsigned x = 0; x < 256; x++)
         ((uint32_t *)allocs[ga].mem)[y * 256 + x] = 0xff00ff00;
   D3DDDI_OPENALLOCATIONINFO oi = {};
   oi.hAllocation = ga;
   oi.pPrivateDriverData = &allocs[ga].priv;
   oi.PrivateDriverDataSize = sizeof allocs[ga].priv;
   D3D10DDIARG_OPENRESOURCE orr = {};
   orr.NumAllocations = 1;
   orr.pOpenAllocationInfo = &oi;
   void *omem = calloc(1, f.pfnCalcPrivateOpenedResourceSize(hDev, &orr));
   D3D10DDI_HRESOURCE hOpen = { omem };
   D3D10DDI_HRTRESOURCE hRTOpen = { (HANDLE)0x0be };
   last_error = S_OK;
   f.pfnOpenResource(hDev, &orr, hOpen, hRTOpen);
   LOG("OpenResource (GDI allocation %u) error 0x%08lx", ga, last_error);
   f.pfnResourceCopy(hDev, hRes, hOpen);
   hr = df.pfnPresent(&pr);
   bool ok2 = SUCCEEDED(hr) && presents == 2 && SUCCEEDED(last_error) && (present_pixel & 0xffffff) == 0x00ff00;
   LOG("Present 2 (after copy from the opened allocation) hr=0x%08lx center 0x%08x -> %s", hr, present_pixel,
       ok2 ? "PASS": "FAIL");

   f.pfnDestroyResource(hDev, hOpen);
   f.pfnDestroyRenderTargetView(hDev, hRtv);
   f.pfnDestroyResource(hDev, hRes);
   f.pfnDestroyDevice(hDev);
   af.pfnCloseAdapter(oa.hAdapter);
   unsigned live = 0;
   for (unsigned i = 1; i < 64; i++)
      live += allocs[i].used && i != ga;
   LOG("teardown: %u UMD allocations left, GDI allocation locks %d", live, allocs[ga].locks);
   bool ok = ok1 && ok2 && live == 0 && allocs[ga].locks == 0;
   LOG("RESULT %s", ok ? "PASS": "FAIL");
   return ok ? 0: 1;
}
