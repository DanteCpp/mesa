/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Host test of the Windows render-node transport (win/agx_win_kmd.c: frame
 * marshalling, errno mapping, pseudo-fds) against the stub node
 * (win/agx_win_kmd_stub.c) and its G18R recorder. Runs on any host; no GPU,
 * no Windows.
 *
 * usage: asahi-win-kmd-stub [record.g18r]
 */

#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drm-uapi/asahi_drm.h"
#include "win/agx_win_kmd.h"

#ifdef _WIN32
static void
set_env(const char *k, const char *v)
{
   _putenv_s(k, v ? v : "");
}
#else
static void
set_env(const char *k, const char *v)
{
   if (v)
      setenv(k, v, 1);
   else
      unsetenv(k);
}
#endif

static int
call_asahi(int fd, unsigned long drm_cmd, void *arg)
{
   return agx_win_kmd_call(fd, agx_win_asahi_nr(drm_cmd), arg,
                           agx_win_asahi_size(drm_cmd));
}

#define PAGE 16384u

int
main(int argc, char **argv)
{
   int fd = -1;

   /* No driver: open reports ENODEV. */
   set_env("AGX_WIN_KMD_STUB", NULL);
   errno = 0;
   assert(agx_win_kmd_open(&fd) == -1 && errno == ENODEV);

   set_env("AGX_WIN_KMD_STUB", "fake");
   set_env("AGX_WIN_KMD_STUB_LOG", argc > 1 ? argv[1] : NULL);
   assert(agx_win_kmd_open(&fd) == 0);
   assert(agx_win_kmd_is_fd(fd) && agx_win_kmd_is_stub(fd));
   assert(!agx_win_kmd_is_fd(3));

   /* The ioctl word decodes to the asahi command number and struct size. */
   assert(agx_win_asahi_nr(DRM_IOCTL_ASAHI_GET_PARAMS) == G18_RN_GET_PARAMS);
   assert(agx_win_asahi_nr(DRM_IOCTL_ASAHI_SUBMIT) == G18_RN_SUBMIT);
   assert(agx_win_asahi_size(DRM_IOCTL_ASAHI_VM_BIND) ==
          sizeof(struct drm_asahi_vm_bind));

   /* VERSION: aurora 1.0.0, live. */
   struct g18_rn_version ver = {0};
   assert(agx_win_kmd_call(fd, G18_RN_VERSION, &ver, sizeof(ver)) == 0);
   assert(ver.abi == G18_RN_ABI && !strcmp(ver.name, "aurora"));
   assert(ver.state == G18_STATE_LIVE);

   /* GET_PARAMS group 0, built as the kernel's K4. */
   struct drm_asahi_params_global params;
   memset(&params, 0xa5, sizeof(params));
   struct drm_asahi_get_params gp = {
      .pointer = (uintptr_t)&params,
      .size = sizeof(params),
   };
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &gp) == 0);
   assert(gp.pointer == (uintptr_t)&params); /* caller's pointer kept */
   assert(params.chip_id == 0x8152 && params.gpu_generation == 18);
   assert(params.gpu_variant == 'G' && params.gpu_revision == 0x11);
   assert(params.num_clusters_total == 2 && params.core_masks[1] == 0x3f);
   assert(params.features == G18_RN_FEATURES_G4);
   assert(params.max_commands_per_submission == 1);
   assert(params.vm_start == 0x4000 && params.vm_end == (1ull << 42) - 0x8000);

   /* A shorter buffer gets the prefix only. */
   uint64_t features = 0;
   gp = (struct drm_asahi_get_params){.pointer = (uintptr_t)&features,
                                      .size = sizeof(features)};
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &gp) == 0);
   assert(features == G18_RN_FEATURES_G4);

   /* Group 3 (status mirror) is EOPNOTSUPP: the CRT value, not Linux's 95. */
   struct drm_asahi_vm_status_mirror mirror = {0};
   gp = (struct drm_asahi_get_params){
      .param_group = DRM_ASAHI_PARAM_GROUP_VM_STATUS_MIRROR,
      .pointer = (uintptr_t)&mirror,
      .size = sizeof(mirror)};
   errno = 0;
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &gp) == -1);
   assert(errno == EOPNOTSUPP);

   struct drm_asahi_vm_create vm = {
      .kernel_start = 0x1000000000ull,
      .kernel_end = 0x1800000000ull,
   };
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_VM_CREATE, &vm) == 0);
   assert(vm.vm_id == 1);

   /* VM_STATUS (group 1) answers for a VM of this file. */
   struct drm_asahi_vm_status st = {.vm_id = vm.vm_id};
   gp = (struct drm_asahi_get_params){
      .param_group = DRM_ASAHI_PARAM_GROUP_VM_STATUS,
      .pointer = (uintptr_t)&st,
      .size = sizeof(st)};
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &gp) == 0 && !st.error);

   struct drm_asahi_gem_create gc = {
      .size = 3 * PAGE - 5,
      .flags = DRM_ASAHI_GEM_VM_PRIVATE,
      .vm_id = vm.vm_id,
   };
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &gc) == 0);
   assert(gc.handle == 1 && gc.size == 3 * PAGE);
   struct drm_asahi_gem_create gc2 = gc;
   gc2.size = PAGE;
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &gc2) == 0);
   assert(gc2.handle == 2);

   struct g18_rn_gem_map map = {.handle = gc.handle};
   assert(agx_win_kmd_call(fd, G18_RN_GEM_MAP, &map, sizeof(map)) == 0);
   assert(map.va && map.size == 3 * PAGE);
   memset((void *)(uintptr_t)map.va, 0x11, PAGE);
   struct g18_rn_gem_map placed = {.handle = gc.handle, .fixed_va = map.va};
   assert(agx_win_kmd_call(fd, G18_RN_GEM_MAP, &placed, sizeof(placed)) ==
             -1 && errno == EINVAL);

   /* VM_BIND: the ops travel inline; a later bind replaces an earlier one. */
   struct drm_asahi_gem_bind_op ops[3] = {
      {.flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_SINGLE_PAGE,
       .handle = gc.handle, .range = 8 * PAGE, .addr = 0x5ffd000000ull},
      {.flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE,
       .handle = gc.handle, .range = 3 * PAGE, .addr = 0x5ffd004000ull},
      {.flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE,
       .handle = gc2.handle, .range = PAGE, .addr = 0x100000000ull},
   };
   struct drm_asahi_vm_bind vb = {
      .vm_id = vm.vm_id,
      .num_binds = 3,
      .stride = sizeof(ops[0]),
      .userptr = (uintptr_t)ops,
   };
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_VM_BIND, &vb) == 0);
   assert(vb.userptr == (uintptr_t)ops);

   /* A bad op fails the whole call: range past the BO. */
   struct drm_asahi_gem_bind_op bad = {
      .flags = DRM_ASAHI_BIND_READ, .handle = gc2.handle,
      .range = 2 * PAGE, .addr = 0x200000000ull};
   vb = (struct drm_asahi_vm_bind){.vm_id = vm.vm_id, .num_binds = 1,
                                   .stride = sizeof(bad),
                                   .userptr = (uintptr_t)&bad};
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_VM_BIND, &vb) == -1 &&
          errno == EINVAL);

   /* QUEUE_CREATE at the 24-byte legacy size Mesa sends. */
   struct drm_asahi_queue_create qc = {
      .vm_id = vm.vm_id,
      .priority = 1,
      .usc_exec_base = 0x10000000000ull,
   };
   unsigned long qreq = DRM_IOCTL_ASAHI_QUEUE_CREATE;
   qreq = (qreq & ~(0x1fffu << 16)) |
          ((unsigned long)offsetof(struct drm_asahi_queue_create,
                                   render_scs_layout) << 16);
   assert(agx_win_asahi_size(qreq) == 24);
   assert(call_asahi(fd, qreq, &qc) == 0 && qc.queue_id == 1);

   /* SUBMIT: SET_COMPUTE_ATTACHMENTS (empty) + one G18 compute command. */
   struct {
      struct drm_asahi_cmd_header h0, h1;
      struct g18_asahi_cmd_compute_g18 c;
   } cb = {0};
   cb.h0 = (struct drm_asahi_cmd_header){
      .cmd_type = DRM_ASAHI_SET_COMPUTE_ATTACHMENTS,
      .vdm_barrier = 0xffff, .cdm_barrier = 0xffff};
   cb.h1 = (struct drm_asahi_cmd_header){
      .cmd_type = DRM_ASAHI_CMD_COMPUTE, .size = sizeof(cb.c),
      .vdm_barrier = 0xffff, .cdm_barrier = 0xffff};
   cb.c.base.legacy.flags = G18_ASAHI_COMPUTE_G18_RESOURCES;
   cb.c.base.legacy.cdm_ctrl_stream_base = 0x5ffd004400ull;
   cb.c.base.legacy.cdm_ctrl_stream_end = 0x5ffd004430ull;
   cb.c.resources.version = 1;
   struct drm_asahi_sync sync = {.sync_type = DRM_ASAHI_SYNC_SYNCOBJ,
                                 .handle = 1};
   struct drm_asahi_submit sub = {
      .queue_id = qc.queue_id,
      .cmdbuf = (uintptr_t)&cb,
      .cmdbuf_size = sizeof(cb),
      .syncs = (uintptr_t)&sync,
      .out_sync_count = 1,
   };
   /* Syncs never reach the kernel (user mode, G4-PLAN 3.1). */
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_SUBMIT, &sub) == -1 &&
          errno == EINVAL);
   sub.syncs = 0;
   sub.out_sync_count = 0;
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_SUBMIT, &sub) == 0);

   /* Admission: samplers are refused. */
   cb.c.base.legacy.sampler_count = 1;
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_SUBMIT, &sub) == -1 &&
          errno == EINVAL);

   /* Not served in G4. */
   struct drm_asahi_gem_bind_object bo_obj = {.handle = gc.handle};
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, &bo_obj) == -1 &&
          errno == EOPNOTSUPP);

   struct drm_asahi_gem_madvise mad = {.handle = gc.handle};
   assert(call_asahi(fd, DRM_IOCTL_ASAHI_GEM_MADVISE, &mad) == 0 &&
          mad.retained == 1);

   struct g18_rn_gem_unmap unmap = {.va = map.va};
   assert(agx_win_kmd_call(fd, G18_RN_GEM_UNMAP, &unmap, sizeof(unmap)) == 0);
   struct g18_rn_gem_close close_bo = {.handle = gc2.handle};
   assert(agx_win_kmd_call(fd, G18_RN_GEM_CLOSE, &close_bo,
                           sizeof(close_bo)) == 0);
   assert(agx_win_kmd_call(fd, G18_RN_GEM_CLOSE, &close_bo,
                           sizeof(close_bo)) == -1 && errno == ENOENT);

   /* A frame the driver cannot use fails the call itself. */
   assert(agx_win_kmd_call(fd, 0x30, &close_bo, sizeof(close_bo)) == -1 &&
          errno == ENOTTY);
   uint8_t big[G18_RN_ARG_MAX + 8] = {0};
   assert(agx_win_kmd_call(fd, G18_RN_GET_TIME, big, sizeof(big)) == -1 &&
          errno == EINVAL);

   /* A second open is a second file with its own handles. */
   int fd2 = -1;
   assert(agx_win_kmd_open(&fd2) == 0 && fd2 != fd);
   struct drm_asahi_vm_create vm2 = vm;
   assert(call_asahi(fd2, DRM_IOCTL_ASAHI_VM_CREATE, &vm2) == 0 &&
          vm2.vm_id == 1);
   agx_win_kmd_close(fd2);

   agx_win_kmd_close(fd);
   assert(!agx_win_kmd_is_fd(fd));
   assert(agx_win_kmd_call(fd, G18_RN_VERSION, &ver, sizeof(ver)) == -1 &&
          errno == EBADF);

   if (argc > 1) {
      FILE *f = fopen(argv[1], "rb");
      assert(f);
      char magic[8];
      assert(fread(magic, 1, 8, f) == 8 && !memcmp(magic, "G18RPLY1", 8));
      fclose(f);
   }

   printf("asahi-win-kmd-stub: OK\n");
   return 0;
}
