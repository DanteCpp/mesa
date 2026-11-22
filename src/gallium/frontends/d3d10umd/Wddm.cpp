/*
 * WDDM ("hardware") mode of the D3D10/11 UMD: used when the KMD (g18wddm.sys) answers
 * DXGKQAITYPE_UMDRIVERPRIVATE with a g18w_adapter_info (g18w_uapi.h, shared with the KMD).
 *
 * Rendering stays on the CPU (Gallium software driver). Resources the kernel has to see
 * (present buffers, primaries, shared resources) get a KMD allocation through
 * pfnAllocateCb, stay locked (pfnLockCb) for their lifetime and are wrapped as Gallium
 * display targets over the locked memory. Present goes through pfnPresentCb. Resources
 * DWM opens (shared, GDI redirection, standard allocations) are wrapped the same way from
 * the KMD's g18w_alloc_priv.
 *
 * SPDX-License-Identifier: MIT
 */

#include "Draw.h"
#include "Format.h"
#include "Resource.h"
#include "State.h"
#include "Wddm.h"

#include "Debug.h"

#include "frontend/winsys_handle.h"
#include "sw/gdi/gdi_sw_winsys.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/box.h"
#include "util/os_time.h"

#include "g18w_uapi.h"
#include "Perf.h"

#define G18W_PITCH_ALIGN 256
/* G6 zero copy: hk's linear row pitch (ail: the cache line, 128 bytes), a multiple of the scan-out's 64 */
#define G18W_ZC_PITCH_ALIGN 128

/* DXGI_FORMAT <-> D3DDDIFORMAT for the formats that reach KMD allocations */
static const struct {
   DXGI_FORMAT dxgi;
   D3DDDIFORMAT ddi;
} format_map[] = {
   { DXGI_FORMAT_B8G8R8A8_UNORM, D3DDDIFMT_A8R8G8B8 },
   { DXGI_FORMAT_B8G8R8X8_UNORM, D3DDDIFMT_X8R8G8B8 },
   { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3DDDIFMT_A8R8G8B8 },
   { DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, D3DDDIFMT_X8R8G8B8 },
   { DXGI_FORMAT_R8G8B8A8_UNORM, D3DDDIFMT_A8B8G8R8 },
   { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3DDDIFMT_A8B8G8R8 },
   { DXGI_FORMAT_R10G10B10A2_UNORM, D3DDDIFMT_A2B10G10R10 },
   { DXGI_FORMAT_R16G16B16A16_FLOAT, D3DDDIFMT_A16B16G16R16F },
   { DXGI_FORMAT_B5G6R5_UNORM, D3DDDIFMT_R5G6B5 },
   { DXGI_FORMAT_A8_UNORM, D3DDDIFMT_A8 },
   { DXGI_FORMAT_R8_UNORM, D3DDDIFMT_L8 },
};

static D3DDDIFORMAT
DxgiToDdi(DXGI_FORMAT f)
{
   for (unsigned i = 0; i < ARRAY_SIZE(format_map); i++)
      if (format_map[i].dxgi == f)
         return format_map[i].ddi;
   return D3DDDIFMT_UNKNOWN;
}

static DXGI_FORMAT
DdiToDxgi(UINT f)
{
   for (unsigned i = 0; i < ARRAY_SIZE(format_map); i++)
      if ((UINT)format_map[i].ddi == f)
         return format_map[i].dxgi;
   return DXGI_FORMAT_UNKNOWN;
}

bool
WddmQueryAdapter(const D3D10DDIARG_OPENADAPTER *pOpenData, Adapter *pAdapter)
{
   pAdapter->wddm = false;
   pAdapter->pitch_align = G18W_PITCH_ALIGN;

   if (!pOpenData->pAdapterCallbacks || !pOpenData->pAdapterCallbacks->pfnQueryAdapterInfoCb)
      return false;

   struct g18w_adapter_info info;
   memset(&info, 0, sizeof info);
   D3DDDICB_QUERYADAPTERINFO q;
   q.pPrivateDriverData = &info;
   q.PrivateDriverDataSize = sizeof info;
   HRESULT hr = pOpenData->pAdapterCallbacks->pfnQueryAdapterInfoCb(pOpenData->hRTAdapter.handle, &q);
   if (UmdTrace())
      DebugPrintf("QueryAdapterInfoCb hr 0x%08lx magic 0x%08x version %u executor %u mode %ux%u fmt %u segment %llu MiB\n",
                  hr, info.magic, info.version, info.executor, info.width, info.height, info.format,
                  (unsigned long long)(info.segment_size >> 20));
   if (FAILED(hr) || info.magic != G18W_ALLOC_MAGIC || info.version != G18W_UAPI_VERSION)
      return false;

   pAdapter->wddm = true;
   pAdapter->zc_kmd = (info.flags & G18W_ADAPTERF_GPU_ALLOC) != 0;
   pAdapter->scan_w = info.width;
   pAdapter->scan_h = info.height;
   pAdapter->segment_size = info.segment_size;
   pAdapter->hRTAdapter = pOpenData->hRTAdapter.handle;
   return true;
}

/*
 * G6 zero copy (track Z). On the GPU screen (zink on hk), a KMD allocation the GPU renders into (DWM's back
 * buffers, any non-shared present/GDI surface, the scan-out buffers below) is created with G18W_ALLOCF_GPU: the
 * KMD keeps it in segment 1, the pool's WDDM window, with hk's linear pitch. The UMD keeps it locked (VidMm does
 * not move a locked allocation) and hands the locked range to zink as user memory: hk imports it through
 * VK_EXT_external_memory_host, the render node's GEM_USERPTR (which admits WDDM window pages only). The zink
 * texture then IS the allocation: no shadow BO in the render node's arena, no copy in or out at Present, and the
 * scan-out flips the pages the GPU wrote. C:\g6u\zc.off turns it off; C:\g6u\zc.direct flips the back buffer
 * itself instead of a GPU copy into a scan-out buffer.
 */
static UINT64
PerfNow(void)
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (UINT64)t.QuadPart;
}

static bool
FlagFile(const char *path)
{
   return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

UINT64
WddmPerfNow(void)
{
   return PerfNow();
}

/* ---- G6 r7 (A): the KMD's UMD flip queue (G18W_ESC_FLIPQ) ---- */
static bool
ZcFlipQuery(Device *pDevice, struct g18w_esc_flipq *q)
{
   memset(q, 0, sizeof *q);
   q->code = G18W_ESC_FLIPQ;
   if (!pDevice->KTCallbacks.pfnEscapeCb)
      return false;
   D3DDDICB_ESCAPE e;
   memset(&e, 0, sizeof e);
   e.hDevice = pDevice->hDevice;
   e.pPrivateDriverData = q;
   e.PrivateDriverDataSize = sizeof *q;
   HRESULT hr = pDevice->KTCallbacks.pfnEscapeCb(pDevice->hRTAdapter, &e);
   if (FAILED(hr) || q->code != G18W_ESC_FLIPQ || q->version < 1)
      return false;
   pDevice->zcSeenDone = q->tag_done;
   pDevice->zcSeenSignaled = q->tag_signaled;
   return true;
}

/* the frame tagged t is behind `seen` (a KMD tag): the same device's later frame, or (another device's frame) any */
static bool
ZcTagPassed(UINT64 seen, UINT64 t)
{
   if (!t)
      return true;
   if (!seen)
      return false;
   return (seen >> 32) != (t >> 32) || seen > t;
}

/* pipelined presents: on unless C:\g6u\flipq.off or the KMD does not answer G18W_ESC_FLIPQ */
static bool
ZcFlipQOn(Device *pDevice)
{
   if (pDevice->zcFlipQ < 0) {
      static LONG devices;
      struct g18w_esc_flipq q;
      const char *why = NULL;
      if (FlagFile("C:\\g6u\\flipq.off"))
         why = "C:\\g6u\\flipq.off";
      else if (!ZcFlipQuery(pDevice, &q))
         why = "the KMD does not answer G18W_ESC_FLIPQ";
      pDevice->zcFlipQ = why ? 0: 1;
      pDevice->zcTagNs = ((UINT64)(GetCurrentProcessId() & 0xffffff) << 40) |
                         ((UINT64)(InterlockedIncrement(&devices) & 0xff) << 32);
      if (UmdTrace())
         DebugPrintf("zc: pipelined presents %s%s%s (tags %#llx; KMD render node %u, done %#llx)\n",
                     pDevice->zcFlipQ ? "on": "off", why ? ": ": "", why ? why: "",
                     (unsigned long long)pDevice->zcTagNs, why ? 0: q.rn, (unsigned long long)(why ? 0: q.tag_done));
   }
   return pDevice->zcFlipQ == 1;
}

/* the GPU finished every job submitted before the frame tagged t was presented (its flip fence signalled) */
static bool
ZcTagSignaled(Device *pDevice, UINT64 t)
{
   if (!t)
      return true;
   UINT64 s = pDevice->zcSeenSignaled;
   if ((s >> 32) == (t >> 32) && s >= t)
      return true;
   struct g18w_esc_flipq q;
   if (!ZcFlipQuery(pDevice, &q))
      return false;
   s = q.tag_signaled;
   return (s >> 32) == (t >> 32) && s >= t;
}

/* G6 r7 (A): DWM calls Flush right before its Present. While its frames reach the panel through pipelined
 * scan-out copies, that Flush is left to the Present: one zink batch (one submit, one render-node round trip) per
 * frame instead of two. Only for the device whose last two presents were pipelined; C:\\g6u\\flushdefer.off. */
bool
WddmDeferFlush(Device *pDevice)
{
   if (pDevice->flushDefer == 0)
      return false;
   if (pDevice->zcFlipQ != 1 || pDevice->zcScanCount < 2 || pDevice->frame - pDevice->zcPipeFrame > 2 || !pDevice->zcPresents)
      return false;
   if (pDevice->flushDefer < 0) {
      pDevice->flushDefer = FlagFile("C:\\g6u\\flushdefer.off") ? 0: 1;
      if (UmdTrace())
         DebugPrintf("zc: Flush before Present %s\n", pDevice->flushDefer ? "left to the Present": "kept (flushdefer.off)");
      if (!pDevice->flushDefer)
         return false;
   }
   pDevice->perfFlushDeferred++;
   return true;
}

/* ---- G6 r7 (A): writer generations of shared surfaces ----
 * The UMD that creates a shared resource puts a share key into its private data (g18w_alloc_priv reserved[0..1])
 * and bumps the key's generation after each CPU copy into the allocation (WddmCopyOut); a reader's proxy is
 * refreshed only when the generation moved (0 or no key: unknown, refreshed every frame as before). A session-local
 * table (named section, NULL DACL). Keys are never removed; a full table records nothing (readers refresh). */
#define G18W_GEN_MAGIC 0x31474e47u /* "GNG1" */
#define G18W_GEN_SLOTS 4095u
struct GenSlot {
   volatile LONG64 key, gen;
};
struct GenTable {
   UINT32 magic, version, nslots, pad;
   GenSlot s[G18W_GEN_SLOTS];
};

static GenTable *
GenTableGet(void)
{
   static GenTable *table;
   static volatile LONG tried;
   if (InterlockedCompareExchange(&tried, 1, 0) == 0) {
      SECURITY_DESCRIPTOR sd;
      SECURITY_ATTRIBUTES sa;
      sa.nLength = sizeof sa;
      sa.lpSecurityDescriptor = NULL;
      sa.bInheritHandle = FALSE;
      if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE))
         sa.lpSecurityDescriptor = &sd;
      HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(GenTable), "Local\\G18WShareGen1");
      GenTable *g = h ? (GenTable *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GenTable)): NULL;
      if (g) {
         InterlockedCompareExchange((volatile LONG *)&g->magic, (LONG)G18W_GEN_MAGIC, 0);
         if (g->magic == G18W_GEN_MAGIC)
            table = g;
      }
      if (UmdTrace())
         DebugPrintf("zc: share generations %s (%lu)\n", table ? "mapped": "unavailable", table ? 0ul: GetLastError());
   }
   return table;
}

static GenSlot *
GenSlotFor(UINT64 key, bool insert)
{
   GenTable *g = key ? GenTableGet(): NULL;
   if (!g)
      return NULL;
   UINT h = (UINT)((key * 0x9E3779B97F4A7C15ull) >> 40) % G18W_GEN_SLOTS;
   for (UINT n = 0; n < 64; n++, h = (h + 1) % G18W_GEN_SLOTS) {
      LONG64 k = g->s[h].key;
      if (k == (LONG64)key)
         return &g->s[h];
      if (k == 0) {
         if (!insert)
            return NULL;
         LONG64 prev = InterlockedCompareExchange64(&g->s[h].key, (LONG64)key, 0);
         if (prev == 0 || prev == (LONG64)key)
            return &g->s[h];
      }
   }
   return NULL;
}

static void
GenBump(UINT64 key)
{
   GenSlot *s = GenSlotFor(key, true);
   if (s)
      InterlockedIncrement64(&s->gen);
}

static UINT64
GenRead(UINT64 key)
{
   GenSlot *s = GenSlotFor(key, false);
   return s ? (UINT64)s->gen: 0;
}

static UINT64
ShareKeyNew(void)
{
   static volatile LONG n;
   LARGE_INTEGER q;
   QueryPerformanceCounter(&q);
   UINT64 k = ((UINT64)GetCurrentProcessId() << 44) ^ ((UINT64)q.QuadPart << 8) ^ (UINT64)InterlockedIncrement(&n);
   return k ? k: 1;
}

static UINT
ZcPitch(UINT width, UINT cpp)
{
   return align(width * cpp, G18W_ZC_PITCH_ALIGN);
}

/* GPU work so far is done (zero-copy surfaces are read by the KMD's executor and the scan-out, which do not wait
 * for the GPU) */
static void
ZcFinish(Device *pDevice)
{
   UINT64 t0 = PerfNow();
   struct pipe_screen *screen = pDevice->pipe->screen;
   struct pipe_fence_handle *f = NULL;
   pDevice->pipe->flush(pDevice->pipe, &f, 0);
   if (f) {
      screen->fence_finish(screen, NULL, f, OS_TIMEOUT_INFINITE);
      screen->fence_reference(screen, &f, NULL);
   }
   pDevice->perfFinish += PerfNow() - t0;
}

/* wrap the locked [pData, pData + pitch * height) as the pipe texture (linear, imported). False: not possible
 * (the caller shadows it as before). */
