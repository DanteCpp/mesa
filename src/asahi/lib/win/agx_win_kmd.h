/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Transport between the asahi G18 backend (agx_device_g18.c) and the
 * Windows render node of the g18win kernel driver.
 *
 * The ABI is not defined here. win/g18_uapi.h is a verbatim copy of the
 * kernel driver's uapi/g18_uapi.h (header revision 2, refreshed only by
 * win/sync-g18-uapi.sh) and is the single source of every command number,
 * IOCTL code, frame rule and struct layout. This header adds what the user
 * side needs on top: static checks that the g18 mirrors equal Mesa's
 * drm_asahi_* structs, and the transport entry points.
 *
 * Model (g18_uapi.h, "Render node"): one device, \\.\G18Win0, opened any
 * number of times; one open file is one DRM file. Every call is one
 * METHOD_BUFFERED DeviceIoControl on a frame: g18_rn_hdr, then the argument
 * struct, then inline data. The u64 "pointer" fields of an argument travel as
 * frame offsets; agx_win_kmd_call() does that marshalling, so callers pass
 * the asahi structs with ordinary CPU pointers, exactly as for drmIoctl.
 *
 * Errors follow drmIoctl: 0, or -1 with errno set to the CRT value of the
 * kernel's (Linux-numbered) error.
 *
 * File descriptors: Mesa keeps the device as an int fd. The transport hands
 * out pseudo-fds >= AGX_WIN_KMD_FD_BASE and keeps the HANDLE (or the stub
 * node) behind them. close() must never see one; agx_win_kmd_close() is the
 * only way to release it.
 *
 * Stub: with AGX_WIN_KMD_STUB=fake in the environment, agx_win_kmd_open()
 * opens an in-process fake render node instead (win/agx_win_kmd_stub.c): no
 * GPU, the same frame rules and G4 semantics as the kernel driver, GET_PARAMS
 * as the kernel builds it (K4), SUBMIT accepted without execution, and every
 * call recorded as a G18R v1 file at $AGX_WIN_KMD_STUB_LOG (if set) for the
 * comparison with the Linux capture L1 (G4-PLAN C3).
 */

#ifndef AGX_WIN_KMD_H
#define AGX_WIN_KMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm-uapi/asahi_drm.h"
#include "g18_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The g18 mirrors are Mesa's structs ---------------------------------
 * The kernel validates against g18_uapi.h; Mesa fills drm_asahi_*. They must
 * not drift. (g18win's tools/check-rn-uapi.sh checks the same from the other
 * side, field by field.) */
