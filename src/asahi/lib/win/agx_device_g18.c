/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * G18 render-node backend for agx_device_ops_t, next to native DRM
 * (agx_device.c), virtio native context (agx_device_virtio.c) and the other
 * Windows backends (agx_device_d3dkmt.c, agx_device_win.c).
 *
 * It speaks the asahi uAPI to the g18win render node through
 * win/agx_win_kmd.h (ABI: win/g18_uapi.h). The asahi structs are passed
 * unchanged; the transport reframes them for DeviceIoControl. What DRM core
 * gives Linux (GEM close, mmap) is the render node's generic commands.
 *
 * Synchronisation lives in user mode (G4-PLAN 3.1). The kernel's SUBMIT is
 * synchronous: it returns once the GPU job has completed. So the sync
 * objects are a process-local table (agx_g18_sync_provider_create(), a
 * util_sync_provider with libdrm syncobj semantics: binary and timeline,
 * wait, transfer), and agx_g18_submit() waits for the in-syncs, issues the
 * SUBMIT without syncs, then signals the out-syncs. There is no sync_file
 * and no cross-process sharing on Windows yet (G5/G7).
 *
 * G6 perf: a render SUBMIT goes with G18_SUBMIT_ASYNC (AGX_WIN_ASYNC=0
 * turns it off; a render node that refuses the flag turns it off for the
 * process). The kernel returns after the doorbell and retires the frame at
 * its next call. The out-syncs are still signalled at once in the table,
 * but every observation of the table (wait, query, the in-syncs of the
 * next SUBMIT) first retires the frame with a synchronization-only SUBMIT,
 * so nothing reads a sync as signalled before the GPU finished. Recording
 * the next frame overlaps the GPU's work on this one. A SUBMIT itself
 * needs no such retire (G6 track Q): the kernel retires the pending frame
 * before it runs anything, so its in-syncs are waited for in the table
 * only, and one SUBMIT carries up to the kernel's
 * max_commands_per_submission commands.
 *
 * Also Windows specific, from G4-PLAN 3.3/3.7:
 *  - AGX_WIN_FEATURE_CLEAR (hex) clears feature bits the kernel advertises
 *    (it can only clear), for variants without a rebuild;
 *  - CPU maps are write-combined and never placed (no fixed address).
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drm-uapi/asahi_drm.h"
#include "drm-uapi/drm.h"
#include "util/cnd_monotonic.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/stack_array.h"
#include "util/u_debug.h"
#include "util/u_sync_provider.h"

#include "agx_bo.h"
#include "agx_device.h"
#include "agx_device_g18.h"
#include "agx_win_kmd.h"

static void agx_g18_bo_free(struct agx_device *dev, uint32_t handle);

static int
agx_g18_asahi_call(struct agx_device *dev, unsigned long drm_cmd, void *arg)
{
   return agx_win_kmd_call(dev->fd, agx_win_asahi_nr(drm_cmd), arg,
                           agx_win_asahi_size(drm_cmd));
}

/*
 * simple_ioctl receives DRM_IOCTL_ASAHI_* words (VM_CREATE, QUEUE_CREATE,
 * QUEUE_DESTROY, GET_TIME, GET_PARAMS). The size in the word is the one the
 * caller chose, which may be an older, shorter struct (QUEUE_CREATE is sent
 * at 24 bytes without render SCS layouts); it is forwarded as is, like the
 * Linux ioctl.
 */
static int
agx_g18_simple_ioctl(struct agx_device *dev, unsigned cmd, void *req)
{
   if (((cmd >> 8) & 0xff) != DRM_IOCTL_BASE ||
       agx_win_asahi_nr(cmd) > G18_RN_ASAHI_LAST) {
      /* A generic DRM ioctl: there is no DRM core on Windows. */
      errno = ENOTTY;
      return -1;
   }

   return agx_g18_asahi_call(dev, cmd, req);
}

static int
agx_g18_bo_bind(struct agx_device *dev, struct drm_asahi_gem_bind_op *ops,
                uint32_t count)
{
   struct drm_asahi_vm_bind vm_bind = {
      .vm_id = dev->vm_id,
      .num_binds = count,
      .stride = sizeof(*ops),
      .userptr = (uint64_t)(uintptr_t)ops,
   };

   int ret = agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_VM_BIND, &vm_bind);
   if (ret) {
      const int err = errno;
      fprintf(stderr, "agx-g18: VM_BIND failed: %s (vm=%u, ops=%u",
              strerror(err), dev->vm_id, count);
      for (uint32_t i = 0; i < count && i < 4; ++i) {
         fprintf(stderr, "; addr=%#" PRIx64 " range=%#" PRIx64
                 " offset=%#" PRIx64 " handle=%u flags=%#x",
                 (uint64_t)ops[i].addr, (uint64_t)ops[i].range,
                 (uint64_t)ops[i].offset, ops[i].handle, ops[i].flags);
      }
      fprintf(stderr, ")\n");
      errno = err;
   }

   return ret;
}

static struct agx_bo *
agx_g18_bo_alloc(struct agx_device *dev, size_t size, size_t align,
                 enum agx_bo_flags flags)
{
   /* executable implies low va */
   assert(!(flags & AGX_BO_EXEC) || (flags & AGX_BO_LOW_VA));

