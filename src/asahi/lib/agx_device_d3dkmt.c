#include "agx_d3dkmt_bridge.h"
#include "agx_bridge_vma.h"
/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * agx_device_ops_t over D3DKMT. See agx_device_d3dkmt.h for why, and
 * agx_d3dkmt.h's CONTRACT ASSUMPTIONS block for every kernel-side guess this
 * file makes, tagged with the contract question it answers.
 *
 * Each op below is annotated with the AGX ioctl it replaces and with the one
 * thing that changes semantically, because in three places the change is real
 * rather than cosmetic:
 *
 *   VM_BIND    DRM binds synchronously; WDDM page-table updates complete on a
 *              paging fence. This file restores synchrony by waiting.
 *   SUBMIT     DRM submits waits, work and signals as one atomic ioctl; WDDM
 *              has no sync list on SubmitCommand and expresses the same thing
 *              as three ordered calls on one context.
 *   GEM handle DRM GEM handles are dense small integers; D3DKMT handles are
 *              not, and agx_device.c iterates the handle space.
 *
 * NOT RUNTIME-VERIFIED: there is no WDDM miniport for this GPU yet and no
 * hardware available this session. Everything here cross-compiles for
 * aarch64-w64-mingw32 as part of the real ICD; nothing here has executed.
 */

#include "agx_device_d3dkmt.h"
#include "agx_d3dkmt_priv.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winternl.h>

#include "util/macros.h"
#include "util/u_math.h"
#include "util/u_atomic.h"
#include "agx_bo.h"
#include "layout.h"

/* GPU virtual-address reservations are owned by dxgkrnl's process/adapter
 * address space, not by an individual D3DKMT device.  Vulkan keeps a
 * physical-device winsys alive while creating one or more logical-device
 * winsyses, so blindly reserving the fixed Honeykrisp layout for every
 * winsys collides with the first reservation.  Keep an exact process-local
 * lease keyed by adapter LUID and release it only after the last winsys.
 *
 * A partially failed last release is retained with a per-range mask.  The
 * next acquire repairs only the missing ranges before publishing a new
 * reference, so a FreeGpuVirtualAddress failure cannot turn into an ABA hole.
 */
struct agx_d3dkmt_va_lease {
   struct agx_kmt_luid luid;
   struct agx_d3dkmt_va_range ranges[AGX_D3DKMT_MAX_VA_LEASE_RANGES];
   uint32_t range_count;
   uint32_t reserved_mask;
   uint32_t references;
   struct agx_d3dkmt_va_lease *next;
};

struct agx_d3dkmt_device_lease {
   struct agx_kmt_luid luid;
   agx_kmt_handle device;
   uint32_t references;
   struct agx_d3dkmt_device_lease *next;
};

static SRWLOCK agx_d3dkmt_va_lease_lock = SRWLOCK_INIT;
static struct agx_d3dkmt_va_lease *agx_d3dkmt_va_leases;
static SRWLOCK agx_d3dkmt_device_lease_lock = SRWLOCK_INIT;
static struct agx_d3dkmt_device_lease *agx_d3dkmt_device_leases;

/* The mirrored constants in agx_d3dkmt_core.h must equal the real ones. This
 * is the tripwire that keeps the Mesa-free core honest. */
static_assert(AGX_D3DKMT_BIND_UNBIND == DRM_ASAHI_BIND_UNBIND, "BIND_UNBIND");
static_assert(AGX_D3DKMT_BIND_READ == DRM_ASAHI_BIND_READ, "BIND_READ");
static_assert(AGX_D3DKMT_BIND_WRITE == DRM_ASAHI_BIND_WRITE, "BIND_WRITE");
static_assert(AGX_D3DKMT_BIND_SINGLE_PAGE == DRM_ASAHI_BIND_SINGLE_PAGE,
              "BIND_SINGLE_PAGE");
static_assert(AGX_D3DKMT_BO_SHARED == AGX_BO_SHARED, "BO_SHARED");
static_assert(AGX_D3DKMT_BO_LOW_VA == AGX_BO_LOW_VA, "BO_LOW_VA");
static_assert(AGX_D3DKMT_BO_EXEC == AGX_BO_EXEC, "BO_EXEC");
static_assert(AGX_D3DKMT_BO_WRITEBACK == AGX_BO_WRITEBACK, "BO_WRITEBACK");
static_assert(AGX_D3DKMT_BO_SHAREABLE == AGX_BO_SHAREABLE, "BO_SHAREABLE");
static_assert(AGX_D3DKMT_BO_READONLY == AGX_BO_READONLY, "BO_READONLY");
static_assert(AGX_D3DKMT_PAGE_SIZE == AIL_PAGESIZE, "page size");
static_assert(AGX_D3DKMT_PARAMS_GLOBAL_SIZE ==
                 sizeof(struct drm_asahi_params_global),
              "GET_PARAMS escape payload size");
static_assert(AGX_D3DKMT_SUPPORTED_FEATURES == DRM_ASAHI_FEATURE_SOFT_FAULTS,
              "features the D3DKMT winsys carries");
static_assert(AGX_D3DKMT_G17_FEATURE_VM_STATUS == DRM_ASAHI_FEATURE_VM_STATUS,
              "G17 VM_STATUS feature bit");
static_assert(AGX_D3DKMT_PARAM_GROUP_VM_STATUS ==
                 DRM_ASAHI_PARAM_GROUP_VM_STATUS,
              "GET_PARAMS group VM_STATUS");
static_assert(sizeof(struct agx_d3dkmt_vm_status) ==
                 sizeof(struct drm_asahi_vm_status),
              "VM status escape payload is drm_asahi_vm_status");
static_assert(offsetof(struct agx_d3dkmt_vm_status, vm_id) ==
                 offsetof(struct drm_asahi_vm_status, vm_id) &&
              offsetof(struct agx_d3dkmt_vm_status, flags) ==
                 offsetof(struct drm_asahi_vm_status, flags) &&
              offsetof(struct agx_d3dkmt_vm_status, error) ==
                 offsetof(struct drm_asahi_vm_status, error),
              "VM status escape payload layout");
static_assert(AGX_D3DKMT_BO_SCANOUT_DONOR == AGX_BO_SCANOUT_DONOR,
              "BO_SCANOUT_DONOR");
static_assert(AGXW_GET_TIME_SIZE == sizeof(struct drm_asahi_get_time),
              "GET_TIME escape payload size");
static_assert((int)AGXW_SYNC_BINARY == (int)DRM_ASAHI_SYNC_SYNCOBJ,
              "SYNC_SYNCOBJ");
static_assert((int)AGXW_SYNC_TIMELINE == (int)DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ,
              "SYNC_TIMELINE_SYNCOBJ");
static_assert(sizeof(struct agxw_sync) == sizeof(struct drm_asahi_sync),
              "agxw_sync mirrors drm_asahi_sync");
static_assert((int)AGXW_PRIORITY_LOW == (int)DRM_ASAHI_PRIORITY_LOW,
              "priority LOW");
static_assert((int)AGXW_PRIORITY_REALTIME == (int)DRM_ASAHI_PRIORITY_REALTIME,
              "priority REALTIME");

/* -------------------------------------------------------------------------
 * Diagnostics.
 *
 * Every stderr line this winsys writes is flushed as it is written, so the
 * line survives a bugcheck that follows it; stderr's buffering is otherwise
 * whatever the application set (setvbuf). errno is kept for callers that
 * report it after logging.
 * ------------------------------------------------------------------------- */
static void PRINTFLIKE(1, 2)
d3dkmt_log_flushed(const char *fmt, ...)
{
   const int saved_errno = errno;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fflush(stderr);
   errno = saved_errno;
}

/* -------------------------------------------------------------------------
 * Error mapping and waits.
 * ------------------------------------------------------------------------- */
int
agx_d3dkmt_fail(agx_kmt_status st)
{
   switch ((uint32_t)st) {
   case 0xC0000017u: /* STATUS_NO_MEMORY */
   case 0xC000009Au: /* STATUS_INSUFFICIENT_RESOURCES */
      errno = ENOMEM;
      break;
   case 0xC000000Du: /* STATUS_INVALID_PARAMETER */
   case 0xC0000008u: /* STATUS_INVALID_HANDLE */
      errno = EINVAL;
      break;
   case 0xC0000022u: /* STATUS_ACCESS_DENIED */
      errno = EACCES;
      break;
   case 0x00000102u: /* STATUS_TIMEOUT */
      errno = ETIME;
      break;
   case 0xC00000BBu: /* STATUS_NOT_SUPPORTED */
      errno = ENOTSUP;
      break;
   case 0xC01E0116u: /* STATUS_GRAPHICS_ALLOCATION_INVALID */
   case 0xC01E0110u: /* STATUS_GRAPHICS_NO_VIDEO_MEMORY */
      errno = ENOMEM;
      break;
   default:
      errno = EIO;
      break;
   }

   return -1;
}

int
agx_d3dkmt_poll_cpu_fences(const volatile uint64_t *const *current,
                           const uint64_t *wanted, uint32_t count,
                           bool wait_any, uint32_t timeout_ms,
                           uint32_t *first_signaled)
{
   if (timeout_ms == AGX_D3DKMT_INFINITE) {
      errno = EINVAL;
      return -1;
   }

   ULONGLONG start = GetTickCount64();
   for (;;) {
      enum agx_d3dkmt_cpu_wait_state state = agx_d3dkmt_cpu_wait_probe(
         current, wanted, count, wait_any, first_signaled);
      if (state == AGX_D3DKMT_CPU_WAIT_SATISFIED) {
         MemoryBarrier();
         return 0;
      }
      if (state == AGX_D3DKMT_CPU_WAIT_INVALID) {
         errno = EIO;
         return -1;
      }

      if (GetTickCount64() - start >= timeout_ms) {
         errno = ETIME;
         return -1;
      }

      if (!SwitchToThread())
         Sleep(1);
   }
}

int
agx_d3dkmt_wait_fence(struct agx_d3dkmt_winsys *ws, agx_kmt_handle fence,
                      const volatile uint64_t *fence_cpu, uint64_t value,
                      uint32_t timeout_ms)
{
   const agx_kmt_handle handles[1] = {fence};
   const uint64_t values[1] = {value};
   const volatile uint64_t *current[1] = {fence_cpu};

   enum agx_d3dkmt_cpu_wait_state state =
      agx_d3dkmt_cpu_wait_probe(current, values, 1, true, NULL);
   if (state == AGX_D3DKMT_CPU_WAIT_SATISFIED) {
      MemoryBarrier();
      return 0;
   }
   if (state == AGX_D3DKMT_CPU_WAIT_INVALID) {
      errno = EIO;
      return -1;
   }

   if (timeout_ms != AGX_D3DKMT_INFINITE)
      return agx_d3dkmt_poll_cpu_fences(current, values, 1, true, timeout_ms,
                                        NULL);

   struct agx_kmt_waitsyncfromcpu wait = {
      .hDevice = ws->device,
      .ObjectCount = 1,
      .ObjectHandleArray = handles,
      .FenceValueArray = values,
      .hAsyncEvent = NULL,
   };

   /*
    * D3DKMTWaitForSynchronizationObjectFromCpu has no timeout parameter at
    * all. Its NULL-event form is a synchronous, per-call wait. The event form
    * is intentionally unused: there is no documented cancellation API, so an
    * event cannot safely be closed or reused after a caller-side timeout.
    */
   agx_kmt_status st = ws->fns.WaitForSynchronizationObjectFromCpu(&wait);
   if (!AGX_KMT_SUCCESS(st))
      return agx_d3dkmt_fail(st);

   MemoryBarrier();
   if (agx_d3dkmt_cpu_wait_probe(current, values, 1, true, NULL) ==
       AGX_D3DKMT_CPU_WAIT_SATISFIED)
      return 0;

   errno = EIO;
   return -1;
}

int
agx_d3dkmt_wait_paging(struct agx_d3dkmt_winsys *ws, uint64_t value)
{
   if (value == 0)
      return 0;

   /* Fast path: the paging fence has a CPU-visible value, so most waits cost
    * a load rather than a kernel transition. */
   if (ws->paging_fence_cpu != NULL && *ws->paging_fence_cpu >= value)
      return 0;

   return agx_d3dkmt_wait_fence(ws, ws->paging_fence, ws->paging_fence_cpu,
                                value,
                                AGX_D3DKMT_INFINITE);
}

/* -------------------------------------------------------------------------
 * Monitored fences.
 *
 * VidMm maps each monitored fence's value page into the process GPU VA at an
 * address it picks itself (FenceValueGPUVirtualAddress), at the first free
 * address it finds. On G17 that must never be the driver window
 * [0x10_0000_0000, 0x18_0000_0000): the KMD reserves all of it per process
 * except the 32 MiB head, which d3dkmt_vm_create() reserves before any fence
 * exists (the guard). The KMD paging guard stays the enforcement; this is the
 * winsys's own fail-closed check and its log of where every page went.
 * ------------------------------------------------------------------------- */
int
agx_d3dkmt_create_monitored_fence(struct agx_d3dkmt_winsys *ws,
                                  struct agx_kmt_createsyncobj2 *create,
                                  const char *what)
{
   if (ws->m5_guard_required && !ws->m5_guard_reserved) {
      d3dkmt_log_flushed(
              "D3DKMT: refusing monitored fence (%s) before the G17 "
              "driver-window guard [0x%llx, 0x%llx) is reserved\n",
              what, (unsigned long long)AGX_M5_DRIVER_BASE,
              (unsigned long long)AGX_D3DKMT_M5_GUARD_END);
      errno = EINVAL;
      return -1;
   }

   agx_kmt_status st = ws->fns.CreateSynchronizationObject2(create);
   if (!AGX_KMT_SUCCESS(st))
      return agx_d3dkmt_fail(st);

   p_atomic_inc(&ws->monitored_fences_created);
   if (!ws->m5_guard_required)
      return 0;

   const uint64_t gpu_va = create->Info.MonitoredFence.FenceValueGPUVirtualAddress;
   const bool in_driver_window =
      gpu_va >= AGX_M5_DRIVER_BASE &&
      gpu_va - AGX_M5_DRIVER_BASE < AGX_M5_DRIVER_SIZE;

   d3dkmt_log_flushed(
           "D3DKMT: monitored fence (%s) 0x%08x GPU VA 0x%llx%s\n", what,
           (unsigned)create->hSyncObject, (unsigned long long)gpu_va,
           in_driver_window ? " INSIDE the driver window: refused" : "");
   if (in_driver_window) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = create->hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      create->hSyncObject = 0;
      errno = EFAULT;
      return -1;
   }

   return 0;
}

/* -------------------------------------------------------------------------
 * Contexts.
 * ------------------------------------------------------------------------- */
static struct agx_d3dkmt_ctx *
ctx_get(struct agx_d3dkmt_winsys *ws, uint32_t queue_id)
{
   if (queue_id == 0 || queue_id > ws->ctx_capacity)
      return NULL;

   struct agx_d3dkmt_ctx *ctx = &ws->ctxs[queue_id - 1];
   return ctx->hContext ? ctx : NULL;
}

/* -------------------------------------------------------------------------
 * GEM_CREATE / GEM_CLOSE.
 * ------------------------------------------------------------------------- */
static void
d3dkmt_destroy_record(struct agx_d3dkmt_winsys *ws,
                      const struct agx_d3dkmt_allocation_record *record)
{
   if (record == NULL ||
       (record->allocation == 0 && record->resource == 0))
      return;

   /* The outer D3D runtime owns same-process imports. This winsys owns only
    * the BO wrapper and GPUVA binding, so every unwind/free path must stop
    * before DestroyAllocation2. */
   if (record->borrowed)
      return;

   const agx_kmt_handle list[1] = {record->allocation};
   struct agx_kmt_destroyallocation2 destroy = {
      .hDevice = ws->device,
      .hResource = record->resource,
      .phAllocationList = record->resource ? NULL : list,
      .AllocationCount = record->resource ? 0 : 1,
   };

   agx_kmt_status st = ws->fns.DestroyAllocation2(&destroy);
   if (!AGX_KMT_SUCCESS(st))
      agx_d3dkmt_fail(st);
}

static bool
d3dkmt_make_resident(struct agx_d3dkmt_winsys *ws,
                     agx_kmt_handle allocation)
{
   if (!ws->policy.needs_residency)
      return true;

   const agx_kmt_handle list[1] = {allocation};
   struct agx_kmt_makeresident mr = {
      .hPagingQueue = ws->paging_queue,
      .NumAllocations = 1,
      .AllocationList = list,
   };

   agx_kmt_status st = ws->fns.MakeResident(&mr);
   if (!AGX_KMT_SUCCESS(st)) {
      /* Loud on purpose, matching the UpdateGpuVirtualAddress diagnostic
       * below: MakeResident was the only kernel refusal on this path that
       * failed without naming itself, which cost a hardware cycle. A
       * 0xC000000D here for an allocation this winsys did not create is
       * dxgkrnl's per-device handle scoping, not a KMD or VidMm failure. */
      d3dkmt_log_flushed(
              "D3DKMTMakeResident failed status=0x%08x paging_queue=0x%08x "
              "allocation=0x%08x\n",
              (unsigned)st, (unsigned)ws->paging_queue,
              (unsigned)allocation);
      agx_d3dkmt_fail(st);
      return false;
   }

   return agx_d3dkmt_wait_paging(ws, mr.PagingFenceValue) == 0;
}