#define AGX_WIN_SAME_SIZE(n)                                                   \
   _Static_assert(sizeof(struct g18_asahi_##n) == sizeof(struct drm_asahi_##n), \
                  "g18_asahi_" #n " != drm_asahi_" #n)
#define AGX_WIN_SAME_FIELD(n, f)                                               \
   _Static_assert(offsetof(struct g18_asahi_##n, f) ==                         \
                     offsetof(struct drm_asahi_##n, f),                        \
                  "g18_asahi_" #n "." #f)
AGX_WIN_SAME_SIZE(params_global);
AGX_WIN_SAME_SIZE(get_params);
AGX_WIN_SAME_SIZE(vm_status);
AGX_WIN_SAME_SIZE(vm_create);
AGX_WIN_SAME_SIZE(vm_destroy);
AGX_WIN_SAME_SIZE(gem_create);
AGX_WIN_SAME_SIZE(gem_mmap_offset);
AGX_WIN_SAME_SIZE(gem_madvise);
AGX_WIN_SAME_SIZE(gem_bind_op);
AGX_WIN_SAME_SIZE(vm_bind);
AGX_WIN_SAME_SIZE(gem_bind_object);
/* queue_create grows at the end (24, 40, 48 bytes, the last with
 * compute_timeout_ms); the kernel's min-size copy takes any of them
 * (g18_uapi.h "Sizes"). */
_Static_assert(sizeof(struct drm_asahi_queue_create) <=
                  sizeof(struct g18_asahi_queue_create),
               "queue_create grows only at the end");
AGX_WIN_SAME_SIZE(queue_destroy);
AGX_WIN_SAME_SIZE(sync);
AGX_WIN_SAME_SIZE(submit);
AGX_WIN_SAME_SIZE(cmd_header);
AGX_WIN_SAME_SIZE(cmd_compute);
AGX_WIN_SAME_SIZE(get_time);
AGX_WIN_SAME_FIELD(params_global, memory_limit);
AGX_WIN_SAME_FIELD(get_params, pointer);
AGX_WIN_SAME_FIELD(get_params, size);
AGX_WIN_SAME_FIELD(vm_bind, num_binds);
AGX_WIN_SAME_FIELD(vm_bind, stride);
AGX_WIN_SAME_FIELD(vm_bind, userptr);
AGX_WIN_SAME_FIELD(submit, syncs);
AGX_WIN_SAME_FIELD(submit, cmdbuf);
AGX_WIN_SAME_FIELD(submit, in_sync_count);
AGX_WIN_SAME_FIELD(submit, out_sync_count);
AGX_WIN_SAME_FIELD(submit, cmdbuf_size);
AGX_WIN_SAME_FIELD(queue_create, render_scs_layout);
AGX_WIN_SAME_FIELD(queue_create, usc_exec_base);
AGX_WIN_SAME_FIELD(queue_create, compute_timeout_ms);
AGX_WIN_SAME_SIZE(gem_userptr);
_Static_assert(DRM_ASAHI_GET_PARAMS == G18_RN_GET_PARAMS &&
                  DRM_ASAHI_GET_TIME == G18_RN_GET_TIME &&
                  DRM_ASAHI_VM_CREATE == G18_RN_VM_CREATE &&
                  DRM_ASAHI_VM_DESTROY == G18_RN_VM_DESTROY &&
                  DRM_ASAHI_VM_BIND == G18_RN_VM_BIND &&
                  DRM_ASAHI_GEM_CREATE == G18_RN_GEM_CREATE &&
                  DRM_ASAHI_GEM_MMAP_OFFSET == G18_RN_GEM_MMAP_OFFSET &&
                  DRM_ASAHI_GEM_BIND_OBJECT == G18_RN_GEM_BIND_OBJECT &&
                  DRM_ASAHI_QUEUE_CREATE == G18_RN_QUEUE_CREATE &&
                  DRM_ASAHI_QUEUE_DESTROY == G18_RN_QUEUE_DESTROY &&
                  DRM_ASAHI_SUBMIT == G18_RN_SUBMIT &&
                  DRM_ASAHI_GEM_MADVISE == G18_RN_GEM_MADVISE,
               "asahi command numbers are the render-node command numbers");
_Static_assert(DRM_ASAHI_FEATURE_VM_STATUS == G18_ASAHI_FEATURE_VM_STATUS &&
                  DRM_ASAHI_FEATURE_SOFT_FAULTS ==
                     G18_ASAHI_FEATURE_SOFT_FAULTS &&
                  DRM_ASAHI_FEATURE_VM_STATUS_MIRROR ==
                     G18_ASAHI_FEATURE_VM_STATUS_MIRROR,
               "feature bits");
#undef AGX_WIN_SAME_SIZE
#undef AGX_WIN_SAME_FIELD

/* Pseudo-fd range handed out by agx_win_kmd_open(). */
#define AGX_WIN_KMD_FD_BASE  0x4b000
#define AGX_WIN_KMD_MAX_OPEN 16

/* Open the render node (or the stub, see above). 0 and *out_fd, or -1 with
 * errno: ENODEV when there is no render node (no driver, or not this
 * machine), EACCES, EMFILE, ENOMEM. */
int agx_win_kmd_open(int *out_fd);

/* Release a pseudo-fd from agx_win_kmd_open(). */
void agx_win_kmd_close(int fd);

/* True if fd is a live pseudo-fd from agx_win_kmd_open(). */
bool agx_win_kmd_is_fd(int fd);

/* True if fd is a stub node (AGX_WIN_KMD_STUB=fake). */
bool agx_win_kmd_is_stub(int fd);

/* One render-node command (G18_RN_*). arg/arg_size is the argument struct as
 * the caller declares it (asahi structs may be shorter than the kernel's, see
 * g18_uapi.h "Sizes"); it is updated in place with what the kernel writes
 * back. The pointer fields named in g18_uapi.h (GET_PARAMS pointer/size,
 * VM_BIND userptr, SUBMIT cmdbuf) hold CPU pointers here and are marshalled
 * into the frame; SUBMIT must carry no syncs (EINVAL). 0, or -1 with errno. */
/* g18win uapi/g18_uapi.h from g6-perf on (until the next sync of the
 * header): a render SUBMIT returns after its doorbell; the render node
 * retires it at its next call. */
#ifndef G18_SUBMIT_ASYNC
#define G18_SUBMIT_ASYNC (1u << 31)
#endif

int agx_win_kmd_call(int fd, uint32_t cmd, void *arg, uint32_t arg_size);

/* CRT errno value for a Linux errno from g18_rn_hdr.result. */
int agx_win_kmd_errno(int32_t linux_errno);

/* Write one line to the driver's serial log (G18_IOCTL_LOG on the control
 * side of the same device). Best effort: false if there is no driver. */
bool agx_win_kmd_log(const char *line);

/* Asahi command number and argument size from a DRM_IOCTL_ASAHI_* word.
 * Bits 0-7 hold DRM_COMMAND_BASE + nr and bits 16-28 the size in both the
 * Linux and the BSD encodings (include/win32_stub/sys/ioccom.h). */
static inline uint32_t
agx_win_asahi_nr(unsigned long drm_cmd)
{
   return (uint32_t)(drm_cmd & 0xff) - 0x40 /* DRM_COMMAND_BASE */;
}

static inline uint32_t
agx_win_asahi_size(unsigned long drm_cmd)
{
   return (uint32_t)((drm_cmd >> 16) & 0x1fff);
}

/* ---- Stub node (internal to the transport) -------------------------------
 * The fake render node takes the same IOCTL code and frame the kernel driver
 * takes. agx_win_stub_ioctl() returns 0 when the frame was handled (the
 * result is in hdr.result, like a successful DeviceIoControl) or -EINVAL /
 * -ENOTTY for a frame the driver refuses outright (STATUS_INVALID_PARAMETER,
 * STATUS_INVALID_DEVICE_REQUEST). */
struct agx_win_stub;
struct agx_win_stub *agx_win_stub_open(void);
void agx_win_stub_close(struct agx_win_stub *stub);
int agx_win_stub_ioctl(struct agx_win_stub *stub, uint32_t code, void *frame,
                       uint32_t size);
/* Append a line to the stub's G18R record (MARK chunk), e.g. a C3 marker. */
void agx_win_stub_mark(uint32_t code, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* AGX_WIN_KMD_H */
