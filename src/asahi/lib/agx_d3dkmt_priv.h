/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Internal state shared by agx_device_d3dkmt.c and agx_sync_d3dkmt.c. Not
 * installed, not included by anything outside those two files: the public
 * surface is agx_device_d3dkmt.h.
 */

#include <windows.h>

#include "util/simple_mtx.h"

#include "agx_d3dkmt.h"
#include "agx_d3dkmt_core.h"
#include "agx_m5_va.h"

/*
 * Staging ring for AGX command buffers.
 *
 * hk_queue.c builds the flat drm_asahi_cmd_header + payload stream in a
 * util_dynarray of plain host memory and hands the winsys a userspace pointer
 * (hk_queue.c:864-872). D3DKMTSubmitCommand instead takes a GPU virtual
 * address, so the winsys copies the stream into a GPU-mapped BO. Two buffers
 * per context are enough to keep the CPU one submission ahead of the GPU
 * without unbounded memory; the ring slot is not reused until the context
 * fence proves the previous submission that used it has retired.
 */
#define AGX_D3DKMT_STAGING_RING 2

/* The process VA lease: the M5 plan's windows plus, on G17, the
 * driver-window guard reserved ahead of them. */
#define AGX_D3DKMT_MAX_VA_LEASE_RANGES (AGX_D3DKMT_MAX_VM_RESERVATIONS + 1u)

/* G17 driver-window guard: the 32 MiB head of the M5 driver window below
 * the range the KMD reserves per process (AppleAgxWddmProcess.c
 * AgxwG17ReserveDriverRange: [0x10_0200_0000, 0x18_0000_0000)). The head
 * holds user VA [0x10_0000_0000, 0x10_0040_0000) and the start of the KMD's
 * internal range, which the KMD only guards in its paging path. */
#define AGX_D3DKMT_M5_GUARD_BASE AGX_M5_DRIVER_BASE
#define AGX_D3DKMT_M5_GUARD_END UINT64_C(0x1002000000)

struct agx_d3dkmt_ctx {
   agx_kmt_handle hContext; /* 0 = free slot */

   /* Per-context completion fence. Owned by the winsys and separate from
    * anything Honeykrisp signals, so staging reuse never depends on the
    * caller's choice of out-syncs. */
   agx_kmt_handle fence;
   const volatile uint64_t *fence_cpu;
   uint64_t fence_next;
   bool submit_failed;
   bool completion_unknown;
   uint32_t submit_refs;

   struct agx_bo *staging[AGX_D3DKMT_STAGING_RING];
   uint64_t staging_retire[AGX_D3DKMT_STAGING_RING];
   unsigned staging_slot;
};

struct agx_d3dkmt_winsys {
   struct agx_d3dkmt_fns fns;
   struct agx_d3dkmt_policy policy;
   struct agx_kmt_luid luid;

   agx_kmt_handle adapter;
   agx_kmt_handle device;
   bool device_lease_held;

   /* Paging queue: every GPU VA and residency operation is asynchronous on
    * WDDM and completes on this queue's monitored fence. DRM's VM_BIND is
    * synchronous, so the winsys waits on it before returning -- see
    * d3dkmt_wait_paging_fence(). */
   agx_kmt_handle paging_queue;
   agx_kmt_handle paging_fence;
   const volatile uint64_t *paging_fence_cpu;

   /* UpdateGpuVirtualAddress is a rendering-context interlock, not a paging-
    * queue operation. Keep one ordinary WDDM virtual-addressing context and
    * its monitored fence alive for the device lifetime. Fence N is the value
    * the update waits for; successful completion signals N + 1. */
   agx_kmt_handle va_context;
   agx_kmt_handle va_fence;
   const volatile uint64_t *va_fence_cpu;
   uint64_t va_fence_next;

   simple_mtx_t lock; /* guards pool, syncs, sync_cache, ctxs and the staging rings */
   struct agx_d3dkmt_handle_pool pool;
   struct agx_d3dkmt_sync_table syncs;

   /* Recycled monitored-fence pool: avoids per-frame Create/DestroySyncObject
    * kernel transitions and VidMm GPU VA 0x10200 page-table map/unmap stalls. */
#define AGX_D3DKMT_SYNC_CACHE_SIZE 64u
   struct {
      agx_kmt_handle kmt;
      const volatile uint64_t *cpu_va;
      uint64_t min_ready;
   } sync_cache[AGX_D3DKMT_SYNC_CACHE_SIZE];
   uint32_t sync_cache_count;

   struct agx_d3dkmt_ctx *ctxs;
   uint32_t ctx_capacity;
   uint32_t active_submits;
   bool device_lost;

   /* Lazily created SynchronizationOnly context used to implement DRM
    * syncobj transfer as a GPU-side wait-then-signal. */
   agx_kmt_handle xfer_ctx;

   /* The user GPU VA window, reserved at VM_CREATE time. Up to two ranges:
    * CONTRACT.md SS4.2 puts a 16 KiB kernel/firmware carve-out page in the
    * MIDDLE of the window, so it is not one contiguous reservation. On G17
    * the driver-window guard comes first, then the M5 plan's windows. */
   struct agx_d3dkmt_va_range va[AGX_D3DKMT_MAX_VA_LEASE_RANGES];
   uint32_t va_count;

