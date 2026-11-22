/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Render-node transport (agx_win_kmd.h): pseudo-fds, frame marshalling,
 * errno mapping, and the two backends: DeviceIoControl on \\.\G18Win0
 * (Windows only) and the in-process stub (any host, for tests and C3).
 *
 * Plain C11 plus <windows.h> on Windows, so the host test can build it with
 * the stub anywhere.
 */

#include "agx_win_kmd.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef EOPNOTSUPP
#define EOPNOTSUPP ENOTSUP
#endif

/* The debug-log build (G4-PLAN 6, S7 variant 3) is compiled with
 * -DAGX_WIN_KMD_TRACE=1: every render-node call, open and close is logged
 * (stderr and, when the device opens, the driver's serial log through
 * G18_IOCTL_LOG). The normal build compiles none of it. */
#ifdef AGX_WIN_KMD_TRACE
#include <stdarg.h>
static void agx_win_trace(const char *fmt, ...);
static const char *
agx_win_cmd_name(uint32_t cmd)
{
   switch (cmd) {
   case G18_RN_GET_PARAMS: return "GET_PARAMS";
   case G18_RN_GET_TIME: return "GET_TIME";
   case G18_RN_VM_CREATE: return "VM_CREATE";
   case G18_RN_VM_DESTROY: return "VM_DESTROY";
   case G18_RN_VM_BIND: return "VM_BIND";
   case G18_RN_GEM_CREATE: return "GEM_CREATE";
   case G18_RN_GEM_MMAP_OFFSET: return "GEM_MMAP_OFFSET";
   case G18_RN_GEM_BIND_OBJECT: return "GEM_BIND_OBJECT";
   case G18_RN_QUEUE_CREATE: return "QUEUE_CREATE";
   case G18_RN_QUEUE_DESTROY: return "QUEUE_DESTROY";
   case G18_RN_SUBMIT: return "SUBMIT";
   case G18_RN_GEM_MADVISE: return "GEM_MADVISE";
   case G18_RN_GEM_USERPTR: return "GEM_USERPTR";
   case G18_RN_GEM_RESIDENCY: return "GEM_RESIDENCY";
   case G18_RN_VERSION: return "VERSION";
   case G18_RN_GEM_CLOSE: return "GEM_CLOSE";
   case G18_RN_GEM_MAP: return "GEM_MAP";
   case G18_RN_GEM_UNMAP: return "GEM_UNMAP";
   default: return "?";
   }
}
#define AGX_WIN_TRACE(...) agx_win_trace(__VA_ARGS__)
#else
#define AGX_WIN_TRACE(...) ((void)0)
#endif

enum agx_win_kind {
   AGX_WIN_NONE = 0,
   AGX_WIN_KERNEL,
   AGX_WIN_STUB,
};

struct agx_win_node {
   enum agx_win_kind kind;
#ifdef _WIN32
   HANDLE handle;
#endif
   struct agx_win_stub *stub;
};

static struct agx_win_node agx_win_nodes[AGX_WIN_KMD_MAX_OPEN];
static atomic_flag agx_win_lock = ATOMIC_FLAG_INIT;

/* Held only around table updates and lookups, never across a call into the
 * kernel (a SUBMIT waits for the GPU) or the stub (which has its own lock). */
static void
agx_win_table_lock(void)
{
   while (atomic_flag_test_and_set_explicit(&agx_win_lock,
                                            memory_order_acquire))
      ;
}

static void
agx_win_table_unlock(void)
{
   atomic_flag_clear_explicit(&agx_win_lock, memory_order_release);
}

static struct agx_win_node *
agx_win_node_locked(int fd)
{
   int i = fd - AGX_WIN_KMD_FD_BASE;
   if (i < 0 || i >= AGX_WIN_KMD_MAX_OPEN ||
       agx_win_nodes[i].kind == AGX_WIN_NONE)
      return NULL;
   return &agx_win_nodes[i];
}