static bool
d3dkmt_create_allocation(struct agx_d3dkmt_winsys *ws, uint64_t size,
                         enum agx_bo_flags flags,
                         enum agx_d3dkmt_external_memory_type external_type,
                         struct agx_d3dkmt_allocation_record *record)
{
   const uint64_t allocation_granularity = UINT64_C(0x10000);
   if (size == 0 || size > UINT64_MAX - (allocation_granularity - 1)) {
      errno = EINVAL;
      return false;
   }
   size = ALIGN_POT(size, allocation_granularity);

   /*
    * CONTRACT.md SS12.1: the private-data size is pinned, and treated as a
    * memory-safety property rather than a convenience, because the D3D
    * runtime does not validate what the UMD claims against what the kernel
    * reads. Assert the width; never infer it.
    */
   struct agxw_alloc_private priv = {
      .version = AGXW_ALLOC_PRIVATE_VERSION,
      .struct_size = sizeof(priv),
      .size = size,
      .flags = agx_d3dkmt_agxw_alloc_flags((uint32_t)flags),
      .align_log2 = 16,
   };
   static_assert(sizeof(priv) == AGXW_ALLOC_PRIVATE_SIZE,
                 "allocation private data width");

   struct agx_kmt_allocationinfo2 info = {
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };

   /* Honeykrisp has no resource/allocation split at this layer, so the
    * resource-level blob is absent -- explicitly legal (CONTRACT.md SS3.2). */
   struct agx_kmt_createallocation create = {
      .hDevice = ws->device,
      .NumAllocations = 1,
      .pAllocationInfo2 = &info,
      .Flags = agx_d3dkmt_create_flags_external((uint32_t)flags,
                                                external_type),
   };

   agx_kmt_status st = ws->fns.CreateAllocation2(&create);
   if (!AGX_KMT_SUCCESS(st)) {
      d3dkmt_log_flushed(
         "D3DKMTCreateAllocation2 fail: st=0x%08x size=0x%llx bo_flags=0x%x "
         "priv_flags=0x%x ext=%d create_flags=0x%x\n",
         (unsigned)st, (unsigned long long)size, (unsigned)flags,
         (unsigned)priv.flags, (int)external_type, (unsigned)create.Flags);
      agx_d3dkmt_fail(st);
      return false;
   }

   *record = (struct agx_d3dkmt_allocation_record){
      .allocation = info.hAllocation,
      .resource = create.hResource,
      .global_share = create.hGlobalShare,
      .external_type = external_type,
   };

   bool expected_resource =
      external_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
   bool expected_global =
      external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT;
   /* Bridge-mode export degrade (degraded-create tolerance): an exportable
    * create with NO Triton resource handoff pending is converted by the
    * bridge into a plain VM-private allocation rather than refused
    * (0.1.339: 475x REFUSED no-handoff was every compositor allocation
    * failure). The record is then legitimately resourceless; accept it and
    * drop the external type so a later export attempt fails cleanly at
    * bo_export (record.resource == 0) instead of here. Non-bridge builds
    * keep the strict shape check. */
   if (agxw_bridge_is_sentinel(ws->device) && expected_resource &&
       record->allocation != 0 && record->resource == 0 &&
       record->global_share == 0) {
      record->external_type = AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
      expected_resource = false;
      expected_global = false;
   }
   if (record->allocation == 0 || (!!record->resource != expected_resource) ||
       (!!record->global_share != expected_global)) {
      d3dkmt_log_flushed(
         "D3DKMTCreateAllocation2 shape fail: size=0x%llx ext=%d alloc=0x%08x "
         "res=0x%08x(want=%d) global=0x%08x(want=%d) dev=0x%08x\n",
         (unsigned long long)size, (int)external_type,
         (unsigned)record->allocation, (unsigned)record->resource,
         expected_resource ? 1 : 0, (unsigned)record->global_share,
         expected_global ? 1 : 0, (unsigned)ws->device);
      errno = EIO;
      d3dkmt_destroy_record(ws, record);
      memset(record, 0, sizeof(*record));
      return false;
   }

   if (!d3dkmt_make_resident(ws, record->allocation)) {
      d3dkmt_destroy_record(ws, record);
      memset(record, 0, sizeof(*record));
      return false;
   }

   return true;
}

static struct agx_bo *
d3dkmt_adopt_allocation(
   struct agx_device *dev, size_t size, size_t align, enum agx_bo_flags flags,
   const struct agx_d3dkmt_allocation_record *record)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_bo *bo;

   simple_mtx_lock(&ws->lock);
   uint32_t handle = agx_d3dkmt_pool_alloc_record(&ws->pool, record);
   simple_mtx_unlock(&ws->lock);

   if (handle == 0) {
      d3dkmt_destroy_record(ws, record);
      return NULL;
   }

   simple_mtx_lock(&dev->bo_map_lock);
   bo = agx_lookup_bo(dev, handle);
   dev->max_handle = MAX2(dev->max_handle, handle);
   simple_mtx_unlock(&dev->bo_map_lock);

   /* util_sparse_array_get grows its node tree on demand and returns NULL on
    * allocation failure; release builds compile the zero-slot assert out, so
    * without this guard the field writes below would be the process's first
    * dereference of NULL (adopt-null-bo hardening). Fail cleanly instead. */
   if (bo == NULL) {
      simple_mtx_lock(&ws->lock);
      struct agx_d3dkmt_allocation_record released;
      bool found =
         agx_d3dkmt_pool_release_record(&ws->pool, handle, &released);
      simple_mtx_unlock(&ws->lock);
      if (found)
         d3dkmt_destroy_record(ws, &released);
      d3dkmt_log_flushed("d3dkmt_adopt_allocation: BO map slot unavailable\n");
      return NULL;
   }

   assert(!memcmp(bo, &((struct agx_bo){}), sizeof(*bo)));
   if (record->cpu_mapping_forbidden)
      flags |= AGX_BO_NO_CPU_MAP;
   bo->dev = dev;
   bo->size = size;
   bo->align = align;
   bo->flags = flags;
   bo->handle = handle;
   bo->uapi_handle = record->allocation;
   bo->prime_fd = -1;

   enum agx_va_flags va_flags = flags & AGX_BO_LOW_VA ? AGX_VA_USC : 0;
   bo->va = agx_va_alloc(dev, size, bo->align, va_flags, 0);
   if (!bo->va) {
      d3dkmt_log_flushed("Failed to allocate BO VMA\n");
      agx_bo_free(dev, bo);
      return NULL;
   }

   uint32_t bind = DRM_ASAHI_BIND_READ;
   if (!(flags & AGX_BO_READONLY))
      bind |= DRM_ASAHI_BIND_WRITE;

   if (agx_bo_bind(dev, bo, bo->va->addr, bo->size, 0, bind)) {
      agx_bo_free(dev, bo);
      return NULL;
   }

   return bo;
}

/*
 * Clone of agx_bo_alloc()/win_bo_alloc() with GEM_CREATE replaced by
 * D3DKMTCreateAllocation2. The handle split is the only structural change:
 * bo->handle stays a dense index into dev->bo_map (see agx_d3dkmt_core.h),
 * bo->uapi_handle carries the D3DKMT allocation handle that every kernel-
 * facing call site already passes.
 */
static int d3dkmt_query_allocation(struct agx_d3dkmt_winsys *ws,
                                   agx_kmt_handle allocation,
                                   struct agx_d3dkmt_allocation_info *info);

static struct agx_bo *
d3dkmt_bo_alloc(struct agx_device *dev, size_t size, size_t align,
                enum agx_bo_flags flags)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   /* executable implies low va */
   assert(!(flags & AGX_BO_EXEC) || (flags & AGX_BO_LOW_VA));
   flags = (enum agx_bo_flags)agx_d3dkmt_cpu_visible_flags((uint32_t)flags);

   struct agx_d3dkmt_allocation_record record;
   enum agx_d3dkmt_external_memory_type external_type =
      (flags & AGX_BO_SHAREABLE)
         ? AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32
         : AGX_D3DKMT_EXTERNAL_MEMORY_NONE;
   if (!d3dkmt_create_allocation(ws, size, flags,
                                 external_type, &record)) {
      d3dkmt_log_flushed("D3DKMTCreateAllocation2 failed\n");
      return NULL;
   }

   if (external_type != AGX_D3DKMT_EXTERNAL_MEMORY_NONE) {
      struct agx_d3dkmt_allocation_info kmd;
      if (d3dkmt_query_allocation(ws, record.allocation, &kmd) == 0) {
         if (kmd.flags & (AGXW_ALLOC_PRIMARY | AGXW_ALLOC_DIRECT_SCANOUT))
            flags |= AGX_BO_PRIMARY;
         if (kmd.flags & AGXW_ALLOC_NO_CPU_MAP)
            record.cpu_mapping_forbidden = true;
      }
   }

   struct agx_bo *bo =
      d3dkmt_adopt_allocation(dev, size, align, flags, &record);
   return bo;
}

/*
 * GEM_CLOSE. Takes the DENSE handle, because agx_bo_free() memsets struct
 * agx_bo to zero before calling this (agx_device.c:100-105, deliberately, to
 * avoid a lookup race) -- so bo->uapi_handle is already gone and the pool is
 * the only remaining way to recover the D3DKMT handle.
 */
static void
d3dkmt_bo_free(struct agx_device *dev, uint32_t handle)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_allocation_record record;
   bool found = agx_d3dkmt_pool_release_record(&ws->pool, handle, &record);
   simple_mtx_unlock(&ws->lock);

   if (!found)
      return;

   d3dkmt_destroy_record(ws, &record);
}

/* A shared resource opened here may have been created by the outer D3D
 * runtime (pfnAllocateCb with an hResource) or by the OS as a standard
 * allocation. Both attach blobs Honeykrisp never consumes: the runtime's
 * resource metadata and the UMD/KMD resource-level driver blob. They must be
 * received (dxgkrnl requires the open buffers to match the queried sizes
 * exactly) but carry no meaning here, so they are size-bounded and dropped.
 * The load-bearing validation stays the KMD's pinned 32-byte AGXW allocation
 * blob, checked in d3dkmt_external_open(). */
#define AGX_D3DKMT_FOREIGN_BLOB_MAX 4096u

static int d3dkmt_query_allocation(struct agx_d3dkmt_winsys *ws,
                                   agx_kmt_handle allocation,
                                   struct agx_d3dkmt_allocation_info *info);

struct agx_d3dkmt_external_sizes {
   uint32_t runtime_size;
   uint32_t resource_driver_size;
};

static bool
d3dkmt_external_query(struct agx_d3dkmt_winsys *ws, uintptr_t handle,
                      enum agx_d3dkmt_external_memory_type external_type,
                      struct agx_d3dkmt_external_sizes *sizes)
{
   uint32_t runtime_size = 0;
   uint32_t total_driver_size = 0;
   uint32_t resource_driver_size = 0;
   uint32_t allocation_count = 0;
   agx_kmt_status st;

   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32) {
      if (handle == 0)
         return false;
      struct agx_kmt_queryresourceinfofromnthandle query = {
         .hDevice = ws->device,
         .hNtHandle = (void *)handle,
      };
      st = ws->fns.QueryResourceInfoFromNtHandle(&query);
      runtime_size = query.PrivateRuntimeDataSize;
      total_driver_size = query.TotalPrivateDriverDataSize;
      resource_driver_size = query.ResourcePrivateDriverDataSize;
      allocation_count = query.NumAllocations;
   } else if (external_type ==
              AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT) {
      if (handle == 0 || handle > UINT32_MAX)
         return false;
      struct agx_kmt_queryresourceinfo query = {
         .hDevice = ws->device,
         .hGlobalShare = (agx_kmt_handle)handle,
      };
      st = ws->fns.QueryResourceInfo(&query);
      runtime_size = query.PrivateRuntimeDataSize;
      total_driver_size = query.TotalPrivateDriverDataSize;
      resource_driver_size = query.ResourcePrivateDriverDataSize;
      allocation_count = query.NumAllocations;
   } else {
      errno = EINVAL;
      return false;
   }

   if (!AGX_KMT_SUCCESS(st)) {
      agx_d3dkmt_fail(st);
      return false;
   }

   /* This miniport's source-pinned contract is one allocation per resource
    * carrying exactly one AGXW allocation blob. Runtime and resource-level
    * blobs are tolerated within a hard bound so that D3D-runtime-created and
    * standard-allocation shared resources can be adopted; their content is
    * never interpreted. Refuse anything else before supplying output buffers
    * to OpenResource. */
   if (allocation_count != AGXW_MAX_ALLOCATIONS_PER_RESOURCE ||
       runtime_size > AGX_D3DKMT_FOREIGN_BLOB_MAX ||
       resource_driver_size > AGX_D3DKMT_FOREIGN_BLOB_MAX ||
       total_driver_size != AGXW_ALLOC_PRIVATE_SIZE) {
      errno = EINVAL;
      return false;
   }

   sizes->runtime_size = runtime_size;
   sizes->resource_driver_size = resource_driver_size;
   return true;
}

static bool
d3dkmt_external_open(
   struct agx_d3dkmt_winsys *ws, uintptr_t handle,
   enum agx_d3dkmt_external_memory_type external_type,
   struct agx_d3dkmt_allocation_record *record,
   struct agxw_alloc_private *private_data)
{
   struct agx_d3dkmt_external_sizes sizes = {0};
   if (!d3dkmt_external_query(ws, handle, external_type, &sizes))
      return false;

   uint8_t total_driver_data[AGXW_ALLOC_PRIVATE_SIZE] = {0};
   struct agx_kmt_openallocationinfo2 info = {0};
   agx_kmt_handle resource = 0;
   agx_kmt_status st;

   /* dxgkrnl refuses an open whose receive buffers do not match the queried
    * blob sizes, so the foreign blobs are received into bounded scratch and
    * then discarded unread. Heap, not stack: this runs inside
    * vkAllocateMemory, and DWM is known to call in on threads whose stacks
    * cannot take multi-KiB frames (see 0054-heap-agx-parallel-copy-context).
    */
   uint8_t *foreign_blobs = NULL;
   if (sizes.runtime_size > 0 || sizes.resource_driver_size > 0) {
      foreign_blobs = calloc(
         1, (size_t)sizes.runtime_size + sizes.resource_driver_size);
      if (foreign_blobs == NULL) {
         errno = ENOMEM;
         return false;
      }
   }
   void *runtime_buf = sizes.runtime_size ? foreign_blobs : NULL;
   void *resource_buf =
      sizes.resource_driver_size ? foreign_blobs + sizes.runtime_size : NULL;

   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32) {
      struct agx_kmt_openresourcefromnthandle open = {
         .hDevice = ws->device,
         .hNtHandle = (void *)handle,
         .NumAllocations = AGXW_MAX_ALLOCATIONS_PER_RESOURCE,
         .pOpenAllocationInfo2 = &info,
         .PrivateRuntimeDataSize = sizes.runtime_size,
         .pPrivateRuntimeData = runtime_buf,
         .ResourcePrivateDriverDataSize = sizes.resource_driver_size,
         .pResourcePrivateDriverData = resource_buf,
         .TotalPrivateDriverDataBufferSize = sizeof(total_driver_data),
         .pTotalPrivateDriverDataBuffer = total_driver_data,
      };
      st = ws->fns.OpenResourceFromNtHandle(&open);
      resource = open.hResource;
      if (AGX_KMT_SUCCESS(st) &&
          (open.hKeyedMutex != 0 || open.hSyncObject != 0)) {
         errno = ENOTSUP;
         st = (agx_kmt_status)0xc00000bbu; /* STATUS_NOT_SUPPORTED */
      }
   } else {
      struct agx_kmt_openresource open = {
         .hDevice = ws->device,
         .hGlobalShare = (agx_kmt_handle)handle,
         .NumAllocations = AGXW_MAX_ALLOCATIONS_PER_RESOURCE,
         .pOpenAllocationInfo2 = &info,
         .pPrivateRuntimeData = runtime_buf,
         .PrivateRuntimeDataSize = sizes.runtime_size,
         .pResourcePrivateDriverData = resource_buf,
         .ResourcePrivateDriverDataSize = sizes.resource_driver_size,
         .pTotalPrivateDriverDataBuffer = total_driver_data,
         .TotalPrivateDriverDataBufferSize = sizeof(total_driver_data),
      };
      st = ws->fns.OpenResource(&open);
      resource = open.hResource;
   }

   free(foreign_blobs);

   if (!AGX_KMT_SUCCESS(st)) {
      d3dkmt_log_flushed(
              "D3DKMT external open failed status=0x%08x type=%d "
              "runtime_size=%u resource_driver_size=%u\n",
              (unsigned)st, (int)external_type, sizes.runtime_size,
              sizes.resource_driver_size);
      if (resource != 0) {
         const struct agx_d3dkmt_allocation_record opened = {
            .allocation = info.hAllocation,
            .resource = resource,
         };
         d3dkmt_destroy_record(ws, &opened);
      }
      agx_d3dkmt_fail(st);
      return false;
   }

   const uintptr_t data_begin = (uintptr_t)total_driver_data;
   const uintptr_t data_end = data_begin + sizeof(total_driver_data);
   const uintptr_t private_begin = (uintptr_t)info.pPrivateDriverData;
   if (resource == 0 || info.hAllocation == 0 ||
       info.PrivateDriverDataSize != AGXW_ALLOC_PRIVATE_SIZE ||
       private_begin < data_begin || private_begin > data_end ||
       AGXW_ALLOC_PRIVATE_SIZE > data_end - private_begin) {
      errno = EINVAL;
      const struct agx_d3dkmt_allocation_record opened = {
         .allocation = info.hAllocation,
         .resource = resource,
      };
      d3dkmt_destroy_record(ws, &opened);
      return false;
   }

   memcpy(private_data, info.pPrivateDriverData, sizeof(*private_data));
   if (agxw_alloc_private_validate(private_data, sizeof(*private_data)) != 0 ||
       !(private_data->flags & AGXW_ALLOC_SHAREABLE) ||
       (private_data->flags & AGXW_ALLOC_VM_PRIVATE) ||
       private_data->size == 0 ||
       private_data->size > SIZE_MAX - (AIL_PAGESIZE - 1)) {
      errno = EINVAL;
      const struct agx_d3dkmt_allocation_record opened = {
         .allocation = info.hAllocation,
         .resource = resource,
      };
      d3dkmt_destroy_record(ws, &opened);
      return false;
   }

   *record = (struct agx_d3dkmt_allocation_record){
      .allocation = info.hAllocation,
      .resource = resource,
      .global_share =
         external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT
            ? (agx_kmt_handle)handle
            : 0,
      .external_type = external_type,
   };

   if (!d3dkmt_make_resident(ws, record->allocation)) {
      d3dkmt_destroy_record(ws, record);
      memset(record, 0, sizeof(*record));
      return false;
   }

   return true;
}