   struct drm_asahi_gem_create gem_create = {.size = size};

   if (flags & AGX_BO_WRITEBACK)
      gem_create.flags |= DRM_ASAHI_GEM_WRITEBACK;

   /* No dma-buf on Windows: only VM-private BOs are useful, and the kernel
    * skips the export bookkeeping for them. */
   if (!(flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE))) {
      gem_create.flags |= DRM_ASAHI_GEM_VM_PRIVATE;
      gem_create.vm_id = dev->vm_id;
   }

   int ret = agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_GEM_CREATE, &gem_create);
   if (ret) {
      fprintf(stderr, "agx-g18: GEM_CREATE of %zu bytes failed: %s\n", size,
              strerror(errno));
      return NULL;
   }

   uint32_t handle = gem_create.handle;

   simple_mtx_lock(&dev->bo_map_lock);
   struct agx_bo *bo = agx_lookup_bo(dev, handle);
   agx_include_bo_handle(dev, handle);
   simple_mtx_unlock(&dev->bo_map_lock);

   /* Fresh handle */
   assert(!memcmp(bo, &((struct agx_bo){}), sizeof(*bo)));

   bo->dev = dev;
   bo->size = gem_create.size;
   bo->align = align;
   bo->flags = flags;
   bo->handle = bo->uapi_handle = handle;
   bo->prime_fd = -1; /* no dma-buf on Windows */

   enum agx_va_flags va_flags = flags & AGX_BO_LOW_VA ? AGX_VA_USC : 0;
   bo->va = agx_va_alloc(dev, size, bo->align, va_flags, 0);
   if (!bo->va) {
      fprintf(stderr, "agx-g18: failed to allocate BO VMA\n");
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
 * G6 zero copy: a BO over [ptr, ptr + size) of this process, the CPU view of
 * a WDDM allocation the D3D UMD holds locked (g18wddm segment 1, the pool's
 * WDDM window). The render node resolves the range in this process and
 * admits only window pages. The CPU map is the caller's own pointer.
 */
static int
agx_g18_gem_userptr(struct agx_device *dev, void *ptr, size_t size,
                    bool read_only, uint32_t *handle)
{
   struct g18_asahi_gem_userptr u = {
      .userptr = (uint64_t)(uintptr_t)ptr,
      .size = size,
      .flags = G18_ASAHI_GEM_VM_PRIVATE |
               (read_only ? G18_ASAHI_GEM_USERPTR_READ_ONLY : 0),
      .vm_id = dev->vm_id,
   };

   if (agx_win_kmd_call(dev->fd, G18_RN_GEM_USERPTR, &u, sizeof(u)))
      return -errno;

   *handle = u.handle;
   return 0;
}

/*
 * agx_bo_import_host_ptr() for the render node, with the access rules of the
 * DRM one. The node imports whole 16 KiB pages and checks the physical
 * start; the WDDM allocation behind the pointer covers them (the UMD sizes
 * GPU allocations in 64 KiB, zink asks for an image's exact size).
 */
struct agx_bo *
agx_g18_bo_import_host_ptr(struct agx_device *dev, void *ptr, size_t size,
                           enum agx_host_ptr_access access)
{
   if (!ptr || !size || ((uintptr_t)ptr & 0xfff)) {
      errno = EINVAL;
      return NULL;
   }
   size = (size + 0x3fff) & ~(size_t)0x3fff;

   enum agx_bo_flags flags = AGX_BO_HOST_PTR | AGX_BO_NO_CACHE;
   uint32_t handle = 0;
   int ret = agx_g18_gem_userptr(dev, ptr, size, false, &handle);
   if (ret == 0 && access == AGX_HOST_PTR_READ_ONLY) {
      agx_g18_bo_free(dev, handle);
      errno = EACCES;
      return NULL;
   }
   if (ret == -EFAULT && access != AGX_HOST_PTR_WRITABLE) {
      ret = agx_g18_gem_userptr(dev, ptr, size, true, &handle);
      flags |= AGX_BO_READONLY;
   }
   if (ret) {
      fprintf(stderr, "agx-g18: GEM_USERPTR of %zu bytes at %p failed: %s\n",
              size, ptr, strerror(-ret));
      errno = -ret;
      return NULL;
   }

   simple_mtx_lock(&dev->bo_map_lock);
   struct agx_bo *bo = agx_lookup_bo(dev, handle);
   agx_include_bo_handle(dev, handle);
   simple_mtx_unlock(&dev->bo_map_lock);

   assert(!memcmp(bo, &((struct agx_bo){}), sizeof(*bo)));

   bo->dev = dev;
   bo->size = size;
   bo->align = 16384;
   bo->flags = flags;
   bo->handle = bo->uapi_handle = handle;
   bo->prime_fd = -1;
   bo->refcnt = 1;
   bo->label = "Host memory";
   bo->_map = ptr;
   bo->_map_size = size;

   bo->va = agx_va_alloc(dev, size, bo->align, 0, 0);
   if (!bo->va) {
      fprintf(stderr, "agx-g18: no VA for a %zu-byte import\n", size);
      agx_bo_free(dev, bo);
      errno = ENOMEM;
      return NULL;
   }

   uint32_t bind = DRM_ASAHI_BIND_READ;
   if (!(flags & AGX_BO_READONLY))
      bind |= DRM_ASAHI_BIND_WRITE;
   if (agx_bo_bind(dev, bo, bo->va->addr, bo->size, 0, bind)) {
      int err = errno;
      agx_bo_free(dev, bo);
      errno = err;
      return NULL;
   }

   return bo;
}

static void
agx_g18_bo_mmap(struct agx_device *dev, struct agx_bo *bo, void *fixed_addr,
                size_t length)
{
   assert(bo->_map == NULL && "not double mapped");

   bo->_map = NULL;
   bo->_map_size = 0;

   /* No placed maps in G4 (VK_EXT_map_memory_placed is off on Windows). */
   if (fixed_addr) {
      fprintf(stderr, "agx-g18: placed BO maps are not supported\n");
      return;
   }

   struct g18_rn_gem_map map = {
      .handle = bo->uapi_handle,
      .size = length ? length : bo->size,
   };

   int ret = agx_win_kmd_call(dev->fd, G18_RN_GEM_MAP, &map, sizeof(map));
   if (ret || !map.va) {
      fprintf(stderr,
              "agx-g18: GEM_MAP failed: %s (handle=%u size=%#" PRIx64 ")\n",
              ret ? strerror(errno) : "no address", bo->uapi_handle,
              (uint64_t)map.size);
      return;
   }

   bo->_map = (void *)(uintptr_t)map.va;
   bo->_map_size = map.size;
}

static void
agx_g18_bo_munmap(struct agx_device *dev, struct agx_bo *bo)
{
   /* An import's map is the caller's own pointer (the locked allocation). */
   if (bo->flags & AGX_BO_HOST_PTR) {
      bo->_map_size = 0;
      return;
   }

   struct g18_rn_gem_unmap unmap = {
      .va = (uint64_t)(uintptr_t)bo->_map,
      .size = agx_bo_map_size(bo),
   };

   agx_win_kmd_call(dev->fd, G18_RN_GEM_UNMAP, &unmap, sizeof(unmap));
   bo->_map_size = 0;
}

static void
agx_g18_bo_free(struct agx_device *dev, uint32_t handle)
{
   struct g18_rn_gem_close args = {.handle = handle};
   agx_win_kmd_call(dev->fd, G18_RN_GEM_CLOSE, &args, sizeof(args));
}

static int
agx_g18_bo_madvise(struct agx_device *dev, uint32_t handle, bool dontneed,
                   bool *retained)
{
   struct drm_asahi_gem_madvise args = {
      .handle = handle,
      .madv = dontneed ? DRM_ASAHI_GEM_MADV_DONTNEED
                       : DRM_ASAHI_GEM_MADV_WILLNEED,
   };

   if (agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_GEM_MADVISE, &args))
      return -errno;

   *retained = args.retained != 0;
   return 0;
}

