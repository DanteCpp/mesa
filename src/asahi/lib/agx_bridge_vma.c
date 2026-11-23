/*
 * Copyright 2026 the Asahi Linux contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_bridge_vma.h"
#include "agx_va_runtime_hole.h"
#include "agx_bridge_vma_core.h"

#include "agx_bo.h"     /* enum agx_va_flags */
#include "agx_device.h" /* struct agx_device, util_vma_heap */
#include "agx_device_d3dkmt.h" /* agx_device_d3dkmt_ops */
#include "agx_d3dkmt_bridge.h" /* enabled(), rtdevice() */

#include "util/vma.h"

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AGX_GPU_DIAG
#define AGX_GPU_DIAG 0
#endif

/* One arena per process/adapter GPU-VA root. This module's globals are already
 * process-local; WDDM shares TTBR0 across every runtime device on an adapter.
 * Keep a few slots for processes which open several physical adapters. */
#define AGX_BRIDGE_ARENA_MAX 8u
/* One registration per live agx_device (a device may open >1 winsys). */
#define AGX_BRIDGE_REG_MAX 256u
struct agx_bridge_arena {
   bool in_use;
   uint64_t vm_key; /* process-local adapter LUID = shared TTBR0 key */
   uint32_t refs;  /* registered devices on this arena */
   uint64_t main_start, main_size, usc_start, usc_size;
   uint64_t guard_size, sparse_ro_offset;
   struct util_vma_heap main_heap;
   struct util_vma_heap usc_heap;
   struct util_vma_heap replay_heap;
   enum agx_chip chip;
   bool replay_initialized;
   struct agx_bridge_bootstrap_core bootstrap;
   SRWLOCK lock; /* guards heaps and fixed-alias transactions */
   CONDITION_VARIABLE bootstrap_changed;
   DWORD bootstrap_owner;
   const struct agx_device *bootstrap_retiring;
   const struct agx_device *bootstrap_callback;
   volatile LONG poisoned; /* uncertain unbind: never admit a new heap user */
};

struct agx_bridge_reg {
   const struct agx_device *dev;
   unsigned arena;
   bool in_use;
};

/* Keep the claim transaction exclusive while allowing the runtime callback
 * to run with no arena lock held. A same-thread nested bootstrap operation is
 * rejected instead of deadlocking; ordinary allocator callbacks may reenter. */
static bool
bootstrap_begin(struct agx_bridge_arena *a)
{
   AcquireSRWLockExclusive(&a->lock);
   while (a->bootstrap_owner != 0) {
      if (a->bootstrap_owner == GetCurrentThreadId()) {
         ReleaseSRWLockExclusive(&a->lock);
         errno = EBUSY;
         return false;
      }
      SleepConditionVariableSRW(&a->bootstrap_changed, &a->lock, INFINITE, 0);
   }
   a->bootstrap_owner = GetCurrentThreadId();
   return true; /* lock held until callback or bootstrap_end */
}

static void
bootstrap_end(struct agx_bridge_arena *a)
{
   a->bootstrap_retiring = NULL;
   a->bootstrap_callback = NULL;
   a->bootstrap_owner = 0;
   WakeAllConditionVariable(&a->bootstrap_changed);
   ReleaseSRWLockExclusive(&a->lock);
}

static int
bootstrap_bind_unlocked(struct agx_bridge_arena *a, struct agx_device *dev,
                        struct agx_bo *bo, uint64_t addr, uint64_t size,
                        uint64_t offset, unsigned flags)
{
   a->bootstrap_callback = dev;
   ReleaseSRWLockExclusive(&a->lock);
   int ret = agx_bo_bind(dev, bo, addr, size, offset, flags);
   AcquireSRWLockExclusive(&a->lock);
   a->bootstrap_callback = NULL;
   return ret;
}

static SRWLOCK g_lock = SRWLOCK_INIT; /* guards g_arena[] and g_reg[] */
static struct agx_bridge_arena g_arena[AGX_BRIDGE_ARENA_MAX];
static struct agx_bridge_reg g_reg[AGX_BRIDGE_REG_MAX];