struct agx_bo *
agx_d3dkmt_bo_create_external(
   struct agx_device *dev, size_t size, unsigned align,
   enum agx_bo_flags flags,
   enum agx_d3dkmt_external_memory_type external_type, const char *label)
{
   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_NONE ||
       external_type > AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT ||
       size == 0 || size > SIZE_MAX - (AIL_PAGESIZE - 1)) {
      errno = EINVAL;
      return NULL;
   }

   size = ALIGN_POT(size, AIL_PAGESIZE);
   align = MAX2(align, AIL_PAGESIZE);
   flags |= AGX_BO_SHARED | AGX_BO_SHAREABLE;
   flags = (enum agx_bo_flags)agx_d3dkmt_cpu_visible_flags((uint32_t)flags);

   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_allocation_record record;
   if (!d3dkmt_create_allocation(ws, size, flags, external_type, &record))
      return NULL;

   {
      struct agx_d3dkmt_allocation_info kmd;
      if (d3dkmt_query_allocation(ws, record.allocation, &kmd) == 0) {
         if (kmd.flags & (AGXW_ALLOC_PRIMARY | AGXW_ALLOC_DIRECT_SCANOUT))
            flags |= AGX_BO_PRIMARY;
         if (kmd.flags & AGXW_ALLOC_NO_CPU_MAP)
            record.cpu_mapping_forbidden = true;
      }
   }

   struct agx_bo *bo =
      d3dkmt_adopt_allocation(dev, size, align, flags, &record);
   if (bo != NULL) {
      bo->label = label;
      p_atomic_set(&bo->refcnt, 1);
   }
   return bo;
}

struct agx_bo *
agx_d3dkmt_bo_import_external(
   struct agx_device *dev, uintptr_t handle,
   enum agx_d3dkmt_external_memory_type external_type, const char *label)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_allocation_record record;
   struct agxw_alloc_private private_data;
   if (!d3dkmt_external_open(ws, handle, external_type, &record,
                             &private_data))
      return NULL;

   enum agx_bo_flags flags = AGX_BO_SHARED | AGX_BO_SHAREABLE;
   if (private_data.flags & AGXW_ALLOC_WRITEBACK)
      flags |= AGX_BO_WRITEBACK;

   /* private_data.size is the sharer's claim. Size the BO, its CPU lock and
    * its GPU VA only from a claim the KMD's own allocation confirms. */
   struct agx_d3dkmt_allocation_info kmd;
   uint64_t checked_size = 0;
   if (d3dkmt_query_allocation(ws, record.allocation, &kmd) != 0 ||
       agx_d3dkmt_import_size(&private_data, &kmd, record.allocation,
                              &checked_size) != AGX_D3DKMT_OK ||
       checked_size > SIZE_MAX) {
      d3dkmt_log_flushed(
              "D3DKMT import refused: allocation=0x%08x claimed=%llu "
              "kmd_requested=%llu kmd_size=%llu\n",
              (unsigned)record.allocation,
              (unsigned long long)private_data.size,
              (unsigned long long)kmd.requested_size,
              (unsigned long long)kmd.allocation_size);
      d3dkmt_destroy_record(ws, &record);
      errno = EINVAL;
      return NULL;
   }
   const size_t size = (size_t)checked_size;
   record.cpu_mapping_forbidden =
      (kmd.flags & AGXW_ALLOC_NO_CPU_MAP) != 0;
   if (kmd.flags & (AGXW_ALLOC_PRIMARY | AGXW_ALLOC_DIRECT_SCANOUT))
      flags |= AGX_BO_PRIMARY;

   struct agx_bo *bo =
      d3dkmt_adopt_allocation(dev, size, AIL_PAGESIZE, flags, &record);
   if (bo != NULL) {
      bo->label = label;
      p_atomic_set(&bo->refcnt, 1);
   }
   return bo;
}

struct agx_bo *
agx_d3dkmt_bo_import_local(struct agx_device *dev,
                           agx_kmt_handle allocation, size_t size,
                           const char *label)
{
   if (allocation == 0 || size == 0 ||
       size > SIZE_MAX - (AIL_PAGESIZE - 1)) {
      errno = EINVAL;
      return NULL;
   }

   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_allocation_record record = {
      .allocation = allocation,
      .external_type = AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT,
      .borrowed = true,
   };

   /* The only size claim here is the application's allocationSize for a
    * foreign allocation. It must fit inside what the KMD allocated. */
   struct agx_d3dkmt_allocation_info kmd;
   uint64_t checked_size = 0;
   if (d3dkmt_query_allocation(ws, allocation, &kmd) != 0 ||
       agx_d3dkmt_borrowed_import_size(size, &kmd, allocation,
                                       &checked_size) != AGX_D3DKMT_OK ||
       checked_size > SIZE_MAX) {
      d3dkmt_log_flushed(
              "borrowed local import refused: allocation=0x%08x claimed=%zu "
              "kmd_size=%llu\n",
              (unsigned)allocation, size,
              (unsigned long long)kmd.allocation_size);
      errno = EINVAL;
      return NULL;
   }
   size = (size_t)checked_size;
   record.cpu_mapping_forbidden =
      (kmd.flags & AGXW_ALLOC_NO_CPU_MAP) != 0;

   if (!d3dkmt_make_resident(ws, allocation)) {
      d3dkmt_log_flushed(
              "borrowed local import: MakeResident refused foreign "
              "allocation=0x%08x errno=%d\n",
              (unsigned)allocation, errno);
      return NULL;
   }

   enum agx_bo_flags flags = AGX_BO_SHARED | AGX_BO_SHAREABLE;
   if (kmd.flags & (AGXW_ALLOC_PRIMARY | AGXW_ALLOC_DIRECT_SCANOUT))
      flags |= AGX_BO_PRIMARY;
   struct agx_bo *bo = d3dkmt_adopt_allocation(
      dev, size, AIL_PAGESIZE, flags, &record);
   if (bo == NULL) {
      d3dkmt_log_flushed(
              "borrowed local import: adopt/bind failed for foreign "
              "allocation=0x%08x errno=%d\n",
              (unsigned)allocation, errno);
   }
   if (bo != NULL) {
      bo->label = label;
      p_atomic_set(&bo->refcnt, 1);
   }
   return bo;
}

int
agx_d3dkmt_bo_export_external(
   struct agx_device *dev, struct agx_bo *bo,
   enum agx_d3dkmt_external_memory_type external_type, uintptr_t *handle)
{
   if (bo == NULL || handle == NULL ||
       external_type == AGX_D3DKMT_EXTERNAL_MEMORY_NONE ||
       external_type > AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT) {
      errno = EINVAL;
      return -1;
   }

   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_allocation_record record;
   simple_mtx_lock(&ws->lock);
   bool found =
      agx_d3dkmt_pool_get_record(&ws->pool, bo->handle, &record);
   simple_mtx_unlock(&ws->lock);
   if (!found || record.allocation != bo->uapi_handle ||
       record.external_type != external_type || record.resource == 0) {
      errno = EINVAL;
      return -1;
   }

   if (external_type == AGX_D3DKMT_EXTERNAL_MEMORY_OPAQUE_WIN32_KMT) {
      if (record.global_share == 0) {
         errno = EINVAL;
         return -1;
      }
      /* Non-owning by Vulkan definition: return the stable identifier itself,
       * not a duplicated/closable kernel handle. */
      *handle = (uintptr_t)record.global_share;
      return 0;
   }

   OBJECT_ATTRIBUTES attributes;
   InitializeObjectAttributes(&attributes, NULL, 0, NULL, NULL);
   void *nt_handle = NULL;
   agx_kmt_status st = ws->fns.ShareObjects(
      1, &record.resource, &attributes,
      AGX_KMT_SHARED_ALLOCATION_ALL_ACCESS, &nt_handle);
   if (!AGX_KMT_SUCCESS(st) || nt_handle == NULL) {
      if (AGX_KMT_SUCCESS(st))
         errno = EIO;
      else
         agx_d3dkmt_fail(st);
      return -1;
   }

   /* The caller owns this NT handle and closes it. The BO retains no copy. */
   *handle = (uintptr_t)nt_handle;
   return 0;
}

/* -------------------------------------------------------------------------
 * VM_BIND.
 * ------------------------------------------------------------------------- */
static bool enum_log_enabled(void);

/*
 * This path runs only for an invariant that would otherwise cross into VidMm
 * and can bugcheck the machine.  Emit the same bounded record to stderr and
 * OutputDebugString, then best-effort append the first failure in a process
 * to ProgramData.  The file is intentionally fixed and append-only so a DWM
 * failure remains inspectable after that process is restarted.
 */
static void
d3dkmt_gpuva_guard_report(const char *reason, const struct agx_device *dev,
                          const struct agx_d3dkmt_winsys *ws,
                          const struct agx_d3dkmt_bind *binds,
                          uint32_t bind_count,
                          const struct agx_kmt_updategpuva_op *ops,
                          uint32_t op_count)
{
   static volatile LONG persisted;
   const struct agx_d3dkmt_bind *bind = bind_count > 0 ? &binds[0] : NULL;
   const struct agx_kmt_updategpuva_op *op = op_count > 0 ? &ops[0] : NULL;
   SYSTEMTIME now;
   char message[1024];

   GetSystemTime(&now);
   int length = snprintf(
      message, sizeof(message),
      "NTAGX_GPUVA_GUARD utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ "
      "pid=%lu tid=%lu reason=%s shader=0x%llx policy_base=0x%llx "
      "policy_size=0x%llx binds=%u bind0_addr=0x%llx "
      "bind0_range=0x%llx bind0_flags=0x%x ops=%u op0_type=%d "
      "op0_base=0x%llx op0_size=0x%llx op0_prot=0x%llx "
      "op0_driver=0x%llx\n",
      (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
      (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond,
      (unsigned)now.wMilliseconds, (unsigned long)GetCurrentProcessId(),
      (unsigned long)GetCurrentThreadId(), reason,
      (unsigned long long)(dev ? dev->shader_base : 0),
      (unsigned long long)(ws ? ws->policy.usc_base : 0),
      (unsigned long long)(ws ? ws->policy.usc_size : 0), bind_count,
      (unsigned long long)(bind ? bind->addr : 0),
      (unsigned long long)(bind ? bind->range : 0), bind ? bind->flags : 0,
      op_count, op ? op->OperationType : -1,
      (unsigned long long)(op ? op->MapProtect.BaseAddress : 0),
      (unsigned long long)(op ? op->MapProtect.SizeInBytes : 0),
      (unsigned long long)(op ? op->MapProtect.Protection.Value : 0),
      (unsigned long long)(op ? op->MapProtect.DriverProtection : 0));
   if (length < 0)
      return;

   const DWORD bytes =
      (DWORD)MIN2((size_t)length, sizeof(message) - (size_t)1);
   fputs(message, stderr);
   fflush(stderr);
   OutputDebugStringA(message);

   if (InterlockedCompareExchange(&persisted, 1, 0) != 0)
      return;

   const char directory[] = "C:\\ProgramData\\AuroraSilicon";
   const char path[] =
      "C:\\ProgramData\\AuroraSilicon\\honeykrisp-gpuva-guard.log";
   if (!CreateDirectoryA(directory, NULL) &&
       GetLastError() != ERROR_ALREADY_EXISTS)
      return;

   HANDLE file = CreateFileA(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;

   DWORD written;
   (void)WriteFile(file, message, bytes, &written, NULL);
   CloseHandle(file);
}

static int
d3dkmt_va_update_init(struct agx_d3dkmt_winsys *ws)
{
   if (ws->va_context != 0 && ws->va_fence != 0)
      return 0;
   if (ws->va_context != 0 || ws->va_fence != 0 ||
       ws->policy.usc_size == 0) {
      errno = EINVAL;
      return -1;
   }

   struct agx_kmt_createsyncobj2 fence = {
      .hDevice = ws->device,
      .Info = {
         .Type = AGX_KMT_SYNC_MONITORED_FENCE,
         .MonitoredFence = {
            .InitialFenceValue = 0,
            .EngineAffinity = 1,
         },
      },
   };
   if (agx_d3dkmt_create_monitored_fence(ws, &fence, "VA interlock") != 0)
      return -1;

   struct agxw_context_private priv = {
      .version = AGXW_CONTEXT_PRIVATE_VERSION,
      .struct_size = sizeof(priv),
      .priority = AGXW_PRIORITY_MEDIUM,
      .flags = AGXW_CONTEXT_PRIVATE_FLAG_VA_INTERLOCK,
      .usc_exec_base = ws->policy.usc_base,
   };
   if (agxw_context_private_validate_layout(
          &priv, sizeof(priv), ws->policy.usc_base, ws->policy.usc_size,
          &ws->policy.layout) != 0) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      errno = EINVAL;
      return -1;
   }

   /* CreateContextVirtual itself selects the GPU-VA KMD context. Do not set
    * SynchronizationOnly: AppleAgxWddm intentionally admits the same real
    * virtual-addressing context shape used by Honeykrisp queues, and
    * UpdateGpuVirtualAddress requires such a rendering context. */
   struct agx_kmt_createcontextvirtual create = {
      .hDevice = ws->device,
      .NodeOrdinal = ws->policy.node_ordinal,
      .EngineAffinity = 1,
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };
   agx_kmt_status st = ws->fns.CreateContextVirtual(&create);
   if (!AGX_KMT_SUCCESS(st)) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      return agx_d3dkmt_fail(st);
   }

   ws->va_context = create.hContext;
   ws->va_fence = fence.hSyncObject;
   ws->va_fence_cpu =
      (const volatile uint64_t *)fence.Info.MonitoredFence
         .FenceValueCPUVirtualAddress;
   ws->va_fence_next = 0;
   if (enum_log_enabled())
      d3dkmt_log_flushed(
              "NTAGX_ENUM va_update_context=0x%08x fence=0x%08x cpu=%p\n",
              (unsigned)ws->va_context, (unsigned)ws->va_fence,
              (const void *)ws->va_fence_cpu);
   return 0;
}

static int
d3dkmt_bo_bind(struct agx_device *dev, struct drm_asahi_gem_bind_op *ops,
               uint32_t count)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   struct agx_d3dkmt_policy bind_policy;
   struct agx_d3dkmt_bind stack_binds[8];
   struct agx_kmt_updategpuva_op stack_ops[16];
   struct agx_d3dkmt_bind *binds = stack_binds;
   struct agx_kmt_updategpuva_op *uops = stack_ops;
   uint32_t nops = 0;
   int ret = -1;

   if (count == 0)
      return 0;

   if (count > ARRAY_SIZE(stack_binds)) {
      binds = malloc(count * sizeof(*binds));
      if (!binds) {
         errno = ENOMEM;
         return -1;
      }
   }

   /* Resolve dense GEM handles to D3DKMT allocation handles. */
   simple_mtx_lock(&ws->lock);
   for (uint32_t i = 0; i < count; ++i) {
      const uint64_t granularity = UINT64_C(0x10000);
      uint64_t range = ops[i].range;

      if ((ops[i].addr & (granularity - 1)) != 0 ||
          (ops[i].offset & (granularity - 1)) != 0 || range == 0 ||
          range > UINT64_MAX - (granularity - 1)) {
         simple_mtx_unlock(&ws->lock);
         d3dkmt_log_flushed(
            "d3dkmt_bo_bind align fail: i=%u addr=0x%llx offset=0x%llx range=0x%llx\n",
            i, (unsigned long long)ops[i].addr,
            (unsigned long long)ops[i].offset, (unsigned long long)range);
         errno = EINVAL;
         goto out;
      }
      range = ALIGN_POT(range, granularity);
      if ((ops[i].flags & (DRM_ASAHI_BIND_SINGLE_PAGE | DRM_ASAHI_BIND_WRITE)) ==
          (DRM_ASAHI_BIND_SINGLE_PAGE | DRM_ASAHI_BIND_WRITE)) {
         d3dkmt_log_flushed(
            "d3dkmt_bo_bind WARN writable SINGLE_PAGE: i=%u addr=0x%llx range=0x%llx handle=0x%08x flags=0x%x\n",
            i, (unsigned long long)ops[i].addr, (unsigned long long)range,
            (unsigned)ops[i].handle, ops[i].flags);
      }
      binds[i] = (struct agx_d3dkmt_bind){
         .flags = ops[i].flags,
         .kmt_handle = ops[i].handle,
         .offset = ops[i].offset,
         .range = range,
         .addr = ops[i].addr,
      };
   }
   simple_mtx_unlock(&ws->lock);

   if (agx_d3dkmt_bind_policy(dev->shader_base, &ws->policy, &bind_policy) !=
       AGX_D3DKMT_OK) {
      d3dkmt_gpuva_guard_report("policy", dev, ws, binds, count, NULL, 0);
      errno = EIO;
      goto out;
   }

   if (agx_d3dkmt_translate_binds(binds, count, &bind_policy, NULL, 0,
                                  &nops) != AGX_D3DKMT_OK) {
      d3dkmt_log_flushed(
         "d3dkmt_bo_bind translate fail: count=%u addr0=0x%llx range0=0x%llx flags0=0x%x kmt0=0x%08x\n",
         count, (unsigned long long)binds[0].addr,
         (unsigned long long)binds[0].range, binds[0].flags,
         (unsigned)binds[0].kmt_handle);
      errno = EINVAL;
      goto out;
   }

   if (nops > ARRAY_SIZE(stack_ops)) {
      uops = malloc((size_t)nops * sizeof(*uops));
      if (!uops) {
         errno = ENOMEM;
         goto out;
      }
   }

   if (agx_d3dkmt_translate_binds(binds, count, &bind_policy, uops, nops,
                                  &nops) != AGX_D3DKMT_OK) {
      errno = EINVAL;
      goto out;
   }

   /*
    * The one real semantic change in this op. DRM_IOCTL_ASAHI_VM_BIND returns
    * with the binding in effect; D3DKMTUpdateGpuVirtualAddress queues page-
    * table work that completes on a monitored fence. agx_bo_alloc() maps and
    * then immediately writes through the mapping, so the winsys must restore
    * the synchronous contract rather than push asynchrony up into Honeykrisp.
    */
   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_policy submit_policy;
   if (agx_d3dkmt_bind_policy(dev->shader_base, &ws->policy,
                              &submit_policy) != AGX_D3DKMT_OK ||
       agx_d3dkmt_validate_translated_binds(
          binds, count, &submit_policy, uops, nops) != AGX_D3DKMT_OK) {
      simple_mtx_unlock(&ws->lock);
      d3dkmt_gpuva_guard_report("pre-submit", dev, ws, binds, count, uops,
                                nops);
      errno = EIO;
      goto out;
   }
   if (ws->device_lost || ws->va_context == 0 || ws->va_fence == 0 ||
       ws->va_fence_next == UINT64_MAX) {
      simple_mtx_unlock(&ws->lock);
      errno = ws->device_lost ? EIO : EINVAL;
      goto out;
   }
   uint64_t fence_value = ws->va_fence_next;
   uint64_t completion_value = fence_value + 1;
   struct agx_kmt_updategpuva update = {
      .hDevice = ws->device,
      .hContext = ws->va_context,
      .hFenceObject = ws->va_fence,
      .NumOperations = nops,
      .Operations = uops,
      .FenceValue = fence_value,
      .Flags = 1u, /* D3DKMT_UPDATEGPUVIRTUALADDRESS::DoNotWait */
   };
   agx_kmt_status st = ws->fns.UpdateGpuVirtualAddress(&update);
   if (AGX_KMT_SUCCESS(st))
      ws->va_fence_next = completion_value;
   simple_mtx_unlock(&ws->lock);

   if (!AGX_KMT_SUCCESS(st)) {
      d3dkmt_log_flushed(
              "D3DKMTUpdateGpuVirtualAddress failed status=0x%08x "
              "device=0x%08x context=0x%08x fence=0x%08x "
              "fence_value=%llu nops=%u flags=0x%08x\n",
              (unsigned)st, (unsigned)update.hDevice,
              (unsigned)update.hContext, (unsigned)update.hFenceObject,
              (unsigned long long)update.FenceValue, update.NumOperations,
              update.Flags);
      for (uint32_t i = 0; i < nops; ++i) {
         d3dkmt_log_flushed(
                 "D3DKMTUpdateGpuVirtualAddress op[%u] type=%d "
                 "base=0x%llx size=0x%llx alloc=0x%08x "
                 "offset=0x%llx alloc_size=0x%llx prot=0x%llx "
                 "driver=0x%llx\n",
                 i, uops[i].OperationType,
                 (unsigned long long)uops[i].MapProtect.BaseAddress,
                 (unsigned long long)uops[i].MapProtect.SizeInBytes,
                 (unsigned)uops[i].MapProtect.hAllocation,
                 (unsigned long long)uops[i].MapProtect.AllocationOffsetInBytes,
                 (unsigned long long)uops[i].MapProtect.AllocationSizeInBytes,
                 (unsigned long long)uops[i].MapProtect.Protection.Value,
                 (unsigned long long)uops[i].MapProtect.DriverProtection);
      }
      ret = agx_d3dkmt_fail(st);
      goto out;
   }

   if (ws->va_fence_cpu == NULL || *ws->va_fence_cpu < completion_value)
      ret = agx_d3dkmt_wait_fence(ws, ws->va_fence, ws->va_fence_cpu,
                                  completion_value, AGX_D3DKMT_INFINITE);
   else
      ret = 0;
   if (ret != 0)
      agx_d3dkmt_mark_device_lost(ws, NULL);

   if (ws->v6_log && count == 1 && ops[0].range >= (16u << 20)) {
      static uint32_t large_bind_events;
      if (__atomic_fetch_add(&large_bind_events, 1, __ATOMIC_RELAXED) < 128)
         d3dkmt_log_flushed("agxw-v6: bind ctx=0x%x flags=0x%x allocation=0x%x "
                            "va=0x%llx len=0x%llx fence=%llu result=%d\n",
                            (unsigned)ws->va_context, ops[0].flags, ops[0].handle,
                            (unsigned long long)ops[0].addr,
                            (unsigned long long)ops[0].range,
                            (unsigned long long)completion_value, ret);
   }

out:
   if (binds != stack_binds)
      free(binds);
   if (uops != stack_ops)
      free(uops);
   return ret;
}

