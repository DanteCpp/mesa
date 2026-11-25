#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include "asahi_drm.h"

static int (*real_ioctl)(int, unsigned long, void *);
static _Atomic uint64_t *counts;

__attribute__((constructor)) static void init(void)
{
   real_ioctl = dlsym(RTLD_NEXT, "drmIoctl");
   const char *path = getenv("GB_OWN_GPU_COUNTERS");
   if (!path) return;
   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) abort();
   counts = mmap(NULL, 32, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   close(fd);
   if (counts == MAP_FAILED || !real_ioctl) abort();
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
   int ret = real_ioctl(fd, request, arg);
   if (ret || !counts || request != DRM_IOCTL_ASAHI_SUBMIT) return ret;
   const struct drm_asahi_submit *sub = arg;
   const unsigned char *p = (const void *)(uintptr_t)sub->cmdbuf;
   size_t remaining = sub->cmdbuf_size;
   uint64_t render = 0, compute = 0;
   while (remaining >= sizeof(struct drm_asahi_cmd_header)) {
      struct drm_asahi_cmd_header h;
      memcpy(&h, p, sizeof(h));
      size_t size = sizeof(h) + h.size;
      if (size > remaining) break;
      render += h.cmd_type == DRM_ASAHI_CMD_RENDER;
      compute += h.cmd_type == DRM_ASAHI_CMD_COMPUTE;
      p += size; remaining -= size;
   }
   atomic_fetch_add_explicit(&counts[0], 1, memory_order_relaxed);
   atomic_fetch_add_explicit(&counts[1], render, memory_order_relaxed);
   atomic_fetch_add_explicit(&counts[2], compute, memory_order_relaxed);
   if (remaining) atomic_fetch_add_explicit(&counts[3], 1, memory_order_relaxed);
   return ret;
}