static bool
ZcWrap(Device *pDevice, Resource *pResource, const struct pipe_resource *templ, void *pData, UINT pitch)
{
   struct pipe_screen *screen = pDevice->pipe->screen;
   if (!pDevice->zc || templ->target != PIPE_TEXTURE_2D || templ->last_level != 0 || templ->array_size != 1 ||
       templ->nr_samples > 1 || (!pDevice->dt_capable && !screen->resource_from_user_memory))
      return false;
   struct pipe_resource t = *templ;
   struct pipe_resource *r;
   uint64_t stride = 0;
   bool ok;
   if (pDevice->dt_capable) {
      /* C:\g6u\zc.sim (QEMU, softpipe): the same allocations, presents and scan-out buffers, the texture a
       * display target over the locked memory (no render node to import into) */
      struct winsys_handle wh;
      memset(&wh, 0, sizeof wh);
      wh.type = GDI_SW_HANDLE_TYPE_USER_MEMORY;
      wh.handle = pData;
      wh.stride = pitch;
      t.bind |= PIPE_BIND_DISPLAY_TARGET;
      r = screen->resource_from_handle(screen, &t, &wh, 0);
      stride = pitch;
      ok = r != NULL;
   } else {
      t.bind &= ~(PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED | PIPE_BIND_SCANOUT);
      t.bind |= PIPE_BIND_LINEAR;
      r = screen->resource_from_user_memory(screen, &t, pData);
      ok = r && screen->resource_get_param &&
           screen->resource_get_param(screen, pDevice->pipe, r, 0, 0, 0, PIPE_RESOURCE_PARAM_STRIDE, 0, &stride) &&
           stride == pitch;
   }
   static unsigned logged;
   if (UmdTrace() && (logged++ < 32 || !ok))
      DebugPrintf("zc: wrap %ux%u fmt %s at %p pitch %u: %s (stride %llu)\n", t.width0, t.height0,
                  util_format_short_name(t.format), pData, pitch, !r ? "import refused": ok ? "zero copy": "pitch differs",
                  (unsigned long long)stride);
   if (!ok) {
      pipe_resource_reference(&r, NULL);
      return false;
   }
   pResource->resource = r;
   pResource->shadowed = false;
   pResource->zeroCopy = true;
   return true;
}


HRESULT
WddmCreateDevice(Device *pDevice)
{
   D3DDDICB_CREATECONTEXT cc;
   memset(&cc, 0, sizeof cc);
   cc.NodeOrdinal = 0;
   cc.EngineAffinity = 0;
   HRESULT hr = pDevice->KTCallbacks.pfnCreateContextCb(pDevice->hDevice, &cc);
   if (UmdTrace())
      DebugPrintf("CreateContextCb hr 0x%08lx hContext %p cmdbuf %p/%u alloclist %u patchlist %u\n",
                  hr, cc.hContext, cc.pCommandBuffer, cc.CommandBufferSize, cc.AllocationListSize,
                  cc.PatchLocationListSize);
   if (FAILED(hr))
      return hr;
   pDevice->hContext = cc.hContext;
   pDevice->zcFlipQ = -1;
   pDevice->pq = -1;
   pDevice->flushDefer = -1;
   pDevice->pCommandBuffer = cc.pCommandBuffer;
   pDevice->CommandBufferSize = cc.CommandBufferSize;
   pDevice->pAllocationList = cc.pAllocationList;
   pDevice->AllocationListSize = cc.AllocationListSize;
   pDevice->pPatchLocationList = cc.pPatchLocationList;
   pDevice->PatchLocationListSize = cc.PatchLocationListSize;

   if (pDevice->zc) {
      /* zero copy needs zink's user-memory import and room in segment 1 for the back buffer, the scan-out
       * buffers and the primaries (four full-screen surfaces at least) */
      struct pipe_screen *screen = pDevice->pipe ? pDevice->pipe->screen: NULL;
      UINT64 frame = (UINT64)ZcPitch(pDevice->scanW, 4) * pDevice->scanH;
      const char *why = NULL;
      if (FlagFile("C:\\g6u\\zc.off"))
         why = "C:\\g6u\\zc.off";
      else if (pDevice->dt_capable && !FlagFile("C:\\g6u\\zc.sim"))
         why = "software screen (C:\\g6u\\zc.sim exercises it)";
      else if (!pDevice->dt_capable && screen && !screen->resource_from_user_memory)
         why = "the screen imports no user memory";
      else if (pDevice->segmentSize < 4 * frame)
         why = "segment 1 holds fewer than four full-screen surfaces";
      if (why)
         pDevice->zc = false;
      if (UmdTrace())
         DebugPrintf("zc: %s (segment 1 %llu MiB, scan-out %ux%u)%s%s\n", pDevice->zc ? "on": "off",
                     (unsigned long long)(pDevice->segmentSize >> 20), pDevice->scanW, pDevice->scanH,
                     why ? ": ": "", why ? why: "");
   }
   return S_OK;
}


static void ZcScanFree(Device *pDevice);
static void ZcAsyncStop(Device *pDevice);

void
WddmDestroyDevice(Device *pDevice)
{
   ZcAsyncStop(pDevice);
   if (pDevice->rotTemp)
      pipe_resource_reference(&pDevice->rotTemp, NULL);
   ZcScanFree(pDevice);
   if (pDevice->hZcSync) {
      D3DDDICB_DEALLOCATE d;
      memset(&d, 0, sizeof d);
      d.NumAllocations = 1;
      d.HandleList = &pDevice->hZcSync;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
      pDevice->hZcSync = 0;
   }
   if (UmdTrace() && pDevice->zcPresents)
      DebugPrintf("zc: %llu zero-copy presents, %llu flipped the back buffer itself\n",
                  (unsigned long long)pDevice->zcPresents, (unsigned long long)pDevice->zcFlipsDirect);
   if (pDevice->hContext && pDevice->KTCallbacks.pfnDestroyContextCb) {
      D3DDDICB_DESTROYCONTEXT dc;
      dc.hContext = pDevice->hContext;
      pDevice->KTCallbacks.pfnDestroyContextCb(pDevice->hDevice, &dc);
      pDevice->hContext = NULL;
   }
}


bool
WddmNeedsAllocation(const D3D10DDIARG_CREATERESOURCE *pCreate)
{
   if (pCreate->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D)
      return false;
   return pCreate->pPrimaryDesc != NULL ||
          (pCreate->BindFlags & D3D10_DDI_BIND_PRESENT) ||
          (pCreate->MiscFlags & (D3D10_DDI_RESOURCE_MISC_SHARED |
                                 0x100 /* D3D11 SHARED_KEYEDMUTEX */ |
                                 0x200 /* D3D11 GDI_COMPATIBLE */));
}


static bool
SubmitPacket(Device *pDevice, const void *pkt, UINT bytes, const D3DKMT_HANDLE *allocs, const UINT *write,
             const UINT *patch_offsets, UINT n);
static bool
SubmitPacketN(Device *pDevice, const void *pkt, UINT bytes, const D3DKMT_HANDLE *allocs, const UINT *write,
              const UINT *patch_offsets, UINT n, UINT npatch);

/* G6 zero copy (M6 run 20261120-180702): a new allocation is not in segment 1 until a DMA buffer names it
 * (WDDM 1.3); a Lock before that maps VidMm's pageable backing store, contiguous only per 64 KiB, and the
 * render node refuses the import. A 4-byte executor fill names the allocation first, so VidMm pages it into
 * segment 1; the Lock below waits for that fill and then maps the segment pages themselves. */
static void
ZcMakeResident(Device *pDevice, D3DKMT_HANDLE hAllocation)
{
   struct g18w_fill f;
   memset(&f, 0, sizeof f);
   f.hdr.op = G18W_OP_FILL;
   f.hdr.bytes = sizeof f;
   f.dst.index = 0;
   f.bytes = 4;
   f.pattern = 0;
   UINT w = 1, po = offsetof(struct g18w_fill, dst);
   bool ok = SubmitPacket(pDevice, &f, sizeof f, &hAllocation, &w, &po, 1);
   static unsigned logged;
   if (UmdTrace() && (logged++ < 16 || !ok))
      DebugPrintf("zc: residency fill for alloc 0x%x: %s\n", hAllocation, ok ? "submitted": "FAILED");
}

static bool
LockAndWrap(Device *pDevice, Resource *pResource, D3DKMT_HANDLE hAllocation,
            const struct pipe_resource *templ, UINT pitch, bool quiet = false, bool zc = false)
{
   if (zc && !pDevice->dt_capable)
      ZcMakeResident(pDevice, hAllocation);
   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof lock);
   lock.hAllocation = hAllocation;
   HRESULT hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
   if (UmdTrace() && (!quiet || SUCCEEDED(hr)))
      DebugPrintf("LockCb alloc 0x%x hr 0x%08lx pData %p\n", hAllocation, hr, lock.pData);
   if (FAILED(hr) || !lock.pData)
      return false;

   struct pipe_screen *screen = pDevice->pipe->screen;
   if (zc && ZcWrap(pDevice, pResource, templ, lock.pData, pitch)) {
      /* G6 zero copy: the texture is the locked allocation */
   } else if (pDevice->dt_capable) {
      struct winsys_handle wh;
      memset(&wh, 0, sizeof wh);
      wh.type = GDI_SW_HANDLE_TYPE_USER_MEMORY;
      wh.handle = lock.pData;
      wh.stride = pitch;
      pResource->resource = screen->resource_from_handle(screen, templ, &wh, 0);
      pResource->shadowed = false;
   } else {
      /* GPU screen (zink): own the texture, copy to/from the allocation at the sync points */
      struct pipe_resource t = *templ;
      t.bind &= ~(PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED | PIPE_BIND_SCANOUT);
      /* M6 (track H): a linear shadow is mapped in place for the copies (no full-size staging
       * buffer per copy: a 3024x1964 surface is 23 MiB, and DWM ran the render node's BO arena
       * dry); a tiled one only when the screen refuses linear */
      if (t.target == PIPE_TEXTURE_2D && t.last_level == 0 && t.array_size == 1 && t.nr_samples <= 1) {
         struct pipe_resource lt = t;
         lt.bind |= PIPE_BIND_LINEAR;
         pResource->resource = screen->resource_create(screen, &lt);
      }
      if (!pResource->resource)
         pResource->resource = screen->resource_create(screen, &t);
      pResource->shadowed = true;
   }
   if (!pResource->resource) {
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &hAllocation;
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
      return false;
   }
   pResource->hAllocation = hAllocation;
   pResource->pAllocationData = lock.pData;
   pResource->allocationPitch = pitch;
   return true;
}


/* G6 zero copy for opened shared surfaces (M6 runs 20261120-180702/190408: DWM refreshed every bound shared
 * surface once per frame by a KMD BLT into a private copy plus a CPU upload of that copy into a zink texture,
 * two 23 MiB CPU copies per full-screen surface per frame, ~13 flips/s). The private copy is now a segment-1
 * GPU allocation imported zero copy: the KMD's BLT writes the texture's own pages, no upload. False: the
 * caller makes the old proxy. */
static bool
ZcProxy(Device *pDevice, Resource *pResource, const struct pipe_resource *templ, D3DKMT_HANDLE hOpened,
        const struct g18w_alloc_priv *priv)
{
   UINT bpp = priv->bpp ? priv->bpp: 4;
   /* R6L: the proxy is unlocked once imported (a DMA buffer naming a locked allocation is refused), so its
    * refresh BLT goes through; C:\\g6u\\zcproxy.off keeps the old proxy */
   static int optout = -1;
   if (optout < 0)
      optout = FlagFile("C:\\g6u\\zcproxy.off");
   if (optout || !pDevice->zc || pDevice->dt_capable || bpp != 4)
      return false;
   struct g18w_alloc_priv p2 = *priv;
   p2.kind = G18W_ALLOC_SURFACE;
   p2.flags = G18W_ALLOCF_GPU;
   p2.bpp = 4;
   p2.pitch = ZcPitch(p2.width, 4);
   p2.size = align64((uint64_t)p2.pitch * p2.height, G18W_GPU_ALIGN);
   D3DDDI_ALLOCATIONINFO ai;
   memset(&ai, 0, sizeof ai);
   ai.pPrivateDriverData = &p2;
   ai.PrivateDriverDataSize = sizeof p2;
   D3DDDICB_ALLOCATE a;
   memset(&a, 0, sizeof a);
   a.NumAllocations = 1;
   a.pAllocationInfo = &ai;
   HRESULT hr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &a);
   if (FAILED(hr))
      return false;
   ZcMakeResident(pDevice, ai.hAllocation);
   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof lock);
   lock.hAllocation = ai.hAllocation;
   HRESULT hl = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
   bool ok = SUCCEEDED(hl) && lock.pData && ZcWrap(pDevice, pResource, templ, lock.pData, p2.pitch);
   if (UmdTrace())
      DebugPrintf("zc: proxy for shared 0x%x %ux%u pitch %u -> alloc 0x%x lock 0x%08lx %p: %s\n", hOpened, p2.width,
                  p2.height, p2.pitch, ai.hAllocation, hl, lock.pData, ok ? "zero copy": "old proxy");
   if (!ok) {
      if (SUCCEEDED(hl)) {
         D3DDDICB_UNLOCK ul;
         ul.NumAllocations = 1;
         ul.phAllocations = &ai.hAllocation;
         pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
      }
      D3DDDICB_DEALLOCATE d;
      memset(&d, 0, sizeof d);
      d.NumAllocations = 1;
      d.HandleList = &ai.hAllocation;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
      return false;
   }
   /* R6L: unlocked after the import (only the executor's BLT and the GPU touch the pages from here on) */
   {
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &ai.hAllocation;
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
   }
   pResource->hAllocation = ai.hAllocation;
   pResource->pAllocationData = NULL;
   pResource->allocationPitch = p2.pitch;
   pResource->ownsAllocation = true;
   pResource->hShared = hOpened;
   pResource->sharedBpp = 4;
   pResource->sharedPitch = priv->pitch;
   pResource->proxiedNext = pDevice->proxiedHead;
   pDevice->proxiedHead = pResource;
   pResource->transfers = (struct pipe_transfer **)calloc(1, sizeof *pResource->transfers);
   return true;
}


/* ---- G6 r7 (A): pipelined proxies of opened shared surfaces ----
 * R6M: every refresh of a zero-copy proxy waited for the GPU (it could still sample the old content), then for the
 * executor's BLT (a lock round trip), ~28 ms each. Now the BLT writes one of two staging allocations, the GPU copies
 * staging -> the proxy texture in the frame that samples it (zink orders it before the draw; nothing waits for the
 * GPU), and a refresh happens only when the writer's generation moved (G18WShareGen1) or, for a surface without one,
 * when the executor saw the source change (G18W_BLTF_IF_CHANGED + G18W_FILLF_BLT_STATUS). BLTs of changed surfaces
 * are issued at the frame's first draw, the wait at the sampling draw usually finds them done. A staging is written
 * again only once the GPU finished the frame that copied from it (its flip tag signalled), else the other one.
 * C:\\g6u\\zc.sim: the staging is copied on the CPU (a lock waits for the BLT). C:\\g6u\\proxyq.off: the R6 proxies. */
static D3DKMT_HANDLE
PqAllocSync(Device *pDevice)
{
   struct g18w_alloc_priv sp;
   memset(&sp, 0, sizeof sp);
   sp.magic = G18W_ALLOC_MAGIC;
   sp.version = G18W_UAPI_VERSION;
   sp.kind = G18W_ALLOC_BUFFER;
   sp.size = 4096;
   D3DDDI_ALLOCATIONINFO ai;
   memset(&ai, 0, sizeof ai);
   ai.pPrivateDriverData = &sp;
   ai.PrivateDriverDataSize = sizeof sp;
   D3DDDICB_ALLOCATE a;
   memset(&a, 0, sizeof a);
   a.NumAllocations = 1;
   a.pAllocationInfo = &ai;
   return SUCCEEDED(pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &a)) ? ai.hAllocation: 0;
}