/* -------------------------------------------------------------------------
 * GEM_MMAP / GEM_MUNMAP.
 *
 * Linux does GEM_MMAP_OFFSET plus mmap(2); the AGXKMD port invented a
 * GEM_MMAP ioctl. WDDM already has the operation: D3DKMTLock2 returns a CPU
 * mapping of an allocation. It does NOT accept a placement hint, which is why
 * VK_EXT_map_memory_placed stays disabled on Windows (already the case,
 * patch 0007) -- agx_bo_map_placed()'s fixed_addr is unusable here and is
 * asserted away rather than silently ignored.
 * ------------------------------------------------------------------------- */
static void
d3dkmt_bo_mmap(struct agx_device *dev, struct agx_bo *bo, void *fixed_addr,
               size_t length)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   assert(bo->_map == NULL && "not double mapped");
   assert(fixed_addr == NULL &&
          "placed maps need VK_EXT_map_memory_placed, disabled on Windows");
   (void)fixed_addr;

   struct agx_d3dkmt_allocation_record record;
   simple_mtx_lock(&ws->lock);
   const bool found =
      agx_d3dkmt_pool_get_record(&ws->pool, bo->handle, &record);
   simple_mtx_unlock(&ws->lock);
   if (!found || record.allocation != bo->uapi_handle ||
       record.cpu_mapping_forbidden) {
      bo->_map = NULL;
      bo->_map_size = 0;
      errno = found && record.allocation == bo->uapi_handle
                 ? EACCES : EINVAL;
      return;
   }

   /* Lock2 always maps the whole allocation, so a shorter request is
    * satisfied by it and a longer one cannot be. */
   if (length > bo->size) {
      bo->_map = NULL;
      bo->_map_size = 0;
      errno = EINVAL;
      return;
   }

   struct agx_kmt_lock2 lock = {
      .hDevice = ws->device,
      .hAllocation = bo->uapi_handle,
   };

   agx_kmt_status st = ws->fns.Lock2(&lock);
   if (!AGX_KMT_SUCCESS(st)) {
      bo->_map = NULL;
      bo->_map_size = 0;
      agx_d3dkmt_fail(st);
      d3dkmt_log_flushed("D3DKMTLock2 failed: handle=%u st=0x%08x\n",
                         bo->uapi_handle, (unsigned)st);
      return;
   }

   bo->_map = lock.pData;
   bo->_map_size = bo->size;
}

static void
d3dkmt_bo_munmap(struct agx_device *dev, struct agx_bo *bo)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   struct agx_kmt_unlock2 unlock = {
      .hDevice = ws->device,
      .hAllocation = bo->uapi_handle,
   };

   ws->fns.Unlock2(&unlock);
   bo->_map = NULL;
   bo->_map_size = 0;
}

static int
d3dkmt_bo_sync_for_device(struct agx_device *dev, struct agx_bo *bo,
                          size_t offset, size_t size)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   if (bo == NULL || bo->uapi_handle == 0 || offset > bo->size ||
       size > bo->size - offset) {
      errno = EINVAL;
      return -1;
   }
   if (size == 0)
      return 0;

   const struct agx_kmt_invalidatecache invalidate = {
      .hDevice = ws->device,
      .hAllocation = bo->uapi_handle,
      .Offset = offset,
      .Length = size,
   };
   agx_kmt_status st = ws->fns.InvalidateCache(&invalidate);
   if (!AGX_KMT_SUCCESS(st)) {
      agx_d3dkmt_mark_device_lost(ws, NULL);
      return agx_d3dkmt_fail(st);
   }
   return 0;
}

/* Build the coherency declaration from the complete set of live CPU-writable
 * mappings, not from one shader or one command type.  A CPU cannot modify an
 * ordinary BO without Lock2 mapping it; imported/shareable BOs are included
 * even when this winsys did not create the CPU mapping because another API or
 * process may own it.  Whole-BO ranges deliberately trade a bounded amount of
 * cache maintenance for completeness across executable pools, command pools,
 * descriptors, vertex/index/uniform/texture/indirect data, events and buffer
 * device addresses.
 *
 * That is the version-3 rule. Under the version-6 primary-write contract
 * (dev->primary_write_contract) the list names only shared and shareable BOs,
 * whole: on the J813 path the kernel uses the v6 read list for nothing but
 * standard primaries (always shareable), copying each named one in at
 * publication, and refuses a submit when any declared page is unmapped at
 * acquire or remapped before publication. The selection is the one predicate
 * agx_d3dkmt_read_set_names_*() (agx_d3dkmt_core.c, host-tested), so the v3
 * list is exactly what it was. *named_bos counts the BOs named. */
static int
d3dkmt_build_read_set(struct agx_device *dev, struct agx_bo *staging,
                      struct agxw_submit_read_range *ranges,
                      uint32_t *range_count, uint32_t *named_bos)
{
   const bool v6 = dev->primary_write_contract;
   uint32_t count = 0, named = 0;
   uint64_t staging_va = staging->va->addr;

   if (agx_d3dkmt_read_set_names_staging(v6) &&
       agx_d3dkmt_read_range_add_layout(
          &agx_d3dkmt_ws(dev)->policy.layout, ranges, &count,
          AGXW_MAX_READ_RANGES_PER_SUBMIT,
          staging_va, staging->size) != AGX_D3DKMT_OK)
      return -1;

   simple_mtx_lock(&dev->bo_map_lock);
   for (uint32_t handle = 0; handle < dev->max_handle; handle++) {
      struct agx_bo *bo = agx_lookup_bo(dev, handle);
      uint64_t gpu_va;

      if (bo->refcnt <= 0 || bo->va == NULL || bo->size == 0 ||
          !agx_d3dkmt_read_set_names_bo(
             v6, bo->_map != NULL,
             v6 ? ((bo->flags & AGX_BO_PRIMARY) != 0)
                : ((bo->flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE)) != 0)))
         continue;
      gpu_va = bo->va->addr;
      if (agx_d3dkmt_read_range_add_layout(
             &agx_d3dkmt_ws(dev)->policy.layout, ranges, &count,
             AGXW_MAX_READ_RANGES_PER_SUBMIT,
             gpu_va, bo->size) != AGX_D3DKMT_OK) {
         simple_mtx_unlock(&dev->bo_map_lock);
         errno = ENOSPC;
         return -1;
      }
      named++;
   }
   simple_mtx_unlock(&dev->bo_map_lock);
   *range_count = count;
   if (named_bos)
      *named_bos = named;
   return 0;
}

/* 16 KiB pages a sorted range vector touches (the validator's own count). */
static uint64_t
d3dkmt_v6_pages(const struct agxw_submit_read_range *r, uint32_t n)
{
   uint64_t pages = 0;
   for (uint32_t i = 0; i < n; i++)
      pages += ((r[i].gpu_va + r[i].length - 1u) >> 14) - (r[i].gpu_va >> 14) + 1u;
   return pages;
}

/* The per-submit line: the first 20 submits and every hundredth after. */
static bool
d3dkmt_v6_sample(struct agx_d3dkmt_winsys *ws, uint32_t n)
{
   return ws->v6_log && (n <= 20 || n % 100 == 0);
}

/* -------------------------------------------------------------------------
 * GET_PARAMS.
 *
 * KMTQAITYPE_UMDRIVERPRIVATE is the WDDM-native miniport->UMD capability
 * channel (it is what DxgkDdiQueryAdapterInfo(DXGKQAITYPE_UMDRIVERPRIVATE)
 * exists to serve, and what every vendor UMD uses for exactly this). It is
 * queried once at open; this op is a copy out of that cached blob.
 * ------------------------------------------------------------------------- */
static int
d3dkmt_escape(struct agx_d3dkmt_winsys *ws, uint32_t op, void *payload,
              uint32_t payload_size)
{
   /*
    * CONTRACT.md SS8: exactly two escapes exist, both pure device-property
    * reads with no WDDM equivalent -- GET_PARAMS and GET_TIME. Nothing that
    * allocates, binds, submits or synchronises goes through one, because an
    * escape for any of those would rebuild the private-side-channel split
    * this whole port exists to remove.
    */
   const uint32_t total = (uint32_t)sizeof(struct agxw_escape_header) +
                          payload_size;
   uint8_t *blob = malloc(total);
   if (blob == NULL) {
      errno = ENOMEM;
      return -1;
   }

   struct agxw_escape_header hdr = {
      .magic = AGXW_ESCAPE_MAGIC,
      .version = AGXW_ESCAPE_VERSION,
      .op = op,
      .payload_size = payload_size,
   };
   memcpy(blob, &hdr, sizeof(hdr));
   memset(blob + sizeof(hdr), 0, payload_size);

   struct agx_kmt_escape escape = {
      .hAdapter = ws->adapter,
      .pPrivateDriverData = blob,
      .PrivateDriverDataSize = total,
   };

   agx_kmt_status st = ws->fns.Escape(&escape);
   if (!AGX_KMT_SUCCESS(st)) {
      free(blob);
      return agx_d3dkmt_fail(st);
   }

   memcpy(&hdr, blob, sizeof(hdr));
   if (hdr.status != 0) {
      free(blob);
      errno = -hdr.status;
      return -1;
   }

   memcpy(payload, blob + sizeof(hdr), payload_size);
   free(blob);
   return 0;
}

/*
 * The KMD's own geometry for an allocation this device has opened (see
 * agx_d3dkmt_core.h, "Imported allocation geometry"). Unlike the device
 * property escapes above it carries an input -- the allocation handle -- and
 * is issued on ws->device so the KMD resolves that handle in this process.
 * Any failure, including a KMD that does not know the op, leaves *info zeroed
 * and returns -1, which the import paths treat as "no KMD size": fail closed.
 */
static int
d3dkmt_query_allocation(struct agx_d3dkmt_winsys *ws, agx_kmt_handle allocation,
                        struct agx_d3dkmt_allocation_info *info)
{
   struct {
      struct agxw_escape_header hdr;
      struct agx_d3dkmt_allocation_info info;
   } blob;
   static_assert(sizeof(blob) == sizeof(struct agxw_escape_header) +
                                    sizeof(struct agx_d3dkmt_allocation_info),
                 "allocation query escape is header + payload, unpadded");

   memset(info, 0, sizeof(*info));
   memset(&blob, 0, sizeof(blob));
   blob.hdr = (struct agxw_escape_header){
      .magic = AGXW_ESCAPE_MAGIC,
      .version = AGXW_ESCAPE_VERSION,
      .op = AGX_D3DKMT_ESC_QUERY_ALLOCATION,
      .payload_size = sizeof(blob.info),
   };
   blob.info = (struct agx_d3dkmt_allocation_info){
      .version = AGX_D3DKMT_ALLOCATION_INFO_VERSION,
      .struct_size = sizeof(blob.info),
      .allocation = allocation,
   };

   struct agx_kmt_escape escape = {
      .hAdapter = ws->adapter,
      .hDevice = ws->device,
      .pPrivateDriverData = &blob,
      .PrivateDriverDataSize = sizeof(blob),
   };

   agx_kmt_status st = ws->fns.Escape(&escape);
   if (!AGX_KMT_SUCCESS(st)) {
      errno = ENOTSUP;
      return -1;
   }
   if (blob.hdr.status != 0) {
      errno = -blob.hdr.status;
      return -1;
   }