/* ------------------------------------------------------------------------ */
/* Diagnostic log (console-free).                                            */
/* ------------------------------------------------------------------------ */
/* Is the VA/allocation/queue trace log switched on?
 *
 * This writes a formatted line -- CreateDirectoryA, CreateFileA, WriteFile,
 * CloseHandle -- to C:\ProgramData\AuroraSilicon\honeykrisp-va.log on EVERY
 * allocation, queue and VMA event.  That is four synchronous filesystem calls
 * per event on the runtime's hot path.
 *
 * It is compiled out below diagnostics level 2.  Even in a level-2 build the
 * environment variable must explicitly opt in; an absent variable can never
 * turn synchronous hot-path file I/O on. Read once and cached. */
bool
agx_bridge_valog_enabled(void)
{
#if AGX_GPU_DIAG < 2
   return false;
#else
   /* 0 = unknown, 1 = on, -1 = off.  Racing threads can only ever compute
    * the same answer, so an unsynchronised cache is safe here. */
   static int cached;

   if (cached == 0) {
      const char *v = getenv("HONEYKRISP_VA_LOG");

      cached = (v != NULL && v[0] == '1' && v[1] == '\0') ? 1 : -1;
   }
   return cached > 0;
#endif
}

void
agx_bridge_valog(const char *fmt, ...)
{
   char body[512];
   char line[640];
   va_list ap;

   if (!agx_bridge_valog_enabled())
      return;

   va_start(ap, fmt);
   int n = vsnprintf(body, sizeof(body), fmt, ap);
   va_end(ap);
   if (n < 0)
      return;

   SYSTEMTIME t;
   GetSystemTime(&t);
   int m = snprintf(line, sizeof(line),
                    "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu %s\n",
                    (unsigned)t.wYear, (unsigned)t.wMonth, (unsigned)t.wDay,
                    (unsigned)t.wHour, (unsigned)t.wMinute,
                    (unsigned)t.wSecond, (unsigned)t.wMilliseconds,
                    (unsigned long)GetCurrentProcessId(), body);
   if (m < 0)
      return;
   const DWORD bytes =
      (DWORD)((size_t)m < sizeof(line) ? (size_t)m : sizeof(line) - 1);

   const char dir[] = "C:\\ProgramData\\AuroraSilicon";
   const char path[] = "C:\\ProgramData\\AuroraSilicon\\honeykrisp-va.log";
   if (!CreateDirectoryA(dir, NULL) &&
       GetLastError() != ERROR_ALREADY_EXISTS)
      return;
   HANDLE f = CreateFileA(path, FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE,
                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (f == INVALID_HANDLE_VALUE)
      return;
   DWORD w;
   (void)WriteFile(f, line, bytes, &w, NULL);
   CloseHandle(f);
}

/* ------------------------------------------------------------------------ */
/* Registry helpers (caller holds g_lock).                                   */
/* ------------------------------------------------------------------------ */
static int
arena_find(uint64_t vm_key)
{
   for (unsigned i = 0; i < AGX_BRIDGE_ARENA_MAX; ++i)
      if (g_arena[i].in_use && g_arena[i].vm_key == vm_key)
         return (int)i;
   return -1;
}

static int
reg_find(const struct agx_device *dev)
{
   for (unsigned i = 0; i < AGX_BRIDGE_REG_MAX; ++i)
      if (g_reg[i].in_use && g_reg[i].dev == dev)
         return (int)i;
   return -1;
}

static struct agx_bridge_arena *
arena_of(const struct agx_device *dev)
{
   struct agx_bridge_arena *a = NULL;
   AcquireSRWLockShared(&g_lock);
   int ri = reg_find(dev);
   if (ri >= 0)
      a = &g_arena[g_reg[ri].arena];
   ReleaseSRWLockShared(&g_lock);
   return a;
}

/* ------------------------------------------------------------------------ */
/* Public API.                                                               */
/* ------------------------------------------------------------------------ */
bool
agx_bridge_vma_register(struct agx_device *dev, uint64_t main_start,
                        uint64_t main_size, uint64_t usc_start,
                        uint64_t usc_size)
{
   if (dev == NULL)
      return true;
   /* Only d3dkmt devices carry a bridge winsys; never cast another backend's
    * hdev. */
   if (dev->ops.simple_ioctl != agx_device_d3dkmt_ops.simple_ioctl)
      return true;
   /* Direct-installed native bridge devices (AuroraD3D11) have rt != NULL
    * without requiring HONEYKRISP_RT_DEVICE in the environment, and still
    * share the process TTBR0 across every D3D11 device in the process. */
   const void *rt =
      agx_d3dkmt_bridge_rtdevice((const struct agx_d3dkmt_winsys *)dev->hdev);
   if (rt == NULL)
      return !agx_d3dkmt_bridge_enabled();
   const uint64_t vm_key = agx_d3dkmt_bridge_vm_key(
      (const struct agx_d3dkmt_winsys *)dev->hdev);
   if (vm_key == 0 || main_size == 0 || usc_size == 0) {
      agx_bridge_valog(
         "VMA register REFUSED dev=%p rt=%p vm=0x%016llx invalid bounds",
         (void *)dev, rt, (unsigned long long)vm_key);
      return false;
   }

   /* Once per process: stamp the build into the append-only log, so lines
    * from different deployed builds can never be conflated again (the 0.1.337
    * "52 collisions" were 0.1.336-era lines in the same file). */
   static LONG epoch_logged;
   if (InterlockedCompareExchange(&epoch_logged, 1, 0) == 0)
      agx_bridge_valog("VMA EPOCH built=" __DATE__ " " __TIME__
                       " model=defer-handover-v2");

   AcquireSRWLockExclusive(&g_lock);
   if (reg_find(dev) >= 0) { /* idempotent */
      ReleaseSRWLockExclusive(&g_lock);
      return true;
   }
   int ri = -1;
   for (unsigned i = 0; i < AGX_BRIDGE_REG_MAX; ++i)
      if (!g_reg[i].in_use) {
         ri = (int)i;
         break;
      }
   if (ri < 0) {
      ReleaseSRWLockExclusive(&g_lock);
      agx_bridge_valog("VMA reg table full; dev=%p not shared (collision risk)",
                       (void *)dev);
      return false;
   }

   int ai = arena_find(vm_key);
   if (ai < 0) {
      for (unsigned i = 0; i < AGX_BRIDGE_ARENA_MAX; ++i)
         if (!g_arena[i].in_use) {
            ai = (int)i;
            break;
         }
      if (ai < 0) {
         ReleaseSRWLockExclusive(&g_lock);
         agx_bridge_valog("VMA arena table full; dev=%p rt=%p not shared",
                          (void *)dev, rt);
         return false;
      }
      struct agx_bridge_arena *a = &g_arena[ai];
      memset(a, 0, sizeof(*a));
      a->in_use = true;
      a->vm_key = vm_key;
      a->chip = dev->chip;
      a->guard_size = dev->guard_size;
      a->sparse_ro_offset = dev->sparse_ro_offset;
      a->main_start = main_start;
      a->main_size = main_size;
      a->usc_start = usc_start;
      a->usc_size = usc_size;
      util_vma_heap_init(&a->main_heap, main_start, main_size);
      util_vma_heap_init(&a->usc_heap, usc_start, usc_size);
      bool layout_ok = agx_va_reserve_runtime_graph_hole(
         dev->chip, &a->main_heap, main_start, main_size, dev->sparse_ro_offset,
         &a->usc_heap, usc_start, usc_size);
      if (layout_ok && (agx_chip_native_agx3(dev->chip) || agx_chip_g17_m5(dev->chip))) {
         a->replay_initialized = agx_capture_replay_heap_init(
            &a->main_heap, &a->replay_heap, main_size);
         layout_ok = a->replay_initialized;
      }
      /* Stage-helper page is an invariant fixed alias, not a dynamic claim.
       * Reserve it once in the shared USC heap; per-device bootstrap ownership
       * controls its backing and handover. */
      if (layout_ok && (agx_chip_native_agx3(dev->chip) ||
                        agx_chip_g17_m5(dev->chip) || dev->chip == AGX_CHIP_G18G))
         layout_ok = util_vma_heap_alloc_addr(&a->usc_heap, usc_start,
                                               AIL_PAGESIZE + dev->guard_size);
      if (!layout_ok) {
         if (a->replay_initialized) util_vma_heap_finish(&a->replay_heap);
         util_vma_heap_finish(&a->usc_heap);
         util_vma_heap_finish(&a->main_heap);
         memset(a, 0, sizeof(*a));
         ReleaseSRWLockExclusive(&g_lock);
         return false;
      }

      agx_bridge_valog("VMA arena CREATE vm=0x%016llx main=[0x%llx+0x%llx] "
                       "usc=[0x%llx+0x%llx]",
                       (unsigned long long)vm_key,
                       (unsigned long long)main_start,
                       (unsigned long long)main_size,
                       (unsigned long long)usc_start,
                       (unsigned long long)usc_size);
   } else {
      struct agx_bridge_arena *a = &g_arena[ai];
      if (InterlockedCompareExchange(&a->poisoned, 0, 0) != 0 || a->chip != dev->chip || a->guard_size != dev->guard_size ||
           a->sparse_ro_offset != dev->sparse_ro_offset || a->main_start != main_start || a->main_size != main_size ||
          a->usc_start != usc_start || a->usc_size != usc_size) {
         agx_bridge_valog("VMA arena BOUNDS MISMATCH vm=0x%016llx have "
                          "main=[0x%llx+0x%llx] got main=[0x%llx+0x%llx]",
                          (unsigned long long)vm_key,
                          (unsigned long long)a->main_start,
                          (unsigned long long)a->main_size,
                          (unsigned long long)main_start,
                          (unsigned long long)main_size);
         ReleaseSRWLockExclusive(&g_lock);
         return false;
      }
   }

   g_reg[ri].dev = dev;
   g_reg[ri].arena = (unsigned)ai;
   g_reg[ri].in_use = true;
   g_arena[ai].refs++;
   uint32_t refs = g_arena[ai].refs;
   ReleaseSRWLockExclusive(&g_lock);

   agx_bridge_valog(
      "VMA register dev=%p rt=%p vm=0x%016llx arena=%d devices=%u",
      (void *)dev, rt, (unsigned long long)vm_key, ai, refs);
   return true;
}

void
agx_bridge_vma_unregister(struct agx_device *dev)
{
   if (dev == NULL)
      return;

   /* Defensive ownership boundary. agx_device.c calls this explicitly before
    * releasing its BOs; doing it here as well makes an early-open failure or
    * future caller fail safe instead of leaving a fixed alias behind. */
   agx_bridge_vma_bootstrap_unbind_all(dev);

   AcquireSRWLockExclusive(&g_lock);
   int ri = reg_find(dev);
   if (ri >= 0) {
      unsigned ai = g_reg[ri].arena;
      memset(&g_reg[ri], 0, sizeof(g_reg[ri]));
      if (ai < AGX_BRIDGE_ARENA_MAX && g_arena[ai].in_use &&
          g_arena[ai].refs > 0) {
         if (--g_arena[ai].refs == 0) {
            if (InterlockedCompareExchange(&g_arena[ai].poisoned, 0, 0) != 0) {
               /* Keep this process/root quarantined even after its final
                * device leaves; clear stale owner pointers, retain VA claims. */
               memset(&g_arena[ai].bootstrap, 0, sizeof(g_arena[ai].bootstrap));
               ReleaseSRWLockExclusive(&g_lock);
               return;
            }
            if (g_arena[ai].replay_initialized)
               util_vma_heap_finish(&g_arena[ai].replay_heap);
            util_vma_heap_finish(&g_arena[ai].main_heap);
            util_vma_heap_finish(&g_arena[ai].usc_heap);
            const uint64_t vm_key = g_arena[ai].vm_key;
            memset(&g_arena[ai], 0, sizeof(g_arena[ai]));
            agx_bridge_valog("VMA arena DESTROY vm=0x%016llx",
                             (unsigned long long)vm_key);
         }
      }
   }
   ReleaseSRWLockExclusive(&g_lock);
}

bool
agx_bridge_vma_registered(const struct agx_device *dev)
{
   if (dev == NULL)
      return false;
   AcquireSRWLockShared(&g_lock);
   bool r = reg_find(dev) >= 0;
   ReleaseSRWLockShared(&g_lock);
   return r;
}

/* Teardown may not return to a caller that frees BOs while an enclosing
 * callback still owns their fixed-alias transaction. Foreign threads wait for
 * the actual transaction; same-thread teardown of a claimant is an invalid
 * reentry and deliberately preserves the object/stack until process exit.
 * There is no timeout that turns this into successful destruction. */
void
agx_bridge_vma_wait_teardown(struct agx_device *dev)
{
   if (dev && dev->ops.simple_ioctl == agx_device_d3dkmt_ops.simple_ioctl)
      agx_d3dkmt_bridge_wait_teardown(dev->hdev);
   struct agx_bridge_arena *a = arena_of(dev);
   if (!a)
      return;
   AcquireSRWLockExclusive(&a->lock);
   while (a->bootstrap_owner != 0) {
      bool owns_alias = a->bootstrap_retiring == dev || a->bootstrap_callback == dev;
      for (unsigned i = 0; i < AGX_BRIDGE_BOOTSTRAP_MAX; ++i) {
         struct agx_bridge_bootstrap_mapping *m = &a->bootstrap.map[i];
         if (m->in_use && agx_bridge_bootstrap_backing_of(
                &a->bootstrap, m->addr, m->size, dev) != NULL)
            owns_alias = true;
      }
      /* A failed nested open which never claimed an alias has no backing
       * participating in the enclosing transaction and may clean up. */
      if (!owns_alias)
         break;
      if (a->bootstrap_owner == GetCurrentThreadId()) {
         InterlockedExchange(&a->poisoned, 1);
         ReleaseSRWLockExclusive(&a->lock);
         fprintf(stderr, "agx bridge: preserving reentrant fixed-alias teardown; "
                         "process cleanup required\n");
         for (;;)
            Sleep(1000);
      }
      SleepConditionVariableSRW(&a->bootstrap_changed, &a->lock, INFINITE, 0);
   }
   ReleaseSRWLockExclusive(&a->lock);
}

bool
agx_bridge_vma_winsys_healthy(const struct agx_d3dkmt_winsys *ws)
{
   const uint64_t key = agx_d3dkmt_bridge_vm_key(ws);
   AcquireSRWLockShared(&g_lock);
   int ai = arena_find(key);
   bool healthy = ai < 0 || InterlockedCompareExchange(&g_arena[ai].poisoned, 0, 0) == 0;
   ReleaseSRWLockShared(&g_lock);
   return healthy;
}

uint64_t
agx_bridge_vma_alloc(struct agx_device *dev, uint64_t size_B, uint64_t align_B,
                     unsigned flags, uint64_t fixed_va)
{
   struct agx_bridge_arena *a = arena_of(dev);
   if (a == NULL)
      return 0;

   const bool usc = (flags & AGX_VA_USC) != 0;
   if ((flags & AGX_VA_CAPTURE_REPLAY) && !a->replay_initialized)
      return 0;
   struct util_vma_heap *heap = usc ? &a->usc_heap :
      (flags & AGX_VA_CAPTURE_REPLAY) ? &a->replay_heap : &a->main_heap;
   uint64_t addr = 0;

   AcquireSRWLockExclusive(&a->lock);
   if (InterlockedCompareExchange(&a->poisoned, 0, 0) != 0) {
      ReleaseSRWLockExclusive(&a->lock);
      return 0;
   }
   if (flags & AGX_VA_FIXED) {
      if (util_vma_heap_alloc_addr(heap, fixed_va, size_B)) {
         addr = fixed_va;
      } else {
         agx_bridge_valog(
            "VMA FIXED COLLISION addr=0x%llx size=0x%llx usc=%d "
            "vm=0x%016llx dev=%p (capture-replay/sparse claim refused)",
            (unsigned long long)fixed_va, (unsigned long long)size_B,
            usc ? 1 : 0, (unsigned long long)a->vm_key, (void *)dev);
      }
   } else {
      const bool high = heap->alloc_high;
      if (flags & AGX_VA_LOW) heap->alloc_high = false;
      addr = util_vma_heap_alloc(heap, size_B, align_B);
      heap->alloc_high = high;
      if (addr == 0)
         agx_bridge_valog(
            "VMA %s alloc FAIL size=0x%llx align=0x%llx heap=[0x%llx+0x%llx] "
            "vm=0x%016llx",
            usc ? "USC" : "MAIN", (unsigned long long)size_B,
            (unsigned long long)align_B,
            (unsigned long long)(usc ? a->usc_start : a->main_start),
            (unsigned long long)(usc ? a->usc_size : a->main_size),
            (unsigned long long)a->vm_key);
   }
   ReleaseSRWLockExclusive(&a->lock);
   return addr;
}

void
agx_bridge_vma_free(struct agx_device *dev, uint64_t addr, uint64_t size_B,
                    unsigned flags)
{
   struct agx_bridge_arena *a = arena_of(dev);
   if (a == NULL)
      return;

   struct util_vma_heap *heap =
      (flags & AGX_VA_USC) ? &a->usc_heap :
      (flags & AGX_VA_CAPTURE_REPLAY) ? &a->replay_heap : &a->main_heap;

   AcquireSRWLockExclusive(&a->lock);
   util_vma_heap_free(heap, addr, size_B);
   ReleaseSRWLockExclusive(&a->lock);
}

int
agx_bridge_vma_bootstrap_bind(struct agx_device *dev, struct agx_bo *bo,
                              uint64_t addr, uint64_t size_B,
                              uint64_t bo_offset, unsigned bind_flags,
                              const char *label)
{
   struct agx_bridge_arena *a = arena_of(dev);
   if (a == NULL)
      return agx_bo_bind(dev, bo, addr, size_B, bo_offset, bind_flags);

   const void *rt = agx_d3dkmt_bridge_rtdevice(
      (const struct agx_d3dkmt_winsys *)dev->hdev);
   const struct agx_bridge_bootstrap_mapping *conflict = NULL;

   if (!bootstrap_begin(a))
      return -1;
   if (InterlockedCompareExchange(&a->poisoned, 0, 0) != 0) {
      bootstrap_end(a);
      errno = EIO;
      return -1;
   }
   enum agx_bridge_bootstrap_claim_result claim = agx_bridge_bootstrap_claim(
      &a->bootstrap, dev, rt, bo != NULL ? bo->uapi_handle : 0, bo, addr,
      size_B, bind_flags, &conflict);
   if (claim == AGX_BRIDGE_BOOTSTRAP_CLAIM_COLLISION) {
      /* Three distinct causes, named so one boot adjudicates:
       *   shape             different size/flags or partial overlap
       *   owner-rebacking   the BOUND owner re-claimed with a different BO
       *   claimant-rebacking a deferred claimant re-claimed with a
       *                     different BO
       * The latter two print both backings; identical shape is implied. */
      const void *recorded =
         agx_bridge_bootstrap_backing_of(&a->bootstrap, addr, size_B, dev);
      const bool shape_mismatch =
         conflict == NULL || conflict->addr != addr ||
         conflict->size != size_B || conflict->flags != bind_flags;
      const char *cause = shape_mismatch          ? "shape"
                          : (conflict->owner == dev) ? "owner-rebacking"
                                                     : "claimant-rebacking";
      agx_bridge_valog(
         "BOOTSTRAP COLLISION vm=0x%016llx kind=%s cause=%s addr=0x%llx "
         "size=0x%llx flags=0x%x new_dev=%p new_rt=%p new_alloc=0x%llx "
         "new_backing=%p recorded_backing=%p owner_dev=%p owner_rt=%p "
         "owner_alloc=0x%llx owner_size=0x%llx owner_flags=0x%x "
         "(mismatched shape or rebacking; bind refused)",
         (unsigned long long)a->vm_key, label != NULL ? label : "unknown",
         cause, (unsigned long long)addr, (unsigned long long)size_B,
         bind_flags, (void *)dev, rt,
         (unsigned long long)(bo != NULL ? bo->uapi_handle : 0), (void *)bo,
         recorded, conflict != NULL ? (void *)conflict->owner : NULL,
         conflict != NULL ? conflict->rtdevice : NULL,
         (unsigned long long)(conflict != NULL ? conflict->allocation : 0),
         (unsigned long long)(conflict != NULL ? conflict->size : 0),
         conflict != NULL ? conflict->flags : 0u);
      bootstrap_end(a);
      errno = EADDRINUSE;
      return -1;
   }
   if (claim == AGX_BRIDGE_BOOTSTRAP_CLAIM_INVALID ||
       claim == AGX_BRIDGE_BOOTSTRAP_CLAIM_FULL) {
      agx_bridge_valog(
         "BOOTSTRAP CLAIM FAIL vm=0x%016llx kind=%s addr=0x%llx "
         "size=0x%llx dev=%p result=%d",
         (unsigned long long)a->vm_key, label != NULL ? label : "unknown",
         (unsigned long long)addr, (unsigned long long)size_B, (void *)dev,
         (int)claim);
      bootstrap_end(a);
      errno = claim == AGX_BRIDGE_BOOTSTRAP_CLAIM_FULL ? ENOSPC : EINVAL;
      return -1;
   }
   if (claim == AGX_BRIDGE_BOOTSTRAP_CLAIM_SAME) {
      bootstrap_end(a);
      return 0;
   }
   if (claim == AGX_BRIDGE_BOOTSTRAP_CLAIM_DEFERRED) {
      /* The alias is already mapped by a sibling with an identical-shape
       * backing (content-defined page); this device's own backing stays
       * unbound in reserve for owner-exit handover. Do NOT bind. */
      agx_bridge_valog(
         "BOOTSTRAP DEFER vm=0x%016llx kind=%s addr=0x%llx size=0x%llx "
         "flags=0x%x dev=%p rt=%p alloc=0x%llx claimants=%u",
         (unsigned long long)a->vm_key, label != NULL ? label : "unknown",
         (unsigned long long)addr, (unsigned long long)size_B, bind_flags,
         (void *)dev, rt,
         (unsigned long long)(bo != NULL ? bo->uapi_handle : 0),
         agx_bridge_bootstrap_claimant_count(&a->bootstrap, addr, size_B));
      bootstrap_end(a);
      return 0;
   }

   int ret = bootstrap_bind_unlocked(a, dev, bo, addr, size_B, bo_offset, bind_flags);
   if (ret != 0) {
      InterlockedExchange(&a->poisoned, 1);
      struct agx_bridge_bootstrap_claimant unused;
      (void)agx_bridge_bootstrap_release(&a->bootstrap, dev, addr, size_B,
                                         &unused);
      agx_bridge_valog(
         "BOOTSTRAP BIND FAIL vm=0x%016llx kind=%s addr=0x%llx "
         "size=0x%llx dev=%p rt=%p alloc=0x%llx ret=%d errno=%d",
         (unsigned long long)a->vm_key, label != NULL ? label : "unknown",
         (unsigned long long)addr, (unsigned long long)size_B, (void *)dev, rt,
         (unsigned long long)bo->uapi_handle, ret, errno);
   } else {
      agx_bridge_valog(
         "BOOTSTRAP BIND vm=0x%016llx kind=%s addr=0x%llx size=0x%llx "
         "flags=0x%x dev=%p rt=%p alloc=0x%llx",
         (unsigned long long)a->vm_key, label != NULL ? label : "unknown",
         (unsigned long long)addr, (unsigned long long)size_B, bind_flags,
         (void *)dev, rt, (unsigned long long)bo->uapi_handle);
   }
   bootstrap_end(a);
   return ret;
}

void
agx_bridge_vma_bootstrap_unbind_all(struct agx_device *dev)
{
   struct agx_bridge_arena *a = arena_of(dev);
   if (a == NULL)
      return;

   if (!bootstrap_begin(a))
      return;
   a->bootstrap_retiring = dev;
   for (unsigned i = 0; i < AGX_BRIDGE_BOOTSTRAP_MAX; ++i) {
      struct agx_bridge_bootstrap_mapping *m = &a->bootstrap.map[i];
      if (!m->in_use)
         continue;

      /* Snapshot before release(): RELEASE_LAST clears the mapping. */
      const uint64_t addr = m->addr;
      const uint64_t size = m->size;
      const unsigned flags = m->flags;

      struct agx_bridge_bootstrap_claimant successor;
      enum agx_bridge_bootstrap_release_result rel =
         agx_bridge_bootstrap_release(&a->bootstrap, dev, addr, size,
                                      &successor);
      switch (rel) {
      case AGX_BRIDGE_BOOTSTRAP_RELEASE_NOT_FOUND:
         break; /* dev has no stake in this alias */
      case AGX_BRIDGE_BOOTSTRAP_RELEASE_DEFERRED:
         /* dev never bound anything here; its reserve backing simply leaves
          * with it. */
         agx_bridge_valog(
            "BOOTSTRAP LEAVE vm=0x%016llx addr=0x%llx size=0x%llx dev=%p "
            "(deferred claimant)",
            (unsigned long long)a->vm_key, (unsigned long long)addr,
            (unsigned long long)size, (void *)dev);
         break;
      case AGX_BRIDGE_BOOTSTRAP_RELEASE_LAST: {
         int ret = bootstrap_bind_unlocked(a, dev, NULL, addr, size, 0,
                               DRM_ASAHI_BIND_UNBIND);
         if (ret != 0) InterlockedExchange(&a->poisoned, 1);
         agx_bridge_valog(
            "BOOTSTRAP UNBIND vm=0x%016llx addr=0x%llx size=0x%llx dev=%p "
            "ret=%d errno=%d",
            (unsigned long long)a->vm_key, (unsigned long long)addr,
            (unsigned long long)size, (void *)dev, ret,
            ret != 0 ? errno : 0);
         break;
      }
      case AGX_BRIDGE_BOOTSTRAP_RELEASE_HANDOVER: {
         /* The departing owner's backing is about to be destroyed, which
          * unmaps its pages from the shared root. Replace the alias with a
          * live sibling's identical-shape backing FIRST. Try a direct
          * replacing bind; if the kernel refuses map-over-mapped, cycle
          * through unbind+bind (content-defined page: any moment of the
          * swap is equivalent). On failure, fall through the remaining
          * claimants; if none can bind, unbind so the root never holds a
          * dangling bootstrap PTE. */
         bool bound = false;
         while (!bound) {
            struct agx_device *sdev = (struct agx_device *)(uintptr_t)
               (const char *)successor.owner;
            struct agx_bo *sbo =
               (struct agx_bo *)(uintptr_t)(const char *)successor.backing;
            int ret = bootstrap_bind_unlocked(a, sdev, sbo, addr, size, 0, flags);
            const char *how = "direct";
            if (ret != 0) {
               (void)bootstrap_bind_unlocked(a, sdev, NULL, addr, size, 0,
                                 DRM_ASAHI_BIND_UNBIND);
               ret = bootstrap_bind_unlocked(a, sdev, sbo, addr, size, 0, flags);
               how = "cycled";
            }
            agx_bridge_valog(
               "BOOTSTRAP HANDOVER vm=0x%016llx addr=0x%llx size=0x%llx "
               "from=%p to=%p alloc=0x%llx bind=%s ret=%d errno=%d",
               (unsigned long long)a->vm_key, (unsigned long long)addr,
               (unsigned long long)size, (void *)dev, (void *)sdev,
               (unsigned long long)successor.allocation, how, ret,
               ret != 0 ? errno : 0);
            if (ret == 0) {
               bound = true;
               break;
            }
            (void)agx_bridge_bootstrap_demote_owner(&a->bootstrap, addr,
                                                    size);
            if (!agx_bridge_bootstrap_promote_next(&a->bootstrap, addr, size,
                                                   &successor)) {
               int unbind_ret = bootstrap_bind_unlocked(a, dev, NULL, addr, size, 0,
                                            DRM_ASAHI_BIND_UNBIND);
               InterlockedExchange(&a->poisoned, 1);
               agx_bridge_valog(
                  "BOOTSTRAP HANDOVER EXHAUSTED vm=0x%016llx addr=0x%llx "
                  "size=0x%llx dev=%p unbind_ret=%d",
                  (unsigned long long)a->vm_key, (unsigned long long)addr,
                  (unsigned long long)size, (void *)dev, unbind_ret);
               break;
            }
         }
         break;
      }
      }
   }
   bootstrap_end(a);
}