static void
PqFreeAlloc(Device *pDevice, D3DKMT_HANDLE h)
{
   if (!h)
      return;
   D3DDDICB_DEALLOCATE d;
   memset(&d, 0, sizeof d);
   d.NumAllocations = 1;
   d.HandleList = &h;
   pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
}

/* a segment-1 staging allocation: imported zero copy and unlocked (GPU screens), or a plain allocation (zc.sim) */
static Resource *
PqStageNew(Device *pDevice, const struct pipe_resource *templ)
{
   UINT pitch = ZcPitch(templ->width0, 4);
   struct g18w_alloc_priv priv;
   memset(&priv, 0, sizeof priv);
   priv.magic = G18W_ALLOC_MAGIC;
   priv.version = G18W_UAPI_VERSION;
   priv.kind = G18W_ALLOC_SURFACE;
   priv.flags = G18W_ALLOCF_GPU;
   priv.format = D3DDDIFMT_A8R8G8B8;
   priv.bpp = 4;
   priv.width = templ->width0;
   priv.height = templ->height0;
   priv.pitch = pitch;
   priv.size = align64((uint64_t)pitch * templ->height0, G18W_GPU_ALIGN);
   D3DDDI_ALLOCATIONINFO ai;
   memset(&ai, 0, sizeof ai);
   ai.pPrivateDriverData = &priv;
   ai.PrivateDriverDataSize = sizeof priv;
   D3DDDICB_ALLOCATE a;
   memset(&a, 0, sizeof a);
   a.NumAllocations = 1;
   a.pAllocationInfo = &ai;
   if (FAILED(pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &a)))
      return NULL;
   Resource *r = (Resource *)calloc(1, sizeof *r);
   if (!r) {
      PqFreeAlloc(pDevice, ai.hAllocation);
      return NULL;
   }
   r->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   r->MipLevels = 1;
   r->NumSubResources = 1;
   r->ownsAllocation = true;
   r->hAllocation = ai.hAllocation;
   r->allocationPitch = pitch;
   if (pDevice->dt_capable) {
      ZcMakeResident(pDevice, ai.hAllocation);
      return r;
   }
   struct pipe_resource t = *templ;
   t.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   if (!LockAndWrap(pDevice, r, ai.hAllocation, &t, pitch, false, true) || !r->zeroCopy) {
      if (r->resource)
         pipe_resource_reference(&r->resource, NULL);
      r->hAllocation = ai.hAllocation;
      WddmDestroyResource(pDevice, r);
      free(r);
      return NULL;
   }
   if (r->pAllocationData) {
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &r->hAllocation;
      if (SUCCEEDED(pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul)))
         r->pAllocationData = NULL;
   }
   return r;
}

static void
PqDestroy(Device *pDevice, Resource *pr)
{
   bool any = false;
   for (UINT k = 0; k < 2; k++)
      any |= pr->pqStage[k] != NULL;
   if (any && !pDevice->dt_capable)
      ZcFinish(pDevice); /* the GPU may still copy from a staging */
   for (UINT k = 0; k < 2; k++) {
      Resource *s = pr->pqStage[k];
      if (s) {
         pipe_resource_reference(&s->resource, NULL);
         s->zeroCopy = false;
         WddmDestroyResource(pDevice, s);
         free(s);
         pr->pqStage[k] = NULL;
      }
      PqFreeAlloc(pDevice, pr->pqSync[k]);
      pr->pqSync[k] = 0;
   }
}

static bool
PqOn(Device *pDevice)
{
   if (pDevice->pq < 0) {
      const char *why = NULL;
      if (FlagFile("C:\\g6u\\proxyq.off"))
         why = "C:\\g6u\\proxyq.off";
      else if (!pDevice->zc)
         why = "zero copy off";
      pDevice->pq = why ? 0: 1;
      if (UmdTrace())
         DebugPrintf("zc: pipelined proxies %s%s%s\n", why ? "off": "on", why ? ": ": "", why ? why: "");
   }
   return pDevice->pq == 1;
}

/* the proxy of an opened shared surface: the texture DWM samples plus two stagings and their status words */
static bool
PqCreate(Device *pDevice, Resource *pResource, const struct pipe_resource *templ, D3DKMT_HANDLE hOpened,
         const struct g18w_alloc_priv *priv)
{
   UINT bpp = priv->bpp ? priv->bpp: 4;
   if (!PqOn(pDevice) || bpp != 4)
      return false;
   struct pipe_resource t = *templ;
   t.bind &= ~(PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED | PIPE_BIND_SCANOUT);
   pResource->resource = pDevice->pipe->screen->resource_create(pDevice->pipe->screen, &t);
   bool ok = pResource->resource != NULL;
   for (UINT k = 0; ok && k < 2; k++) {
      pResource->pqStage[k] = PqStageNew(pDevice, &t);
      pResource->pqSync[k] = PqAllocSync(pDevice);
      ok = pResource->pqStage[k] && pResource->pqSync[k];
   }
   if (UmdTrace())
      DebugPrintf("zc: pipelined proxy for shared 0x%x %ux%u pitch %u key %#llx: stagings 0x%x 0x%x sync 0x%x 0x%x: %s\n",
                  hOpened, t.width0, t.height0, priv->pitch,
                  (unsigned long long)(priv->reserved[0] | ((UINT64)priv->reserved[1] << 32)),
                  pResource->pqStage[0] ? pResource->pqStage[0]->hAllocation: 0,
                  pResource->pqStage[1] ? pResource->pqStage[1]->hAllocation: 0, pResource->pqSync[0],
                  pResource->pqSync[1], ok ? "ok": "R6 proxy");
   if (!ok) {
      PqDestroy(pDevice, pResource);
      pipe_resource_reference(&pResource->resource, NULL);
      return false;
   }
   pResource->hAllocation = 0;
   pResource->pAllocationData = NULL;
   pResource->ownsAllocation = false;
   pResource->shadowed = false;
   pResource->hShared = hOpened;
   pResource->sharedBpp = 4;
   pResource->sharedPitch = priv->pitch;
   pResource->shareKey = priv->reserved[0] | ((UINT64)priv->reserved[1] << 32);
   pResource->pqRFrom = 2;
   pResource->proxiedNext = pDevice->proxiedHead;
   pDevice->proxiedHead = pResource;
   pResource->transfers = (struct pipe_transfer **)calloc(1, sizeof *pResource->transfers);
   return true;
}

/* BLT shared -> staging k, then the status FILL (no wait) */
static bool
PqIssue(Device *pDevice, Resource *pr, UINT k, bool ifChanged)
{
   Resource *st = pr->pqStage[k];
   struct {
      struct g18w_blt b;
      struct g18w_fill f;
   } pk;
   memset(&pk, 0, sizeof pk);
   pk.b.hdr.op = G18W_OP_BLT;
   pk.b.hdr.bytes = sizeof pk.b;
   pk.b.dst.index = 0;
   pk.b.src.index = 1;
   pk.b.dst_pitch = st->allocationPitch;
   pk.b.src_pitch = pr->sharedPitch;
   pk.b.bpp = 4;
   pk.b.flags = ifChanged ? G18W_BLTF_IF_CHANGED: 0;
   pk.b.w = pr->resource->width0;
   pk.b.h = pr->resource->height0;
   pk.f.hdr.op = G18W_OP_FILL;
   pk.f.hdr.bytes = sizeof pk.f;
   pk.f.dst.index = 2;
   pk.f.bytes = 4;
   pk.f.pattern = G18W_BLT_COPIED; /* an older KMD: always "copied" */
   pk.f.flags = ifChanged ? G18W_FILLF_BLT_STATUS: 0;
   D3DKMT_HANDLE h[3] = { st->hAllocation, pr->hShared, pr->pqSync[k] };
   UINT w[3] = { 1, 0, 1 };
   UINT po[3] = { offsetof(struct g18w_blt, dst), offsetof(struct g18w_blt, src),
                  (UINT)(sizeof(struct g18w_blt) + offsetof(struct g18w_fill, dst)) };
   bool ok = SubmitPacketN(pDevice, &pk, sizeof pk, h, w, po, 3, 3);
   pDevice->perfPqBlts++;
   pr->pqStBlts++;
   pr->pqState[k] = ok ? 1: 0;
   static unsigned logged;
   if (UmdTrace() && (logged++ < 32 || !ok))
      DebugPrintf("zc: pq BLT shared 0x%x -> staging %u (0x%x) %ux%u frame %u%s: %s\n", pr->hShared, k, st->hAllocation,
                  pk.b.w, pk.b.h, pDevice->frame, ifChanged ? " if changed": "", ok ? "issued": "FAILED");
   return ok;
}

/* wait for staging k's BLT (the status lock); G18W_BLT_COPIED / _UNCHANGED, 0 on failure */
static UINT
PqWait(Device *pDevice, Resource *pr, UINT k)
{
   UINT64 t0 = PerfNow();
   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof lock);
   lock.hAllocation = pr->pqSync[k];
   lock.Flags.ReadOnly = 1;
   UINT status = 0;
   HRESULT hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
   if (SUCCEEDED(hr)) {
      if (lock.pData)
         status = *(volatile UINT *)lock.pData;
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &pr->pqSync[k];
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
   }
   pr->pqState[k] = 0;
   pDevice->perfPqWait += PerfNow() - t0;
   if (UmdTrace() && FAILED(hr))
      DebugPrintf("zc: pq status lock 0x%x: 0x%08lx\n", pr->pqSync[k], hr);
   return SUCCEEDED(hr) ? (status == G18W_BLT_UNCHANGED ? G18W_BLT_UNCHANGED: G18W_BLT_COPIED): 0;
}

/* staging k -> the proxy texture: a GPU copy ahead of the draws that follow (zc.sim: on the CPU, under a lock) */
static void
PqCopy(Device *pDevice, Resource *pr, UINT k)
{
   Resource *st = pr->pqStage[k];
   struct pipe_resource *res = pr->resource;
   struct pipe_box box;
   u_box_2d(0, 0, res->width0, res->height0, &box);
   if (pDevice->dt_capable) {
      D3DDDICB_LOCK lock;
      memset(&lock, 0, sizeof lock);
      lock.hAllocation = st->hAllocation;
      lock.Flags.ReadOnly = 1;
      if (SUCCEEDED(pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock)) && lock.pData) {
         pDevice->pipe->texture_subdata(pDevice->pipe, res, 0, PIPE_MAP_WRITE, &box, lock.pData, st->allocationPitch, 0);
         D3DDDICB_UNLOCK ul;
         ul.NumAllocations = 1;
         ul.phAllocations = &st->hAllocation;
         pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
      }
      pr->pqState[k] = 0;
   } else {
      pDevice->pipe->resource_copy_region(pDevice->pipe, res, 0, 0, 0, 0, st->resource, 0, &box);
      pr->pqState[k] = 2;
      pr->pqCopyTag[k] = pDevice->zcTagNs | (UINT32)(pDevice->zcTag + 1); /* this frame's flip */
   }
   pr->pqRFrom = k;
   pDevice->perfPqCopies++;
   pr->pqStCopies++;
}

/* a staging the executor may write: idle, or its GPU copy finished; `wait`: else the GPU is waited for */
static int
PqFreeStage(Device *pDevice, Resource *pr, bool wait)
{
   for (UINT n = 0; n < 2; n++) {
      UINT k = (pr->pqCur + 1 + n) % 2; /* the other one first */
      if (pr->pqState[k] == 2 && ZcTagSignaled(pDevice, pr->pqCopyTag[k]))
         pr->pqState[k] = 0;
      if (pr->pqState[k] == 0)
         return (int)k;
   }
   if (!wait)
      return -1;
   /* R8 (H): a BLT nobody waits for any more (issued for an older writer generation: PqSync only frees a staging when
    * the current one does not hold this generation's BLT) is collected here by its status lock. Without this, two such
    * BLTs left both stagings pending forever: every refresh of the proxy then waited for the whole GPU and found no
    * staging, and the proxy kept stale content (r8 run 20261121-104818: DWM's 1024x1024 proxy, ~8 ms per frame). */
   bool reclaimed = false;
   for (UINT k = 0; k < 2; k++)
      if (pr->pqState[k] == 1) {
         PqWait(pDevice, pr, k);
         if (pr->pqRFrom == k)
            pr->pqRFrom = 2; /* the executor's hash for k now describes the collected BLT's source, not the texture */
         pDevice->perfPqReclaims++;
         reclaimed = true;
      }
   if (reclaimed)
      for (UINT n = 0; n < 2; n++) {
         UINT k = (pr->pqCur + 1 + n) % 2;
         if (pr->pqState[k] == 0)
            return (int)k;
      }
   ZcFinish(pDevice);
   for (UINT k = 0; k < 2; k++)
      if (pr->pqState[k] == 2)
         pr->pqState[k] = 0;
   for (UINT n = 0; n < 2; n++) {
      UINT k = (pr->pqCur + 1 + n) % 2;
      if (pr->pqState[k] == 0)
         return (int)k;
   }
   return -1;
}

/* the frame's first draw: BLTs for the proxies whose writer moved on (or that have no generation), recently used */
static void
PqPrefetch(Device *pDevice)
{
   for (Resource *r = pDevice->proxiedHead; r; r = r->proxiedNext) {
      if (!r->pqStage[0] || pDevice->frame - r->pqUsedFrame > 120)
         continue;
      UINT64 gen = GenRead(r->shareKey);
      if ((gen && gen == r->pqGen && r->pqFresh) || r->pqState[r->pqCur] == 1)
         continue;
      int k = PqFreeStage(pDevice, r, false);
      if (k < 0)
         continue;
      if (PqIssue(pDevice, r, (UINT)k, gen == 0)) {
         r->pqCur = (UINT)k;
         r->pqGenIssued[k] = gen;
      }
   }
}

/* the refresh before a draw samples the proxy (once per frame) */
static void
PqSync(Device *pDevice, Resource *pr)
{
   pr->pqUsedFrame = pDevice->frame;
   UINT64 gen = GenRead(pr->shareKey);
   if (gen && gen == pr->pqGen && pr->pqFresh) {
      pDevice->perfPqSkips++;
      return;
   }
   UINT k = pr->pqCur;
   if (!(pr->pqState[k] == 1 && pr->pqGenIssued[k] == gen)) {
      /* nothing in flight for this generation: issue now */
      int f = PqFreeStage(pDevice, pr, true);
      if (f < 0) {
         pDevice->perfPqNoStage++;
         return;
      }
      if (!PqIssue(pDevice, pr, (UINT)f, gen == 0)) {
         pDevice->perfPqFail++;
         return;
      }
      k = (UINT)f;
      pr->pqCur = k;
      pr->pqGenIssued[k] = gen;
   }
   UINT st = PqWait(pDevice, pr, k);
   if (!st) {
      pDevice->perfPqFail++;
      return;
   }
   if (st == G18W_BLT_UNCHANGED && pr->pqRFrom == k)
      pDevice->perfPqSkips++;
   else
      PqCopy(pDevice, pr, k);
   pr->pqGen = gen;
   pr->pqFresh = gen != 0;
}