   *info = blob.info;
   return 0;
}

/*
 * GET_PARAMS. One of the two escapes -- the AGX global parameter block has no
 * WDDM equivalent at all, and KMTQAITYPE_UMDRIVERPRIVATE is spoken for by the
 * adapter identity blob (CONTRACT.md SS2.4, SS8).
 *
 * Fetched once at open, because agx_open_device() calls this before anything
 * else and the values never change; this op is a copy out of that cache.
 */
static ssize_t
d3dkmt_get_params(struct agx_device *dev, void *buf, size_t size)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   memset(buf, 0, size);

   if (ws->params == NULL || ws->params_size == 0)
      return -EINVAL;

   memcpy(buf, ws->params, MIN2(size, (size_t)ws->params_size));
   return (ssize_t)ws->params_size;
}

/* -------------------------------------------------------------------------
 * GEM_BIND_OBJECT (timestamp objects).
 *
 * AGX registers a BO with the firmware's timestamp-object table and gets back
 * a firmware-scoped handle that goes into drm_asahi_cmd_render::ts_vtx /
 * ts_frag.
 *
 * CONTRACT.md SS3.5 puts this on AGXW_ALLOC_TIMESTAMP: the KMD does the
 * context-0 bind at ALLOCATION time and returns object_handle in the output
 * private data. That is the right shape, and it is not an escape -- but it
 * does not fit the current call flow, and this is a contract gap to close
 * rather than something the UMD can paper over:
 *
 *   1. struct agxw_alloc_private has no object_handle field. It is
 *      {version, struct_size, size, flags, align_log2, reserved}, so there is
 *      nowhere for SS3.5's promised output to go.
 *   2. hk_query_pool.c:287 calls agx_bind_timestamps() on a BO that
 *      agx_bo_create() has ALREADY created, so the winsys cannot know at
 *      create time that an allocation will be a timestamp buffer. Making it
 *      knowable means a new agx_bo_flags bit plumbed through
 *      hk_create_query_pool -- small, but a Honeykrisp change, not a winsys
 *      one.
 *
 * Until both are settled this op is an identity: the "object handle" IS the
 * allocation handle, which is a value the miniport already has and can map to
 * a firmware slot lazily when it parses ts_vtx/ts_frag out of the command
 * stream. That is still not an escape, and it keeps timestamp queries on the
 * same allocator as everything else.
 * ------------------------------------------------------------------------- */
static int
d3dkmt_bo_bind_object(struct agx_device *dev,
                      struct drm_asahi_gem_bind_object *bind)
{
   (void)dev;

   if (bind->op != DRM_ASAHI_BIND_OBJECT_OP_BIND) {
      errno = EINVAL;
      return -1;
   }

   if (bind->handle == 0) {
      errno = EINVAL;
      return -1;
   }

   bind->object_handle = bind->handle;
   return 0;
}

static int
d3dkmt_bo_unbind_object(struct agx_device *dev, uint32_t object_handle)
{
   (void)dev;
   (void)object_handle;
   return 0;
}

/* -------------------------------------------------------------------------
 * SUBMIT.
 * ------------------------------------------------------------------------- */
static struct agx_bo *
staging_for(struct agx_device *dev, struct agx_d3dkmt_ctx *ctx, uint32_t size)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   unsigned slot = ctx->staging_slot;
   struct agx_bo *bo = ctx->staging[slot];

   if (bo != NULL && bo->size < size) {
      /* Wait for the slot to retire before recycling its memory. */
      if (agx_d3dkmt_wait_fence(ws, ctx->fence, ctx->fence_cpu,
                                ctx->staging_retire[slot],
                                AGX_D3DKMT_INFINITE))
         return NULL;
      agx_bo_unreference(dev, bo);
      bo = ctx->staging[slot] = NULL;
   }

   if (bo == NULL) {
      size_t alloc = ALIGN_POT(MAX2((size_t)size, (size_t)AIL_PAGESIZE),
                               AIL_PAGESIZE);
      bo = agx_bo_create(dev, alloc, 0, AGX_BO_WRITEBACK, "D3DKMT cmdbuf");
      if (bo == NULL)
         return NULL;
      ctx->staging[slot] = bo;
      ctx->staging_retire[slot] = 0;
   } else if (ctx->staging_retire[slot] != 0) {
      if (agx_d3dkmt_wait_fence(ws, ctx->fence, ctx->fence_cpu,
                                ctx->staging_retire[slot],
                                AGX_D3DKMT_INFINITE))
         return NULL;
   }

   return bo;
}

/*
 * DRM_IOCTL_ASAHI_SUBMIT carries in-syncs, the command stream and out-syncs in
 * ONE ioctl. Under the contract that becomes:
 *
 *   1. D3DKMTWaitForSynchronizationObjectFromGpu on the context, for the
 *      in-syncs. NOT the submit private data's in_sync_count, which
 *      CONTRACT.md SS7.3 requires to be 0 in contract version 1 -- AppleAgxGpu
 *      rejects a non-zero one (submit.c:629-630) and the WDDM front end
 *      inherits that gap. The contract notes this is the idiomatic shape
 *      regardless, and stays the recommendation after the gap closes.
 *   2. D3DKMTSubmitCommand, with Commands pointing at an AGXW_DMA_HEADER
 *      followed by the verbatim AGX command stream. No allocation list, no
 *      patch locations: final GPU VAs are already in the stream, exactly as on
 *      Linux (CONTRACT.md SS7.2 item 3).
 *   3. D3DKMTSignalSynchronizationObjectFromGpu, on the same context, for
 *      every out-sync and the staging-retire fence. This queues an ordered
 *      dxgkrnl software signal packet behind the submitted DMA. D3DKMT sync
 *      handles are dxgkrnl handles and MUST NOT appear in KMD private data.
 *
 * A D3DKMT context is a strictly serial queue, so (1)-(3) are equivalent
 * to the atomic ioctl PROVIDED nothing fails in between -- a failure after the
 * wait would leave a dangling dependency on the context. Everything under
 * UMD control (context lookup, staging allocation, sync resolution, blob
 * sizing) is therefore done BEFORE the first D3DKMT call. A thunk failure
 * after sync values are reserved poisons the context fail-closed.
 */
static int
d3dkmt_submit(struct agx_device *dev, struct drm_asahi_submit *submit,
              struct agx_submit_virt *virt)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   if (!agx_bridge_vma_winsys_healthy(ws)) {
      simple_mtx_lock(&ws->lock);
      agx_d3dkmt_mark_device_lost(ws, NULL);
      simple_mtx_unlock(&ws->lock);
      errno = EIO;
      return -1;
   }
   const struct drm_asahi_sync *syncs =
      (const struct drm_asahi_sync *)(uintptr_t)submit->syncs;
   agx_kmt_handle *waits = NULL;
   uint64_t *wait_values = NULL;
   agx_kmt_handle *signals = NULL;
   uint64_t *signal_values = NULL;
   struct agxw_submit_private_v3 *priv = NULL;
   uint32_t priv_size = 0;
   int ret = -1;

   const struct agx_primary_write_set *primary = virt ? virt->primary_writes : NULL;
   struct agx_primary_write_set resolved_primary = {0};
   if (dev->primary_write_contract) {
      if (!primary || !primary->observed || primary->incomplete ||
          (!primary->conservative_primaries &&
           primary->count > AGX_PRIMARY_WRITE_RANGES)) {
         if (agx_d3dkmt_v6_first(ws, AGX_D3DKMT_V6_WRITE_SET))
            d3dkmt_log_flushed("agxw-v6: refused kind=write-set where=winsys set=%s observed=%d "
                               "incomplete=%d causes=0x%x ranges=%u\n",
                               primary ? "yes" : "none", primary ? (int)primary->observed : 0,
                               primary ? (int)primary->incomplete : 0,
                               primary ? (unsigned)primary->causes : 0u,
                               primary ? (unsigned)primary->count : 0u);
         errno = ENOTSUP;
         return -1;
      }
      if (primary->conservative_primaries) {
         resolved_primary.observed = true;
         simple_mtx_lock(&dev->bo_map_lock);
         for (uint32_t handle = 0; handle < dev->max_handle; handle++) {
            struct agx_bo *bo = agx_lookup_bo(dev, handle);
            if (bo->refcnt <= 0 || bo->va == NULL || bo->size == 0 ||
                (bo->flags & AGX_BO_PRIMARY) == 0)
               continue;
            agx_primary_write_add(&resolved_primary, bo->va->addr, bo->size);
         }
         simple_mtx_unlock(&dev->bo_map_lock);
         if (resolved_primary.incomplete ||
             resolved_primary.count > AGX_PRIMARY_WRITE_RANGES) {
            if (agx_d3dkmt_v6_first(ws, AGX_D3DKMT_V6_WRITE_SET))
               d3dkmt_log_flushed("agxw-v6: refused kind=write-set where=conservative "
                                  "causes=0x%x ranges=%u\n",
                                  (unsigned)resolved_primary.causes,
                                  (unsigned)resolved_primary.count);
            errno = ENOTSUP;
            return -1;
         }
         primary = &resolved_primary;
      }
   }

   const uint32_t n_in = submit->in_sync_count;
   const uint32_t n_out = submit->out_sync_count;

   /* +1 for the winsys's own staging-retire fence. */
   if ((uint64_t)n_in + n_out + 1 > AGXW_MAX_SYNCS_PER_SUBMIT ||
       submit->cmdbuf_size > AGXW_MAX_CMDBUF_BYTES) {
      errno = EINVAL;
      return -1;
   }

   if (submit->flags & ~AGX_SUBMIT_WRITE_SET_COMPLETE) {
      errno = EINVAL;
      return -1;
   }

   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_ctx *ctx = ctx_get(ws, submit->queue_id);
   if (ctx == NULL) {
      simple_mtx_unlock(&ws->lock);
      errno = EINVAL;
      return -1;
   }
   if (ws->device_lost || ctx->submit_failed || ctx->submit_refs != 0) {
      simple_mtx_unlock(&ws->lock);
      errno = (ws->device_lost || ctx->submit_failed) ? EIO : EBUSY;
      return -1;
   }
   ctx->submit_refs = 1;
   ws->active_submits++;
   const unsigned staging_slot = ctx->staging_slot;
   simple_mtx_unlock(&ws->lock);

   signals = malloc((size_t)(n_out + 1) * sizeof(*signals));
   signal_values = malloc((size_t)(n_out + 1) * sizeof(*signal_values));
   if (n_in > 0) {
      waits = malloc((size_t)n_in * sizeof(*waits));
      wait_values = malloc((size_t)n_in * sizeof(*wait_values));
   }
   if (signals == NULL || signal_values == NULL ||
       (n_in > 0 && (waits == NULL || wait_values == NULL))) {
      errno = ENOMEM;
      goto out;
   }

   /* The DMA buffer: header + verbatim AGX command stream, in a BO this
    * winsys owns and has already mapped to GPU VA. */
   struct agx_bo *staging =
      staging_for(dev, ctx,
                  (uint32_t)sizeof(struct agxw_dma_header) +
                     submit->cmdbuf_size);
   if (staging == NULL) {
      errno = ENOMEM;
      goto out;
   }

   uint8_t *map = agx_bo_map(staging);
   if (map == NULL) {
      errno = ENOMEM;
      goto out;
   }

   struct agxw_dma_header dma = {
      .magic = AGXW_DMA_MAGIC,
      .header_size = sizeof(dma),
      .cmdbuf_size = submit->cmdbuf_size,
   };
   memcpy(map, &dma, sizeof(dma));
   memcpy(map + sizeof(dma), (const void *)(uintptr_t)submit->cmdbuf,
          submit->cmdbuf_size);

   const size_t priv_capacity = sizeof(*priv) +
      (AGXW_MAX_READ_RANGES_PER_SUBMIT +
       (dev->primary_write_contract ? AGXW_MAX_PRIMARY_WRITE_RANGES : 0)) *
         sizeof(struct agxw_submit_read_range);
   priv = calloc(1, priv_capacity);
   if (priv == NULL) {
      errno = ENOMEM;
      goto out;
   }
   struct agxw_submit_read_range *read_ranges =
      (struct agxw_submit_read_range *)(priv + 1);
   uint32_t named_bos = 0;
   if (d3dkmt_build_read_set(
          dev, staging,
          read_ranges, &priv->read_range_count, &named_bos) != 0) {
      if (agx_d3dkmt_v6_first(ws, AGX_D3DKMT_V6_READ_SET))
         d3dkmt_log_flushed("agxw-v6: refused kind=read-set errno=%d (more than %u ranges, "
                            "or a range outside the user window)\n",
                            errno, AGXW_MAX_READ_RANGES_PER_SUBMIT);
      goto out;
   }
   /* The record built here is the v3 shape (base + read ranges). Name that
    * version explicitly: the contract's current AGXW_SUBMIT_PRIVATE_VERSION
    * has moved on to v5 (split clean set), and tagging a v3 struct with it
    * makes the shared validator refuse every submit. */
   priv->base.version = AGXW_SUBMIT_PRIVATE_VERSION_V3;
   priv->base.struct_size = sizeof(*priv);
   priv->base.in_sync_count = 0;
   priv->base.out_sync_count = 0;
   priv->base.flags = AGXW_SUBMIT_READ_SET_COMPLETE |
      ((submit->flags & AGX_SUBMIT_WRITE_SET_COMPLETE)
          ? AGXW_SUBMIT_WRITE_SET_COMPLETE : 0);
   priv_size = (uint32_t)sizeof(*priv) +
      priv->read_range_count * sizeof(*read_ranges);
   if (dev->primary_write_contract) {
      static_assert(sizeof(struct agxw_submit_private_v3) ==
                    sizeof(struct agxw_submit_private_v6), "v6 vector offset");
      static_assert(AGX_PRIMARY_WRITE_RANGES == AGXW_MAX_PRIMARY_WRITE_RANGES,
                    "primary write capacity");
      struct agxw_submit_private_v6 *v6 = (void *)priv;
      /* drivers/AppleAgxWddm/core/agxw_agreement.h, section 2. */
      v6->base.version = AGXW_AGREE_SUBMIT_VERSION;
      v6->base.flags = AGXW_AGREE_V6_FLAGS;
      v6->primary_write_range_count = primary->count;
      struct agxw_submit_read_range *writes = read_ranges + v6->read_range_count;
      for (unsigned i = 0; i < primary->count; i++)
         writes[i] = (struct agxw_submit_read_range){
            primary->ranges[i].address, primary->ranges[i].size};
      priv_size += primary->count * sizeof(*writes);
   }
   if (agxw_submit_private_validate_layout(&priv->base, priv_size,
                                           &ws->policy.layout) != 0) {
      if (agx_d3dkmt_v6_first(ws, AGX_D3DKMT_V6_VALIDATOR)) {
         const uint32_t w = dev->primary_write_contract ? primary->count : 0;
         const struct agxw_submit_read_range *wvec = read_ranges + priv->read_range_count;
         d3dkmt_log_flushed("agxw-v6: refused kind=validator reads=%u read_pages=%llu "
                            "writes=%u write_pages=%llu bytes=%u w0=0x%llx+0x%llx\n",
                            priv->read_range_count,
                            (unsigned long long)d3dkmt_v6_pages(read_ranges, priv->read_range_count),
                            w,
                            (unsigned long long)d3dkmt_v6_pages(wvec, w),
                            priv_size,
                            (unsigned long long)(w ? wvec[0].gpu_va : 0ull),
                            (unsigned long long)(w ? wvec[0].length : 0ull));
      }
      errno = EINVAL;
      goto out;
   }
   if (agx_bo_sync_for_device(
          staging, 0,
          (size_t)sizeof(struct agxw_dma_header) + submit->cmdbuf_size))
      goto out;

   /* Resolve every syncobj handle before the first D3DKMT call. */
   uint32_t n_sig = 0;
   uint64_t retire;

   simple_mtx_lock(&ws->lock);
   for (uint32_t i = 0; i < n_in; ++i) {
      struct agx_d3dkmt_sync *s =
         agx_d3dkmt_sync_table_get(&ws->syncs, syncs[i].handle);
      if (s == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }
      waits[i] = s->kmt;
      wait_values[i] = syncs[i].sync_type == DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ
                          ? syncs[i].timeline_value
                          : agx_d3dkmt_binary_wait_value(s);
   }

   for (uint32_t i = 0; i < n_out; ++i) {
      const struct drm_asahi_sync *src = &syncs[n_in + i];
      struct agx_d3dkmt_sync *e =
         agx_d3dkmt_sync_table_get(&ws->syncs, src->handle);
      if (e == NULL) {
         simple_mtx_unlock(&ws->lock);
         errno = EINVAL;
         goto out;
      }

      uint64_t v;
      if (src->sync_type == DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ) {
         v = src->timeline_value;
      } else {
         /* DRM binary out_sync replaces any prior fence without requiring an
          * explicit reset (drm_syncobj_replace_fence). Advance the epoch past
          * any signaled or in-flight value so reused batch syncobjs block
          * until this submission retires. */
         e->base = agx_d3dkmt_binary_reset_base(e);
         v = agx_d3dkmt_binary_signal_value(e, NULL);
      }

      signals[n_sig] = e->kmt;
      signal_values[n_sig] = v;
      if (v > e->last_request)
         e->last_request = v;
      n_sig++;
   }

   retire = ++ctx->fence_next;
   signals[n_sig] = ctx->fence;
   signal_values[n_sig] = retire;
   n_sig++;
   simple_mtx_unlock(&ws->lock);

   if (n_in > 0) {
      struct agx_kmt_waitsyncfromgpu wait = {
         .hContext = ctx->hContext,
         .ObjectCount = n_in,
         .ObjectHandleArray = waits,
         .MonitoredFenceValueArray = wait_values,
      };
      agx_kmt_status st = ws->fns.WaitForSynchronizationObjectFromGpu(&wait);
      if (!AGX_KMT_SUCCESS(st)) {
         if (agx_d3dkmt_v6_first(ws, AGX_D3DKMT_V6_GPU_WAIT))
            d3dkmt_log_flushed("agxw-v6: refused kind=gpu-wait status=0x%08x objects=%u\n",
                               (unsigned)st, n_in);
         ret = agx_d3dkmt_fail(st);
         agx_d3dkmt_mark_device_lost(ws, ctx);
         goto out;
      }
   }

   struct agx_kmt_submitcommand cmd = {
      .Commands = staging->va->addr,
      .CommandLength =
         (uint32_t)sizeof(struct agxw_dma_header) + submit->cmdbuf_size,
      .pPrivateDriverData = priv,
      .PrivateDriverDataSize = priv_size,
      .BroadcastContextCount = 1,
      .BroadcastContext = {ctx->hContext},
   };

   agx_kmt_status st = ws->fns.SubmitCommand(&cmd);
   if (ws->v6_log) {
      const uint32_t n = __atomic_add_fetch(&ws->v6_submits, 1u, __ATOMIC_RELAXED);
      const struct agxw_submit_private_v6 *v6p = (const void *)priv;
      const struct agxw_submit_read_range *w = read_ranges + v6p->read_range_count;
      if (d3dkmt_v6_sample(ws, n)) {
         d3dkmt_log_flushed("agxw-v6: submit n=%u version=%u reads=%u read_pages=%llu "
                            "shared_bos=%u writes=%u write_pages=%llu status=0x%08x\n",
                            n, v6p->base.version, v6p->read_range_count,
                            (unsigned long long)d3dkmt_v6_pages(read_ranges, v6p->read_range_count),
                            named_bos, v6p->primary_write_range_count,
                            (unsigned long long)d3dkmt_v6_pages(w, v6p->primary_write_range_count),
                            (unsigned)st);
         for (uint32_t i = 0; i < v6p->read_range_count; i++)
            d3dkmt_log_flushed("agxw-v6:   read[%u] va=0x%llx len=0x%llx\n", i,
                               (unsigned long long)read_ranges[i].gpu_va,
                               (unsigned long long)read_ranges[i].length);
         for (uint32_t i = 0; i < v6p->primary_write_range_count; i++)
            d3dkmt_log_flushed("agxw-v6:   write[%u] va=0x%llx len=0x%llx\n", i,
                               (unsigned long long)w[i].gpu_va,
                               (unsigned long long)w[i].length);
      }
      if (!AGX_KMT_SUCCESS(st) && agx_d3dkmt_v6_first(ws, AGX_D3DKMT_V6_SUBMIT))
         d3dkmt_log_flushed("agxw-v6: refused kind=submit n=%u status=0x%08x reads=%u writes=%u\n",
                            n, (unsigned)st, v6p->read_range_count,
                            v6p->primary_write_range_count);
   }
   if (!AGX_KMT_SUCCESS(st)) {
      ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_device_lost(ws, ctx);
      goto out;
   }

   struct agx_kmt_signalsyncfromgpu signal = {
      .hContext = ctx->hContext,
      .ObjectCount = n_sig,
      .ObjectHandleArray = signals,
      .MonitoredFenceValueArray = signal_values,
   };
   st = ws->fns.SignalSynchronizationObjectFromGpu(&signal);
   if (!AGX_KMT_SUCCESS(st)) {
      if (agx_d3dkmt_v6_first(ws, AGX_D3DKMT_V6_SIGNAL))
         d3dkmt_log_flushed("agxw-v6: refused kind=signal status=0x%08x objects=%u\n",
                            (unsigned)st, n_sig);
      /* The DMA is already in flight but its completion signals were not
       * queued. Poison the context so staging can never be reused unsafely. */
      ret = agx_d3dkmt_fail(st);
      agx_d3dkmt_mark_completion_unknown(ws, ctx);
      goto out;
   }

   simple_mtx_lock(&ws->lock);
   ctx->staging_retire[staging_slot] = retire;
   ctx->staging_slot = (staging_slot + 1) % AGX_D3DKMT_STAGING_RING;
   simple_mtx_unlock(&ws->lock);
   ret = 0;