int
agx_win_kmd_errno(int32_t e)
{
   /* The wire carries Linux numbers (g18_uapi.h G18_RN_E*); the UCRT numbers
    * differ from EAGAIN on. Map by name. */
   switch (e) {
   case 0: return 0;
   case G18_RN_EPERM: return EPERM;
   case G18_RN_ENOENT: return ENOENT;
   case G18_RN_EIO: return EIO;
   case G18_RN_E2BIG: return E2BIG;
   case G18_RN_EAGAIN: return EAGAIN;
   case G18_RN_ENOMEM: return ENOMEM;
   case G18_RN_EACCES: return EACCES;
   case G18_RN_EFAULT: return EFAULT;
   case G18_RN_EBUSY: return EBUSY;
   case G18_RN_EEXIST: return EEXIST;
   case G18_RN_ENODEV: return ENODEV;
   case G18_RN_EINVAL: return EINVAL;
   case G18_RN_ENOTTY: return ENOTTY;
   case G18_RN_ENOSPC: return ENOSPC;
   case G18_RN_ERANGE: return ERANGE;
   case G18_RN_ENOSYS: return ENOSYS;
   case G18_RN_EOPNOTSUPP: return EOPNOTSUPP;
   case G18_RN_ETIMEDOUT: return ETIMEDOUT;
   case G18_RN_ECANCELED: return ECANCELED;
   default: return EIO;
   }
}

static int
agx_win_fail(int e)
{
   errno = e;
   return -1;
}