bool
WddmCreateResource(Device *pDevice, const D3D10DDIARG_CREATERESOURCE *pCreate,
                   D3D10DDI_HRTRESOURCE hRTResource, const struct pipe_resource *templ,
                   Resource *pResource)
{
   if (templ->last_level != 0 || templ->array_size != 1 || templ->nr_samples > 1) {
      DebugPrintf("%s: unsupported layout for a KMD allocation (levels %u, array %u, samples %u)\n",
                  __func__, templ->last_level + 1, templ->array_size, templ->nr_samples);
      return false;
   }

   UINT cpp = util_format_get_blocksize(templ->format);
   UINT pitch = align(templ->width0 * cpp, pDevice->pitch_align);
   /* G6 zero copy: what the GPU renders into and nobody else locks (not shared, not a primary) */
   bool zc = pDevice->zc && !pCreate->pPrimaryDesc &&
             !(pCreate->MiscFlags & (D3D10_DDI_RESOURCE_MISC_SHARED | 0x100 /* D3D11 SHARED_KEYEDMUTEX */)) &&
             cpp == 4;
retry:
   if (zc)
      pitch = ZcPitch(templ->width0, cpp);

   struct g18w_alloc_priv priv;
   memset(&priv, 0, sizeof priv);
   priv.magic = G18W_ALLOC_MAGIC;
   priv.version = G18W_UAPI_VERSION;
   priv.kind = pCreate->pPrimaryDesc ? G18W_ALLOC_PRIMARY: G18W_ALLOC_SURFACE;
   priv.flags = pCreate->pPrimaryDesc ? G18W_ALLOCF_PRIMARY: 0;
   /* shared (legacy handle, keyed mutex): the KMD puts it in its aperture segment (VidMm wants shareable
    * CPU-visible allocations aperture-only) */
   if (!pCreate->pPrimaryDesc &&
       (pCreate->MiscFlags & (D3D10_DDI_RESOURCE_MISC_SHARED | 0x100 /* D3D11 SHARED_KEYEDMUTEX */))) {
      priv.flags |= G18W_ALLOCF_SHARED;
      /* G6 r7 (A): the share key readers find the writer generation by */
      UINT64 key = ShareKeyNew();
      priv.reserved[0] = (uint32_t)key;
      priv.reserved[1] = (uint32_t)(key >> 32);
      pResource->shareKey = key;
   }
   priv.format = DxgiToDdi(pCreate->Format);
   priv.bpp = cpp;
   priv.width = templ->width0;
   priv.height = templ->height0;
   priv.pitch = pitch;
   priv.size = (uint64_t)pitch * templ->height0;
   if (zc) {
      priv.flags |= G18W_ALLOCF_GPU;
      priv.size = align64(priv.size, G18W_GPU_ALIGN);
   }
   if (pCreate->pPrimaryDesc) {
      priv.vidpn_source = pCreate->pPrimaryDesc->VidPnSourceId;
      priv.refresh_num = pCreate->pPrimaryDesc->ModeDesc.RefreshRate.Numerator;
      priv.refresh_den = pCreate->pPrimaryDesc->ModeDesc.RefreshRate.Denominator;
   }

   D3DDDI_ALLOCATIONINFO ai;
   memset(&ai, 0, sizeof ai);
   ai.pPrivateDriverData = &priv;
   ai.PrivateDriverDataSize = sizeof priv;
   if (pCreate->pPrimaryDesc) {
      ai.VidPnSourceId = pCreate->pPrimaryDesc->VidPnSourceId;
      ai.Flags.Primary = 1;
   }

   D3DDDICB_ALLOCATE a;
   memset(&a, 0, sizeof a);
   a.hResource = hRTResource.handle;
   /* resource-level private data too (what another process's OpenResource gets back) */
   a.pPrivateDriverData = &priv;
   a.PrivateDriverDataSize = sizeof priv;
   a.NumAllocations = 1;
   a.pAllocationInfo = &ai;
   HRESULT hr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &a);
   if (UmdTrace())
      DebugPrintf("AllocateCb %ux%u fmt %u (dxgi %u) pitch %u kind %u flags 0x%x bind 0x%x misc 0x%x -> hr 0x%08lx alloc 0x%x kmres 0x%x\n",
                  priv.width, priv.height, priv.format, pCreate->Format, pitch, priv.kind, priv.flags,
                  pCreate->BindFlags, pCreate->MiscFlags, hr, ai.hAllocation, a.hKMResource);
   if (FAILED(hr) && zc) {
      /* segment 1 full: the shadowed way */
      zc = false;
      pitch = align(templ->width0 * cpp, pDevice->pitch_align);
      goto retry;
   }
   if (FAILED(hr))
      return false;

   if (pCreate->pPrimaryDesc) {
      /* DWM's DXGI flips are refused by dxgkrnl on this adapter (not the source owner): ask DXGI for
       * copy presentation into its scan-out surface instead (C:\g6u\scanout.on keeps flips) */
      static int noscanout = -1;
      if (noscanout < 0)
         noscanout = GetFileAttributesA("C:\\g6u\\scanout.on") == INVALID_FILE_ATTRIBUTES;
      DXGI_DDI_PRIMARY_DESC *pd = (DXGI_DDI_PRIMARY_DESC *)pCreate->pPrimaryDesc;
      if (UmdTrace())
         DebugPrintf("primary: flags 0x%x vidpn %u %ux%u refresh %u/%u driverflags in 0x%x%s\n", pd->Flags,
                     pd->VidPnSourceId, pd->ModeDesc.Width, pd->ModeDesc.Height, pd->ModeDesc.RefreshRate.Numerator,
                     pd->ModeDesc.RefreshRate.Denominator, pd->DriverFlags, noscanout ? " -> NO_SCANOUT": "");
      if (noscanout)
         pd->DriverFlags |= DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT;
      /* Primaries stay unlocked: VidMm cannot pin a locked allocation for scan-out (SetDisplayMode:
       * "Couldn't allocate segment resource"). Render into an own texture, copy at Present. */
      struct pipe_resource t = *templ;
      t.bind &= ~(PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED | PIPE_BIND_SCANOUT);
      pResource->resource = pDevice->pipe->screen->resource_create(pDevice->pipe->screen, &t);
      if (pResource->resource) {
         pResource->hAllocation = ai.hAllocation;
         pResource->pAllocationData = NULL;
         pResource->allocationPitch = pitch;
         pResource->shadowed = true;
         pResource->isPrimary = true;
         pResource->copyModel = noscanout != 0;
         pResource->ownsAllocation = true;
         pResource->shadowNext = pDevice->shadowHead;
         pDevice->shadowHead = pResource;
         return true;
      }
      D3DDDICB_DEALLOCATE d;
      memset(&d, 0, sizeof d);
      d.NumAllocations = 1;
      d.HandleList = &ai.hAllocation;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
      return false;
   }

   if (!LockAndWrap(pDevice, pResource, ai.hAllocation, templ, pitch, false, zc)) {
      D3DDDICB_DEALLOCATE d;
      memset(&d, 0, sizeof d);
      d.NumAllocations = 1;
      d.HandleList = &ai.hAllocation;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
      return false;
   }
   pResource->ownsAllocation = true;
   if (pResource->shareKey && !pResource->shadowed) {
      /* G6 r7 (A): rendered into its allocation directly (software screens): its generation moves at each flush */
      pResource->sharedNext = pDevice->sharedHead;
      pDevice->sharedHead = pResource;
   }
   return true;
}


void
WddmOpenResource(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_OPENRESOURCE *pOpen,
                 D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRTResource)
{
   Device *pDevice = CastDevice(hDevice);
   Resource *pResource = CastResource(hResource);
   memset(pResource, 0, sizeof *pResource);

   if (pOpen->NumAllocations != 1 ||
       pOpen->pOpenAllocationInfo[0].PrivateDriverDataSize < sizeof(struct g18w_alloc_priv)) {
      DebugPrintf("%s: %u allocations, private data %u bytes\n", __func__, pOpen->NumAllocations,
                  pOpen->NumAllocations ? pOpen->pOpenAllocationInfo[0].PrivateDriverDataSize: 0);
      SetError(hDevice, E_INVALIDARG);
      return;
   }
   const struct g18w_alloc_priv *priv =
      (const struct g18w_alloc_priv *)pOpen->pOpenAllocationInfo[0].pPrivateDriverData;
   if (UmdTrace())
      DebugPrintf("OpenResource alloc 0x%x magic 0x%08x kind %u %ux%u fmt %u pitch %u\n",
                  pOpen->pOpenAllocationInfo[0].hAllocation, priv->magic, priv->kind, priv->width,
                  priv->height, priv->format, priv->pitch);
   DXGI_FORMAT dxgi = DdiToDxgi(priv->format);
   if (priv->magic != G18W_ALLOC_MAGIC || priv->kind == G18W_ALLOC_BUFFER || dxgi == DXGI_FORMAT_UNKNOWN) {
      DebugPrintf("%s: cannot wrap kind %u fmt %u\n", __func__, priv->kind, priv->format);
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   struct pipe_resource templ;
   memset(&templ, 0, sizeof templ);
   templ.target = PIPE_TEXTURE_2D;
   templ.format = FormatTranslate(dxgi, false);
   templ.width0 = priv->width;
   templ.height0 = priv->height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                PIPE_BIND_SHARED;

   pResource->Format = dxgi;
   pResource->MipLevels = 1;
   pResource->NumSubResources = 1;
   pResource->buffer = false;
   D3DKMT_HANDLE hOpened = pOpen->pOpenAllocationInfo[0].hAllocation;
   if (!LockAndWrap(pDevice, pResource, hOpened, &templ, priv->pitch, true,
                    (priv->flags & G18W_ALLOCF_GPU) != 0)) {
      /* VidMm lets only the owner lock a shared allocation: read it through a private copy that the
       * KMD's executor refreshes (G18W_OP_BLT) before each frame that reads it */
      if (PqCreate(pDevice, pResource, &templ, hOpened, priv) || ZcProxy(pDevice, pResource, &templ, hOpened, priv)) {
         WddmSyncSharedResource(pDevice, pResource);
         return;
      }
      struct g18w_alloc_priv p2 = *priv;
      p2.kind = G18W_ALLOC_SURFACE;
      p2.flags = 0;
      p2.size = (uint64_t)p2.pitch * p2.height;
      D3DDDI_ALLOCATIONINFO ai;
      memset(&ai, 0, sizeof ai);
      ai.pPrivateDriverData = &p2;
      ai.PrivateDriverDataSize = sizeof p2;
      D3DDDICB_ALLOCATE a;
      memset(&a, 0, sizeof a);
      a.NumAllocations = 1;
      a.pAllocationInfo = &ai;
      HRESULT hr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &a);
      if (UmdTrace())
         DebugPrintf("OpenResource: proxy for shared 0x%x: AllocateCb %ux%u pitch %u -> hr 0x%08lx alloc 0x%x\n",
                     hOpened, p2.width, p2.height, p2.pitch, hr, ai.hAllocation);
      struct pipe_resource t = templ;
      t.bind &= ~(PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED | PIPE_BIND_SCANOUT);
      if (SUCCEEDED(hr) && !pDevice->dt_capable) {
         /* G6 track Z: a linear texture takes the per-frame refresh in place (a tiled one needs a full-size
          * staging buffer in the render node's arena and a GPU copy for every refresh) */
         struct pipe_resource lt = t;
         lt.bind |= PIPE_BIND_LINEAR;
         pResource->resource = pDevice->pipe->screen->resource_create(pDevice->pipe->screen, &lt);
      }
      if (SUCCEEDED(hr) && !pResource->resource)
         pResource->resource = pDevice->pipe->screen->resource_create(pDevice->pipe->screen, &t);
      if (FAILED(hr) || !pResource->resource) {
         if (SUCCEEDED(hr)) {
            D3DDDICB_DEALLOCATE d;
            memset(&d, 0, sizeof d);
            d.NumAllocations = 1;
            d.HandleList = &ai.hAllocation;
            pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
         }
         SetError(hDevice, E_OUTOFMEMORY);
         return;
      }
      pResource->hAllocation = ai.hAllocation;
      pResource->pAllocationData = NULL; /* locked only while copying in */
      pResource->allocationPitch = p2.pitch;
      pResource->shadowed = false;
      pResource->ownsAllocation = true;
      pResource->hShared = hOpened;
      pResource->sharedBpp = p2.bpp ? p2.bpp: 4;
      pResource->sharedPitch = p2.pitch;
      pResource->proxiedNext = pDevice->proxiedHead;
      pDevice->proxiedHead = pResource;
      pResource->transfers = (struct pipe_transfer **)calloc(1, sizeof *pResource->transfers);
      WddmSyncSharedResource(pDevice, pResource);
      return;
   }
   pResource->ownsAllocation = false;
   pResource->transfers = (struct pipe_transfer **)calloc(1, sizeof *pResource->transfers);
   if (pResource->shadowed)
      WddmCopyIn(pDevice, pResource);
}


void
WddmDestroyResource(Device *pDevice, Resource *pResource)
{
   if (pResource->hShared) {
      for (Resource **pp = &pDevice->proxiedHead; *pp; pp = &(*pp)->proxiedNext)
         if (*pp == pResource) {
            *pp = pResource->proxiedNext;
            break;
         }
      pResource->hShared = 0;
   }
   for (Resource **pp = &pDevice->shadowHead; *pp; pp = &(*pp)->shadowNext)
      if (*pp == pResource) {
         *pp = pResource->shadowNext;
         break;
      }
   for (Resource **pp = &pDevice->sharedHead; *pp; pp = &(*pp)->sharedNext)
      if (*pp == pResource) {
         *pp = pResource->sharedNext;
         break;
      }
   if (pResource->pqStage[0] || pResource->pqStage[1] || pResource->pqSync[0] || pResource->pqSync[1])
      PqDestroy(pDevice, pResource);
   if (!pResource->hAllocation)
      return;
   if (pResource->zeroCopy) {
      /* the GPU is done with the pages before they are unlocked and freed (hk unbinds them when zink frees the
       * memory object) */
      ZcFinish(pDevice);
      pResource->zeroCopy = false;
   }
   if (pResource->pAllocationData) {
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &pResource->hAllocation;
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
   }
   if (pResource->ownsAllocation) {
      D3DDDICB_DEALLOCATE d;
      memset(&d, 0, sizeof d);
      d.NumAllocations = 1;
      d.HandleList = &pResource->hAllocation;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
   }
   pResource->hAllocation = 0;
   pResource->pAllocationData = NULL;
}