out:
   simple_mtx_lock(&ws->lock);
   assert(ctx->submit_refs == 1 && ws->active_submits > 0);
   ctx->submit_refs = 0;
   ws->active_submits--;
   simple_mtx_unlock(&ws->lock);
   free(waits);
   free(wait_values);
   free(signals);
   free(signal_values);
   free(priv);
   return ret;
}

/* -------------------------------------------------------------------------
 * simple_ioctl: VM_CREATE, QUEUE_CREATE, QUEUE_DESTROY, GET_TIME.
 * ------------------------------------------------------------------------- */
static int
d3dkmt_reserve(struct agx_d3dkmt_winsys *ws, uint64_t base, uint64_t size)
{
   struct agx_kmt_reservegpuva reserve = {
      .hPagingQueue = ws->paging_queue,
      .BaseAddress = base,
      .MinimumAddress = base,
      .MaximumAddress = base + size,
      .Size = size,
      .ReservationType = AGX_KMT_GPUVA_RESERVE_NO_ACCESS,
   };

   agx_kmt_status st = ws->fns.ReserveGpuVirtualAddress(&reserve);
   if (!AGX_KMT_SUCCESS(st)) {
      d3dkmt_log_flushed("D3DKMTReserveGpuVirtualAddress(0x%llx, 0x%llx) failed\n",
              (unsigned long long)base, (unsigned long long)size);
      return agx_d3dkmt_fail(st);
   }

   /*
    * CONTRACT.md SS4.3 item 2 decides that a non-zero BaseAddress is
    * MANDATORY, not advisory -- but flags it as the one place it wants a
    * hardware datum, because whether VidMm passes BaseAddress through rather
    * than overriding it is unproven. This check is the seam: if VidMm
    * relocates, exactly one call site fails loudly here instead of every GPU
    * address in the driver being silently wrong. Honeykrisp's VA layout is
    * not relocatable -- shader_base, the zero page at 2^32 and the printf
    * buffer at 2^36 are baked into precompiled shaders
    * (agx_device.c:580-600, LIBAGX_PRINTF_BUFFER_ADDRESS) -- so a driver that
    * cannot pin its own addresses cannot run at all, and saying so on the
    * first reservation is the cheapest possible discovery.
    */
   if (reserve.VirtualAddress != base) {
      d3dkmt_log_flushed(
              "D3DKMTReserveGpuVirtualAddress ignored the base hint "
              "(wanted 0x%llx, got 0x%llx); Honeykrisp's VA layout is fixed\n",
              (unsigned long long)base,
              (unsigned long long)reserve.VirtualAddress);
      /* Do not leave the misplaced reservation behind: the caller's rollback
       * only knows the ranges it asked for. */
      struct agx_kmt_freegpuva misplaced = {
         .hAdapter = ws->adapter,
         .BaseAddress = reserve.VirtualAddress,
         .Size = size,
      };
      if (reserve.VirtualAddress != 0)
         ws->fns.FreeGpuVirtualAddress(&misplaced);
      errno = ENOTSUP;
      return -1;
   }

   return agx_d3dkmt_wait_paging(ws, reserve.PagingFenceValue);
}

static bool
d3dkmt_luid_equal(struct agx_kmt_luid a, struct agx_kmt_luid b)
{
   return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static bool
d3dkmt_ranges_equal(const struct agx_d3dkmt_va_lease *lease,
                    const struct agx_d3dkmt_va_range *ranges, uint32_t count)
{
   if (lease->range_count != count)
      return false;

   for (uint32_t i = 0; i < count; ++i) {
      if (lease->ranges[i].base != ranges[i].base ||
          lease->ranges[i].size != ranges[i].size)
         return false;
   }

   return true;
}

static int
d3dkmt_free_va_range(struct agx_d3dkmt_winsys *ws,
                     const struct agx_d3dkmt_va_range *range)
{
   struct agx_kmt_freegpuva free_va = {
      .hAdapter = ws->adapter,
      .BaseAddress = range->base,
      .Size = range->size,
   };
   agx_kmt_status st = ws->fns.FreeGpuVirtualAddress(&free_va);

   if (!AGX_KMT_SUCCESS(st)) {
      d3dkmt_log_flushed(
              "D3DKMTFreeGpuVirtualAddress(0x%llx, 0x%llx) failed "
              "status=0x%08x\n",
              (unsigned long long)range->base,
              (unsigned long long)range->size, (unsigned)st);
      return agx_d3dkmt_fail(st);
   }

   return 0;
}

static int
d3dkmt_acquire_va_lease(struct agx_d3dkmt_winsys *ws,
                        const struct agx_d3dkmt_va_range *ranges,
                        uint32_t count)
{
   struct agx_d3dkmt_va_lease **link;
   struct agx_d3dkmt_va_lease *lease;
   uint32_t acquired_mask = 0;
   int saved_errno = 0;

   if (count == 0 || count > ARRAY_SIZE(ws->va)) {
      errno = EINVAL;
      return -1;
   }

   AcquireSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   for (link = &agx_d3dkmt_va_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }

   lease = *link;
   if (lease != NULL) {
      if (!d3dkmt_ranges_equal(lease, ranges, count) ||
          lease->references != 0) {
         if (!d3dkmt_ranges_equal(lease, ranges, count)) {
            ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
            errno = EINVAL;
            return -1;
         }

         lease->references++;
         memcpy(ws->va, ranges, count * sizeof(ranges[0]));
         ws->va_count = count;
         ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
         return 0;
      }
   } else {
      lease = calloc(1, sizeof(*lease));
      if (lease == NULL) {
         ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
         errno = ENOMEM;
         return -1;
      }
      lease->luid = ws->luid;
      lease->range_count = count;
      memcpy(lease->ranges, ranges, count * sizeof(ranges[0]));
      lease->next = agx_d3dkmt_va_leases;
      agx_d3dkmt_va_leases = lease;
   }

   /* references==0 means a prior final release either completed or retained
    * only the ranges whose free failed. Rebuild the missing subset exactly. */
   for (uint32_t i = 0; i < count; ++i) {
      const uint32_t bit = 1u << i;
      if ((lease->reserved_mask & bit) != 0)
         continue;
      if (d3dkmt_reserve(ws, ranges[i].base, ranges[i].size)) {
         saved_errno = errno;
         goto rollback;
      }
      lease->reserved_mask |= bit;
      acquired_mask |= bit;
   }

   lease->references = 1;
   memcpy(ws->va, ranges, count * sizeof(ranges[0]));
   ws->va_count = count;
   ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   return 0;

rollback:
   for (uint32_t i = 0; i < count; ++i) {
      const uint32_t bit = 1u << i;
      if ((acquired_mask & bit) != 0 &&
          d3dkmt_free_va_range(ws, &ranges[i]) == 0)
         lease->reserved_mask &= ~bit;
   }
   if (lease->reserved_mask == 0) {
      for (link = &agx_d3dkmt_va_leases; *link != NULL;
           link = &(*link)->next) {
         if (*link == lease) {
            *link = lease->next;
            break;
         }
      }
      free(lease);
   }
   ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   errno = saved_errno;
   return -1;
}

static void
d3dkmt_release_va_lease(struct agx_d3dkmt_winsys *ws)
{
   struct agx_d3dkmt_va_lease **link;
   struct agx_d3dkmt_va_lease *lease;

   if (ws->va_count == 0)
      return;

   AcquireSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   for (link = &agx_d3dkmt_va_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }
   lease = *link;
   if (lease == NULL || !d3dkmt_ranges_equal(lease, ws->va, ws->va_count) ||
       lease->references == 0) {
      d3dkmt_log_flushed("D3DKMT GPUVA lease invariant failure\n");
      ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
      ws->va_count = 0;
      return;
   }

   lease->references--;
   if (lease->references != 0) {
      ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
      ws->va_count = 0;
      return;
   }

   for (uint32_t i = 0; i < lease->range_count; ++i) {
      const uint32_t bit = 1u << i;
      if ((lease->reserved_mask & bit) != 0 &&
          d3dkmt_free_va_range(ws, &lease->ranges[i]) == 0)
         lease->reserved_mask &= ~bit;
   }
   if (lease->reserved_mask == 0) {
      *link = lease->next;
      free(lease);
   }
   ReleaseSRWLockExclusive(&agx_d3dkmt_va_lease_lock);
   ws->va_count = 0;
}

static int
d3dkmt_vm_create(struct agx_device *dev, struct drm_asahi_vm_create *vm)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   /*
    * There is no VM object to create: CONTRACT.md SS5.1 makes the AGX VM the
    * D3DKMT device, which agx_d3dkmt_open_device() already made. What DOES
    * have to happen here, and can only happen here, is reserving the user VA
    * window: D3DKMTMapGpuVirtualAddress can only map inside a reservation,
    * and vm_create is the moment Honeykrisp has decided where its heaps go.
    *
    * vm->kernel_start/kernel_end are INPUTS on DRM -- userspace picks where
    * the kernel's slice sits. Under WDDM the miniport owns its own kernel VA
    * (it lives in TTBR1 and is not part of the WDDM VA space at all,
    * CONTRACT.md SS4.2), so they are advisory here and only the window below
    * kernel_start is claimed.
    */
   struct agx_d3dkmt_va_range ranges[AGX_D3DKMT_MAX_VA_LEASE_RANGES];
   uint32_t n;
   uint32_t guard_count = 0;

   /*
    * G17: reserve the head of the driver window, [0x10_0000_0000,
    * 0x10_0200_0000), NO_ACCESS, as the FIRST range of the lease -- the first
    * address-space action after the D3DKMT device and the paging queue exist,
    * ahead of the three binding windows and of every monitored fence and
    * context. VidMm places a monitored fence's value page at the first free
    * address it finds; on boot 105 that was 0x10_0029_0000, just past the
    * triangle client's reservations. The KMD reserves the rest of the driver
    * window per process ([0x10_0200_0000, 0x18_0000_0000)), so with this the
    * whole window is closed to VidMm. Nothing maps here: it is not a binding
    * window, and Honeykrisp's heaps all lie in the plan's windows.
    *
    * Fail closed, no fallback: a fence that already exists may already sit in
    * the head, the layout must be the one the guard was derived from, and
    * d3dkmt_reserve() refuses both a failed reservation and a moved base.
    */
   if (ws->m5_guard_required != agx_chip_g17_m5(dev->chip)) {
      d3dkmt_log_flushed("D3DKMT: G17 guard decision disagrees with the chip\n");
      errno = EINVAL;
      return -1;
   }
   if (ws->m5_guard_required) {
      const struct agxw_va_layout *l = &ws->policy.layout;

      if (p_atomic_read(&ws->monitored_fences_created) != 0) {
         d3dkmt_log_flushed(
                 "D3DKMT: %u monitored fence(s) exist before the G17 "
                 "driver-window guard; refusing VM_CREATE\n",
                 (unsigned)p_atomic_read(&ws->monitored_fences_created));
         errno = EBUSY;
         return -1;
      }
      if (vm->kernel_start != AGX_D3DKMT_M5_GUARD_BASE ||
          vm->kernel_end < AGX_D3DKMT_M5_GUARD_END ||
          l->user_start > AGX_D3DKMT_M5_GUARD_BASE ||
          l->user_end < AGX_D3DKMT_M5_GUARD_END ||
          l->internal_start <= AGX_D3DKMT_M5_GUARD_BASE ||
          l->internal_start >= AGX_D3DKMT_M5_GUARD_END ||
          l->internal_end < AGX_D3DKMT_M5_GUARD_END) {
         d3dkmt_log_flushed(
                 "D3DKMT: the G17 driver-window guard [0x%llx, 0x%llx) does "
                 "not fit driver range [0x%llx, 0x%llx), user VA [0x%llx, "
                 "0x%llx), KMD range [0x%llx, 0x%llx)\n",
                 (unsigned long long)AGX_D3DKMT_M5_GUARD_BASE,
                 (unsigned long long)AGX_D3DKMT_M5_GUARD_END,
                 (unsigned long long)vm->kernel_start,
                 (unsigned long long)vm->kernel_end,
                 (unsigned long long)l->user_start,
                 (unsigned long long)l->user_end,
                 (unsigned long long)l->internal_start,
                 (unsigned long long)l->internal_end);
         errno = EINVAL;
         return -1;
      }
      ranges[0].base = AGX_D3DKMT_M5_GUARD_BASE;
      ranges[0].size = AGX_D3DKMT_M5_GUARD_END - AGX_D3DKMT_M5_GUARD_BASE;
      guard_count = 1;
   }

   if (agx_chip_g17_m5(dev->chip) || dev->chip == AGX_CHIP_G18G) {
      /*
       * The M5 layout keeps the driver range VM_CREATE names at
       * [0x10_0000_0000, ...) between the heaps, so [vm_start, kernel_start)
       * is empty. Reserve the windows the layout actually binds instead
       * (agx_d3dkmt_plan_m5_vm), never the driver range.
       */
      struct agx_d3dkmt_vm_plan plan;
      if (agx_d3dkmt_plan_m5_vm(&ws->policy.layout, dev->params.vm_start,
                                dev->params.vm_end,
                                dev->params.vm_kernel_min_size,
                                dev->shader_base, vm->kernel_start,
                                vm->kernel_end, &plan) != AGX_D3DKMT_OK) {
         d3dkmt_log_flushed(
                 "D3DKMT: the M5 address layout (USC 0x%llx, driver range "
                 "[0x%llx, 0x%llx)) does not fit the adapter's VM [0x%llx, "
                 "0x%llx)\n",
                 (unsigned long long)dev->shader_base,
                 (unsigned long long)vm->kernel_start,
                 (unsigned long long)vm->kernel_end,
                 (unsigned long long)ws->policy.layout.user_start,
                 (unsigned long long)ws->policy.layout.user_end);
         errno = EINVAL;
         return -1;
      }
      for (uint32_t i = 0; i < plan.outside_count; ++i) {
         d3dkmt_log_flushed(
                 "D3DKMT: Honeykrisp binds [0x%llx, 0x%llx) but the adapter's "
                 "user VA is [0x%llx, 0x%llx); those binds will fail\n",
                 (unsigned long long)plan.outside[i].base,
                 (unsigned long long)(plan.outside[i].base +
                                      plan.outside[i].size),
                 (unsigned long long)ws->policy.layout.user_start,
                 (unsigned long long)ws->policy.layout.user_end);
      }
      memcpy(ranges + guard_count, plan.ranges,
             plan.count * sizeof(ranges[0]));
      n = guard_count + plan.count;
   } else {
      uint64_t base = dev->params.vm_start;
      uint64_t end = vm->kernel_start;

      if (end <= base) {
         errno = EINVAL;
         return -1;
      }

      /*
       * CONTRACT.md SS4.2: a single 16 KiB kernel/firmware page sits in the
       * MIDDLE of the user window. Reserving across it would ask VidMm for a
       * range that includes an address it has declared internal, so the
       * window is reserved as the one or two sub-ranges that avoid it.
       */
      n = agx_d3dkmt_va_split_carveout(base, end - base, ranges);
      if (n == 0) {
         errno = EINVAL;
         return -1;
      }
   }

   if (d3dkmt_acquire_va_lease(ws, ranges, n)) {
      if (guard_count != 0)
         d3dkmt_log_flushed(
                 "D3DKMT: reserving the G17 driver-window guard and windows "
                 "failed (errno %d); device creation fails\n",
                 errno);
      return -1;
   }
   /* The lease holds exactly `ranges`, guard first, whether this call
    * reserved them or another device in the process already had. */
   ws->m5_guard_reserved = guard_count != 0;
   if (guard_count != 0)
      d3dkmt_log_flushed(
              "D3DKMT: G17 driver-window guard [0x%llx, 0x%llx) held\n",
              (unsigned long long)ranges[0].base,
              (unsigned long long)(ranges[0].base + ranges[0].size));

   /*
    * CONTRACT.md SS3.3: AGX_BO_EXEC is a MAPPING protection, not an
    * allocation flag, and a bind op carries no execute bit -- so the winsys
    * derives it from the address. agx_open_device() has already chosen
    * shader_base and the 4 GiB USC heap above it (agx_device.c:596-612)
    * before it calls VM_CREATE, so this is the first and only place the
    * window is knowable.
    */
   ws->policy.usc_base = dev->shader_base;
   ws->policy.usc_size = AGX_D3DKMT_USC_SIZE;

   /* UpdateGpuVirtualAddress is defined against a real rendering context and
    * its monitored fence. Establish that interlock after the VA reservations
    * and USC policy exist, but before agx_open_device() performs its first
    * zero-page bind. */
   if (d3dkmt_va_update_init(ws) != 0) {
      /* Drop only this device's reference: another device in the process
       * may hold the same lease, and freeing the ranges directly would pull
       * its reservations (and the guard) out from under it. */
      int saved_errno = errno;
      d3dkmt_release_va_lease(ws);
      ws->m5_guard_reserved = false;
      errno = saved_errno;
      return -1;
   }

   /* One VM per device; the id is never interpreted by the kernel here. */
   vm->vm_id = 1;
   return 0;
}