static ssize_t
agx_g18_get_params(struct agx_device *dev, void *buf, size_t size)
{
   struct drm_asahi_get_params get_param = {
      .param_group = 0,
      .pointer = (uint64_t)(uintptr_t)buf,
      .size = size,
   };

   memset(buf, 0, size);

   int ret = agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_GET_PARAMS, &get_param);
   if (ret) {
      fprintf(stderr, "agx-g18: GET_PARAMS failed: %s\n", strerror(errno));
      return -EINVAL;
   }

   /* The transport imports host memory with the node's GEM_USERPTR
    * (agx_g18_bo_import_host_ptr()), which the node does not report in the
    * asahi feature word. */
   if (size >= sizeof(uint64_t)) {
      uint64_t features;
      memcpy(&features, buf, sizeof(features));
      features |= DRM_ASAHI_FEATURE_USERPTR;
      memcpy(buf, &features, sizeof(features));
   }

   /* AGX_WIN_FEATURE_CLEAR can only take bits away (G4-PLAN 3.3, B15). */
   if (size >= sizeof(uint64_t)) {
      const char *opt = os_get_option("AGX_WIN_FEATURE_CLEAR");
      uint64_t clear = opt ? strtoull(opt, NULL, 16) : 0;
      if (clear) {
         uint64_t features;
         memcpy(&features, buf, sizeof(features));
         fprintf(stderr,
                 "agx-g18: features %#" PRIx64 " & ~%#" PRIx64 " = %#" PRIx64
                 " (AGX_WIN_FEATURE_CLEAR)\n",
                 features, clear, features & ~clear);
         features &= ~clear;
         memcpy(buf, &features, sizeof(features));
      }
   }

   return get_param.size;
}

static int agx_g18_sync_wait_submit(const struct drm_asahi_sync *syncs,
                                    uint32_t count);
static void agx_g18_sync_signal_submit(const struct drm_asahi_sync *syncs,
                                       uint32_t count);

/* The asynchronous render the kernel may still be running (one per
 * process: the render node has one job slot). The lock is held across the
 * SUBMIT that starts it and the SUBMIT that retires it, so an observer that
 * finds nothing pending knows the GPU is done. */