/* R8: RotateResourceIdentities without copies. Handle i takes the storage handle i+1 had (handle n-1 the storage
 * handle 0 had): the pipe resource and everything that belongs to it move together, i.e. the KMD allocation
 * (hAllocation, its lock pData, pitch, ownership), a zero-copy texture's import of that allocation (the texture IS
 * the locked segment-1 allocation, so the pair cannot be split), the shadow/dirty/scan-sourced state, the share key
 * and proxy state of a shared allocation, and open maps. Device lists (shadowed, shared, proxied) hold the resource
 * structs: membership follows the storage. What stays with the handle: its description and its views (the caller
 * moves the views). The KMD's scan-out slot registry only holds the device's own scan-out buffers (zcScan), which
 * are never runtime resources, so it stays as it is; flips name allocations, which move with their storage.
 * False: not rotatable here (the caller copies). */
static bool
InList(Resource *head, Resource *Resource::*next, Resource *r)
{
   for (Resource *p = head; p; p = p->*next)
      if (p == r)
         return true;
   return false;
}

static void
ListRemove(Resource **head, Resource *Resource::*next, Resource *r)
{
   for (Resource **pp = head; *pp; pp = &((*pp)->*next))
      if (*pp == r) {
         *pp = r->*next;
         r->*next = NULL;
         return;
      }
}

bool
WddmRotateStorage(Device *pDevice, Resource *const *r, UINT n)
{
   if (n < 2 || n > 16)
      return false;
   for (UINT i = 0; i < n; i++) {
      if (!r[i] || !r[i]->resource)
         return false;
      for (UINT k = 0; k < pDevice->zcScanCount; k++)
         if (pDevice->zcScan[k] == r[i])
            return false;
      for (UINT j = 0; j < i; j++)
         if (r[j] == r[i])
            return false;
      const struct pipe_resource *a = r[0]->resource, *b = r[i]->resource;
      if (a->target != b->target || a->format != b->format || a->width0 != b->width0 || a->height0 != b->height0 ||
          a->depth0 != b->depth0 || a->array_size != b->array_size || a->last_level != b->last_level ||
          a->nr_samples != b->nr_samples || r[0]->Format != r[i]->Format || r[0]->MipLevels != r[i]->MipLevels ||
          r[0]->NumSubResources != r[i]->NumSubResources || r[0]->buffer != r[i]->buffer)
         return false;
   }
   Resource *Resource::*const lists[3] = { &Resource::shadowNext, &Resource::sharedNext, &Resource::proxiedNext };
   Resource **heads[3] = { &pDevice->shadowHead, &pDevice->sharedHead, &pDevice->proxiedHead };
   bool member[16][3];
   Resource snap[16];
   for (UINT i = 0; i < n; i++) {
      for (UINT l = 0; l < 3; l++)
         member[i][l] = InList(*heads[l], lists[l], r[i]);
      snap[i] = *r[i];
   }
   for (UINT i = 0; i < n; i++) {
      const Resource &from = snap[(i + 1) % n];
      Resource *to = r[i];
      Resource keep = *to;
      *to = from;
      /* the handle's own: description, list links, views */
      to->Format = keep.Format;
      to->MipLevels = keep.MipLevels;
      to->NumSubResources = keep.NumSubResources;
      to->buffer = keep.buffer;
      to->shadowNext = keep.shadowNext;
      to->sharedNext = keep.sharedNext;
      to->proxiedNext = keep.proxiedNext;
      to->views = keep.views;
   }
   unsigned moved = 0;
   for (UINT i = 0; i < n; i++)
      for (UINT l = 0; l < 3; l++) {
         bool want = member[(i + 1) % n][l];
         if (want == member[i][l])
            continue;
         moved++;
         if (want) {
            r[i]->*lists[l] = *heads[l];
            *heads[l] = r[i];
         } else {
            ListRemove(heads[l], lists[l], r[i]);
         }
      }
   static unsigned logged;
   if (UmdTrace() && (logged++ < 4 || moved))
      DebugPrintf("rotate: %u resources %ux%u: identities (allocations 0x%x -> 0x%x%s), list moves %u\n", n,
                  r[0]->resource->width0, r[0]->resource->height0, snap[0].hAllocation, r[0]->hAllocation,
                  r[0]->zeroCopy ? ", zero copy": r[0]->shadowed ? ", shadowed": "", moved);
   return true;
}


/* G6 zero copy: up to three full-screen GPU allocations the zero-copy present copies the back buffer into on
 * the GPU, then flips (DWM keeps drawing into its back buffer while the panel shows a finished frame). Fewer
 * than two (segment 1 full): the back buffer itself is flipped. */
static void
ZcScanFree(Device *pDevice)
{
   for (UINT i = 0; i < pDevice->zcScanCount; i++) {
      Resource *r = pDevice->zcScan[i];
      if (!r)
         continue;
      pipe_resource_reference(&r->resource, NULL);
      WddmDestroyResource(pDevice, r);
      free(r);
      pDevice->zcScan[i] = NULL;
   }
   pDevice->zcScanCount = 0;
}

static void
ZcScanInit(Device *pDevice, const struct pipe_resource *like)
{
   pDevice->zcScanTried = true;
   if (FlagFile("C:\\g6u\\zc.direct"))
      return;
   UINT pitch = ZcPitch(like->width0, 4);
   for (UINT i = 0; i < ARRAY_SIZE(pDevice->zcScan); i++) {
      struct g18w_alloc_priv priv;
      memset(&priv, 0, sizeof priv);
      priv.magic = G18W_ALLOC_MAGIC;
      priv.version = G18W_UAPI_VERSION;
      priv.kind = G18W_ALLOC_SURFACE;
      priv.flags = G18W_ALLOCF_GPU;
      priv.format = D3DDDIFMT_A8R8G8B8;
      priv.bpp = 4;
      priv.width = like->width0;
      priv.height = like->height0;
      priv.pitch = pitch;
      priv.size = align64((uint64_t)pitch * like->height0, G18W_GPU_ALIGN);
      D3DDDI_ALLOCATIONINFO ai;
      memset(&ai, 0, sizeof ai);
      ai.pPrivateDriverData = &priv;
      ai.PrivateDriverDataSize = sizeof priv;
      D3DDDICB_ALLOCATE a;
      memset(&a, 0, sizeof a);
      a.NumAllocations = 1;
      a.pAllocationInfo = &ai;
      HRESULT hr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &a);
      Resource *r = SUCCEEDED(hr) ? (Resource *)calloc(1, sizeof *r): NULL;
      if (r) {
         struct pipe_resource t = *like;
         t.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
         r->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
         r->MipLevels = 1;
         r->NumSubResources = 1;
         r->ownsAllocation = true;
         if (!LockAndWrap(pDevice, r, ai.hAllocation, &t, pitch, false, true) || !r->zeroCopy) {
            if (r->resource)
               pipe_resource_reference(&r->resource, NULL);
            r->hAllocation = ai.hAllocation;
            WddmDestroyResource(pDevice, r);
            free(r);
            r = NULL;
         }
      } else if (SUCCEEDED(hr)) {
         D3DDDICB_DEALLOCATE d;
         memset(&d, 0, sizeof d);
         d.NumAllocations = 1;
         d.HandleList = &ai.hAllocation;
         pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &d);
      }
      if (UmdTrace())
         DebugPrintf("zc: scan-out buffer %u %ux%u pitch %u: AllocateCb 0x%08lx alloc 0x%x: %s\n", i, priv.width,
                     priv.height, pitch, hr, SUCCEEDED(hr) ? ai.hAllocation: 0, r ? "zero copy": "none");
      if (!r)
         break;
      /* R6J: dxgkrnl refuses a DMA buffer (the executor's FLIP) that names a locked allocation, so every UMD
       * flip of a scan-out buffer failed and fell back to UmdBlt into the primary. The import holds the pages
       * (GEM_USERPTR resolved them while locked; only the GPU touches them from here on), so the buffer is
       * unlocked; it is resident (residency fill, then every frame's FLIP names it) and VidMm has no reason to
       * move it in a 704 MiB segment (a move would show as "zc ... moved" in the KMD's exec line). */
      if (r->pAllocationData) {
         D3DDDICB_UNLOCK ul;
         ul.NumAllocations = 1;
         ul.phAllocations = &r->hAllocation;
         HRESULT hu = pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
         if (SUCCEEDED(hu))
            r->pAllocationData = NULL;
         if (UmdTrace())
            DebugPrintf("zc: scan-out buffer %u unlocked: 0x%08lx\n", i, hu);
      }
      UINT slot = pDevice->zcScanCount;
      pDevice->zcScan[pDevice->zcScanCount++] = r;
      /* R7 (H): the KMD remembers it as slot `slot` for G18W_ESC_FLIPNOW */
      pDevice->zcScanReg[slot] = WddmUmdFlip(pDevice, r, G18W_FLIPF_REGISTER, slot);
   }
   if (pDevice->zcScanCount < 2)
      ZcScanFree(pDevice);
}

/* the frame in pSrc (zero copy) to the panel: copied on the GPU into the next scan-out buffer, or pSrc itself;
 * the GPU is done before the KMD's executor flips it */
/* R6G: scan-out buffers are mode-sized; a source at least that large (DWM's 3040x1984 DComp-padded back buffer
 * on the 3024x1964 panel) fills them from its top-left corner */
static void
ZcScanInitFor(Device *pDevice, const struct pipe_resource *src)
{
   if (pDevice->zcScanTried || !pDevice->scanW || !pDevice->scanH || src->width0 < pDevice->scanW ||
       src->height0 < pDevice->scanH)
      return;
   struct pipe_resource like = *src;
   like.width0 = pDevice->scanW;
   like.height0 = pDevice->scanH;
   ZcScanInit(pDevice, &like);
}

/* R6E: scan-out buffers exist for a frame of pSrc's size (a shadowed source cannot be flipped itself) */
static bool
ZcScanReady(Device *pDevice, Resource *pSrc)
{
   struct pipe_resource *src = pSrc->resource;
   /* G6 r7: also with C:\\g6u\\zc.sim (softpipe): ZcPresentFlip copies on the CPU there */
   if (!pDevice->zc || !src)
      return false;
   ZcScanInitFor(pDevice, src);
   return pDevice->zcScanCount >= 2 && src->width0 >= pDevice->zcScan[0]->resource->width0 &&
          src->height0 >= pDevice->zcScan[0]->resource->height0;
}

/* G6 r7 (A), C:\\g6u\\zc.sim (softpipe in QEMU): the scan-out buffers are unlocked after their creation (as on the
 * GPU), so the frame is copied on the CPU into a fresh lock of the buffer */
static void
ZcSimCopy(Device *pDevice, Resource *target, struct pipe_resource *src)
{
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_transfer *tr;
   UINT w = MIN2(src->width0, target->resource->width0), h = MIN2(src->height0, target->resource->height0);
   const uint8_t *s = (const uint8_t *)pipe_texture_map(pipe, src, 0, 0, PIPE_MAP_READ, 0, 0, w, h, &tr);
   if (!s)
      return;
   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof lock);
   lock.hAllocation = target->hAllocation;
   lock.Flags.WriteOnly = 1;
   if (SUCCEEDED(pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock)) && lock.pData) {
      for (UINT y = 0; y < h; y++)
         memcpy((uint8_t *)lock.pData + (size_t)y * target->allocationPitch, s + (size_t)y * tr->stride, (size_t)w * 4);
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &target->hAllocation;
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
   }
   pipe_texture_unmap(pipe, tr);
}

/* G6 r7 (A): scan-out buffer i may be written again: a later frame's scan-out copy finished (KMD tag_done), so
 * neither the panel nor a queued flip reads it. Polls the KMD; 100 ms at most (then the GPU is waited for and the
 * frame goes on; three such timeouts in a row turn pipelining off for the device). */
/*
 * R7 (H): async flips. A's pipelined present waited on DWM's thread (a second, empty flush) until zink's submit
 * thread had handed the frame to the render node, i.e. for the whole synchronous SUBMIT (~25 ms per frame in r7 run
 * 20261121-042432). Here a helper thread does that wait (zink_fence_wait_flushed: zink's own queue fences, no hk
 * sync) and then asks the KMD to queue the fenced flip of the registered scan-out slot (G18W_ESC_FLIPNOW). Fences
 * are released on DWM's thread only. C:\g6w\asyncflip.off: off.
 */
static bool (*g_fenceWaitFlushed)(struct pipe_fence_handle *, uint64_t);
extern "C" void
G18SetFenceWaitFlushed(bool (*fn)(struct pipe_fence_handle *, uint64_t))
{
   g_fenceWaitFlushed = fn;
}
static void (*g_flushNoSync)(struct pipe_context *, struct pipe_fence_handle **);
extern "C" void
G18SetFlushNoSync(void (*fn)(struct pipe_context *, struct pipe_fence_handle **))
{
   g_flushNoSync = fn;
}

struct ZcAsync {
   CRITICAL_SECTION cs;
   CONDITION_VARIABLE cv;
   HANDLE thread;
   bool quit;
   HANDLE hAdapter, hDevice;
   PFND3DDDI_ESCAPECB escape;
   struct { struct pipe_fence_handle *f; UINT slot; UINT64 tag; bool done; } q[8];
   unsigned head, tail, sent; /* [sent, tail) waiting; [head, sent) handed back for unref */
   UINT64 escapes, fails, timeouts, waitTicks;
};

static DWORD WINAPI
ZcAsyncThread(LPVOID arg)
{
   ZcAsync *z = (ZcAsync *)arg;
   EnterCriticalSection(&z->cs);
   for (;;) {
      while (z->sent == z->tail && !z->quit)
         SleepConditionVariableCS(&z->cv, &z->cs, INFINITE);
      if (z->sent == z->tail)
         break;
      unsigned k = z->sent % 8;
      struct pipe_fence_handle *f = z->q[k].f;
      UINT slot = z->q[k].slot;
      UINT64 tag = z->q[k].tag;
      LeaveCriticalSection(&z->cs);
      UINT64 t0 = PerfNow();
      bool ok = g_fenceWaitFlushed(f, 1000000000ull);
      z->waitTicks += PerfNow() - t0;
      if (!ok)
         z->timeouts++;
      struct g18w_esc_flipnow n;
      memset(&n, 0, sizeof n);
      n.code = G18W_ESC_FLIPNOW;
      n.slot = slot;
      n.tag = tag;
      D3DDDICB_ESCAPE e;
      memset(&e, 0, sizeof e);
      e.hDevice = z->hDevice;
      e.pPrivateDriverData = &n;
      e.PrivateDriverDataSize = sizeof n;
      HRESULT hr = z->escape(z->hAdapter, &e);
      z->escapes++;
      if (FAILED(hr) || n.version != 1)
         z->fails++;
      EnterCriticalSection(&z->cs);
      z->q[k].done = true;
      z->sent++;
   }
   LeaveCriticalSection(&z->cs);
   return 0;
}

