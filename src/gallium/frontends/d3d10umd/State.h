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
 * State.h --
 *    State declarations.
 */


#include "DriverIncludes.h"
#include "util/u_hash_table.h"
#include "cso_cache/cso_context.h"

#define SUPPORT_MSAA 0
#define SUPPORT_D3D10_1 1
#define SUPPORT_D3D11 1


struct Adapter
{
   struct pipe_screen *screen;
   bool wddm;                 /* KMD answered the g18w magic: hardware (WDDM) mode */
   bool dt_capable;           /* screen renders into winsys display targets (softpipe/llvmpipe);
                                 else (zink) presents and KMD allocations go through copies */
   UINT pitch_align;
   /* G6 zero copy: the KMD takes G18W_ALLOCF_GPU allocations (its segment 1 is the pool's WDDM window) */
   bool zc_kmd;
   UINT scan_w, scan_h;       /* the scan-out mode */
   UINT64 segment_size;       /* segment 1 bytes */
   HANDLE hRTAdapter;         /* G6 r7: the runtime's adapter handle (pfnEscapeCb) */
};


static inline Adapter *
CastAdapter(D3D10DDI_HADAPTER hAdapter)
{
   return static_cast<Adapter *>(hAdapter.pDrvPrivate);
}

struct Shader
{
   void *handle;
   uint type;
   struct pipe_shader_state state;
   unsigned output_mapping[PIPE_MAX_SHADER_OUTPUTS];
   bool output_resolved;
   unsigned ntokens;                 /* SM4 dwords (pixel shaders; draw tracing) */
};

struct Query;
struct ElementLayout;

struct Device
{
   struct pipe_context *pipe;