static struct {
   once_flag once;
   mtx_t lock;
   bool pending;
   int fd;
   uint32_t queue_id;
   int enabled; /* -1 unknown, 0 off, 1 on */
} agx_g18_async = {.once = ONCE_FLAG_INIT, .enabled = -1};

static void
agx_g18_async_init(void)
{
   mtx_init(&agx_g18_async.lock, mtx_plain);
   agx_g18_async.enabled = os_get_option("AGX_WIN_ASYNC") &&
                                 !debug_get_bool_option("AGX_WIN_ASYNC", true)
                              ? 0
                              : 1;
}

/* Lock held: a synchronization-only SUBMIT, which the kernel answers once
 * the pending frame is retired. Its errors (a closed file, a lost device)
 * belong to the next real call, which reports them. */
static void
agx_g18_async_retire_locked(void)
{
   if (!agx_g18_async.pending)
      return;
   struct drm_asahi_submit k = {.queue_id = agx_g18_async.queue_id};
   if (agx_win_kmd_call(agx_g18_async.fd, G18_RN_SUBMIT, &k, sizeof(k)))
      fprintf(stderr, "agx-g18: retiring an asynchronous render: %s\n",
              strerror(errno));
   agx_g18_async.pending = false;
}

/* Every observation of the sync table starts here. */
static void
agx_g18_async_retire(void)
{
   call_once(&agx_g18_async.once, agx_g18_async_init);
   if (!agx_g18_async.enabled)
      return;
   mtx_lock(&agx_g18_async.lock);
   agx_g18_async_retire_locked();
   mtx_unlock(&agx_g18_async.lock);
}

/* The hardware commands of a SUBMIT: whether any is a render (only
 * renders run asynchronously in the kernel; compute ignores the flag) and
 * whether the last one is (then a frame is still in flight after the
 * SUBMIT; after a last compute the kernel has retired everything).
 * Software commands (the attachment lists hk sends on G18G, the resource
 * declarations) are skipped, as the kernel's parser does (track F, run
 * 20261120-044007). G6 track Q: a SUBMIT carries several commands. */
static void
agx_g18_cmdbuf_renders(const struct drm_asahi_submit *submit, bool *any,
                       bool *last)
{
   const uint8_t *p = (const uint8_t *)(uintptr_t)submit->cmdbuf;
   uint64_t off = 0, size = submit->cmdbuf_size;

   *any = *last = false;
   while (p && size - off >= sizeof(struct drm_asahi_cmd_header)) {
      const struct drm_asahi_cmd_header *h =
         (const struct drm_asahi_cmd_header *)(p + off);
      if (h->cmd_type == DRM_ASAHI_CMD_RENDER)
         *any = *last = true;
      else if (h->cmd_type == DRM_ASAHI_CMD_COMPUTE)
         *last = false;
      off += sizeof(*h) + (uint64_t)h->size;
      if (off > size)
         return;
   }
}

/* G4-PLAN 3.1: wait for the in-syncs, submit without syncs (the kernel runs
 * the job to completion), then signal the out-syncs. */
static int
agx_g18_submit(struct agx_device *dev, struct drm_asahi_submit *submit,
               struct agx_submit_virt *virt)
{
   (void)virt;
   const struct drm_asahi_sync *syncs =
      (const struct drm_asahi_sync *)(uintptr_t)submit->syncs;

   if (submit->in_sync_count) {
      int ret = agx_g18_sync_wait_submit(syncs, submit->in_sync_count);
      if (ret) {
         errno = -ret;
         return -1;
      }
   }

   struct drm_asahi_submit k = *submit;
   k.syncs = 0;
   k.in_sync_count = 0;
   k.out_sync_count = 0;

   int ret = 0;
   if (k.cmdbuf_size) {
      bool any_render, last_render;
      agx_g18_cmdbuf_renders(&k, &any_render, &last_render);
      call_once(&agx_g18_async.once, agx_g18_async_init);
      if (agx_g18_async.enabled && any_render) {
         mtx_lock(&agx_g18_async.lock);
         /* The kernel retires the previous frame before this SUBMIT runs
          * anything (G6 track Q: no sync-only SUBMIT first). */
         k.flags |= G18_SUBMIT_ASYNC;
         ret = agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_SUBMIT, &k);
         if (ret && errno == EINVAL && agx_g18_async.enabled == 1) {
            /* A render node without G18_SUBMIT_ASYNC: synchronous from
             * now on. */
            agx_g18_async.enabled = 0;
            k.flags &= ~G18_SUBMIT_ASYNC;
            ret = agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_SUBMIT, &k);
            if (!ret)
               fprintf(stderr, "agx-g18: the render node refuses "
                               "G18_SUBMIT_ASYNC: synchronous SUBMITs\n");
         } else if (!ret) {
            agx_g18_async.pending = last_render;
            agx_g18_async.fd = dev->fd;
            agx_g18_async.queue_id = k.queue_id;
         }
         mtx_unlock(&agx_g18_async.lock);
      } else {
         /* Compute only: serialize with async renders on the render node's
          * one job slot so pending state stays exact across threads. */
         mtx_lock(&agx_g18_async.lock);
         ret = agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_SUBMIT, &k);
         if (!ret)
            agx_g18_async.pending = false;
         mtx_unlock(&agx_g18_async.lock);
      }
   }
   if (ret)
      return ret;

   if (submit->out_sync_count)
      agx_g18_sync_signal_submit(syncs + submit->in_sync_count,
                                 submit->out_sync_count);
   return 0;
}