#ifdef _WIN32
static HANDLE
agx_win_open_handle(int *err)
{
   HANDLE h = CreateFileW(G18_DOS_NAME, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (h == INVALID_HANDLE_VALUE) {
      DWORD e = GetLastError();
      *err = (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION)
                ? EACCES
                : ENODEV;
   }
   return h;
}
#endif

int
agx_win_kmd_open(int *out_fd)
{
   const char *mode = getenv("AGX_WIN_KMD_STUB");
   bool stub = mode && !strcmp(mode, "fake");
   struct agx_win_node node = {0};

   if (stub) {
      node.kind = AGX_WIN_STUB;
      node.stub = agx_win_stub_open();
      if (!node.stub)
         return -1; /* errno set */
   } else {
#ifdef _WIN32
      int err = ENODEV;
      node.handle = agx_win_open_handle(&err);
      if (node.handle == INVALID_HANDLE_VALUE)
         return agx_win_fail(err);
      node.kind = AGX_WIN_KERNEL;
#else
      return agx_win_fail(ENODEV);
#endif
   }

   agx_win_table_lock();
   for (int i = 0; i < AGX_WIN_KMD_MAX_OPEN; i++) {
      if (agx_win_nodes[i].kind != AGX_WIN_NONE)
         continue;
      agx_win_nodes[i] = node;
      agx_win_table_unlock();
      *out_fd = AGX_WIN_KMD_FD_BASE + i;
      AGX_WIN_TRACE("open fd %d (%s)", *out_fd, stub ? "stub" : "\\\\.\\G18Win0");
      return 0;
   }
   agx_win_table_unlock();

   if (stub)
      agx_win_stub_close(node.stub);
#ifdef _WIN32
   else
      CloseHandle(node.handle);
#endif
   return agx_win_fail(EMFILE);
}

void
agx_win_kmd_close(int fd)
{
   agx_win_table_lock();
   struct agx_win_node *n = agx_win_node_locked(fd);
   struct agx_win_node node = n ? *n : (struct agx_win_node){0};
   if (n)
      memset(n, 0, sizeof(*n));
   agx_win_table_unlock();

   AGX_WIN_TRACE("close fd %d%s", fd, n ? "" : " (not open)");
   if (node.kind == AGX_WIN_STUB)
      agx_win_stub_close(node.stub);
#ifdef _WIN32
   else if (node.kind == AGX_WIN_KERNEL)
      CloseHandle(node.handle);
#endif
}

bool
agx_win_kmd_is_fd(int fd)
{
   agx_win_table_lock();
   bool ok = agx_win_node_locked(fd) != NULL;
   agx_win_table_unlock();
   return ok;
}

bool
agx_win_kmd_is_stub(int fd)
{
   agx_win_table_lock();
   struct agx_win_node *n = agx_win_node_locked(fd);
   bool stub = n && n->kind == AGX_WIN_STUB;
   agx_win_table_unlock();
   return stub;
}

/* Send one frame. 0: the frame was handled and hdr.result holds the answer;
 * -1/errno: the driver refused the frame itself (EINVAL, ENOTTY) or the
 * transport failed. */
static int
agx_win_send(int fd, uint32_t cmd, void *frame, uint32_t size)
{
   agx_win_table_lock();
   struct agx_win_node *n = agx_win_node_locked(fd);
   struct agx_win_node node = n ? *n : (struct agx_win_node){0};
   agx_win_table_unlock();

   const uint32_t code = G18_RN_CTL(cmd);

   if (node.kind == AGX_WIN_STUB) {
      int ret = agx_win_stub_ioctl(node.stub, code, frame, size);
      return ret ? agx_win_fail(-ret) : 0;
   }

#ifdef _WIN32
   if (node.kind == AGX_WIN_KERNEL) {
      DWORD got = 0;
      if (DeviceIoControl(node.handle, code, frame, size, frame, size, &got,
                          NULL))
         return got == size ? 0 : agx_win_fail(EIO);

      switch (GetLastError()) {
      case ERROR_INVALID_PARAMETER: /* STATUS_INVALID_PARAMETER */
         return agx_win_fail(EINVAL);
      case ERROR_INVALID_FUNCTION: /* STATUS_INVALID_DEVICE_REQUEST */
      case ERROR_NOT_SUPPORTED:
         return agx_win_fail(ENOTTY);
      case ERROR_NOT_ENOUGH_MEMORY:
      case ERROR_NO_SYSTEM_RESOURCES:
         return agx_win_fail(ENOMEM);
      case ERROR_ACCESS_DENIED:
         return agx_win_fail(EACCES);
      default:
         return agx_win_fail(EIO);
      }
   }
#endif

   return agx_win_fail(EBADF);
}

/* The inline region a command carries, named by a pointer field of its
 * argument (g18_uapi.h, "Frame"). */
struct agx_win_region {
   uint32_t ptr_offset; /* of the u64 pointer field in the argument */
   void *ptr;           /* the caller's CPU pointer */
   uint64_t size;
   bool present;        /* the command has a pointer field */
   bool out;            /* copied back after the call */
};

static int
agx_win_region_for(uint32_t cmd, const void *arg, uint32_t arg_size,
                   struct agx_win_region *r)
{
   memset(r, 0, sizeof(*r));

   switch (cmd) {
   case G18_RN_GET_PARAMS: {
      const struct g18_asahi_get_params *a = arg;
      if (arg_size < sizeof(*a))
         return 0;
      r->present = true;
      r->ptr_offset = offsetof(struct g18_asahi_get_params, pointer);
      r->ptr = (void *)(uintptr_t)a->pointer;
      r->size = a->size;
      r->out = true;
      return 0;
   }
   case G18_RN_VM_BIND: {
      const struct g18_asahi_vm_bind *a = arg;
      if (arg_size < sizeof(*a))
         return 0;
      r->present = true;
      r->ptr_offset = offsetof(struct g18_asahi_vm_bind, userptr);
      r->ptr = (void *)(uintptr_t)a->userptr;
      r->size = (uint64_t)a->num_binds * a->stride;
      return 0;
   }
   case G18_RN_SUBMIT: {
      const struct g18_asahi_submit *a = arg;
      if (arg_size < sizeof(*a))
         return 0;
      /* Syncs are user mode (G4-PLAN 3.1); the backend strips them. */
      if (a->syncs || a->in_sync_count || a->out_sync_count)
         return EINVAL;
      r->present = true;
      r->ptr_offset = offsetof(struct g18_asahi_submit, cmdbuf);
      r->ptr = (void *)(uintptr_t)a->cmdbuf;
      r->size = a->cmdbuf_size;
      return 0;
   }
   default:
      return 0;
   }
}

/* G6 perf: AGX_WIN_CALLSTATS=<n> (Windows): count and time every
 * render-node call in the normal build and print, every n SUBMITs, the
 * calls since the last summary (count, total and largest microseconds per
 * command) and the time spent outside the render node. Off by default. */
#ifdef _WIN32
static struct {
   int on; /* -1 unread */
   unsigned every, submits;
   LARGE_INTEGER qf, since;
   unsigned long long n[G18_RN_GEM_UNMAP + 1], us[G18_RN_GEM_UNMAP + 1],
      max[G18_RN_GEM_UNMAP + 1];
} agx_win_cs = {.on = -1};
static atomic_flag agx_win_cs_lock = ATOMIC_FLAG_INIT;

static void
agx_win_callstats(uint32_t cmd, LARGE_INTEGER t0, LARGE_INTEGER t1)
{
   if (agx_win_cs.on == 0 || cmd > G18_RN_GEM_UNMAP)
      return;
   while (atomic_flag_test_and_set(&agx_win_cs_lock))
      ;
   if (agx_win_cs.on < 0) {
      char v[16];
      DWORD n = GetEnvironmentVariableA("AGX_WIN_CALLSTATS", v, sizeof(v));
      agx_win_cs.every = n && n < sizeof(v) ? (unsigned)strtoul(v, NULL, 10) : 0;
      agx_win_cs.on = agx_win_cs.every != 0;
      QueryPerformanceFrequency(&agx_win_cs.qf);
      agx_win_cs.since = t0;
   }
   if (agx_win_cs.on) {
      unsigned long long us = (unsigned long long)((t1.QuadPart - t0.QuadPart) *
                                                  1000000 / (agx_win_cs.qf.QuadPart ? agx_win_cs.qf.QuadPart : 1));
      agx_win_cs.n[cmd]++;
      agx_win_cs.us[cmd] += us;
      if (us > agx_win_cs.max[cmd])
         agx_win_cs.max[cmd] = us;
      if (cmd == G18_RN_SUBMIT && ++agx_win_cs.submits >= agx_win_cs.every) {
         unsigned long long wall = (unsigned long long)((t1.QuadPart - agx_win_cs.since.QuadPart) *
                                                       1000000 / (agx_win_cs.qf.QuadPart ? agx_win_cs.qf.QuadPart : 1));
         unsigned long long in = 0;
         char line[1024];
         int o = snprintf(line, sizeof(line), "agx-win: callstats %u submits:", agx_win_cs.submits);
         for (uint32_t c = 0; c <= G18_RN_GEM_UNMAP; c++) {
            if (!agx_win_cs.n[c])
               continue;
            in += agx_win_cs.us[c];
            if (o > 0 && (size_t)o < sizeof(line))
               o += snprintf(line + o, sizeof(line) - o, " %#x n %llu us %llu max %llu;", c,
                             agx_win_cs.n[c], agx_win_cs.us[c], agx_win_cs.max[c]);
            agx_win_cs.n[c] = agx_win_cs.us[c] = agx_win_cs.max[c] = 0;
         }
         fprintf(stderr, "%s wall %llu us, in the render node %llu us\n", line, wall, in);
         agx_win_cs.submits = 0;
         agx_win_cs.since = t1;
      }
   }
   atomic_flag_clear(&agx_win_cs_lock);
}
#endif

int
agx_win_kmd_call(int fd, uint32_t cmd, void *arg, uint32_t arg_size)
{
   if (arg_size > G18_RN_ARG_MAX || (arg_size && !arg))
      return agx_win_fail(EINVAL);

   struct agx_win_region r;
   int err = agx_win_region_for(cmd, arg, arg_size, &r);
   if (err)
      return agx_win_fail(err);
   if (r.size && !r.ptr)
      return agx_win_fail(EFAULT);

   const uint32_t data_off = G18_RN_DATA_OFFSET(arg_size);
   const uint64_t frame_size64 = data_off + ((r.size + 7) & ~(uint64_t)7);
   if (frame_size64 > G18_RN_FRAME_MAX)
      return agx_win_fail(E2BIG);
   const uint32_t frame_size = (uint32_t)frame_size64;

   uint64_t small[64]; /* 512 bytes: every call without inline data */
   uint8_t *frame = frame_size <= sizeof(small) ? (uint8_t *)small
                                                 : malloc(frame_size);
   if (!frame)
      return agx_win_fail(ENOMEM);
   memset(frame, 0, frame_size);

   struct g18_rn_hdr *hdr = (struct g18_rn_hdr *)frame;
   *hdr = (struct g18_rn_hdr){
      .abi = G18_RN_ABI,
      .cmd = cmd,
      .frame_size = frame_size,
      .arg_size = arg_size,
   };
   memcpy(frame + G18_RN_ARG_OFFSET, arg, arg_size);
   if (r.size) {
      uint64_t off = data_off;
      memcpy(frame + G18_RN_ARG_OFFSET + r.ptr_offset, &off, sizeof(off));
      memcpy(frame + data_off, r.ptr, r.size);
   } else if (r.present) {
      uint64_t none = 0;
      memcpy(frame + G18_RN_ARG_OFFSET + r.ptr_offset, &none, sizeof(none));
   }

#ifdef _WIN32
   LARGE_INTEGER t0, t1;
   QueryPerformanceCounter(&t0);
#endif
#if defined(AGX_WIN_KMD_TRACE) && defined(_WIN32)
   LARGE_INTEGER qf;
#endif
   int ret = agx_win_send(fd, cmd, frame, frame_size);
#ifdef _WIN32
   QueryPerformanceCounter(&t1);
   agx_win_callstats(cmd, t0, t1);
#endif
#ifdef AGX_WIN_KMD_TRACE
   {
      unsigned long long us = 0;
#ifdef _WIN32
      QueryPerformanceFrequency(&qf);
      us = (unsigned long long)((t1.QuadPart - t0.QuadPart) * 1000000 /
                                (qf.QuadPart ? qf.QuadPart : 1));
#endif
      if (ret)
         AGX_WIN_TRACE("fd %d %s arg %u data %llu: transport errno %d (%llu us)",
                       fd, agx_win_cmd_name(cmd), arg_size,
                       (unsigned long long)r.size, errno, us);
      else
         AGX_WIN_TRACE("fd %d %s arg %u data %llu: result %d out %u (%llu us)",
                       fd, agx_win_cmd_name(cmd), arg_size,
                       (unsigned long long)r.size, (int)hdr->result,
                       hdr->out_size, us);
   }
#endif
   if (ret == 0) {
      uint32_t out = hdr->out_size <= arg_size ? hdr->out_size : arg_size;
      /* Keep the caller's pointer field: the frame holds an offset there. */
      uint64_t keep = 0;
      if (r.size)
         memcpy(&keep, (uint8_t *)arg + r.ptr_offset, sizeof(keep));
      memcpy(arg, frame + G18_RN_ARG_OFFSET, out);
      if (r.size && r.ptr_offset + sizeof(keep) <= out)
         memcpy((uint8_t *)arg + r.ptr_offset, &keep, sizeof(keep));
      if (hdr->result == 0 && r.out && r.size)
         memcpy(r.ptr, frame + data_off, r.size);
      if (hdr->result) {
         errno = agx_win_kmd_errno(hdr->result);
         ret = -1;
      }
   }

   if (frame != (uint8_t *)small) {
      int e = errno;
      free(frame);
      errno = e;
   }
   return ret;
}

bool
agx_win_kmd_log(const char *line)
{
#ifdef _WIN32
   static HANDLE log_handle = INVALID_HANDLE_VALUE;
   static atomic_int tried;

   if (!atomic_exchange(&tried, 1)) {
      int err;
      log_handle = agx_win_open_handle(&err);
   }
   if (log_handle == INVALID_HANDLE_VALUE)
      return false;

   struct g18_log_in in = {
      .size = sizeof(in),
      .version = G18_UAPI_VERSION,
   };
   size_t n = strlen(line);
   if (n >= sizeof(in.line))
      n = sizeof(in.line) - 1;
   memcpy(in.line, line, n);
   in.len = (uint32_t)n;
   DWORD got = 0;
   return DeviceIoControl(log_handle, G18_IOCTL_LOG, &in, sizeof(in), NULL, 0,
                          &got, NULL);
#else
   (void)line;
   return false;
#endif
}

#ifdef AGX_WIN_KMD_TRACE
static void
agx_win_trace(const char *fmt, ...)
{
   char line[200];
   int n = snprintf(line, sizeof(line), "agx-win: ");
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line + n, sizeof(line) - n, fmt, ap);
   va_end(ap);
   int e = errno;
   fprintf(stderr, "%s\n", line);
   agx_win_kmd_log(line);
   errno = e;
}
#endif