/* DWM's thread: unref the fences the helper is done with */
static void
ZcAsyncReap(Device *pDevice)
{
   ZcAsync *z = pDevice->zcAsync;
   if (!z)
      return;
   struct pipe_screen *screen = pDevice->pipe->screen;
   EnterCriticalSection(&z->cs);
   while (z->head != z->sent) {
      struct pipe_fence_handle *f = z->q[z->head % 8].f;
      z->q[z->head % 8].f = NULL;
      z->head++;
      LeaveCriticalSection(&z->cs);
      screen->fence_reference(screen, &f, NULL);
      EnterCriticalSection(&z->cs);
   }
   LeaveCriticalSection(&z->cs);
}

static bool
ZcAsyncOn(Device *pDevice)
{
   if (pDevice->zcAsync)
      return true;
   static int off = -1;
   if (off < 0)
      off = FlagFile("C:\\g6w\\asyncflip.off") || FlagFile("C:\\g6u\\asyncflip.off");
   if (off || !g_fenceWaitFlushed || !pDevice->KTCallbacks.pfnEscapeCb || pDevice->dt_capable)
      return false;
   ZcAsync *z = (ZcAsync *)calloc(1, sizeof *z);
   if (!z)
      return false;
   InitializeCriticalSection(&z->cs);
   InitializeConditionVariable(&z->cv);
   z->hAdapter = pDevice->hRTAdapter;
   z->hDevice = pDevice->hDevice;
   z->escape = pDevice->KTCallbacks.pfnEscapeCb;
   z->thread = CreateThread(NULL, 0, ZcAsyncThread, z, 0, NULL);
   if (!z->thread) {
      DeleteCriticalSection(&z->cs);
      free(z);
      off = 1;
      return false;
   }
   SetThreadPriority(z->thread, THREAD_PRIORITY_HIGHEST);
   pDevice->zcAsync = z;
   if (UmdTrace())
      DebugPrintf("zc: async flips on (a helper thread waits for zink's submit, then G18W_ESC_FLIPNOW)\n");
   return true;
}

static bool
ZcAsyncPush(Device *pDevice, struct pipe_fence_handle *f, UINT slot, UINT64 tag)
{
   ZcAsync *z = pDevice->zcAsync;
   ZcAsyncReap(pDevice);
   bool ok = false;
   EnterCriticalSection(&z->cs);
   if (z->tail - z->head < 8) {
      unsigned k = z->tail % 8;
      z->q[k].f = NULL;
      pDevice->pipe->screen->fence_reference(pDevice->pipe->screen, &z->q[k].f, f);
      z->q[k].slot = slot;
      z->q[k].tag = tag;
      z->q[k].done = false;
      z->tail++;
      ok = true;
      WakeConditionVariable(&z->cv);
   }
   LeaveCriticalSection(&z->cs);
   return ok;
}

static void
ZcAsyncStop(Device *pDevice)
{
   ZcAsync *z = pDevice->zcAsync;
   if (!z)
      return;
   EnterCriticalSection(&z->cs);
   z->quit = true;
   WakeConditionVariable(&z->cv);
   LeaveCriticalSection(&z->cs);
   WaitForSingleObject(z->thread, 5000);
   CloseHandle(z->thread);
   ZcAsyncReap(pDevice);
   if (UmdTrace())
      DebugPrintf("zc: async flips: %llu escapes, %llu failed, %llu submit waits timed out\n",
                  (unsigned long long)z->escapes, (unsigned long long)z->fails, (unsigned long long)z->timeouts);
   DeleteCriticalSection(&z->cs);
   free(z);
   pDevice->zcAsync = NULL;
}

static void
ZcThrottle(Device *pDevice, UINT i)
{
   UINT64 t = pDevice->zcScanTag[i];
   if (!t || ZcTagPassed(pDevice->zcSeenDone, t))
      return;
   UINT64 t0 = PerfNow();
   LARGE_INTEGER fq;
   QueryPerformanceFrequency(&fq);
   struct g18w_esc_flipq q;
   for (unsigned n = 0;; n++) {
      if (!ZcFlipQuery(pDevice, &q) || ZcTagPassed(q.tag_done, t)) {
         pDevice->zcThrottleTimeouts = 0;
         break;
      }
      if ((PerfNow() - t0) * 1000 > (UINT64)fq.QuadPart * 100) {
         if (UmdTrace())
            DebugPrintf("zc: throttle: buffer %u (tag %#llx) not released in 100 ms (done %#llx latched %#llx signalled %#llx pending %u)\n",
                        i, (unsigned long long)t, (unsigned long long)q.tag_done, (unsigned long long)q.tag_latched,
                        (unsigned long long)q.tag_signaled, q.pending);
         ZcFinish(pDevice);
         if (++pDevice->zcThrottleTimeouts >= 3) {
            pDevice->zcFlipQ = 0;
            if (UmdTrace())
               DebugPrintf("zc: pipelined presents off after three throttle timeouts\n");
         }
         break;
      }
      if (n < 4)
         SwitchToThread();
      else
         Sleep(1);
   }
   pDevice->perfThrottle += PerfNow() - t0;
}

static bool
ZcPresentFlip(Device *pDevice, Resource *pSrc)
{
   struct pipe_resource *src = pSrc->resource;
   ZcScanInitFor(pDevice, src);
   Resource *target = pSrc;
   if (pDevice->zcScanCount >= 2 && src->width0 >= pDevice->zcScan[0]->resource->width0 &&
       src->height0 >= pDevice->zcScan[0]->resource->height0) {
      UINT i = pDevice->zcScanNext;
      bool q = ZcFlipQOn(pDevice);
      if (q)
         ZcThrottle(pDevice, i);
      q = q && pDevice->zcFlipQ == 1;
      target = pDevice->zcScan[i];
      pDevice->zcScanNext = (i + 1) % pDevice->zcScanCount;
      if (pDevice->dt_capable) {
         ZcSimCopy(pDevice, target, src);
      } else {
         struct pipe_box box;
         u_box_2d(0, 0, target->resource->width0, target->resource->height0, &box);
         pDevice->pipe->resource_copy_region(pDevice->pipe, target->resource, 0, 0, 0, 0, src, 0, &box);
      }
      if (q) {
         /* G6 r7 (A): no wait: the flush submits the frame and the copy, the KMD's flip waits for the render node */
         UINT64 t0 = PerfNow();
         if (i < 3 && pDevice->zcScanReg[i] && ZcAsyncOn(pDevice)) {
            /* R7 (H): the helper thread waits for zink's submit and queues the flip through the KMD escape */
            struct pipe_fence_handle *f = NULL;
            /* zink's flush waited for its own submit thread (~8 ms per frame in r7 run 20261121-061526): not
             * needed here, the helper waits (C:\g6w\flushsync.on: as before) */
            static int fsync = -1;
            if (fsync < 0)
               fsync = FlagFile("C:\\g6w\\flushsync.on") ? 1: 0;
            if (g_flushNoSync && !fsync)
               g_flushNoSync(pDevice->pipe, &f);
            else
               pDevice->pipe->flush(pDevice->pipe, &f, 0);
            UINT64 tag = pDevice->zcTagNs | (UINT32)++pDevice->zcTag;
            bool pushed = f && ZcAsyncPush(pDevice, f, i, tag);
            if (f)
               pDevice->pipe->screen->fence_reference(pDevice->pipe->screen, &f, NULL);
            pDevice->perfFlushCpu += PerfNow() - t0;
            if (pushed) {
               pDevice->zcScanTag[i] = tag;
               pDevice->zcPipeFrame = pDevice->frame;
               pDevice->zcPresents++;
               if (UmdTrace() && (pDevice->zcPresents <= 4 || pDevice->zcPresents % 600 == 0))
                  DebugPrintf("zc: async present %llu tag %#llx into buffer %u, frame %u\n",
                              (unsigned long long)pDevice->zcPresents, (unsigned long long)tag, i, pDevice->frame);
               return true;
            }
            t0 = PerfNow();
         }
         pDevice->pipe->flush(pDevice->pipe, NULL, 0);
         /* zink submits on its flush thread: a flush with nothing new waits until that submit returned, so the render
          * node took the frame's jobs before the KMD runs the flip (the flip's fence covers what it took by then) */
         pDevice->pipe->flush(pDevice->pipe, NULL, 0);
         pDevice->perfFlushCpu += PerfNow() - t0;
         UINT64 tag = pDevice->zcTagNs | (UINT32)++pDevice->zcTag;
         pDevice->zcScanTag[i] = tag;
         pDevice->zcPipeFrame = pDevice->frame;
         pDevice->zcPresents++;
         if (UmdTrace() && (pDevice->zcPresents <= 4 || pDevice->zcPresents % 600 == 0))
            DebugPrintf("zc: pipelined present %llu tag %#llx into buffer %u, frame %u (KMD done %#llx signalled %#llx)\n",
                        (unsigned long long)pDevice->zcPresents, (unsigned long long)tag, i, pDevice->frame,
                        (unsigned long long)pDevice->zcSeenDone, (unsigned long long)pDevice->zcSeenSignaled);
         return WddmUmdFlip(pDevice, target, G18W_FLIPF_FENCE | G18W_FLIPF_TAG, tag);
      }
   } else {
      pDevice->zcFlipsDirect++;
   }
   ZcFinish(pDevice);
   pDevice->zcPresents++;
   if (UmdTrace() && (pDevice->zcPresents <= 4 || pDevice->zcPresents % 600 == 0))
      DebugPrintf("zc: present %llu (direct %llu), frame %u\n", (unsigned long long)pDevice->zcPresents,
                  (unsigned long long)pDevice->zcFlipsDirect, pDevice->frame);
   return WddmUmdFlip(pDevice, target);
}