static int
d3dkmt_queue_create(struct agx_device *dev, struct drm_asahi_queue_create *q)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   int ret = -1;

   simple_mtx_lock(&ws->lock);

   uint32_t slot = 0;
   while (slot < ws->ctx_capacity && ws->ctxs[slot].hContext != 0)
      slot++;

   if (slot == ws->ctx_capacity) {
      /* A realloc would invalidate a ctx pointer held by submit(). */
      if (ws->active_submits != 0) {
         errno = EBUSY;
         goto out;
      }
      uint32_t cap = ws->ctx_capacity ? ws->ctx_capacity * 2 : 4;
      struct agx_d3dkmt_ctx *ctxs =
         realloc(ws->ctxs, (size_t)cap * sizeof(*ctxs));
      if (ctxs == NULL) {
         errno = ENOMEM;
         goto out;
      }
      memset(ctxs + ws->ctx_capacity, 0,
             (size_t)(cap - ws->ctx_capacity) * sizeof(*ctxs));
      ws->ctxs = ctxs;
      ws->ctx_capacity = cap;
   }

   struct agx_d3dkmt_ctx *ctx = &ws->ctxs[slot];
   memset(ctx, 0, sizeof(*ctx));

   /* Per-context completion fence, used only for staging-ring recycling. */
   struct agx_kmt_createsyncobj2 fence = {
      .hDevice = ws->device,
      .Info = {.Type = AGX_KMT_SYNC_MONITORED_FENCE},
   };
   if (agx_d3dkmt_create_monitored_fence(ws, &fence, "queue context") != 0)
      goto out;
   agx_kmt_status st;

   struct agxw_context_private priv = {
      .version = AGXW_CONTEXT_PRIVATE_VERSION,
      .struct_size = sizeof(priv),
      .priority = q->priority,
      .usc_exec_base = q->usc_exec_base,
   };

   /* Pre-validate with the miniport's own validator rather than a private
    * copy of the rules, so a rejection here means the same thing it would
    * mean in the kernel (CONTRACT.md SS5.2). */
   if (agxw_context_private_validate_layout(
          &priv, sizeof(priv), ws->policy.usc_base, ws->policy.usc_size,
          &ws->policy.layout) != 0) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      errno = EINVAL;
      goto out;
   }

   struct agx_kmt_createcontextvirtual create = {
      .hDevice = ws->device,
      .NodeOrdinal = ws->policy.node_ordinal,
      .EngineAffinity = 1,
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };

   st = ws->fns.CreateContextVirtual(&create);
   if (!AGX_KMT_SUCCESS(st)) {
      struct agx_kmt_destroysyncobj d = {.hSyncObject = fence.hSyncObject};
      ws->fns.DestroySynchronizationObject(&d);
      ret = agx_d3dkmt_fail(st);
      goto out;
   }

   ctx->hContext = create.hContext;
   ctx->fence = fence.hSyncObject;
   ctx->fence_cpu =
      (const volatile uint64_t *)fence.Info.MonitoredFence
         .FenceValueCPUVirtualAddress;

   /* queue_id is a dense slot index, not the D3DKMT context handle: it is
    * stored in drm_asahi_submit::queue_id and looked up on every submit. */
   q->queue_id = slot + 1;
   ret = 0;

out:
   simple_mtx_unlock(&ws->lock);
   return ret;
}

static int
d3dkmt_queue_destroy(struct agx_device *dev, struct drm_asahi_queue_destroy *q)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   simple_mtx_lock(&ws->lock);
   struct agx_d3dkmt_ctx *ctx = ctx_get(ws, q->queue_id);
   if (ctx == NULL) {
      simple_mtx_unlock(&ws->lock);
      errno = EINVAL;
      return -1;
   }
   if (ctx->submit_refs != 0 || ctx->completion_unknown) {
      simple_mtx_unlock(&ws->lock);
      errno = EBUSY;
      return -1;
   }

   struct agx_d3dkmt_ctx local = *ctx;
   memset(ctx, 0, sizeof(*ctx));
   simple_mtx_unlock(&ws->lock);

   struct agx_kmt_destroycontext destroy = {.hContext = local.hContext};
   agx_kmt_status destroy_status = ws->fns.DestroyContext(&destroy);
   if (enum_log_enabled())
      d3dkmt_log_flushed(
              "NTAGX_ENUM destroy_context queue=%u context=0x%08x status=0x%08x\n",
              q->queue_id, (unsigned)local.hContext,
              (unsigned)destroy_status);

   for (unsigned i = 0; i < AGX_D3DKMT_STAGING_RING; ++i) {
      if (local.staging[i] != NULL)
         agx_bo_unreference(dev, local.staging[i]);
   }

   struct agx_kmt_destroysyncobj d = {.hSyncObject = local.fence};
   ws->fns.DestroySynchronizationObject(&d);
   return 0;
}

/*
 * GET_TIME. The second and last escape.
 *
 * D3DKMTQueryClockCalibration exists and is the obvious WDDM-native candidate,
 * but it reports an engine tick whose relationship to AGX's
 * command_timestamp_frequency_hz is unspecified, and Honeykrisp converts this
 * value with exactly that frequency (agx_gpu_timestamp_to_ns, agx_device.h).
 * The contract routes it through the escape so the KMD answers from the same
 * timebase source that serves the ioctl today, which is the property that
 * actually matters (CONTRACT.md SS8).
 */
static int
d3dkmt_get_time(struct agx_device *dev, struct drm_asahi_get_time *t)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);

   return d3dkmt_escape(ws, AGXW_ESC_GET_TIME, t, AGXW_GET_TIME_SIZE);
}

/*
 * GET_PARAMS group VM_STATUS (KMD side: escape op 3; agx_d3dkmt_core.h "VM
 * status"). The sticky error is per device, so the escape names ws->device
 * as well as the adapter. Reached only when the KMD advertised the feature
 * bit: hk_check_status asks only then, and agx_d3dkmt_query_vm_status
 * refuses otherwise without issuing anything.
 */
static int
d3dkmt_vm_status_escape(void *ctx, void *blob, uint32_t size)
{
   struct agx_d3dkmt_winsys *ws = ctx;
   struct agx_kmt_escape escape = {
      .hAdapter = ws->adapter,
      .hDevice = ws->device,
      .pPrivateDriverData = blob,
      .PrivateDriverDataSize = size,
   };

   agx_kmt_status st = ws->fns.Escape(&escape);
   if (!AGX_KMT_SUCCESS(st)) {
      agx_d3dkmt_fail(st);
      return -errno;
   }
   return 0;
}

static int
d3dkmt_get_params_group(struct agx_device *dev, struct drm_asahi_get_params *gp)
{
   int ret = agx_d3dkmt_query_vm_status(
      dev->params.features, gp->param_group, gp->size,
      (void *)(uintptr_t)gp->pointer, d3dkmt_vm_status_escape,
      agx_d3dkmt_ws(dev));
   if (ret != AGX_D3DKMT_OK) {
      errno = ret == AGX_D3DKMT_ERR_INVAL ? EINVAL : -ret;
      return -1;
   }
   return 0;
}

static int
d3dkmt_simple_ioctl(struct agx_device *dev, unsigned cmd, void *req)
{
   /* agx_create_command_queue_with_render_scs() encodes QUEUE_CREATE with the
    * pre-render-SCS struct size when it has no layout to pass. The WDDM
    * context-private record has no render SCS layout field, so a request
    * that carries one is refused rather than silently dropped. */
   const unsigned queue_create_legacy =
      _IOC(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE,
           DRM_COMMAND_BASE + DRM_ASAHI_QUEUE_CREATE,
           offsetof(struct drm_asahi_queue_create, render_scs_layout));
   if (cmd == queue_create_legacy)
      return d3dkmt_queue_create(dev, req);

   switch (cmd) {
   case DRM_IOCTL_ASAHI_VM_CREATE:
      return d3dkmt_vm_create(dev, req);
   case DRM_IOCTL_ASAHI_QUEUE_CREATE: {
      const struct drm_asahi_queue_create *q = req;
      if (q->render_scs_layout[0] != 0 || q->render_scs_layout[1] != 0) {
         errno = EINVAL;
         return -1;
      }
      return d3dkmt_queue_create(dev, req);
   }
   case DRM_IOCTL_ASAHI_QUEUE_DESTROY:
      return d3dkmt_queue_destroy(dev, req);
   case DRM_IOCTL_ASAHI_GET_TIME:
      return d3dkmt_get_time(dev, req);
   case DRM_IOCTL_ASAHI_GET_PARAMS:
      return d3dkmt_get_params_group(dev, req);
   default:
      errno = EINVAL;
      return -1;
   }
}

const agx_device_ops_t agx_device_d3dkmt_ops = {
   .bo_alloc = d3dkmt_bo_alloc,
   .bo_bind = d3dkmt_bo_bind,
   .bo_mmap = d3dkmt_bo_mmap,
   .get_params = d3dkmt_get_params,
   .submit = d3dkmt_submit,
   .bo_bind_object = d3dkmt_bo_bind_object,
   .bo_unbind_object = d3dkmt_bo_unbind_object,
   .simple_ioctl = d3dkmt_simple_ioctl,
   .bo_munmap = d3dkmt_bo_munmap,
   .bo_sync_for_device = d3dkmt_bo_sync_for_device,
   .bo_free = d3dkmt_bo_free,
};

/* -------------------------------------------------------------------------
 * Adapter enumeration and device open.
 * ------------------------------------------------------------------------- */
/*
 * Is this D3DKMT adapter ours?
 *
 * CONTRACT.md SS2.3 is explicit that VendorId/DeviceId are NOT reliable here:
 * the adapter is ACPI-enumerated (ACPI\NTAS0023), there is no PCI config
 * space, and whether dxgkrnl even calls DxgkDdiGetVendorAndDevice for a
 * non-PCI adapter is unproven. The identity blob is the only supported
 * discriminator, and getting it wrong means claiming another vendor's GPU.
 */
static bool
enum_log_enabled(void)
{
#if AGX_GPU_DIAG >= 2
   const char *value = getenv("NTAGX_ENUM_LOG");
   return value != NULL && value[0] != '\0' && value[0] != '0';
#else
   return false;
#endif
}

/* One line per process for an AppleAgxWddm adapter this driver cannot use,
 * whatever the diagnostic level: it is the answer to "why zero devices". */
static void
d3dkmt_report_unusable(unsigned reason, const char *fmt, ...)
{
   static uint32_t reported[4];
   if (reason >= ARRAY_SIZE(reported) || p_atomic_xchg(&reported[reason], 1u))
      return;

   /* One line in three writes, flushed once it is complete. */
   const int saved_errno = errno;
   va_list args;
   va_start(args, fmt);
   fprintf(stderr, "honeykrisp: AppleAgxWddm adapter not usable: ");
   vfprintf(stderr, fmt, args);
   fputc('\n', stderr);
   va_end(args);
   fflush(stderr);
   errno = saved_errno;
}

static bool
query_identity(const struct agx_d3dkmt_fns *fns, agx_kmt_handle adapter,
               struct agx_d3dkmt_policy *policy,
               struct agxw_adapter_identity *out)
{
   struct agxw_adapter_identity blob;

   memset(&blob, 0, sizeof(blob));

   struct agx_kmt_queryadapterinfo query = {
      .hAdapter = adapter,
      .Type = AGX_KMT_QAITYPE_UMDRIVERPRIVATE,
      .pPrivateDriverData = &blob,
      .PrivateDriverDataSize = sizeof(blob),
   };

   agx_kmt_status status = fns->QueryAdapterInfo(&query);
   if (!AGX_KMT_SUCCESS(status)) {
      if (enum_log_enabled())
         d3dkmt_log_flushed(
                 "NTAGX_ENUM identity adapter=0x%08x query=0x%08x\n",
                 (unsigned)adapter, (unsigned)status);
      return false;
   }

   int parsed =
      agx_d3dkmt_parse_identity(&blob, sizeof(blob), policy, out);
   if (parsed != AGX_D3DKMT_OK && blob.magic == AGXW_ADAPTER_IDENTITY_MAGIC &&
       blob.params_blob_size != AGX_D3DKMT_PARAMS_GLOBAL_SIZE) {
      d3dkmt_report_unusable(
         0, "it announces a %u-byte device parameter block (G%u%c, chip "
            "0x%04x); this driver needs the %u-byte layout with USC/HAL "
            "generations and memory limits",
         blob.params_blob_size, blob.gpu_generation,
         (char)blob.gpu_variant, blob.chip_id, AGX_D3DKMT_PARAMS_GLOBAL_SIZE);
   }
   if (enum_log_enabled())
      d3dkmt_log_flushed(
              "NTAGX_ENUM identity adapter=0x%08x query=0x%08x "
              "magic=0x%08x version=%u size=%u chip=0x%04x gen=%u "
              "variant=0x%02x revision=0x%02x contract=%u params=%u "
              "submit_private_max=%u parse=%d\n",
              (unsigned)adapter, (unsigned)status, blob.magic, blob.version,
              blob.struct_size, blob.chip_id, blob.gpu_generation,
              blob.gpu_variant, blob.gpu_revision, blob.contract_version,
              blob.params_blob_size, blob.submit_private_max_version, parsed);
   return parsed == AGX_D3DKMT_OK;
}