static int
agx_g18_bo_bind_object(struct agx_device *dev,
                       struct drm_asahi_gem_bind_object *bind)
{
   /* EOPNOTSUPP in G4 (no timestamps); hk does not need it for compute. */
   int ret = agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, bind);
   if (ret) {
      fprintf(stderr, "agx-g18: GEM_BIND_OBJECT failed: %s (handle=%u)\n",
              strerror(errno), bind->handle);
   }
   return ret;
}

static int
agx_g18_bo_unbind_object(struct agx_device *dev, uint32_t object_handle)
{
   struct drm_asahi_gem_bind_object bind = {
      .op = DRM_ASAHI_BIND_OBJECT_OP_UNBIND,
      .object_handle = object_handle,
   };

   return agx_g18_asahi_call(dev, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, &bind);
}

const agx_device_ops_t agx_g18_device_ops = {
   .bo_alloc = agx_g18_bo_alloc,
   .bo_bind = agx_g18_bo_bind,
   .bo_mmap = agx_g18_bo_mmap,
   /* Each GEM_MAP creates a new view of the BO. */
   .bo_mmap_independent = true,
   .get_params = agx_g18_get_params,
   .submit = agx_g18_submit,
   .bo_bind_object = agx_g18_bo_bind_object,
   .bo_unbind_object = agx_g18_bo_unbind_object,
   .bo_madvise = agx_g18_bo_madvise,
   .simple_ioctl = agx_g18_simple_ioctl,
   .bo_munmap = agx_g18_bo_munmap,
   .bo_free = agx_g18_bo_free,
};

bool
agx_g18_open_device(struct agx_device *dev)
{
   int fd = -1;
   if (agx_win_kmd_open(&fd)) {
      if (errno != ENODEV)
         fprintf(stderr, "agx-g18: cannot open the render node: %s\n",
                 strerror(errno));
      return false;
   }

   struct g18_rn_version version = {0};
   if (agx_win_kmd_call(fd, G18_RN_VERSION, &version, sizeof(version)) ||
       version.abi != G18_RN_ABI) {
      fprintf(stderr, "agx-g18: render node ABI %u, expected %u\n",
              version.abi, G18_RN_ABI);
      agx_win_kmd_close(fd);
      return false;
   }

   version.name[sizeof(version.name) - 1] = '\0';
   version.desc[sizeof(version.desc) - 1] = '\0';
   if (!agx_is_native_drm_driver(version.name)) {
      agx_win_kmd_close(fd);
      return false;
   }

   /* Every other command answers ENODEV until GO_LIVE (G4-PLAN 3.5). */
   if (version.state != G18_STATE_LIVE) {
      fprintf(stderr,
              "agx-g18: render node %s is not live (driver state %u, "
              "flags %#x); run GO_LIVE first\n",
              version.desc, version.state, version.flags);
      agx_win_kmd_close(fd);
      return false;
   }

   dev->fd = fd;
   dev->is_virtio = false;
   /* The render node versions its uAPI like the "aurora" kernel: the
    * render high dwords are gated per runtime, not by version number. */
   dev->render_rsrc_spec_hi = false;
   /* Preset ops: agx_open_device() skips the DRM version probe. */
   dev->ops = agx_g18_device_ops;
   return true;
}

void
agx_g18_close_fd(int fd)
{
   call_once(&agx_g18_async.once, agx_g18_async_init);
   mtx_lock(&agx_g18_async.lock);
   if (agx_g18_async.pending && agx_g18_async.fd == fd)
      agx_g18_async_retire_locked();
   mtx_unlock(&agx_g18_async.lock);
   agx_win_kmd_close(fd);
}

/*
 * User-mode sync objects (G4-PLAN 3.1).
 *
 * One process-wide table; handles are unique in the process (DRM handles
 * are per file, but nothing here crosses a process). An object holds a
 * 64-bit payload: a binary syncobj is 0 (unsignalled) or 1, a timeline its
 * highest signalled point. A transfer from a point that has not signalled
 * yet becomes a forward that fires when it does: that is the fence a DRM
 * transfer would attach. "Available" (WAIT_AVAILABLE, WAIT_FOR_SUBMIT) means
 * signalled or with such a forward pending.
 *
 * Errors follow libdrm's drmSyncobj*: 0, or a negative errno with errno set.
 */

#define AGX_WIN_MAX_FORWARDS 256

struct agx_g18_syncobj {
   uint64_t value;
   bool live;
};

struct agx_g18_forward {
   uint32_t src, dst;
   uint64_t src_point, dst_point;
};

static struct {
   once_flag once;
   mtx_t lock;
   struct u_cnd_monotonic cond;
   struct agx_g18_syncobj *obj; /* handle = index, 0 unused */
   uint32_t count, cap;
   struct agx_g18_forward fwd[AGX_WIN_MAX_FORWARDS];
   uint32_t nfwd;
} agx_g18_syncs = {.once = ONCE_FLAG_INIT};