HRESULT
WddmPresent(Device *pDevice, DXGI_DDI_ARG_PRESENT *pPresentData)
{
   UINT64 perfIn = PerfNow();
   Resource *pSrc = CastResource(pPresentData->hSurfaceToPresent);
   Resource *pDst = pPresentData->hDstResource ? CastResource(pPresentData->hDstResource): NULL;
   RotDiag(pDevice, "Present", (int)(pPresentData->Flags.Value | ((UINT)pPresentData->FlipInterval << 8)), pSrc, pDst);

   /* R6E: DWM's compositor presents from a shadowed (shared, aperture) back buffer to the primary: with zero
    * copy the shadow texture goes to the next scan-out buffer by a GPU copy (ZcPresentFlip) instead of a CPU
    * copy out and the executor's full-frame BLT (two 23 MiB CPU copies per frame) */
   /* R6H: DWM's destination is an opened (shared) primary, not one it created: a destination whose copy
    * present dxgkrnl refused before (the UMD then BLTs and flips it itself) counts too */
   bool dstOk = pDst && pDst->hAllocation && (pDst->isPrimary || pDst->hAllocation == pDevice->zcRefusedDst);
   /* R6I: DWM presents from one of its NO_SCANOUT primaries (shadowed, unlocked) to another */
   bool shadowScan = pSrc && pSrc->shadowed && pSrc->resource &&
                     (pPresentData->Flags.Flip || dstOk) && ZcScanReady(pDevice, pSrc);
   if (pSrc && pSrc->shadowed) {
      static unsigned nshadow;
      if (UmdTrace() && (nshadow++ < 8 || nshadow % 600 == 0))
         DebugPrintf("zc: present %u from a shadowed source 0x%x %ux%u to 0x%x (primary %d, refused before %d) flags 0x%x, "
                     "scan-out buffers %u: %s\n", nshadow, pSrc->hAllocation, pSrc->resource ? pSrc->resource->width0: 0,
                     pSrc->resource ? pSrc->resource->height0: 0, pDst ? pDst->hAllocation: 0, pDst ? pDst->isPrimary: -1,
                     pDst && pDst->hAllocation == pDevice->zcRefusedDst, pPresentData->Flags.Value, pDevice->zcScanCount,
                     shadowScan ? "GPU copy to a scan-out buffer": "CPU copy out");
      if (!shadowScan)
         WddmCopyOut(pDevice, pSrc);
      else
         pSrc->scanSourced = true;
   }
   UINT64 perfF0 = PerfNow();
   /* G6 r7 (A): a frame that reaches the panel through a scan-out buffer is flushed after its copy there and
    * not waited for (ZcPresentFlip) */
   bool pipeQ = pSrc && pSrc->resource && pDevice->zc &&
                (shadowScan || (pSrc->zeroCopy && !pPresentData->Flags.Flip && dstOk)) &&
                ZcScanReady(pDevice, pSrc) && ZcFlipQOn(pDevice);
   if (pipeQ) {
      /* the copy and the flush follow */
   } else if (pSrc && pSrc->zeroCopy) {
      ZcFinish(pDevice); /* the KMD's Present BLT reads the pages the GPU wrote */
   } else {
      UINT64 t0 = PerfNow();
      pDevice->pipe->flush(pDevice->pipe, NULL, 0);
      pDevice->perfFlushCpu += PerfNow() - t0;
   }
   PerfAdd(PERF_P_FLUSH, PerfNow() - perfF0);

   DXGIDDICB_PRESENT p;
   memset(&p, 0, sizeof p);
   p.hSrcAllocation = pSrc ? pSrc->hAllocation: 0;
   p.hDstAllocation = pDst ? pDst->hAllocation: 0;
   p.pDXGIContext = pPresentData->pDXGIContext;
   p.hContext = pDevice->hContext;
   pDevice->frame++;
   if (UmdTrace()) {
      UINT64 now = PerfNow();
      static UINT64 ddi0;
      if (!pDevice->perfT0) {
         pDevice->perfT0 = now;
         ddi0 = UmdDdiTicks;
      }
      if (++pDevice->perfFrames == 120) {
         LARGE_INTEGER fq;
         QueryPerformanceFrequency(&fq);
         double ms = 1000.0 / (double)fq.QuadPart / 120.0;
         UINT64 ddi = UmdDdiTicks;
         DebugPrintf("zc: perf 120 frames: %.2f ms/frame, in UMD DDIs %.2f, in Present %.2f (throttle %.2f), GPU waits %.2f, "
                     "flush CPU %.2f, proxy refreshes %.2f (%llu: blt waits %.2f, %llu blts, %llu GPU copies, %llu skipped), "
                     "flushes %.2f/frame (%.2f left to Present)%s\n",
                     (now - pDevice->perfT0) * ms, (ddi - ddi0) * ms, pDevice->perfPresent * ms, pDevice->perfThrottle * ms,
                     pDevice->perfFinish * ms, pDevice->perfFlushCpu * ms, pDevice->perfSync * ms,
                     (unsigned long long)pDevice->perfSyncs, pDevice->perfPqWait * ms, (unsigned long long)pDevice->perfPqBlts,
                     (unsigned long long)pDevice->perfPqCopies, (unsigned long long)pDevice->perfPqSkips,
                     pDevice->perfFlushes / 120.0, pDevice->perfFlushDeferred / 120.0, pDevice->zcFlipQ == 1 ? ", pipelined": "");
         pDevice->perfT0 = now;
         ddi0 = ddi;
         pDevice->perfFinish = pDevice->perfSync = pDevice->perfFlushes = pDevice->perfSyncs = pDevice->perfPresent = 0;
         pDevice->perfThrottle = pDevice->perfFlushCpu = pDevice->perfPqWait = pDevice->perfPqBlts = 0;
         pDevice->perfPqCopies = pDevice->perfPqSkips = pDevice->perfFlushDeferred = 0;
         pDevice->perfFrames = 0;
         /* R8 (H): every 600 frames, the proxies refreshed since the last dump (which shared surfaces cost the
          * frame: size, BLTs, GPU copies, status-lock waits) */
         if (++pDevice->perfPqDumps % 5 == 0) {
            unsigned lines = 0, quiet = 0;
            for (Resource *r = pDevice->proxiedHead; r; r = r->proxiedNext) {
               if (!r->pqStBlts && !r->pqStSyncs) {
                  quiet++;
                  continue;
               }
               if (lines++ < 12)
                  DebugPrintf("zc: pq stat shared 0x%x %ux%u %s key %#llx gen %llu used %u: %u syncs %u blts %u copies, "
                              "sync time %.2f ms (%.3f ms/frame)\n", r->hShared, r->resource ? r->resource->width0: 0,
                              r->resource ? r->resource->height0: 0,
                              r->pqStage[0] ? "pq": r->zeroCopy ? "zc-proxy": "old-proxy", (unsigned long long)r->shareKey,
                              (unsigned long long)GenRead(r->shareKey), pDevice->frame - r->pqUsedFrame, r->pqStSyncs,
                              r->pqStBlts, r->pqStCopies, r->pqStWait * ms * 120.0, r->pqStWait * ms * 120.0 / 600.0);
               r->pqStBlts = r->pqStCopies = r->pqStSyncs = 0;
               r->pqStWait = 0;
            }
            DebugPrintf("zc: pq stat: %u proxies refreshed in 600 frames (%u listed), %u quiet; pq no free staging %llu, "
                        "pq failures %llu, reclaimed BLTs %llu, zc-proxy syncs %llu, old-proxy syncs %llu\n", lines, lines < 12 ? lines: 12,
                        quiet, (unsigned long long)pDevice->perfPqNoStage, (unsigned long long)pDevice->perfPqFail,
                        (unsigned long long)pDevice->perfPqReclaims,
                        (unsigned long long)pDevice->perfSyncZc, (unsigned long long)pDevice->perfSyncOld);
            pDevice->perfPqNoStage = pDevice->perfPqFail = pDevice->perfSyncZc = pDevice->perfSyncOld = pDevice->perfPqReclaims = 0;
         }
      }
   }
   HRESULT hr = S_OK;
   UINT64 perfC0 = PerfNow();
   if (!(pPresentData->Flags.Flip && pDevice->umdFlips))
      hr = pDevice->pDXGIBaseCallbacks->pfnPresentCb(pDevice->hDevice, &p);
   UINT64 perfC1 = PerfNow();
   PerfAdd(PERF_P_CB, perfC1 - perfC0);
   static unsigned logged;
   if (UmdTrace() && (logged++ < 200 || (logged % 100) == 0 || FAILED(hr)))
      DebugPrintf("PresentCb src 0x%x dst 0x%x flags 0x%x interval %u -> 0x%08lx%s\n",
                  p.hSrcAllocation, p.hDstAllocation, pPresentData->Flags.Value,
                  pPresentData->FlipInterval, hr, pDevice->umdFlips ? " (umd flip)": "");
   if (!pPresentData->Flags.Flip && SUCCEEDED(hr) && pSrc && pDst && pDst->isPrimary && pDst->hAllocation) {
      /* G6 zero copy: the frame goes to the panel without the executor's full-frame copy */
      if (!((pSrc->zeroCopy || shadowScan) && ZcPresentFlip(pDevice, pSrc)) && WddmUmdBlt(pDevice, pDst, pSrc))
         WddmUmdFlip(pDevice, pDst);
   }
   if (!pPresentData->Flags.Flip && FAILED(hr) && pSrc && pDst && pDst->hAllocation) {
      pDevice->zcRefusedDst = pDst->hAllocation;
      /* a refused copy present to a primary: copy with the executor and scan the primary out */
      if ((pSrc->zeroCopy || shadowScan) && ZcPresentFlip(pDevice, pSrc))
         hr = S_OK;
      else if (WddmUmdBlt(pDevice, pDst, pSrc) && WddmUmdFlip(pDevice, pDst))
         hr = S_OK;
   }
   if (pPresentData->Flags.Flip && pSrc && (FAILED(hr) || pDevice->umdFlips)) {
      /* dxgkrnl refuses DWM's flip on this WDDM 1.3 adapter (STATUS_GRAPHICS_NOT_EXCLUSIVE_MODE_OWNER):
       * the KMD's executor scans the buffer out at the next vsync instead (G18W_OP_FLIP with geometry).
       * G6 zero copy: flip-model buffers are flipped themselves (the GPU is done: ZcFinish above). */
      if (shadowScan ? ZcPresentFlip(pDevice, pSrc): WddmUmdFlip(pDevice, pSrc)) {
         pDevice->umdFlips = true;
         hr = S_OK;
      }
   }
   if (pipeQ && pDevice->zcPipeFrame != pDevice->frame) {
      /* no pipelined flip happened after all: what the frame drew still goes to the GPU */
      UINT64 t0 = PerfNow();
      pDevice->pipe->flush(pDevice->pipe, NULL, 0);
      pDevice->perfFlushCpu += PerfNow() - t0;
   }
   if (pDevice->sharedHead)
      WddmBumpShared(pDevice);
   UINT64 perfOut = PerfNow();
   PerfAdd(PERF_P_FLIP, perfOut - perfC1);
   pDevice->perfPresent += perfOut - perfIn;
   PerfFrame();
   return hr;
}


static bool
SubmitPacketN(Device *pDevice, const void *pkt, UINT bytes, const D3DKMT_HANDLE *allocs, const UINT *write,
              const UINT *patch_offsets, UINT n, UINT npatch)
{
   if (!pDevice->pCommandBuffer || pDevice->CommandBufferSize < bytes || pDevice->AllocationListSize < n ||
       pDevice->PatchLocationListSize < npatch || npatch > n)
      return false;
   memcpy(pDevice->pCommandBuffer, pkt, bytes);
   memset(pDevice->pAllocationList, 0, n * sizeof *pDevice->pAllocationList);
   memset(pDevice->pPatchLocationList, 0, npatch * sizeof *pDevice->pPatchLocationList);
   for (UINT i = 0; i < n; i++) {
      pDevice->pAllocationList[i].hAllocation = allocs[i];
      pDevice->pAllocationList[i].WriteOperation = write[i];
      if (i < npatch) {
         pDevice->pPatchLocationList[i].AllocationIndex = i;
         pDevice->pPatchLocationList[i].PatchOffset = patch_offsets[i];
      }
   }
   D3DDDICB_RENDER r;
   memset(&r, 0, sizeof r);
   r.CommandLength = bytes;
   r.NumAllocations = n;
   r.NumPatchLocations = npatch;
   r.NewCommandBufferSize = pDevice->CommandBufferSize;
   r.NewAllocationListSize = pDevice->AllocationListSize;
   r.NewPatchLocationListSize = pDevice->PatchLocationListSize;
   r.hContext = pDevice->hContext;
   HRESULT hr = pDevice->KTCallbacks.pfnRenderCb(pDevice->hDevice, &r);
   if (SUCCEEDED(hr)) {
      if (r.pNewCommandBuffer) {
         pDevice->pCommandBuffer = r.pNewCommandBuffer;
         pDevice->CommandBufferSize = r.NewCommandBufferSize;
      }
      if (r.pNewAllocationList) {
         pDevice->pAllocationList = r.pNewAllocationList;
         pDevice->AllocationListSize = r.NewAllocationListSize;
      }
      if (r.pNewPatchLocationList) {
         pDevice->pPatchLocationList = r.pNewPatchLocationList;
         pDevice->PatchLocationListSize = r.NewPatchLocationListSize;
      }
   }
   if (FAILED(hr) && UmdTrace()) {
      static unsigned nfail;
      if (nfail++ < 16)
         DebugPrintf("zc: RenderCb of %u bytes, %u allocations, %u patches: 0x%08lx\n", bytes, n, npatch, hr);
   }
   return SUCCEEDED(hr);
}

static bool
SubmitPacket(Device *pDevice, const void *pkt, UINT bytes, const D3DKMT_HANDLE *allocs, const UINT *write,
             const UINT *patch_offsets, UINT n)
{
   return SubmitPacketN(pDevice, pkt, bytes, allocs, write, patch_offsets, n, n);
}

bool
WddmUmdBlt(Device *pDevice, Resource *pDst, Resource *pSrc)
{
   if (!pSrc->hAllocation || !pSrc->resource || !pDst->resource)
      return false;
   struct g18w_blt b;
   memset(&b, 0, sizeof b);
   b.hdr.op = G18W_OP_BLT;
   b.hdr.bytes = sizeof b;
   b.dst.index = 0;
   b.src.index = 1;
   b.dst_pitch = pDst->allocationPitch;
   b.src_pitch = pSrc->allocationPitch;
   b.bpp = 4;
   b.w = MIN2(pSrc->resource->width0, pDst->resource->width0);
   b.h = MIN2(pSrc->resource->height0, pDst->resource->height0);
   D3DKMT_HANDLE h[2] = { pDst->hAllocation, pSrc->hAllocation };
   UINT w[2] = { 1, 0 };
   UINT po[2] = { offsetof(struct g18w_blt, dst), offsetof(struct g18w_blt, src) };
   bool ok = SubmitPacket(pDevice, &b, sizeof b, h, w, po, 2);
   static unsigned logged;
   if (UmdTrace() && (logged++ < 8 || !ok))
      DebugPrintf("UmdBlt 0x%x -> 0x%x %ux%u: %s\n", pSrc->hAllocation, pDst->hAllocation, b.w, b.h, ok ? "ok": "FAILED");
   return ok;
}

bool
WddmUmdFlip(Device *pDevice, Resource *pSrc, UINT flags, UINT64 tag)
{
   if (!pSrc->hAllocation || !pSrc->resource || !pDevice->pCommandBuffer ||
       pDevice->CommandBufferSize < sizeof(struct g18w_flip) || pDevice->AllocationListSize < 1 ||
       pDevice->PatchLocationListSize < 1)
      return false;
   struct g18w_flip f;
   memset(&f, 0, sizeof f);
   f.hdr.op = G18W_OP_FLIP;
   f.hdr.bytes = sizeof f;
   f.src.index = 0;
   f.pitch = pSrc->allocationPitch;
   f.width = pSrc->resource->width0;
   f.height = pSrc->resource->height0;
   f.flags = flags;
   f.tag = tag;
   if (flags & G18W_FLIPF_REGISTER) {
      f.vidpn_source = (UINT32)tag;
      f.tag = 0;
   }
   memcpy(pDevice->pCommandBuffer, &f, sizeof f);
   memset(pDevice->pAllocationList, 0, sizeof *pDevice->pAllocationList);
   pDevice->pAllocationList[0].hAllocation = pSrc->hAllocation;
   memset(pDevice->pPatchLocationList, 0, sizeof *pDevice->pPatchLocationList);
   pDevice->pPatchLocationList[0].AllocationIndex = 0;
   pDevice->pPatchLocationList[0].PatchOffset = offsetof(struct g18w_flip, src);
   D3DDDICB_RENDER r;
   memset(&r, 0, sizeof r);
   r.CommandLength = sizeof f;
   r.NumAllocations = 1;
   r.NumPatchLocations = 1;
   r.NewCommandBufferSize = pDevice->CommandBufferSize;
   r.NewAllocationListSize = pDevice->AllocationListSize;
   r.NewPatchLocationListSize = pDevice->PatchLocationListSize;
   r.hContext = pDevice->hContext;
   HRESULT hr = pDevice->KTCallbacks.pfnRenderCb(pDevice->hDevice, &r);
   if (SUCCEEDED(hr)) {
      if (r.pNewCommandBuffer) {
         pDevice->pCommandBuffer = r.pNewCommandBuffer;
         pDevice->CommandBufferSize = r.NewCommandBufferSize;
      }
      if (r.pNewAllocationList) {
         pDevice->pAllocationList = r.pNewAllocationList;
         pDevice->AllocationListSize = r.NewAllocationListSize;
      }
      if (r.pNewPatchLocationList) {
         pDevice->pPatchLocationList = r.pNewPatchLocationList;
         pDevice->PatchLocationListSize = r.NewPatchLocationListSize;
      }
   }
   static unsigned logged;
   if (UmdTrace() && (logged++ < 8 || FAILED(hr)))
      DebugPrintf("UmdFlip 0x%x %ux%u pitch %u flags 0x%x tag %#llx -> 0x%08lx\n", pSrc->hAllocation, f.width, f.height,
                  f.pitch, flags, (unsigned long long)tag, hr);
   return SUCCEEDED(hr);
}


HRESULT
WddmSetDisplayMode(Device *pDevice, DXGI_DDI_ARG_SETDISPLAYMODE *pData)
{
   Resource *pRes = CastResource(pData->hResource);
   D3DDDICB_SETDISPLAYMODE sdm;
   memset(&sdm, 0, sizeof sdm);
   sdm.hPrimaryAllocation = pRes ? pRes->hAllocation: 0;
   HRESULT hr = pDevice->KTCallbacks.pfnSetDisplayModeCb(pDevice->hDevice, &sdm);
   if (UmdTrace())
      DebugPrintf("SetDisplayModeCb alloc 0x%x -> 0x%08lx\n", sdm.hPrimaryAllocation, hr);
   return hr;
}


/* G6 zero-copy proxy refresh: the GPU's earlier work (it may still sample the old content) is waited for once per
 * frame, the KMD BLT writes the imported pages, and a lock of hZcSync (named in the same DMA buffer) waits for it */
