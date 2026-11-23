/*
 * Based on panfrost/pan_fence.c:
 *
 * Copyright 2022 Amazon.com, Inc. or its affiliates.
 * Copyright 2008 VMware, Inc.
 * Copyright 2014 Broadcom
 * Copyright 2018 Alyssa Rosenzweig
 * Copyright 2019 Collabora, Ltd.
 * Copyright 2012 Rob Clark
 * SPDX-License-Identifier: MIT
 */

#ifndef _WIN32
#include <xf86drm.h>
#include "util/libsync.h"
#else
#include <windows.h>
#include "drm-uapi/drm.h"
#endif
#include <string.h>

#include "agx_fence.h"
#include "agx_state.h"

#include "util/os_file.h"
#include "util/os_time.h"
#include "util/u_inlines.h"

#ifdef _WIN32
static int64_t
agx_win_abs_timeout(uint64_t timeout)
{
   if (timeout == 0)
      return 0;
   if (timeout == OS_TIMEOUT_INFINITE)
      return INT64_MAX;
   LARGE_INTEGER freq, ctr;
   if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
      return INT64_MAX;
   QueryPerformanceCounter(&ctr);
   int64_t sec = ctr.QuadPart / freq.QuadPart;
   int64_t rem = ctr.QuadPart % freq.QuadPart;
   int64_t now = sec * INT64_C(1000000000) +
                 (rem * INT64_C(1000000000)) / freq.QuadPart;
   if (timeout > (uint64_t)(INT64_MAX - now))
      return INT64_MAX;
   return now + (int64_t)timeout;
}
#endif

void
agx_fence_reference(struct pipe_screen *pscreen, struct pipe_fence_handle **ptr,
                    struct pipe_fence_handle *fence)
{
   struct agx_screen *screen = agx_screen(pscreen);
   struct pipe_fence_handle *old = *ptr;

   if (pipe_reference(old ? &old->reference : NULL,
                      fence ? &fence->reference : NULL)) {
      if (!old->is_timeline && old->syncobj)
         agx_syncobj_destroy(screen, old->syncobj);
      free(old);
   }

   *ptr = fence;
}

bool
agx_fence_finish(struct pipe_screen *pscreen, struct pipe_context *ctx,
                 struct pipe_fence_handle *fence, uint64_t timeout)
{
   struct agx_screen *screen = agx_screen(pscreen);
   int ret;

   if (fence->signaled)
      return true;

#ifdef _WIN32
   int64_t abs_timeout = agx_win_abs_timeout(timeout);
#else
   uint64_t abs_timeout = os_time_get_absolute_timeout(timeout);
   if (abs_timeout == OS_TIMEOUT_INFINITE)
      abs_timeout = INT64_MAX;
#endif