static void
agx_g18_syncs_init(void)
{
   mtx_init(&agx_g18_syncs.lock, mtx_plain);
   u_cnd_monotonic_init(&agx_g18_syncs.cond);
}

static int
agx_g18_sync_err(int e)
{
   errno = e;
   return -e;
}

static struct agx_g18_syncobj *
agx_g18_sync_get(uint32_t handle)
{
   if (handle == 0 || handle >= agx_g18_syncs.count ||
       !agx_g18_syncs.obj[handle].live)
      return NULL;
   return &agx_g18_syncs.obj[handle];
}

static bool
agx_g18_point_done(const struct agx_g18_syncobj *o, uint64_t point)
{
   return o->value >= (point ? point : 1);
}

static bool
agx_g18_point_available(uint32_t handle, const struct agx_g18_syncobj *o,
                        uint64_t point)
{
   if (agx_g18_point_done(o, point))
      return true;
   for (uint32_t i = 0; i < agx_g18_syncs.nfwd; i++) {
      const struct agx_g18_forward *f = &agx_g18_syncs.fwd[i];
      if (f->dst == handle && f->dst_point >= point)
         return true;
   }
   return false;
}

/* Signal (handle, point) and fire every forward that became ready. Lock
 * held. */
static void
agx_g18_signal_locked(uint32_t handle, uint64_t point)
{
   struct agx_g18_syncobj *o = agx_g18_sync_get(handle);
   if (o) {
      if (point == 0)
         o->value = o->value ? o->value : 1;
      else if (point > o->value)
         o->value = point;
   }

   bool fired;
   do {
      fired = false;
      for (uint32_t i = 0; i < agx_g18_syncs.nfwd; i++) {
         struct agx_g18_forward f = agx_g18_syncs.fwd[i];
         struct agx_g18_syncobj *s = agx_g18_sync_get(f.src);
         if (s && !agx_g18_point_done(s, f.src_point))
            continue;
         /* Ready (or its source is gone: a destroyed source never signals,
          * drop the forward). */
         agx_g18_syncs.fwd[i] = agx_g18_syncs.fwd[--agx_g18_syncs.nfwd];
         if (s) {
            struct agx_g18_syncobj *d = agx_g18_sync_get(f.dst);
            if (d) {
               if (f.dst_point == 0)
                  d->value = d->value ? d->value : 1;
               else if (f.dst_point > d->value)
                  d->value = f.dst_point;
            }
         }
         fired = true;
         break;
      }
   } while (fired);

   u_cnd_monotonic_broadcast(&agx_g18_syncs.cond);
}

static int
agx_g18_sync_create(struct util_sync_provider *p, uint32_t flags,
                    uint32_t *handle)
{
   (void)p;
   call_once(&agx_g18_syncs.once, agx_g18_syncs_init);
   mtx_lock(&agx_g18_syncs.lock);
   uint32_t h = 0;
   for (uint32_t i = 1; i < agx_g18_syncs.count; i++) {
      if (!agx_g18_syncs.obj[i].live) {
         h = i;
         break;
      }
   }
   if (!h) {
      if (agx_g18_syncs.count == agx_g18_syncs.cap) {
         uint32_t cap = agx_g18_syncs.cap ? 2 * agx_g18_syncs.cap : 64;
         void *n = realloc(agx_g18_syncs.obj, cap * sizeof(*agx_g18_syncs.obj));
         if (!n) {
            mtx_unlock(&agx_g18_syncs.lock);
            return agx_g18_sync_err(ENOMEM);
         }
         agx_g18_syncs.obj = n;
         agx_g18_syncs.cap = cap;
         if (agx_g18_syncs.count == 0)
            agx_g18_syncs.count = 1; /* handle 0 is never valid */
      }
      h = agx_g18_syncs.count++;
   }
   agx_g18_syncs.obj[h] = (struct agx_g18_syncobj){
      .value = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0,
      .live = true,
   };
   mtx_unlock(&agx_g18_syncs.lock);
   *handle = h;
   return 0;
}

static int
agx_g18_sync_destroy(struct util_sync_provider *p, uint32_t handle)
{
   (void)p;
   mtx_lock(&agx_g18_syncs.lock);
   struct agx_g18_syncobj *o = agx_g18_sync_get(handle);
   if (o) {
      o->live = false;
      /* Forwards into it are dead; forwards out of it never fire. */
      for (uint32_t i = 0; i < agx_g18_syncs.nfwd;) {
         if (agx_g18_syncs.fwd[i].dst == handle ||
             agx_g18_syncs.fwd[i].src == handle)
            agx_g18_syncs.fwd[i] = agx_g18_syncs.fwd[--agx_g18_syncs.nfwd];
         else
            i++;
      }
      u_cnd_monotonic_broadcast(&agx_g18_syncs.cond);
   }
   mtx_unlock(&agx_g18_syncs.lock);
   return o ? 0 : agx_g18_sync_err(EINVAL);
}

static int
agx_g18_sync_no_fd(struct util_sync_provider *p, uint32_t handle, int *fd)
{
   (void)p, (void)handle, (void)fd;
   return agx_g18_sync_err(EOPNOTSUPP);
}