static void
WddmSyncSharedZc(Device *pDevice, Resource *pResource)
{
   struct pipe_resource *res = pResource->resource;
   if (!pDevice->zcFinishValid || pDevice->zcFinishFrame != pDevice->frame) {
      ZcFinish(pDevice);
      pDevice->zcFinishValid = true;
      pDevice->zcFinishFrame = pDevice->frame;
   }
   if (!pDevice->hZcSync) {
      struct g18w_alloc_priv sp;
      memset(&sp, 0, sizeof sp);
      sp.magic = G18W_ALLOC_MAGIC;
      sp.version = G18W_UAPI_VERSION;
      sp.kind = G18W_ALLOC_BUFFER;
      sp.size = 4096;
      D3DDDI_ALLOCATIONINFO ai;
      memset(&ai, 0, sizeof ai);
      ai.pPrivateDriverData = &sp;
      ai.PrivateDriverDataSize = sizeof sp;
      D3DDDICB_ALLOCATE a;
      memset(&a, 0, sizeof a);
      a.NumAllocations = 1;
      a.pAllocationInfo = &ai;
      if (SUCCEEDED(pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &a)))
         pDevice->hZcSync = ai.hAllocation;
   }
   struct g18w_blt b;
   memset(&b, 0, sizeof b);
   b.hdr.op = G18W_OP_BLT;
   b.hdr.bytes = sizeof b;
   b.dst.index = 0;
   b.src.index = 1;
   b.dst_pitch = pResource->allocationPitch;
   b.src_pitch = pResource->sharedPitch;
   b.bpp = 4;
   b.flags = G18W_BLTF_IF_CHANGED; /* only these BLTs write the proxy: an unchanged source is not copied */
   b.w = res->width0;
   b.h = res->height0;
   /* R6E: a DMA buffer naming the locked proxy for write was refused (RenderCb): the proxy is named read-only
    * (the executor writes it on the CPU; VidMm only orders on these flags), and a 4-byte FILL of the sync
    * allocation follows the BLT in the same buffer, so locking that waits for both */
   struct {
      struct g18w_blt b;
      struct g18w_fill f;
   } pk;
   memset(&pk, 0, sizeof pk);
   pk.b = b;
   pk.f.hdr.op = G18W_OP_FILL;
   pk.f.hdr.bytes = sizeof pk.f;
   pk.f.dst.index = 2;
   pk.f.bytes = 4;
   D3DKMT_HANDLE h[3] = { pResource->hAllocation, pResource->hShared, pDevice->hZcSync };
   UINT w[3] = { 1, 0, 1 };
   UINT po[3] = { offsetof(struct g18w_blt, dst), offsetof(struct g18w_blt, src),
                  (UINT)(sizeof(struct g18w_blt) + offsetof(struct g18w_fill, dst)) };
   bool ok = pDevice->hZcSync ? SubmitPacketN(pDevice, &pk, sizeof pk, h, w, po, 3, 3)
: SubmitPacketN(pDevice, &b, sizeof b, h, w, po, 2, 2);
   HRESULT hl = E_FAIL;
   if (ok && pDevice->hZcSync) {
      D3DDDICB_LOCK lock;
      memset(&lock, 0, sizeof lock);
      lock.hAllocation = pDevice->hZcSync;
      hl = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
      if (SUCCEEDED(hl)) {
         D3DDDICB_UNLOCK ul;
         ul.NumAllocations = 1;
         ul.phAllocations = &pDevice->hZcSync;
         pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
      }
   }
   static unsigned logged;
   if (UmdTrace() && (logged < 64 || !ok || FAILED(hl))) {
      logged++;
      DebugPrintf("zc: SyncShared 0x%x -> 0x%x %ux%u frame %u: blt %s, sync lock 0x%08lx\n", pResource->hShared,
                  pResource->hAllocation, b.w, b.h, pDevice->frame, ok ? "ok": "FAILED", hl);
   }
}

/* opened shared resources behind a private copy: one KMD BLT (shared -> copy) per frame that reads
 * them, then a lock of the copy (VidMm waits for the BLT) */
static void WddmSyncSharedResource1(Device *pDevice, Resource *pResource);

void
WddmSyncSharedResource(Device *pDevice, Resource *pResource)
{
   if (!pResource->hShared || (pResource->refreshed && pResource->refreshFrame == pDevice->frame))
      return;
   UINT64 t0 = PerfNow();
   WddmSyncSharedResource1(pDevice, pResource);
   pResource->pqStSyncs++;
   pResource->pqStWait += PerfNow() - t0;
   pDevice->perfSync += PerfNow() - t0;
   pDevice->perfSyncs++;
}

static void
WddmSyncSharedResource1(Device *pDevice, Resource *pResource)
{
   if (!pDevice->pCommandBuffer || pDevice->CommandBufferSize < sizeof(struct g18w_blt) ||
       pDevice->AllocationListSize < 2 || pDevice->PatchLocationListSize < 2)
      return;
   struct pipe_resource *res = pResource->resource;
   if (UmdTraceAll())
      DebugPrintf("SyncShared begin 0x%x -> 0x%x frame %u\n", pResource->hShared, pResource->hAllocation, pDevice->frame);
   pResource->refreshed = true;
   pResource->refreshFrame = pDevice->frame;
   if (pResource->pqStage[0]) {
      PqSync(pDevice, pResource);
      return;
   }
   if (pResource->zeroCopy) {
      pDevice->perfSyncZc++;
      WddmSyncSharedZc(pDevice, pResource);
      return;
   }
   pDevice->perfSyncOld++;
   /* R6K: the flush moved to just before the upload below */

   struct g18w_blt b;
   memset(&b, 0, sizeof b);
   b.hdr.op = G18W_OP_BLT;
   b.hdr.bytes = sizeof b;
   b.dst.index = 0;
   b.src.index = 1;
   b.dst_pitch = pResource->allocationPitch;
   b.src_pitch = pResource->sharedPitch ? pResource->sharedPitch: pResource->allocationPitch;
   b.bpp = pResource->sharedBpp;
   /* R6K: only these BLTs write the private copy: the executor skips an unchanged source */
   b.flags = pResource->sharedHashValid ? G18W_BLTF_IF_CHANGED: 0;
   b.w = res->width0;
   b.h = res->height0;
   memcpy(pDevice->pCommandBuffer, &b, sizeof b);
   memset(pDevice->pAllocationList, 0, 2 * sizeof *pDevice->pAllocationList);
   pDevice->pAllocationList[0].hAllocation = pResource->hAllocation;
   pDevice->pAllocationList[0].WriteOperation = 1;
   pDevice->pAllocationList[1].hAllocation = pResource->hShared;
   memset(pDevice->pPatchLocationList, 0, 2 * sizeof *pDevice->pPatchLocationList);
   pDevice->pPatchLocationList[0].AllocationIndex = 0;
   pDevice->pPatchLocationList[0].PatchOffset = offsetof(struct g18w_blt, dst);
   pDevice->pPatchLocationList[1].AllocationIndex = 1;
   pDevice->pPatchLocationList[1].PatchOffset = offsetof(struct g18w_blt, src);

   D3DDDICB_RENDER r;
   memset(&r, 0, sizeof r);
   r.CommandLength = sizeof b;
   r.NumAllocations = 2;
   r.NumPatchLocations = 2;
   r.NewCommandBufferSize = pDevice->CommandBufferSize;
   r.NewAllocationListSize = pDevice->AllocationListSize;
   r.NewPatchLocationListSize = pDevice->PatchLocationListSize;
   r.hContext = pDevice->hContext;
   HRESULT hr = pDevice->KTCallbacks.pfnRenderCb(pDevice->hDevice, &r);
   if (SUCCEEDED(hr)) {
      if (r.pNewCommandBuffer) {
         pDevice->pCommandBuffer = r.pNewCommandBuffer;
         pDevice->CommandBufferSize = r.NewCommandBufferSize;
      }
      if (r.pNewAllocationList) {
         pDevice->pAllocationList = r.pNewAllocationList;
         pDevice->AllocationListSize = r.NewAllocationListSize;
      }
      if (r.pNewPatchLocationList) {
         pDevice->pPatchLocationList = r.pNewPatchLocationList;
         pDevice->PatchLocationListSize = r.NewPatchLocationListSize;
      }
   }
   if (UmdTraceAll())
      DebugPrintf("SyncShared RenderCb 0x%08lx\n", hr);
   /* the lock waits for the BLT; then copy into the pipe texture */
   D3DDDICB_LOCK lock;
   memset(&lock, 0, sizeof lock);
   lock.hAllocation = pResource->hAllocation;
   lock.Flags.ReadOnly = 1;
   HRESULT hl = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
   if (SUCCEEDED(hl) && lock.pData) {
      struct pipe_box box;
      u_box_2d(0, 0, res->width0, res->height0, &box);
      pDevice->pipe->flush(pDevice->pipe, NULL, 0);
      pDevice->pipe->texture_subdata(pDevice->pipe, res, 0, PIPE_MAP_WRITE, &box, lock.pData,
                                     pResource->allocationPitch, 0);
      pResource->sharedHashValid = true; /* the next BLTs may be skipped by the executor */
   }
   if (SUCCEEDED(hl)) {
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &pResource->hAllocation;
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
   }
   static unsigned logged;
   if (UmdTrace() && (logged < 64 || FAILED(hr) || FAILED(hl))) {
      logged++;
      DebugPrintf("SyncShared 0x%x -> 0x%x %ux%u frame %u: RenderCb 0x%08lx lock 0x%08lx %p\n",
                  pResource->hShared, pResource->hAllocation, b.w, b.h, pDevice->frame, hr, hl, lock.pData);
   }
}

void
WddmSyncShared(Device *pDevice)
{
   if (!pDevice->pqScanned || pDevice->pqScanFrame != pDevice->frame) {
      pDevice->pqScanned = true;
      pDevice->pqScanFrame = pDevice->frame;
      UINT64 t0 = PerfNow();
      PqPrefetch(pDevice);
      pDevice->perfSync += PerfNow() - t0;
   }
   for (Resource *r = pDevice->proxiedHead; r; r = r->proxiedNext) {
      if (r->refreshed && r->refreshFrame == pDevice->frame)
         continue;
      bool bound = false;
      for (unsigned st = 0; st < MESA_SHADER_STAGES && !bound; st++)
         for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS && !bound; i++)
            bound = pDevice->sampler_views[st][i] && pDevice->sampler_views[st][i]->texture == r->resource;
      if (bound)
         WddmSyncSharedResource(pDevice, r);
   }
}

/* Primaries rendered into own textures: DWM flips them with D3DKMTPresent from dwmcore (not through
 * the UMD's Present), so their content goes to the allocation at every Flush when they were drawn to. */
void
WddmMarkDirty(Device *pDevice, struct pipe_resource *res)
{
   for (Resource *r = pDevice->shadowHead; r; r = r->shadowNext)
      if (r->resource == res)
         r->dirty = true;
}

void
WddmMarkBoundDirty(Device *pDevice)
{
   for (unsigned i = 0; i < pDevice->fb.nr_cbufs; i++)
      if (pDevice->fb.cbufs[i].texture)
         WddmMarkDirty(pDevice, pDevice->fb.cbufs[i].texture);
}

/* G6 r7 (A): after a flush, the shared resources this device renders into directly may have changed */
void
WddmBumpShared(Device *pDevice)
{
   for (Resource *r = pDevice->sharedHead; r; r = r->sharedNext)
      GenBump(r->shareKey);
}

void
WddmFlushShadowed(Device *pDevice)
{
   WddmMarkBoundDirty(pDevice);
   /* R6I: once zero-copy scan-out buffers carry the frames to the panel (GPU copies at Present), DWM's
    * NO_SCANOUT primaries are not read from their allocations: no full-size CPU copy out at every Flush */
   bool scanOwned = pDevice->zc && pDevice->zcScanCount >= 2;
   /* R7: C:\g6w\copyout.skip: nor DWM's (shared, DComp-padded) back buffer once the GPU scan-out copy reads
    * its texture at Present. Off by default: r7 run 20261121-025147 showed Explorer's Mica/backdrop area black
    * with it (DWM reads its own back buffer's allocation through a proxy). */
   static int skip = -1;
   if (skip < 0)
      skip = FlagFile("C:\\g6w\\copyout.skip") ? 1: 0;
   UINT64 t0 = PerfNow();
   for (Resource *r = pDevice->shadowHead; r; r = r->shadowNext)
      if (r->dirty) {
         r->dirty = false;
         if (scanOwned && (r->isPrimary || (r->scanSourced && skip)))
            continue;
         WddmCopyOut(pDevice, r);
         PerfAdd(PERF_F_COPYOUTS, 1);
      }
   PerfAdd(PERF_F_COPYOUT, PerfNow() - t0);
}

/* shadowed resources: allocation memory <-> pipe texture (single level, single layer) */
void
WddmCopyOut(Device *pDevice, Resource *pResource)
{
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_resource *res = pResource->resource;
   struct pipe_transfer *transfer;
   const uint8_t *src = (const uint8_t *)pipe_texture_map(pipe, res, 0, 0, PIPE_MAP_READ, 0, 0,
                                                          res->width0, res->height0, &transfer);
   if (!src)
      return;
   void *dst = pResource->pAllocationData;
   D3DDDICB_LOCK lock;
   if (!dst) { /* lock on demand (primaries) */
      memset(&lock, 0, sizeof lock);
      lock.hAllocation = pResource->hAllocation;
      lock.Flags.WriteOnly = 1;
      HRESULT hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
      static unsigned logged;
      if (UmdTrace() && (FAILED(hr) || logged++ < 4))
         DebugPrintf("CopyOut lock 0x%x -> 0x%08lx %p\n", pResource->hAllocation, hr, lock.pData);
      if (FAILED(hr) || !lock.pData) {
         pipe_texture_unmap(pipe, transfer);
         return;
      }
      dst = lock.pData;
   }
   UINT row = util_format_get_stride(res->format, res->width0);
   for (UINT y = 0; y < res->height0; y++)
      memcpy((uint8_t *)dst + (size_t)y * pResource->allocationPitch,
             src + (size_t)y * transfer->stride, row);
   pipe_texture_unmap(pipe, transfer);
   if (!pResource->pAllocationData) {
      D3DDDICB_UNLOCK ul;
      ul.NumAllocations = 1;
      ul.phAllocations = &pResource->hAllocation;
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &ul);
   }
   if (pResource->shareKey)
      GenBump(pResource->shareKey); /* G6 r7 (A): readers refresh their proxies */
}

void
WddmCopyIn(Device *pDevice, Resource *pResource)
{
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_resource *res = pResource->resource;
   struct pipe_box box;
   u_box_2d(0, 0, res->width0, res->height0, &box);
   pipe->texture_subdata(pipe, res, 0, PIPE_MAP_WRITE, &box, pResource->pAllocationData,
                         pResource->allocationPitch, 0);
}

/* software mode on a GPU screen: read the frame back and draw it into the window with GDI */
HRESULT
ReadbackPresent(Device *pDevice, Resource *pSrc, void *pDXGIContext)
{
   D3DKMT_PRESENT *kp = (D3DKMT_PRESENT *)pDXGIContext;
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_resource *res = pSrc->resource;
   struct pipe_transfer *transfer;
   const void *src = pipe_texture_map(pipe, res, 0, 0, PIPE_MAP_READ, 0, 0, res->width0, res->height0,
                                      &transfer);
   if (!src)
      return E_FAIL;
   BITMAPINFO bmi;
   memset(&bmi, 0, sizeof bmi);
   bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
   bmi.bmiHeader.biWidth = transfer->stride / 4;
   bmi.bmiHeader.biHeight = -(LONG)res->height0;
   bmi.bmiHeader.biPlanes = 1;
   bmi.bmiHeader.biBitCount = 32;
   bmi.bmiHeader.biCompression = BI_RGB;
   HDC dc = GetDC(kp->hWindow);
   StretchDIBits(dc, 0, 0, res->width0, res->height0, 0, 0, res->width0, res->height0, src, &bmi,
                 DIB_RGB_COLORS, SRCCOPY);
   ReleaseDC(kp->hWindow, dc);
   pipe_texture_unmap(pipe, transfer);
   return S_OK;
}