   if (fence->is_timeline) {
      uint32_t handle = screen->flush_syncobj;
      uint64_t point = fence->timeline_value;
      ret = agx_syncobj_timeline_wait(screen, &handle, &point, 1, abs_timeout,
                                      DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
   } else {
      ret = agx_syncobj_wait(screen, &fence->syncobj, 1, abs_timeout,
                             DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
   }

   if (ret < 0 && ret != -ETIME) {
      agx_msg("fence wait failed: %s\n", strerror(-ret));
      return false;
   }

   fence->signaled = (ret >= 0);
   return fence->signaled;
}

int
agx_fence_get_fd(struct pipe_screen *pscreen, struct pipe_fence_handle *f)
{
#ifdef _WIN32
   (void)pscreen;
   (void)f;
   return -1;
#else
   struct agx_screen *screen = agx_screen(pscreen);
   int fd = -1;

   int ret = agx_syncobj_export_sync_file(screen, f->syncobj, &fd);
   if (ret || fd < 0) {
      agx_msg("fence export failed\n");
      return -1;
   }

   return fd;
#endif
}

struct pipe_fence_handle *
agx_fence_from_fd(struct agx_context *ctx, int fd, enum pipe_fd_type type)
{
#ifdef _WIN32
   (void)ctx;
   (void)fd;
   (void)type;
   return NULL;
#else
   struct agx_screen *screen = agx_screen(ctx->base.screen);
   int ret;

   struct pipe_fence_handle *f = calloc(1, sizeof(*f));
   if (!f)
      return NULL;

   if (type == PIPE_FD_TYPE_NATIVE_SYNC) {
      ret = agx_syncobj_create(screen, 0, &f->syncobj);
      if (ret) {
         agx_msg("create syncobj failed\n");
         goto err_free_fence;
      }

      ret = agx_syncobj_import_sync_file(screen, f->syncobj, fd);
      if (ret) {
         agx_msg("import syncfile failed\n");
         goto err_destroy_syncobj;
      }
   } else {
      assert(type == PIPE_FD_TYPE_SYNCOBJ);
      ret = agx_syncobj_fd_to_handle(screen, fd, &f->syncobj);
      if (ret) {
         agx_msg("import syncobj FD failed\n");
         goto err_free_fence;
      }
   }

   pipe_reference_init(&f->reference, 1);

   return f;

err_destroy_syncobj:
   agx_syncobj_destroy(screen, f->syncobj);
err_free_fence:
   free(f);
   return NULL;
#endif
}

struct pipe_fence_handle *
agx_fence_create(struct agx_context *ctx)
{
   struct agx_screen *screen = agx_screen(ctx->base.screen);

   /* A failed submission must not export an earlier rendering success. */
   if (ctx->any_faults)
      return NULL;

#ifdef _WIN32
   /* Snapshot the screen's monotonic timeline point for this context's last
    * submission. Avoids sync-file export/import and extra kernel syncobjs:
    * waiting checks the monitored fence's CPU VA directly. */
   struct pipe_fence_handle *f = calloc(1, sizeof(*f));
   if (!f)
      return NULL;
   pipe_reference_init(&f->reference, 1);
   f->is_timeline = true;
   f->timeline_value = ctx->flush_last_seqid;
   f->signaled = (ctx->flush_last_seqid == 0);
   (void)screen;
   return f;
#else
   int fd = -1, ret;

   /* Snapshot the last rendering out fence. We'd rather have another
    * syncobj instead of a sync file, but this is all we get.
    * (HandleToFD/FDToHandle just gives you another syncobj ID for the
    * same syncobj).
    */
   ret = agx_syncobj_export_sync_file(screen, ctx->syncobj, &fd);
   if (ret || fd == -1) {
      agx_msg("export failed\n");
      return NULL;
   }

   /* fence_server_sync() defers an imported native fence until the next GPU
    * submission.  A fence-only flush has no batch to consume that dependency,
    * but the newly exported fence is still ordered after the server wait.  In
    * particular, Wayland explicit-sync clients use exactly this sequence when
    * forwarding an acquire fence to a release fence without drawing.
    *
    * Snapshot both the last rendering fence and the pending server waits.  A
    * completed rendering fence can be discarded, avoiding a sync-file merge
    * for the common no-work handoff.  Leave ctx->in_sync_fd intact: a later
    * real submission must wait on it too.
    */
   if (ctx->in_sync_fd >= 0) {
      ret = agx_syncobj_wait(screen, &ctx->syncobj, 1, 0,
                             DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);

      if (ret == 0) {
         int pending = os_dupfd_cloexec(ctx->in_sync_fd);
         if (pending >= 0) {
            close(fd);
            fd = pending;
         } else if (agx_fallback_wait(ctx->in_sync_fd, "fence dup failed")) {
            close(fd);
            fd = -1;
         }
      } else if (ret == -ETIME) {
         int merged = sync_merge("asahi-fence", fd, ctx->in_sync_fd);
         if (merged >= 0) {
            close(fd);
            fd = merged;
         } else if (agx_fallback_wait(fd, "fence merge failed") ||
                    agx_fallback_wait(ctx->in_sync_fd, "fence merge failed")) {
            close(fd);
            fd = -1;
         }
      } else {
         close(fd);
         fd = -1;
      }

      if (fd < 0) {
         agx_msg("failed to snapshot pending server wait\n");
         return NULL;
      }
   }

   struct pipe_fence_handle *f =
      agx_fence_from_fd(ctx, fd, PIPE_FD_TYPE_NATIVE_SYNC);

   close(fd);

   return f;
#endif
}

void
agx_create_fence_fd(struct pipe_context *pctx,
                    struct pipe_fence_handle **pfence, int fd,
                    enum pipe_fd_type type)
{
   *pfence = agx_fence_from_fd(agx_context(pctx), fd, type);
}

void
agx_fence_server_sync(struct pipe_context *pctx, struct pipe_fence_handle *f,
                      uint64_t value)
{
#ifdef _WIN32
   (void)pctx;
   (void)f;
   (void)value;
#else
   struct agx_screen *screen = agx_screen(pctx->screen);
   struct agx_context *ctx = agx_context(pctx);
   int fd = -1, ret;
   assert(!value);

   ret = agx_syncobj_export_sync_file(screen, f->syncobj, &fd);
   if (ret || fd < 0) {
      agx_msg("server-wait fence export failed\n");
      ctx->any_faults = true;
      return;
   }

   ret = sync_accumulate("asahi", &ctx->in_sync_fd, fd);
   if (ret || ctx->in_sync_fd < 0) {
      /* Keep the previous accumulated fence and consume this dependency on
       * the CPU if the kernel cannot allocate a merged sync file. Dropping it
       * would let the next GPU submission run before the acquire fence. */
      if (agx_fallback_wait(fd, "fence accumulation failed")) {
         agx_msg("server-wait fence accumulation and fallback wait failed\n");
         const bool first_fault = !ctx->any_faults;
         ctx->any_faults = true;
         if (first_fault && ctx->reset.reset)
            ctx->reset.reset(ctx->reset.data, PIPE_GUILTY_CONTEXT_RESET);
      }
   }
   close(fd);
#endif
}