static int
agx_g18_sync_no_fd_import(struct util_sync_provider *p, int fd,
                          uint32_t *handle)
{
   (void)p, (void)fd, (void)handle;
   return agx_g18_sync_err(EOPNOTSUPP);
}

static int
agx_g18_sync_no_sync_file_import(struct util_sync_provider *p,
                                 uint32_t handle, int fd)
{
   (void)p, (void)handle, (void)fd;
   return agx_g18_sync_err(EOPNOTSUPP);
}

/* `retire`: an observation by the application (the GPU must be done with
 * what the table says is signalled). A SUBMIT's own in-syncs pass false:
 * the render node runs and retires every job in order (one job slot), so
 * the next SUBMIT starts after the GPU finished the pending frame anyway
 * (G6 track Q: one sync-only SUBMIT round trip less per SUBMIT). */
static int
agx_g18_sync_wait_impl(uint32_t *handles, uint64_t *points,
                       unsigned num_handles, int64_t timeout_nsec,
                       unsigned flags, uint32_t *first_signaled, bool retire)
{
   const bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
   const bool avail = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;

   if (num_handles == 0)
      return 0;

   struct timespec deadline = {
      .tv_sec = timeout_nsec / 1000000000ll,
      .tv_nsec = timeout_nsec % 1000000000ll,
   };

   mtx_lock(&agx_g18_syncs.lock);
   for (;;) {
      unsigned done = 0, first = UINT32_MAX;
      for (unsigned i = 0; i < num_handles; i++) {
         struct agx_g18_syncobj *o = agx_g18_sync_get(handles[i]);
         if (!o) {
            mtx_unlock(&agx_g18_syncs.lock);
            return agx_g18_sync_err(EINVAL);
         }
         uint64_t point = points ? points[i] : 0;
         bool ok = avail ? agx_g18_point_available(handles[i], o, point)
                         : agx_g18_point_done(o, point);
         if (ok) {
            done++;
            if (first == UINT32_MAX)
               first = i;
         }
      }

      if (all ? done == num_handles : done > 0) {
         mtx_unlock(&agx_g18_syncs.lock);
         /* Retire AFTER observing the signal: any G18_SUBMIT_ASYNC that
          * produced this signal already set agx_g18_async.pending before
          * acquiring agx_g18_syncs.lock, so retiring here guarantees the
          * GPU has finished before the waiter touches its buffers. */
         if (retire && !avail)
            agx_g18_async_retire();
         if (first_signaled)
            *first_signaled = first;
         return 0;
      }

      if (os_time_get_nano() >= timeout_nsec) {
         mtx_unlock(&agx_g18_syncs.lock);
         return agx_g18_sync_err(ETIME);
      }

      u_cnd_monotonic_timedwait(&agx_g18_syncs.cond, &agx_g18_syncs.lock,
                                &deadline);
   }
}

static int
agx_g18_sync_timeline_wait(struct util_sync_provider *p, uint32_t *handles,
                           uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags,
                           uint32_t *first_signaled)
{
   (void)p;
   return agx_g18_sync_wait_impl(handles, points, num_handles, timeout_nsec,
                                 flags, first_signaled, true);
}

static int
agx_g18_sync_wait(struct util_sync_provider *p, uint32_t *handles,
                  unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                  uint32_t *first_signaled)
{
   return agx_g18_sync_timeline_wait(p, handles, NULL, num_handles,
                                     timeout_nsec, flags, first_signaled);
}

static int
agx_g18_sync_reset(struct util_sync_provider *p, const uint32_t *handles,
                   uint32_t count)
{
   (void)p;
   int ret = 0;
   mtx_lock(&agx_g18_syncs.lock);
   for (uint32_t i = 0; i < count; i++) {
      struct agx_g18_syncobj *o = agx_g18_sync_get(handles[i]);
      if (!o) {
         ret = agx_g18_sync_err(EINVAL);
         continue;
      }
      o->value = 0;
      for (uint32_t j = 0; j < agx_g18_syncs.nfwd;) {
         if (agx_g18_syncs.fwd[j].dst == handles[i])
            agx_g18_syncs.fwd[j] = agx_g18_syncs.fwd[--agx_g18_syncs.nfwd];
         else
            j++;
      }
   }
   mtx_unlock(&agx_g18_syncs.lock);
   return ret;
}

static int
agx_g18_sync_timeline_signal(struct util_sync_provider *p,
                             const uint32_t *handles, uint64_t *points,
                             uint32_t count)
{
   (void)p;
   int ret = 0;
   mtx_lock(&agx_g18_syncs.lock);
   for (uint32_t i = 0; i < count; i++) {
      if (!agx_g18_sync_get(handles[i])) {
         ret = agx_g18_sync_err(EINVAL);
         continue;
      }
      agx_g18_signal_locked(handles[i], points ? points[i] : 0);
   }
   mtx_unlock(&agx_g18_syncs.lock);
   return ret;
}

static int
agx_g18_sync_signal(struct util_sync_provider *p, const uint32_t *handles,
                    uint32_t count)
{
   return agx_g18_sync_timeline_signal(p, handles, NULL, count);
}