   /* G17 only (agx_chip_g17_m5): the head of the driver window,
    * [0x10_0000_0000, 0x10_0200_0000), is reserved NO_ACCESS before any
    * monitored fence exists, so VidMm places no fence page in the driver
    * window. Monitored fences are refused until this winsys holds the guard,
    * and VM_CREATE is refused once one exists without it. */
   bool m5_guard_required;
   bool m5_guard_reserved;
   uint32_t monitored_fences_created;

   /* KMTQAITYPE_UMDRIVERPRIVATE payload: the AGX params blob verbatim. */
   void *params;
   uint32_t params_size;

   /* Version-6 diagnostics to stderr (J813 first draw, round 3). Set only on a
    * device that negotiated the primary-write contract; nothing is logged and
    * nothing changes on any other device. Counters are updated atomically. */
   bool v6_log;
   uint32_t v6_submits;      /* submits seen (bounds the per-submit lines) */
   uint32_t v6_kinds_logged; /* AGX_D3DKMT_V6_* first-of-kind bits already logged */
};

/* First-of-kind refusal lines of the version-6 path. */
#define AGX_D3DKMT_V6_WRITE_SET   (1u << 0) /* the command's write set was incomplete */
#define AGX_D3DKMT_V6_READ_SET    (1u << 1) /* the read list could not be built (more than 256 ranges) */
#define AGX_D3DKMT_V6_VALIDATOR   (1u << 2) /* the shared validator refused the record */
#define AGX_D3DKMT_V6_SUBMIT      (1u << 3) /* SubmitCommand failed */
#define AGX_D3DKMT_V6_SIGNAL      (1u << 4) /* SignalSynchronizationObjectFromGpu failed */
#define AGX_D3DKMT_V6_GPU_WAIT    (1u << 5) /* WaitForSynchronizationObjectFromGpu failed */
#define AGX_D3DKMT_V6_FENCE_2S    (1u << 6) /* the first CPU fence wait over 2 s */

/* True exactly once per winsys and kind, and only on a v6 device. */
static inline bool
agx_d3dkmt_v6_first(struct agx_d3dkmt_winsys *ws, uint32_t kind)
{
   return ws->v6_log &&
          (__atomic_fetch_or(&ws->v6_kinds_logged, kind, __ATOMIC_RELAXED) & kind) == 0;
}

static inline void
agx_d3dkmt_mark_device_lost(struct agx_d3dkmt_winsys *ws,
                            struct agx_d3dkmt_ctx *ctx)
{
   simple_mtx_lock(&ws->lock);
   ws->device_lost = true;
   if (ctx != NULL)
      ctx->submit_failed = true;
   simple_mtx_unlock(&ws->lock);
}

static inline void
agx_d3dkmt_mark_completion_unknown(struct agx_d3dkmt_winsys *ws,
                                    struct agx_d3dkmt_ctx *ctx)
{
   simple_mtx_lock(&ws->lock);
   ws->device_lost = true;
   ctx->submit_failed = true;
   ctx->completion_unknown = true;
   simple_mtx_unlock(&ws->lock);
}

static inline struct agx_d3dkmt_winsys *
agx_d3dkmt_ws(struct agx_device *dev)
{
   return (struct agx_d3dkmt_winsys *)dev->hdev;
}

/*
 * Block until `fence_cpu` reaches `value`, or `timeout_ms` elapses. Infinite
 * waits use D3DKMT's synchronous form. Finite waits poll the required
 * CPU-visible monitored-fence mapping because D3DKMT documents no way to
 * cancel an outstanding asynchronous event wait.
 */
int agx_d3dkmt_wait_fence(struct agx_d3dkmt_winsys *ws, agx_kmt_handle fence,
                          const volatile uint64_t *fence_cpu, uint64_t value,
                          uint32_t timeout_ms);

int agx_d3dkmt_poll_cpu_fences(
   const volatile uint64_t *const *current, const uint64_t *wanted,
   uint32_t count, bool wait_any, uint32_t timeout_ms,
   uint32_t *first_signaled);

/* Wait for a paging-queue operation identified by `value` to complete. */
int agx_d3dkmt_wait_paging(struct agx_d3dkmt_winsys *ws, uint64_t value);

/* Every monitored fence the winsys creates goes through here. `create` is
 * filled in by the caller (hDevice, Info); on success it holds the handle
 * and both fence-value addresses. On G17 the fence is refused before this
 * winsys holds the driver-window guard, its FenceValueGPUVirtualAddress is
 * logged, and a GPU address inside the M5 driver window destroys the fence
 * and fails the call. Returns 0, or -1 with errno set. */
int agx_d3dkmt_create_monitored_fence(struct agx_d3dkmt_winsys *ws,
                                      struct agx_kmt_createsyncobj2 *create,
                                      const char *what);

/* Map an NTSTATUS onto the errno a drmIoctl() caller would have seen, and
 * store it in errno. Returns -1 so call sites can `return
 * agx_d3dkmt_fail(st);`. */
int agx_d3dkmt_fail(agx_kmt_status st);