   struct cso_context *cso;
   struct pipe_framebuffer_state fb;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned vertex_strides[PIPE_MAX_ATTRIBS];
   struct pipe_resource *index_buffer;
   unsigned restart_index;
   unsigned index_size;
   unsigned ib_offset;
   void *samplers[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
   struct pipe_sampler_view *sampler_views[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS];

   void *empty_fs;
   void *empty_vs;

   enum mesa_prim primitive;

   struct pipe_stream_output_target *so_targets[PIPE_MAX_SO_BUFFERS];
   struct pipe_stream_output_target *draw_so_target;
   Shader *bound_empty_gs;
   Shader *bound_vs;
   Shader *bound_ps;                 /* draw tracing */
   struct pipe_resource *ps_cb0;     /* draw tracing (referenced) */
   float vp[4];                      /* viewport 0 x, y, w, h (draw tracing) */

   unsigned max_dual_source_render_targets;

   D3D10DDI_HRTCORELAYER  hRTCoreLayer;

   HANDLE hDevice;
   HANDLE hContext;
   /* WDDM: the context's current command buffer and lists (pfnRenderCb) */
   void *pCommandBuffer;
   UINT CommandBufferSize;
   D3DDDI_ALLOCATIONLIST *pAllocationList;
   UINT AllocationListSize;
   D3DDDI_PATCHLOCATIONLIST *pPatchLocationList;
   UINT PatchLocationListSize;
   UINT frame;                       /* WDDM presents so far */
   bool umdFlips;                    /* flips go to the KMD executor (dxgkrnl refused one) */
   struct Resource *proxiedHead;     /* opened shared resources read through a KMD copy */
   struct Resource *shadowHead;      /* primaries rendered into own textures, copied out at flush */
   bool wddm;
   bool dt_capable;
   UINT pitch_align;
   /* G6 zero copy (track Z): KMD allocations the GPU renders into are imported as the zink texture itself
    * (G18W_ALLOCF_GPU, locked, VK_EXT_external_memory_host -> render node GEM_USERPTR): no shadow, no copy */
   bool zc;
   UINT scanW, scanH;
   UINT64 segmentSize;
   struct Resource *zcScan[3];       /* scan-out buffers a zero-copy present is copied into on the GPU */
   UINT zcScanCount, zcScanNext;
   bool zcScanTried;
   UINT64 zcPresents, zcFlipsDirect;
   D3DKMT_HANDLE hZcSync;            /* G6: a small allocation named in zero-copy proxy BLTs; locking it waits for them */
   UINT zcFinishFrame;               /* G6: the frame whose GPU work was last waited for before a proxy BLT */
   bool zcFinishValid;
   D3DKMT_HANDLE zcRefusedDst;
   /* R6K: per-frame timing for the trace (QPC ticks): flushes, GPU waits at Present, proxy refreshes */
   UINT64 perfT0, perfFinish, perfSync, perfFlushes, perfSyncs, perfFrames, perfPresent, perfPqDumps;       /* R6H: the last present destination dxgkrnl refused (the UMD flips instead) */
   /* G6 r7 (A): pipelined presents. The GPU copy into a scan-out buffer is flushed, not waited for; the KMD's flip
    * waits for the render node (G18W_FLIPF_FENCE). Tags name the frames (high 32 bits per device, low the frame);
    * zcScanTag[i]: the frame scan-out buffer i holds, free again once the KMD's tag_done passes it. */
   HANDLE hRTAdapter;
   int zcFlipQ;                      /* -1 not decided, 0 off (C:\g6u\flipq.off, or the KMD has no G18W_ESC_FLIPQ), 1 on */
   UINT64 zcTagNs, zcTag, zcScanTag[3];
   struct ZcAsync *zcAsync;          /* R7 (H): flips sent by a helper thread once zink's submit thread handed the frame over */
   bool zcScanReg[3];                 /* R7 (H): scan-out buffer i registered with the KMD (G18W_FLIPF_REGISTER) */
   UINT64 zcSeenDone, zcSeenSignaled;   /* the last g18w_esc_flipq answer (tags) */
   UINT zcPipeFrame;                 /* the frame whose present flushed and flipped without a wait */
   UINT zcThrottleTimeouts;
   /* G6 r7 (A): pipelined proxies of opened shared surfaces (C:\g6u\proxyq.off: the R6 path) */
   int pq;
   UINT pqScanFrame;
   bool pqScanned;
   /* G6 r7 (A): perf trace: time spent in Present's throttle, draws (incl. proxy work), maps, query reads, flushes
    * (zink's submit; hk retires the running frame first), and the proxy BLT waits */
   UINT64 perfThrottle, perfDraw, perfMap, perfQuery, perfFlushCpu, perfPqWait, perfPqCopies, perfPqBlts,
      perfPqSkips, perfDraws, perfFlushDeferred;
   UINT64 perfPqNoStage, perfPqFail, perfSyncZc, perfSyncOld, perfPqReclaims; /* R8 (H): proxy refresh paths since the last pq stat */
   struct Resource *sharedHead;      /* G6 r7: shared resources over their allocation (no copy out): bumped at flush */
   int flushDefer;                   /* -1 not decided; C:\\g6u\\flushdefer.off */

   struct pipe_resource *rotTemp;    /* R7: RotateResourceIdentities' temporary, kept (a create is a GEM_CREATE + VM_BIND) */

   D3DDDI_DEVICECALLBACKS KTCallbacks;
   D3D10DDI_CORELAYER_DEVICECALLBACKS UMCallbacks;
   DXGI_DDI_BASE_CALLBACKS *pDXGIBaseCallbacks;

   INT LastEmittedQuerySeqNo;
   INT LastFinishedQuerySeqNo;

   Query *pPredicate;
   BOOL PredicateValue;

   ElementLayout *element_layout;
   BOOL velems_changed;
   BOOL vbuffers_changed;
};


static inline Device *
CastDevice(D3D10DDI_HDEVICE hDevice)
{
   return static_cast<Device *>(hDevice.pDrvPrivate);
}


static inline struct pipe_context *
CastPipeContext(D3D10DDI_HDEVICE hDevice)
{
   Device *pDevice = CastDevice(hDevice);
   return pDevice ? pDevice->pipe : NULL;
}


static inline Device *
CastDevice(DXGI_DDI_HDEVICE hDevice)
{
   return reinterpret_cast<Device *>(hDevice);
}


static inline struct pipe_context *
CastPipeDevice(DXGI_DDI_HDEVICE hDevice)
{
   Device *pDevice = CastDevice(hDevice);
   return pDevice ? pDevice->pipe : NULL;
}


static inline void
SetError(D3D10DDI_HDEVICE hDevice, HRESULT hr)
{
   if (FAILED(hr)) {
      Device *pDevice = CastDevice(hDevice);
      pDevice->UMCallbacks.pfnSetErrorCb(pDevice->hRTCoreLayer, hr);
   }
}


struct Resource
{
   DXGI_FORMAT Format;
   UINT MipLevels;
   UINT NumSubResources;
   bool buffer;
   /* WDDM mode: the KMD allocation behind the resource (kept locked) */
   D3DKMT_HANDLE hAllocation;
   void *pAllocationData;
   bool ownsAllocation;
   bool shadowed;             /* pipe resource is separate from the allocation memory (copies) */
   UINT allocationPitch;
   /* WDDM, opened shared resource the opener cannot lock: hAllocation is a private copy that the
    * KMD refreshes from hShared (pfnRenderCb BLT) once per frame before it is read */
   D3DKMT_HANDLE hShared;
   UINT sharedBpp, refreshFrame;
   UINT sharedPitch;
   UINT64 sharedHash;         /* R6K: hash of the last uploaded proxy content (old proxy path) */
   bool sharedHashValid;          /* G6 zero-copy proxy: the shared allocation's pitch (the proxy has its own) */
   bool refreshed;
   struct Resource *proxiedNext;
   struct Resource *shadowNext;
   bool dirty;
   bool scanSourced;          /* R7: a GPU scan-out copy reads its texture at Present: no CPU copy-out at Flush */
   bool isPrimary;
   bool zeroCopy;             /* G6: the pipe resource IS the locked allocation (imported, linear) */
   /* G6 r7 (A): the writer's share key (g18w_alloc_priv reserved[0..1]) of a shared resource this UMD created or
    * opened; the writer bumps its generation after each CPU copy-out (the reader skips unchanged ones) */
   UINT64 shareKey;
   struct Resource *sharedNext;  /* G6 r7: the device's shared resources it renders into directly (zc.sim) */
   /* G6 r7 (A): pipelined proxy: the KMD BLTs the shared surface into a staging allocation (zero copy), the GPU
    * copies staging -> resource in the frame that samples it. pqSync: 4 KiB the BLT's status FILL lands in */
   struct Resource *pqStage[2];
   D3DKMT_HANDLE pqSync[2];
   UINT pqState[2];           /* 0 idle, 1 BLT pending, 2 copied (the GPU may still read the staging) */
   UINT64 pqCopyTag[2];
   UINT pqCur;                /* staging of the last BLT */
   UINT64 pqGen;              /* writer generation the resource holds (0: none or unknown) */
   UINT64 pqGenIssued[2];     /* writer generation of each staging's last BLT */
   UINT pqRFrom;              /* the staging the resource was last copied from (2: none) */
   UINT pqUsedFrame;          /* last frame it was bound */
   bool pqFresh;              /* the resource holds the latest BLT's content */
   UINT pqStBlts, pqStCopies, pqStSyncs; /* R8 (H): per-proxy refresh stats since the last "zc: pq stat" dump */
   UINT64 pqStWait;
   struct pipe_resource *resource;
   struct pipe_transfer **transfers;
   struct pipe_stream_output_target *so_target;
};


static inline Resource *
CastResource(D3D10DDI_HRESOURCE hResource)
{
   return static_cast<Resource *>(hResource.pDrvPrivate);
}


static inline Resource *
CastResource(DXGI_DDI_HRESOURCE hResource)
{
   return reinterpret_cast<Resource *>(hResource);
}


static inline struct pipe_resource *
CastPipeResource(D3D10DDI_HRESOURCE hResource)
{
   Resource *pResource = CastResource(hResource);
   return pResource ? pResource->resource : NULL;
}


static inline struct pipe_resource *
CastPipeResource(DXGI_DDI_HRESOURCE hResource)
{
   Resource *pResource = CastResource(hResource);
   return pResource ? pResource->resource : NULL;
}


static inline struct pipe_resource *
CastPipeBuffer(D3D10DDI_HRESOURCE hResource)
{
   Resource *pResource = CastResource(hResource);
   if (!pResource) {
      return NULL;
   }
   return static_cast<struct pipe_resource *>(pResource->resource);
}


struct RenderTargetView
{
   struct pipe_surface surface;
   D3D10DDI_HRTRENDERTARGETVIEW hRTRenderTargetView;
};


static inline RenderTargetView *
CastRenderTargetView(D3D10DDI_HRENDERTARGETVIEW hRenderTargetView)
{
   return static_cast<RenderTargetView *>(hRenderTargetView.pDrvPrivate);
}


static inline struct pipe_surface *
CastPipeRenderTargetView(D3D10DDI_HRENDERTARGETVIEW hRenderTargetView)
{
   RenderTargetView *pRenderTargetView = CastRenderTargetView(hRenderTargetView);
   return pRenderTargetView ? &pRenderTargetView->surface : NULL;
}


struct DepthStencilView
{
   struct pipe_surface surface;
   D3D10DDI_HRTDEPTHSTENCILVIEW hRTDepthStencilView;
};


static inline DepthStencilView *
CastDepthStencilView(D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView)
{
   return static_cast<DepthStencilView *>(hDepthStencilView.pDrvPrivate);
}


static inline struct pipe_surface *
CastPipeDepthStencilView(D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView)
{
   DepthStencilView *pDepthStencilView = CastDepthStencilView(hDepthStencilView);
   return pDepthStencilView ? &pDepthStencilView->surface : NULL;
}


struct BlendState
{
   void *handle;
};


static inline BlendState *
CastBlendState(D3D10DDI_HBLENDSTATE hBlendState)
{
   return static_cast<BlendState *>(hBlendState.pDrvPrivate);
}


static inline void *
CastPipeBlendState(D3D10DDI_HBLENDSTATE hBlendState)
{
   BlendState *pBlendState = CastBlendState(hBlendState);
   return pBlendState ? pBlendState->handle : NULL;
}


struct DepthStencilState
{
   void *handle;
};


static inline DepthStencilState *
CastDepthStencilState(D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState)
{
   return static_cast<DepthStencilState *>(hDepthStencilState.pDrvPrivate);
}


static inline void *
CastPipeDepthStencilState(D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState)
{
   DepthStencilState *pDepthStencilState = CastDepthStencilState(hDepthStencilState);
   return pDepthStencilState ? pDepthStencilState->handle : NULL;
}


struct RasterizerState
{
   void *handle;
};


static inline RasterizerState *
CastRasterizerState(D3D10DDI_HRASTERIZERSTATE hRasterizerState)
{
   return static_cast<RasterizerState *>(hRasterizerState.pDrvPrivate);
}


static inline void *
CastPipeRasterizerState(D3D10DDI_HRASTERIZERSTATE hRasterizerState)
{
   RasterizerState *pRasterizerState = CastRasterizerState(hRasterizerState);
   return pRasterizerState ? pRasterizerState->handle : NULL;
}


static inline Shader *
CastShader(D3D10DDI_HSHADER hShader)
{
   return static_cast<Shader *>(hShader.pDrvPrivate);
}


static inline void *
CastPipeShader(D3D10DDI_HSHADER hShader)
{
   Shader *pShader = static_cast<Shader *>(hShader.pDrvPrivate);
   return pShader ? pShader->handle : NULL;
}


struct ElementLayout
{
   struct cso_velems_state state;
};


static inline ElementLayout *
CastElementLayout(D3D10DDI_HELEMENTLAYOUT hElementLayout)
{
   return static_cast<ElementLayout *>(hElementLayout.pDrvPrivate);
}

struct SamplerState
{
   void *handle;
};


static inline SamplerState *
CastSamplerState(D3D10DDI_HSAMPLER hSampler)
{
   return static_cast<SamplerState *>(hSampler.pDrvPrivate);
}


static inline void *
CastPipeSamplerState(D3D10DDI_HSAMPLER hSampler)
{
   SamplerState *pSamplerState = CastSamplerState(hSampler);
   return pSamplerState ? pSamplerState->handle : NULL;
}


struct ShaderResourceView
{
   struct pipe_sampler_view *handle;
};


static inline ShaderResourceView *
CastShaderResourceView(D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)
{
   return static_cast<ShaderResourceView *>(hShaderResourceView.pDrvPrivate);
}


static inline struct pipe_sampler_view *
CastPipeShaderResourceView(D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)
{
   ShaderResourceView *pSRView = CastShaderResourceView(hShaderResourceView);
   return pSRView ? pSRView->handle : NULL;
}


struct Query
{
   D3D10DDI_QUERY Type;
   UINT Flags;

   unsigned pipe_type;
   struct pipe_query *handle;
   INT SeqNo;
   UINT GetDataCount;

   D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS Statistics;
};


static inline Query *
CastQuery(D3D10DDI_HQUERY hQuery)
{
   return static_cast<Query *>(hQuery.pDrvPrivate);
}


static inline struct pipe_query *
CastPipeQuery(D3D10DDI_HQUERY hQuery)
{
   Query *pQuery = CastQuery(hQuery);
   return pQuery ? pQuery->handle : NULL;
}