static int
agx_g18_sync_query(struct util_sync_provider *p, uint32_t *handles,
                   uint64_t *points, uint32_t count, uint32_t flags)
{
   (void)p, (void)flags;
   int ret = 0;
   mtx_lock(&agx_g18_syncs.lock);
   for (uint32_t i = 0; i < count; i++) {
      struct agx_g18_syncobj *o = agx_g18_sync_get(handles[i]);
      if (!o) {
         ret = agx_g18_sync_err(EINVAL);
         continue;
      }
      points[i] = o->value;
   }
   mtx_unlock(&agx_g18_syncs.lock);
   /* Retire AFTER reading the table so every signal reflected in points[]
    * has completed on the GPU before the caller observes it. */
   agx_g18_async_retire();
   return ret;
}

static int
agx_g18_sync_transfer(struct util_sync_provider *p, uint32_t dst_handle,
                      uint64_t dst_point, uint32_t src_handle,
                      uint64_t src_point, uint32_t flags)
{
   (void)p, (void)flags;
   mtx_lock(&agx_g18_syncs.lock);
   struct agx_g18_syncobj *d = agx_g18_sync_get(dst_handle);
   struct agx_g18_syncobj *s = agx_g18_sync_get(src_handle);
   if (!d || !s) {
      mtx_unlock(&agx_g18_syncs.lock);
      return agx_g18_sync_err(EINVAL);
   }

   if (agx_g18_point_done(s, src_point)) {
      agx_g18_signal_locked(dst_handle, dst_point);
   } else {
      if (agx_g18_syncs.nfwd == AGX_WIN_MAX_FORWARDS) {
         mtx_unlock(&agx_g18_syncs.lock);
         return agx_g18_sync_err(ENOMEM);
      }
      /* A binary destination gets a new, unsignalled fence. */
      if (dst_point == 0)
         d->value = 0;
      agx_g18_syncs.fwd[agx_g18_syncs.nfwd++] = (struct agx_g18_forward){
         .src = src_handle,
         .dst = dst_handle,
         .src_point = src_point,
         .dst_point = dst_point,
      };
      u_cnd_monotonic_broadcast(&agx_g18_syncs.cond);
   }
   mtx_unlock(&agx_g18_syncs.lock);
   return 0;
}

/* SUBMIT helpers: in-syncs and out-syncs of a drm_asahi_submit. */
static int
agx_g18_sync_wait_submit(const struct drm_asahi_sync *syncs, uint32_t count)
{
   STACK_ARRAY(uint32_t, handles, count);
   STACK_ARRAY(uint64_t, points, count);
   for (uint32_t i = 0; i < count; i++) {
      handles[i] = syncs[i].handle;
      points[i] = syncs[i].sync_type == DRM_ASAHI_SYNC_SYNCOBJ
                     ? 0
                     : syncs[i].timeline_value;
   }
   /* The kernel would hold the job; a CPU wait does the same here. Bounded
    * by the kernel's job timeout (10 s) plus margin, then device lost. */
   int ret = agx_g18_sync_wait_impl(
      handles, points, count, os_time_get_absolute_timeout(30000000000ull),
      DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL, false);
   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(points);
   if (ret)
      fprintf(stderr, "agx-g18: SUBMIT in-syncs did not signal: %s\n",
              strerror(-ret));
   return ret;
}

static void
agx_g18_sync_signal_submit(const struct drm_asahi_sync *syncs, uint32_t count)
{
   mtx_lock(&agx_g18_syncs.lock);
   for (uint32_t i = 0; i < count; i++) {
      agx_g18_signal_locked(syncs[i].handle,
                            syncs[i].sync_type == DRM_ASAHI_SYNC_SYNCOBJ
                               ? 0
                               : syncs[i].timeline_value);
   }
   mtx_unlock(&agx_g18_syncs.lock);
}

static void
agx_g18_sync_finalize(struct util_sync_provider *p)
{
   free(p);
}

static struct util_sync_provider *
agx_g18_sync_clone(struct util_sync_provider *p)
{
   (void)p;
   return agx_g18_sync_provider_create(-1);
}

struct util_sync_provider *
agx_g18_sync_provider_create(int fd)
{
   (void)fd; /* the table is process-wide */
   call_once(&agx_g18_syncs.once, agx_g18_syncs_init);

   struct util_sync_provider *p = calloc(1, sizeof(*p));
   if (!p)
      return NULL;

   *p = (struct util_sync_provider){
      .create = agx_g18_sync_create,
      .destroy = agx_g18_sync_destroy,
      .handle_to_fd = agx_g18_sync_no_fd,
      .fd_to_handle = agx_g18_sync_no_fd_import,
      .import_sync_file = agx_g18_sync_no_sync_file_import,
      .export_sync_file = agx_g18_sync_no_fd,
      .wait = agx_g18_sync_wait,
      .reset = agx_g18_sync_reset,
      .signal = agx_g18_sync_signal,
      .timeline_signal = agx_g18_sync_timeline_signal,
      .timeline_wait = agx_g18_sync_timeline_wait,
      .query = agx_g18_sync_query,
      .transfer = agx_g18_sync_transfer,
      .finalize = agx_g18_sync_finalize,
      .clone = agx_g18_sync_clone,
   };
   return p;
}