bool
agx_d3dkmt_enumerate_adapters(struct agx_kmt_luid *out, uint32_t capacity,
                              uint32_t *count)
{
   struct agx_d3dkmt_fns fns;
   struct agx_kmt_adapterinfo *infos = NULL;
   uint32_t found = 0;

   *count = 0;

   if (!agx_d3dkmt_load(&fns)) {
      if (enum_log_enabled())
         d3dkmt_log_flushed("NTAGX_ENUM load=failed\n");
      return false;
   }

   struct agx_kmt_enumadapters2 e = {0};
   agx_kmt_status status = fns.EnumAdapters2(&e);
   if (enum_log_enabled())
      d3dkmt_log_flushed("NTAGX_ENUM count status=0x%08x adapters=%u\n",
              (unsigned)status, e.NumAdapters);
   if (!AGX_KMT_SUCCESS(status))
      return false;
   if (e.NumAdapters == 0)
      return true;

   infos = calloc(e.NumAdapters, sizeof(*infos));
   if (infos == NULL)
      return false;

   e.pAdapters = infos;
   status = fns.EnumAdapters2(&e);
   if (enum_log_enabled())
      d3dkmt_log_flushed("NTAGX_ENUM fill status=0x%08x adapters=%u\n",
              (unsigned)status, e.NumAdapters);
   if (!AGX_KMT_SUCCESS(status)) {
      free(infos);
      return false;
   }

   for (uint32_t i = 0; i < e.NumAdapters; ++i) {
      struct agx_d3dkmt_policy policy;
      struct agxw_adapter_identity id;

      if (enum_log_enabled())
         d3dkmt_log_flushed(
                 "NTAGX_ENUM candidate=%u adapter=0x%08x "
                 "luid=%08x:%08x\n",
                 i, (unsigned)infos[i].hAdapter,
                 (unsigned)infos[i].AdapterLuid.HighPart,
                 (unsigned)infos[i].AdapterLuid.LowPart);

      /* A foreign adapter's UMDRIVERPRIVATE blob fails the magic/version/
       * contract-version check and is skipped. */
      if (!query_identity(&fns, infos[i].hAdapter, &policy, &id))
         continue;

      if (found < capacity)
         out[found] = infos[i].AdapterLuid;
      found++;
   }

   free(infos);
   *count = found;
   if (enum_log_enabled())
      d3dkmt_log_flushed("NTAGX_ENUM accepted=%u capacity=%u\n", found,
              capacity);
   return true;
}

static bool
d3dkmt_device_lease_acquire(struct agx_d3dkmt_winsys *ws)
{
   struct agx_d3dkmt_device_lease **link;
   struct agx_d3dkmt_device_lease *lease;
   struct agx_kmt_createdevice create = {.hAdapter = ws->adapter};
   agx_kmt_status status;

   AcquireSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   for (link = &agx_d3dkmt_device_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }
   lease = *link;
   if (lease != NULL) {
      if (lease->references == 0 || lease->references == UINT32_MAX) {
         ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
         errno = EIO;
         return false;
      }
      lease->references++;
      ws->device = lease->device;
      ws->device_lease_held = true;
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      return true;
   }

   lease = calloc(1, sizeof(*lease));
   if (lease == NULL) {
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      errno = ENOMEM;
      return false;
   }
   status = ws->fns.CreateDevice(&create);
   if (!AGX_KMT_SUCCESS(status)) {
      free(lease);
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      agx_d3dkmt_fail(status);
      return false;
   }
   lease->luid = ws->luid;
   lease->device = create.hDevice;
   lease->references = 1;
   lease->next = agx_d3dkmt_device_leases;
   agx_d3dkmt_device_leases = lease;
   ws->device = create.hDevice;
   ws->device_lease_held = true;
   ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   return true;
}

static void
d3dkmt_device_lease_release(struct agx_d3dkmt_winsys *ws)
{
   struct agx_d3dkmt_device_lease **link;
   struct agx_d3dkmt_device_lease *lease;

   if (!ws->device_lease_held)
      return;
   AcquireSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   for (link = &agx_d3dkmt_device_leases; *link != NULL;
        link = &(*link)->next) {
      if (d3dkmt_luid_equal((*link)->luid, ws->luid))
         break;
   }
   lease = *link;
   if (lease == NULL || lease->device != ws->device ||
       lease->references == 0) {
      d3dkmt_log_flushed("D3DKMT device lease invariant failure\n");
      ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
      ws->device_lease_held = false;
      return;
   }
   lease->references--;
   if (lease->references == 0) {
      struct agx_kmt_destroydevice destroy = {.hDevice = lease->device};
      agx_kmt_status status = ws->fns.DestroyDevice(&destroy);

      if (AGX_KMT_SUCCESS(status)) {
         *link = lease->next;
         free(lease);
      } else {
         d3dkmt_log_flushed("D3DKMTDestroyDevice failed status=0x%08x\n",
                 (unsigned)status);
      }
   }
   ReleaseSRWLockExclusive(&agx_d3dkmt_device_lease_lock);
   ws->device_lease_held = false;
}

bool
agx_d3dkmt_open_device_rt(struct agx_device *dev, const struct agx_kmt_luid *luid,
                          const void *rt_bridge)
{
   struct agx_d3dkmt_winsys *ws = calloc(1, sizeof(*ws));
   if (ws == NULL)
      return false;

   if (!agx_d3dkmt_load(&ws->fns))
      goto fail_alloc;

   ws->policy = agx_d3dkmt_policy_default();
   ws->luid = *luid;

   struct agx_kmt_openadapterfromluid open = {.AdapterLuid = *luid};
   if (!AGX_KMT_SUCCESS(ws->fns.OpenAdapterFromLuid(&open)))
      goto fail_alloc;
   ws->adapter = open.hAdapter;

   struct agxw_adapter_identity identity;
   if (!query_identity(&ws->fns, ws->adapter, &ws->policy, &identity))
      goto fail_adapter;

   /* GET_PARAMS is an escape (CONTRACT.md SS8) and its payload never
    * changes, so it is fetched once here and agx_open_device()'s
    * ops.get_params() copies out of this. */
   ws->params_size = identity.params_blob_size;
   ws->params = malloc(ws->params_size);
   if (ws->params == NULL)
      goto fail_adapter;
   int params_status =
      d3dkmt_escape(ws, AGXW_ESC_GET_PARAMS, ws->params, ws->params_size);
   if (enum_log_enabled())
      d3dkmt_log_flushed("NTAGX_ENUM get_params result=%d errno=%d size=%u\n",
              params_status, errno, ws->params_size);
   if (params_status != 0) {
      d3dkmt_report_unusable(
         1, "GET_PARAMS failed (errno %d) for G%u%c chip 0x%04x; the KMD "
            "publishes no device parameters for this GPU",
         errno, identity.gpu_generation, (char)identity.gpu_variant,
         identity.chip_id);
      goto fail_params;
   }

   /* parse_identity pinned params_blob_size to this exact layout. */
   struct drm_asahi_params_global *params = ws->params;
   static_assert(AGX_D3DKMT_PARAMS_GLOBAL_SIZE == sizeof(*params),
                 "identity-announced params layout");
   if (!agx_d3dkmt_params_match_identity(&identity, params->chip_id,
                                         params->gpu_generation,
                                         params->gpu_variant,
                                         params->gpu_revision)) {
      d3dkmt_report_unusable(
         2, "GET_PARAMS describes chip 0x%04x G%u variant 0x%02x revision "
            "0x%02x but the adapter identity says chip 0x%04x G%u variant "
            "0x%02x revision 0x%02x",
         params->chip_id, params->gpu_generation, params->gpu_variant,
         params->gpu_revision, identity.chip_id, identity.gpu_generation,
         identity.gpu_variant, identity.gpu_revision);
      errno = ENODEV;
      goto fail_params;
   }

   /* Every bind, read range, USC window and context/submit record is checked
    * against the user VA this reply publishes: the 42-bit J813 window minus
    * its fixed driver range, or the fixed G14 window before G17. */
   if (agxw_va_layout_from_params(params->gpu_generation, params->vm_start,
                                  params->vm_end, params->vm_kernel_min_size,
                                  &ws->policy.layout) != 0) {
      d3dkmt_report_unusable(
         3, "GET_PARAMS VM window [0x%llx, 0x%llx) with a 0x%llx-byte driver "
            "range is not a well-formed J813 layout",
         (unsigned long long)params->vm_start,
         (unsigned long long)params->vm_end,
         (unsigned long long)params->vm_kernel_min_size);
      errno = ENODEV;
      goto fail_params;
   }

   /* G17: no monitored fence before VM_CREATE has reserved the driver-window
    * guard (agx_d3dkmt_create_monitored_fence, d3dkmt_vm_create). Decided
    * from the same params agx_open_device() derives dev->chip from;
    * d3dkmt_vm_create() refuses if the two ever disagree. */
   ws->m5_guard_required = agx_chip_g17_m5(agx_chip_from_params(params));
   /* This current-Mesa runtime port audits the J813/M5 layout. Native AGX3
    * has mutable fixed context/low-slot aliases requiring a distinct lifetime
    * contract, so it must not inherit the legacy content-defined alias model. */
   if ((rt_bridge != NULL || agx_d3dkmt_bridge_enabled()) &&
       (!ws->m5_guard_required || identity.chip_id != 0x8142 ||
        identity.gpu_generation != 17 || identity.gpu_variant != 'G'))
      goto fail_params;


   /* Hide every feature whose interface this winsys does not translate, so
    * the driver takes the paths that only need the verbatim command stream.
    * VM_STATUS stays when a G17 KMD advertises it: d3dkmt_simple_ioctl
    * answers its GET_PARAMS group through the VM_STATUS escape. */
   params->features =
      agx_d3dkmt_published_features(params->features, ws->m5_guard_required);
   /* max_commands_per_submission is the KMD's, unmodified: like the M5
    * Linux kernel, the G17 KMD publishes 1, and hk's queue_submit_looped()
    * then issues one SubmitCommand per hardware command in stream order,
    * with the input waits on the first and the output signals on the last
    * (d3dkmt_submit() places them exactly where the ioctl carries
    * them). The winsys neither splits nor rewrites the stream. */

   bool device_status =
      rt_bridge != NULL
         ? agx_d3dkmt_bridge_install_direct(ws, rt_bridge)
         : (agx_d3dkmt_bridge_enabled() ? agx_d3dkmt_bridge_install(ws)
                                        : d3dkmt_device_lease_acquire(ws));
   if (enum_log_enabled())
      d3dkmt_log_flushed(
              "NTAGX_ENUM create_device status=%u device=0x%08x shared=%u\n",
              device_status ? 0u : 1u, (unsigned)ws->device,
              (unsigned)ws->device_lease_held);
   if (!device_status)
      goto fail_params;

   struct agx_kmt_createpagingqueue pq = {.hDevice = ws->device};
   agx_kmt_status paging_status = ws->fns.CreatePagingQueue(&pq);
   if (enum_log_enabled())
      d3dkmt_log_flushed(
              "NTAGX_ENUM paging_queue status=0x%08x device=0x%08x "
              "queue=0x%08x fence=0x%08x cpu=%p\n",
              (unsigned)paging_status, (unsigned)ws->device,
              (unsigned)pq.hPagingQueue, (unsigned)pq.hSyncObject,
              pq.FenceValueCPUVirtualAddress);
   if (!AGX_KMT_SUCCESS(paging_status))
      goto fail_device;
   ws->paging_queue = pq.hPagingQueue;
   ws->paging_fence = pq.hSyncObject;
   ws->paging_fence_cpu =
      (const volatile uint64_t *)pq.FenceValueCPUVirtualAddress;

   simple_mtx_init(&ws->lock, mtx_plain);
   agx_d3dkmt_pool_init(&ws->pool);
   agx_d3dkmt_sync_table_init(&ws->syncs);

   dev->hdev = ws;
   dev->fd = -1;
   dev->is_virtio = false;
   dev->ops = agx_device_d3dkmt_ops;
   dev->write_set_contract = true;
   /* agxw_agreement.h, section 2 (WHEN v6) and section 3: the shader window
    * this driver lays out is the one the kernel and our D3D11 DLL use. */
   static_assert(AGX_M5_SHADER_BASE == AGXW_J813_USC_EXEC_BASE,
                 "agreement: J813 shader window base");
   static_assert(AGX_G17P_USC_WINDOW_BASE == AGXW_J813_USC_EXEC_BASE,
                 "agreement: G17 USC window base");
   dev->primary_write_contract = identity.chip_id == AGXW_AGREE_J813_CHIP_ID &&
      identity.gpu_generation == AGXW_AGREE_J813_GPU_GENERATION &&
      identity.gpu_variant == AGXW_AGREE_J813_GPU_VARIANT &&
      identity.submit_private_max_version == AGXW_AGREE_SUBMIT_VERSION;
   ws->v6_log = dev->primary_write_contract;
   d3dkmt_log_flushed("agxw-v6: package %s\n", AGXW_PACKAGE_VERSION_STRING);
   d3dkmt_log_flushed("agxw-v6: device contract=%s chip=0x%x gen=%u variant=%c max_version=%u\n",
                      dev->primary_write_contract ? "v6" : "off",
                      (unsigned)identity.chip_id, (unsigned)identity.gpu_generation,
                      identity.gpu_variant >= 0x20 && identity.gpu_variant < 0x7f
                         ? (int)identity.gpu_variant : '?',
                      (unsigned)identity.submit_private_max_version);
   if (enum_log_enabled())
      d3dkmt_log_flushed("NTAGX_ENUM open_device=ready\n");
   return true; /* caller then runs agx_open_device() */

fail_device:;
   agx_d3dkmt_bridge_wait_teardown(ws);
   agx_d3dkmt_bridge_uninstall(ws);
   d3dkmt_device_lease_release(ws);
fail_params:
   free(ws->params);
fail_adapter:;
   struct agx_kmt_closeadapter ca = {.hAdapter = ws->adapter};
   ws->fns.CloseAdapter(&ca);
fail_alloc:
   free(ws);
   return false;
}

bool
agx_d3dkmt_open_device(struct agx_device *dev, const struct agx_kmt_luid *luid)
{
   return agx_d3dkmt_open_device_rt(dev, luid, NULL);
}

void
agx_d3dkmt_close_device(struct agx_device *dev)
{
   struct agx_d3dkmt_winsys *ws = agx_d3dkmt_ws(dev);
   if (ws == NULL)
      return;

   agx_d3dkmt_bridge_wait_teardown(ws);

   /* Adapter teardown is the forced-retirement boundary for a transfer
    * context or render context whose completion became unknowable. */
   if (ws->xfer_ctx != 0) {
      struct agx_kmt_destroycontext dc = {.hContext = ws->xfer_ctx};
      agx_kmt_status status = ws->fns.DestroyContext(&dc);
      if (enum_log_enabled())
         d3dkmt_log_flushed(
                 "NTAGX_ENUM destroy_context xfer context=0x%08x status=0x%08x\n",
                 (unsigned)dc.hContext, (unsigned)status);
      ws->xfer_ctx = 0;
   }

   for (uint32_t i = 0; i < ws->ctx_capacity; ++i) {
      if (ws->ctxs[i].hContext == 0)
         continue;

      struct agx_kmt_destroycontext dc = {.hContext = ws->ctxs[i].hContext};
      agx_kmt_status status = ws->fns.DestroyContext(&dc);
      if (enum_log_enabled())
         d3dkmt_log_flushed(
                 "NTAGX_ENUM destroy_context close slot=%u context=0x%08x status=0x%08x\n",
                 i, (unsigned)dc.hContext, (unsigned)status);

      for (unsigned slot = 0; slot < AGX_D3DKMT_STAGING_RING; ++slot) {
         if (ws->ctxs[i].staging[slot] != NULL)
            agx_bo_unreference(dev, ws->ctxs[i].staging[slot]);
      }

      struct agx_kmt_destroysyncobj ds = {.hSyncObject = ws->ctxs[i].fence};
      ws->fns.DestroySynchronizationObject(&ds);
   }
   free(ws->ctxs);

   if (ws->va_context != 0) {
      struct agx_kmt_destroycontext dc = {.hContext = ws->va_context};
      ws->fns.DestroyContext(&dc);
      ws->va_context = 0;
   }
   if (ws->va_fence != 0) {
      struct agx_kmt_destroysyncobj ds = {.hSyncObject = ws->va_fence};
      ws->fns.DestroySynchronizationObject(&ds);
      ws->va_fence = 0;
   }

   d3dkmt_release_va_lease(ws);

   struct agx_kmt_destroypagingqueue dpq = {.hPagingQueue = ws->paging_queue};
   ws->fns.DestroyPagingQueue(&dpq);

   agx_d3dkmt_bridge_uninstall(ws);
   d3dkmt_device_lease_release(ws);

   struct agx_kmt_closeadapter ca = {.hAdapter = ws->adapter};
   ws->fns.CloseAdapter(&ca);

   agx_d3dkmt_pool_finish(&ws->pool);
   agx_d3dkmt_sync_table_finish(&ws->syncs);
   simple_mtx_destroy(&ws->lock);
   free(ws->params);
   free(ws);

   dev->hdev = NULL;
}

struct agx_kmt_luid
agx_d3dkmt_device_luid(const struct agx_device *dev)
{
   const struct agx_d3dkmt_winsys *ws =
      (const struct agx_d3dkmt_winsys *)dev->hdev;

   return ws ? ws->luid : (struct agx_kmt_luid){0, 0};
}
