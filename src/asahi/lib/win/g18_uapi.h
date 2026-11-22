/*
 * g18win uAPI.
 *
 * Header revision 3 (G5b: G18_STATE_SPENT, G18_IOCTL_TELEMETRY; revision 2
 * was G4). Two parts on one device:
 * - the control ioctls (functions 0x900-0x9FF), version 1 from G3 plus
 *   GO_LIVE/STOP: the ioctls between g18replay.exe / g4pe.cmd tools and
 *   g18win.sys;
 * - the render node (functions 0xA00-0xA43, G4-PLAN 3.4): the asahi DRM
 *   uAPI reframed for DeviceIoControl, used by Mesa's Windows backend
 *   (src/asahi/lib/win/agx_win_kmd.h takes its numbers and layouts from
 *   here and nowhere else).
 *
 * Control part: shared by the KMDF shell (kmdf/), the WinPE tool (tool/) and, as a Rust
 * mirror checked field by field, by g18kmd (g18kmd/src/uapi.rs; the host
 * test compiles uapi/layout_dump.c and compares every offset). Only
 * fixed-width types, natural alignment, explicit padding: the layout is the
 * same for LLP64 (Windows) and LP64 (the host check).
 *
 * Every struct starts with { size, version }; the driver refuses any other
 * size or version. Device \\.\G18Win0, SY/BA only (INF); exclusive in G3,
 * any number of opens from G4 (G4-PLAN 3.9).
 *
 * The render-node part is checked by tools/check-rn-uapi.sh (static asserts
 * on every compiler, uapi/asahi_check.c against Mesa's header).
 */
#ifndef G18_UAPI_H
#define G18_UAPI_H

#include <stddef.h>
#include <stdint.h>

#define G18_UAPI_VERSION 1u   /* control structs' { size, version }; unchanged in rev 2 */
#define G18_UAPI_HEADER_REV 3u /* this header; g18_info.uapi_version reports it from G4 on */
#define G18_DEVICE_TYPE  0x8A18u
#define G18_DOS_NAME     L"\\\\.\\G18Win0"

/* CTL_CODE without winioctl.h / wdm.h. */
#define G18_CTL(fn, method) (((G18_DEVICE_TYPE) << 16) | (3u << 14) | ((fn) << 2) | (method))
#define G18_M_BUFFERED   0u
#define G18_M_IN_DIRECT  1u
#define G18_M_OUT_DIRECT 2u

#define G18_IOCTL_GET_INFO     G18_CTL(0x900u, G18_M_BUFFERED)   /* out g18_info */
#define G18_IOCTL_LOAD_CAPTURE G18_CTL(0x901u, G18_M_IN_DIRECT)  /* in g18_load_in; data = capture */
#define G18_IOCTL_G1_HELLO     G18_CTL(0x902u, G18_M_BUFFERED)   /* out g18_g1_out; spends the session */
#define G18_IOCTL_G2_INIT      G18_CTL(0x903u, G18_M_BUFFERED)   /* out g18_g2_out */
#define G18_IOCTL_REPLAY       G18_CTL(0x904u, G18_M_BUFFERED)   /* out g18_g2_out */
#define G18_IOCTL_READ_OUTPUT  G18_CTL(0x905u, G18_M_OUT_DIRECT) /* out: the output bytes */
#define G18_IOCTL_MAP_OUTPUT   G18_CTL(0x906u, G18_M_BUFFERED)   /* out g18_map_out */
#define G18_IOCTL_UNMAP_OUTPUT G18_CTL(0x907u, G18_M_BUFFERED)   /* in g18_unmap_in */
#define G18_IOCTL_STATUS       G18_CTL(0x908u, G18_M_BUFFERED)   /* out g18_status */
#define G18_IOCTL_READ_LOG     G18_CTL(0x909u, G18_M_OUT_DIRECT) /* out: '\n'-separated lines */
#define G18_IOCTL_LOG          G18_CTL(0x90Au, G18_M_BUFFERED)   /* in g18_log_in */
#define G18_IOCTL_GO_LIVE      G18_CTL(0x90Bu, G18_M_BUFFERED)   /* in/out g18_live_out; rev 2 */
#define G18_IOCTL_STOP         G18_CTL(0x90Cu, G18_M_BUFFERED)   /* in/out g18_live_out; rev 2 */
#define G18_IOCTL_TELEMETRY    G18_CTL(0x90Du, G18_M_BUFFERED)   /* out g18_tel; rev 3 (G5b K-2) */

/* Driver states (g18_info.state, g18_status.state). */
#define G18_STATE_NONE       0u /* not attached: page refused or not yet read */
#define G18_STATE_ATTACHED   1u /* page valid, windows mapped, no capture */
#define G18_STATE_CAPTURE_OK 2u /* P0 capture checks passed; G1 allowed */
#define G18_STATE_G1_PASS    3u
#define G18_STATE_G2_READY   4u /* held after P11, REPLAY allowed */
#define G18_STATE_G2_DONE    5u /* stopped where the cfg said (before a job), pass */
#define G18_STATE_REPLAYED   6u /* P14 reached and passed */
#define G18_STATE_FAILED     7u /* a GPU step failed or was blocked; nothing more */
#define G18_STATE_NOWINDOWS  8u /* page valid, windows deliberately not mapped (QEMU) */
#define G18_STATE_LIVE       9u /* rev 2: GO_LIVE passed; the render node is served */
#define G18_STATE_STOPPED    10u /* rev 2: STOP after LIVE; GPU idle, nothing more */
#define G18_STATE_LOST       11u /* rev 2: device lost after LIVE (G4-PLAN 3.8); sticky */
#define G18_STATE_SPENT      12u /* rev 3: this boot's firmware session was used before this
				  * device started (a PnP restart, a driver reload, a Windows
				  * restart in one carrier session): nothing mapped, no GPU
				  * step, ENODEV until the M6 reboots (G5B-BUILD 5.3) */

/* g18_info.flags */
#define G18_INFO_SPENT    0x1u /* this boot's firmware session is used */
#define G18_INFO_UART     0x2u /* the DBG2 vUART log channel is mapped */
#define G18_INFO_POOL_OK  0x4u /* the pool is outside Windows RAM, end to end */
#define G18_INFO_SIM      0x8u /* QEMU build: simulated GPU, never on the M6 */
#define G18_INFO_BUSY     0x10u
#define G18_INFO_LIVE     0x20u /* rev 2: state LIVE */
#define G18_INFO_LOST     0x40u /* rev 2: state LOST (also set with STOPPED after a loss) */

/* Verdicts (g1/g2/status). */
#define G18_VERDICT_PASS    0u
#define G18_VERDICT_FAIL    1u
#define G18_VERDICT_BLOCKED 2u

#define G18_TEXT 128u
#define G18_MAX_SITES 64u
#define G18_CFG_MAX 512u
#define G18_LOG_LINE 240u

struct g18_info {
	uint32_t size, version;
	uint32_t uapi_version, abi_version;
	uint32_t state, flags;
	uint64_t handoff_ipa;
	uint64_t pool_base, pool_size;
	uint64_t sgx_base, sgx_size;
	uint64_t ram_base, ram_top;
	uint64_t uart_pa;
	uint32_t carrier_state, nmaps;
	uint32_t log_dropped, refused;
	char receipt[64];
	char build[64];
	char reason[G18_TEXT];
};

struct g18_load_in {
	uint32_t size, version;
	uint32_t capture_len, cfg_len;
	uint8_t capture_sha256[32];
	char cfg[G18_CFG_MAX];
};

struct g18_g1_out {
	uint32_t size, version;
	uint32_t verdict, took_over;
	uint32_t gpu_id[7];
	uint32_t pad0;
	uint64_t lower_root;
	uint32_t fw_version[2];      /* [gfx, gfx1] */
	uint32_t endpoints[2][2];    /* [gfx, gfx1][EPMAP base 0, 1] */
	uint64_t crashlog[2];
	uint64_t elapsed_us[2];
	char reason[G18_TEXT];
};

struct g18_job {
	uint32_t generation, context, stamp, done;
	uint64_t us;
};

struct g18_g2_out {
	uint32_t size, version;
	uint32_t verdict, stop;      /* stop: g18core StopPoint, 0 = g1 .. 6 = job2 */
	char reached[8];
	uint32_t published, held;
	uint32_t hold_polls, phases;
	uint64_t ack_us[2];          /* [gfx, gfx1] */
	uint64_t openings_us, wake_us, heartbeat_ms;
	uint32_t control[2][3];
	uint32_t reports[2][2];
	struct g18_job jobs[2];
	uint32_t compared[2], differing[2];
	uint64_t compare_bytes[2];
	uint32_t equal[2];
	uint32_t reference_ok, crashlogs;
	uint32_t recovery_state, pad0;
	uint8_t out_sha256[32];
	char reason[G18_TEXT];
};

struct g18_map_out {
	uint32_t size, version;
	uint64_t user_va;
	uint64_t length;
	uint32_t ranges, pad0;
};

struct g18_unmap_in {
	uint32_t size, version;
	uint64_t user_va;
};

/* window: 0 sgx, 1 gfx-asc, 2 gfx1-asc, 3 gfx-mbox, 4 gfx1-mbox, 16 + FwRegion. */
struct g18_site {
	uint8_t window, write, width, pad0;
	uint32_t count;
	uint64_t off;
};

struct g18_status {
	uint32_t size, version;
	uint32_t state, verdict;
	char reached[8];
	uint32_t refused, nsites;
	uint32_t total, dropped;
	struct g18_site sites[G18_MAX_SITES];
	uint32_t capture_ok, capture_len;
	uint8_t capture_sha256[32];
	uint32_t width, height;
	uint32_t format, cfg_stop;   /* format 0 rgba8, 1 bgra8 */
	uint64_t heartbeat_ms, job_timeout_ms;
	uint64_t client_root, output_va;
	uint32_t output_bytes, log_dropped;
	uint8_t expect_sha256[32];
	char reason[G18_TEXT];
};

struct g18_log_in {
	uint32_t size, version;
	uint32_t len, pad0;
	char line[G18_LOG_LINE];
};

/*
 * GO_LIVE and STOP (rev 2, G4-PLAN 3.5). In: size, version. Out: the rest.
 *
 * GO_LIVE is accepted only in state REPLAYED with a passing replay (the
 * output of generation 1 equal to L1). It switches the worker from the
 * capture replay to serving the render node: no generation limit, no fixed
 * poll hold; the mailbox is drained until STOP or unload. Any other state:
 * STATUS_INVALID_DEVICE_STATE and verdict BLOCKED.
 *
 * STOP is accepted in LIVE and LOST. It refuses new render-node work
 * (ENODEV), waits for the worker to go idle (the job timeout bounds it),
 * then parks the worker. Open files stay open; their later calls get
 * ENODEV. STOP does not power anything off and frees nothing.
 */
struct g18_live_out {
	uint32_t size, version;
	uint32_t verdict, state;     /* out: G18_VERDICT_*, G18_STATE_* after the call */
	uint32_t generation, files;  /* out: firmware job generations so far; open render-node files */
	uint64_t elapsed_us;         /* out: time the call took */
	char reason[G18_TEXT];
};

/*
 * TELEMETRY (rev 3, G5b K-2; G5B-BUILD 5.1). Out only; any state.
 *
 * Every render-node call that ran a live job is stamped on the CNTVCT clock
 * (hz) at ten points: t0 ioctl entry, t1 the command event set, t2 the
 * worker awake, t3 the job built (next: the doorbell), t4 the doorbell sent,
 * t5 the completion seen, t6 the worker's post, t7 the caller awake, t8 after
 * the DPC flush (t8 = t7 when it is skipped), t9 ioctl return. hist[i] for
 * i < 9 is the log2-microsecond histogram of t(i+1) - t(i), hist[9] of
 * t9 - t0 and hist[10] of the worker's idle waits (the 1 ms command waits
 * that timed out). Bucket 0 is [0, 1) us, bucket k is [2^(k-1), 2^k) us,
 * bucket 15 is 16384 us and up. Counters only grow (one firmware session per
 * boot); nothing is reset.
 */
#define G18_TEL_HISTS   11u
#define G18_TEL_BUCKETS 16u
#define G18_TEL_OPT_JOBLOG       0x1u /* registry JobLog=1: the S8 per-job lines as well */
#define G18_TEL_OPT_HIRES        0x2u /* registry HiResTimer (default 1): high-resolution 1 ms waits */
#define G18_TEL_OPT_FLUSH_IF_DPC 0x4u /* registry FlushOnlyIfDpc (default 1) */
#define G18_TEL_OPT_STOP_ALLOWED 0x8u /* registry StopAllowed=1 (WinPE images only) */
#define G18_TEL_OPT_HIRES_ON     0x10u /* the high-resolution timer was allocated */
struct g18_tel {
	uint32_t size, version;
	uint32_t state, flags;              /* as g18_info */
	uint32_t jobs, lines;               /* jobs stamped; per-job lines queued (first 32 + over 5 ms, at most 96) */
	uint32_t lines_skipped, opts;       /* opts: G18_TEL_OPT_* */
	uint64_t hz;                        /* the stamps' counter frequency (CNTFRQ) */
	uint64_t drains, longest_gap_us;    /* the worker's mailbox drains and the longest gap between two */
	uint32_t timer_res_100ns, active;   /* ExQueryTimerResolution (current) at the call; 1 ms held now */
	uint32_t hist[G18_TEL_HISTS][G18_TEL_BUCKETS];
	uint32_t max_us[G18_TEL_HISTS];
	uint32_t pad0;
	uint64_t sum_us[G18_TEL_HISTS];
};

/* ======================================================================
 * Render node (header rev 2, G4-PLAN 3.4)
 * ======================================================================
 *
 * The asahi DRM render-node uAPI (Mesa include/drm-uapi/asahi_drm.h at
 * 7c34f6f2, with the G18 extensions hk uses) carried over DeviceIoControl on
 * the same device, \\.\G18Win0, which may be opened any number of times
 * (G4-PLAN 3.9). One open file is one DRM file: VMs, BOs, queues and CPU
 * maps belong to it and are released (not freed, G4-PLAN 3.2) when it is
 * cleaned up.
 *
 * Codes. Command n is function G18_RN_FN_BASE + n, METHOD_BUFFERED, read and
 * write access:
 *   0x00-0x0F  the asahi commands, numbered exactly as enum
 *              drm_asahi_ioctl_id (GET_PARAMS = 0 ... GEM_RESIDENCY = 13);
 *   0x40-0x43  what DRM core provides on Linux: VERSION, GEM_CLOSE, GEM_MAP
 *              (mmap), GEM_UNMAP (munmap).
 * Any other function in 0xA00-0xAFF fails the call with
 * STATUS_INVALID_DEVICE_REQUEST (user side: ENOTTY). Sync objects are not
 * here: they live in user mode (G4-PLAN 3.1).
 *
 * Frame. Input and output are the same buffer, both at least frame_size
 * bytes (the user side passes one buffer as both):
 *   +0                     struct g18_rn_hdr
 *   +G18_RN_ARG_OFFSET     the argument struct, hdr.arg_size bytes
 *   +align8(32+arg_size).. inline data: the arrays and buffers that the
 *                          argument's u64 "pointer" fields name on Linux
 * The kernel never dereferences a user virtual address. On Windows, each u64
 * pointer field of an argument holds the byte offset of its data from the
 * start of the frame (0 = none). The data must be 8-byte aligned, lie
 * entirely in [align8(32 + arg_size), frame_size) and not overlap another
 * region of the same frame; otherwise the result is EFAULT, as Linux gives
 * for a bad pointer. The fields and their lengths:
 *   GET_PARAMS  pointer, size              in/out (out for groups 0 and 2)
 *   VM_BIND     userptr, num_binds*stride  in
 *   SUBMIT      cmdbuf, cmdbuf_size        in
 *   SUBMIT      syncs: must be 0, and in_sync_count = out_sync_count = 0
 *               (EINVAL otherwise; syncs are user mode, G4-PLAN 3.1)
 *   GEM_USERPTR userptr: a user VA, not frame data (G6 zero copy below)
 * The kernel writes the whole frame back (IoStatus.Information =
 * frame_size). The user side copies out-data back to the caller's own
 * pointers and puts the original pointer values back in the argument.
 *
 * Sizes. hdr.arg_size is the caller's sizeof(argument), which may be smaller
 * or larger than the kernel's (asahi structs grow at the end; Mesa sends
 * queue_create at 24, 40 or 48 bytes). The kernel copies min(arg_size, its
 * size) and zero-fills the rest; bytes past its size are ignored. It writes
 * back min(arg_size, its size) bytes (hdr.out_size) whenever hdr.result is
 * set, as DRM core does. Records inside inline data follow the asahi rules:
 * VM_BIND copies min(stride, 32) bytes per op and zero-fills; a SUBMIT
 * command payload shorter than the kernel's is zero-filled, a longer one is
 * refused unless the extra bytes are zero.
 *
 * Errors. DeviceIoControl fails only when the frame itself is unusable:
 *   buffer shorter than 32 bytes or than frame_size, abi or cmd wrong,
 *   flags or reserved nonzero, frame_size > G18_RN_FRAME_MAX or
 *   arg_size > G18_RN_ARG_MAX              STATUS_INVALID_PARAMETER (EINVAL)
 *   unknown function                       STATUS_INVALID_DEVICE_REQUEST (ENOTTY)
 * Otherwise it succeeds and hdr.result holds 0 or a positive Linux errno
 * (G18_RN_E*: the wire values are Linux's, not the Windows CRT's; e.g.
 * EOPNOTSUPP is 95 here and 130 in the UCRT). The user side returns -1 and
 * sets errno to the CRT value of that name (never -errno, G4-PLAN 3.4).
 *
 * Gate (G4-PLAN 3.5, 3.8). Until the device is LIVE (GO_LIVE), and again
 * after STOP or a device loss, every render-node command returns ENODEV,
 * with two exceptions that never touch the GPU:
 *   VERSION answers in every state;
 *   in state LOST, GET_PARAMS group 1 (VM_STATUS) answers with the sticky
 *   error (a negative Linux errno) so the user side can report
 *   VK_ERROR_DEVICE_LOST.
 *
 * What G4 serves (K2-K5):
 *   GET_PARAMS     group 0 (struct g18_asahi_params_global) built field by
 *                  field (G4-PLAN 3.3); group 1 VM_STATUS; group 2 EINVAL
 *                  (INDEPENDENT_QUEUES not advertised); group 3
 *                  VM_STATUS_MIRROR EOPNOTSUPP (the bit is not advertised);
 *                  G6 life: group 0x100 struct g18_rn_resources (also
 *                  in state LOST).
 *   GET_TIME       CNTVCT scaled to ns.
 *   VM_CREATE/DESTROY, VM_BIND (READ, WRITE, SINGLE_PAGE, UNBIND; overwrite
 *                  allowed). G6 life: VM_DESTROY, GEM_CLOSE and the
 *                  file's close free tables and BO pages once nothing
 *                  (leaf, CPU map, job) reaches them.
 *   GEM_CREATE     WRITEBACK and VM_PRIVATE accepted; bump allocation.
 *   GEM_MADVISE    no-op, retained = 1.
 *   QUEUE_CREATE/DESTROY, SUBMIT (synchronous, one command, no syncs).
 *   GEM_MMAP_OFFSET, GEM_BIND_OBJECT, GEM_RESIDENCY: EOPNOTSUPP.
 *   GEM_USERPTR (G6 zero copy): a BO over [userptr, userptr + size) of the
 *                  calling process: a WDDM allocation of g18wddm's segment 1
 *                  that the caller holds locked. The shell resolves the range
 *                  in the caller's process (mapped with the access asked for:
 *                  writable unless READ_ONLY; physically contiguous); the
 *                  pages must lie in the pool's WDDM window (the pool top,
 *                  G18W_WINDOW_OFF in wddm/g18w_uapi.h), else EFAULT. userptr
 *                  4 KiB aligned, size a multiple of 16 KiB, the physical
 *                  start 16 KiB aligned. flags: READ_ONLY (binds READ only),
 *                  VM_PRIVATE with vm_id. The pages are never zeroed or given
 *                  to the BO arena; the caller keeps the allocation (and its
 *                  lock) until the BO is closed and unbound.
 *   VERSION, GEM_CLOSE (handle released, pages kept), GEM_MAP (CPU RW,
 *   write-combined, into the calling process), GEM_UNMAP.
 * GEM_MAP and GEM_UNMAP act on the calling process, so the shell runs them in
 * the caller's context (EvtIoInCallerContext).
 */

#define G18_RN_ABI         2u       /* g18_rn_hdr.abi and g18_rn_version.abi */
#define G18_RN_FN_BASE     0xA00u
#define G18_RN_CTL(cmd)    G18_CTL(G18_RN_FN_BASE + (cmd), G18_M_BUFFERED)

/* Command numbers (g18_rn_hdr.cmd). */
#define G18_RN_GET_PARAMS      0x00u /* struct g18_asahi_get_params */
#define G18_RN_GET_TIME        0x01u /* struct g18_asahi_get_time */
#define G18_RN_VM_CREATE       0x02u /* struct g18_asahi_vm_create */
#define G18_RN_VM_DESTROY      0x03u /* struct g18_asahi_vm_destroy */
#define G18_RN_VM_BIND         0x04u /* struct g18_asahi_vm_bind + inline g18_asahi_gem_bind_op[] */
#define G18_RN_GEM_CREATE      0x05u /* struct g18_asahi_gem_create */
#define G18_RN_GEM_MMAP_OFFSET 0x06u /* struct g18_asahi_gem_mmap_offset; EOPNOTSUPP (use GEM_MAP) */
#define G18_RN_GEM_BIND_OBJECT 0x07u /* struct g18_asahi_gem_bind_object; EOPNOTSUPP in G4 */
#define G18_RN_QUEUE_CREATE    0x08u /* struct g18_asahi_queue_create */
#define G18_RN_QUEUE_DESTROY   0x09u /* struct g18_asahi_queue_destroy */
#define G18_RN_SUBMIT          0x0Au /* struct g18_asahi_submit + inline command buffer */
#define G18_RN_GEM_MADVISE     0x0Bu /* struct g18_asahi_gem_madvise */
#define G18_RN_GEM_USERPTR     0x0Cu /* struct g18_asahi_gem_userptr; G6 zero copy (WDDM window pages) */
#define G18_RN_GEM_RESIDENCY   0x0Du /* struct g18_asahi_gem_residency; EOPNOTSUPP in G4 */
#define G18_RN_ASAHI_LAST      0x0Fu /* 0x0E-0x0F reserved for later asahi commands */
#define G18_RN_VERSION         0x40u /* struct g18_rn_version (DRM_IOCTL_VERSION) */
#define G18_RN_GEM_CLOSE       0x41u /* struct g18_rn_gem_close (DRM_IOCTL_GEM_CLOSE) */
#define G18_RN_GEM_MAP         0x42u /* struct g18_rn_gem_map (mmap of the BO) */
#define G18_RN_GEM_UNMAP       0x43u /* struct g18_rn_gem_unmap (munmap) */

#define G18_IOCTL_RN_GET_PARAMS      G18_RN_CTL(G18_RN_GET_PARAMS)      /* 0xA00 */
#define G18_IOCTL_RN_GET_TIME        G18_RN_CTL(G18_RN_GET_TIME)        /* 0xA01 */
#define G18_IOCTL_RN_VM_CREATE       G18_RN_CTL(G18_RN_VM_CREATE)       /* 0xA02 */
#define G18_IOCTL_RN_VM_DESTROY      G18_RN_CTL(G18_RN_VM_DESTROY)      /* 0xA03 */
#define G18_IOCTL_RN_VM_BIND         G18_RN_CTL(G18_RN_VM_BIND)         /* 0xA04 */
#define G18_IOCTL_RN_GEM_CREATE      G18_RN_CTL(G18_RN_GEM_CREATE)      /* 0xA05 */
#define G18_IOCTL_RN_GEM_MMAP_OFFSET G18_RN_CTL(G18_RN_GEM_MMAP_OFFSET) /* 0xA06 */
#define G18_IOCTL_RN_GEM_BIND_OBJECT G18_RN_CTL(G18_RN_GEM_BIND_OBJECT) /* 0xA07 */
#define G18_IOCTL_RN_QUEUE_CREATE    G18_RN_CTL(G18_RN_QUEUE_CREATE)    /* 0xA08 */
#define G18_IOCTL_RN_QUEUE_DESTROY   G18_RN_CTL(G18_RN_QUEUE_DESTROY)   /* 0xA09 */
#define G18_IOCTL_RN_SUBMIT          G18_RN_CTL(G18_RN_SUBMIT)          /* 0xA0A */
#define G18_IOCTL_RN_GEM_MADVISE     G18_RN_CTL(G18_RN_GEM_MADVISE)     /* 0xA0B */
#define G18_IOCTL_RN_GEM_USERPTR     G18_RN_CTL(G18_RN_GEM_USERPTR)     /* 0xA0C */
#define G18_IOCTL_RN_GEM_RESIDENCY   G18_RN_CTL(G18_RN_GEM_RESIDENCY)   /* 0xA0D */
#define G18_IOCTL_RN_VERSION         G18_RN_CTL(G18_RN_VERSION)         /* 0xA40 */
#define G18_IOCTL_RN_GEM_CLOSE       G18_RN_CTL(G18_RN_GEM_CLOSE)       /* 0xA41 */
#define G18_IOCTL_RN_GEM_MAP         G18_RN_CTL(G18_RN_GEM_MAP)         /* 0xA42 */
#define G18_IOCTL_RN_GEM_UNMAP       G18_RN_CTL(G18_RN_GEM_UNMAP)       /* 0xA43 */

/* True for a code of the render node (the shell routes it before the control
 * switch). */
#define G18_IOCTL_IS_RN(code) \
	(((code) & 0xFFFFC003u) == ((G18_DEVICE_TYPE << 16) | (3u << 14) | G18_M_BUFFERED) && \
	 ((((code) >> 2) & 0xFFFu) & 0xF00u) == G18_RN_FN_BASE)

/* Frame limits. */
#define G18_RN_ARG_OFFSET  32u
#define G18_RN_ARG_MAX     1024u     /* > every asahi struct; room to grow */
#define G18_RN_FRAME_MAX   0x40000u  /* 256 KiB: 8190 bind ops of 32 B */
#define G18_RN_ALIGN8(x)   (((x) + 7u) & ~7u)
#define G18_RN_DATA_OFFSET(arg_size) G18_RN_ALIGN8(G18_RN_ARG_OFFSET + (arg_size))

/* Per-file and device limits (K1, R9). Exceeding one gives ENOSPC (handles)
 * or ENOMEM (arena bytes). */
#define G18_RN_MAX_VMS     16u
#define G18_RN_MAX_BOS     4096u
#define G18_RN_MAX_QUEUES  64u
#define G18_RN_MAX_MAPS    4096u

/* Linux errno values carried in g18_rn_hdr.result. */
#define G18_RN_EPERM       1
#define G18_RN_ENOENT      2
#define G18_RN_EIO         5
#define G18_RN_E2BIG       7
#define G18_RN_EAGAIN      11
#define G18_RN_ENOMEM      12
#define G18_RN_EACCES      13
#define G18_RN_EFAULT      14
#define G18_RN_EBUSY       16
#define G18_RN_EEXIST      17
#define G18_RN_ENODEV      19
#define G18_RN_EINVAL      22
#define G18_RN_ENOTTY      25
#define G18_RN_ENOSPC      28
#define G18_RN_ERANGE      34
#define G18_RN_ENOSYS      38
#define G18_RN_EOPNOTSUPP  95
#define G18_RN_ETIMEDOUT   110
#define G18_RN_ECANCELED   125

struct g18_rn_hdr {
	uint32_t abi;        /* in: G18_RN_ABI */
	uint32_t cmd;        /* in: G18_RN_*, equal to the code's function - G18_RN_FN_BASE */
	uint32_t frame_size; /* in: header + argument + inline data, <= G18_RN_FRAME_MAX */
	uint32_t arg_size;   /* in: the caller's sizeof(argument), <= G18_RN_ARG_MAX */
	int32_t result;      /* out: 0 or a positive Linux errno (G18_RN_E*) */
	uint32_t out_size;   /* out: argument bytes written back, min(arg_size, kernel's size) */
	uint32_t flags;      /* in: MBZ */
	uint32_t reserved;   /* in: MBZ */
};

/* VERSION: drmGetVersion. Mesa checks name against "aurora" / "asahi". */
#define G18_RN_DRIVER_NAME "aurora"
#define G18_RN_DRIVER_MAJOR 1u
#define G18_RN_DRIVER_MINOR 0u
#define G18_RN_DRIVER_PATCH 0u

struct g18_rn_version {
	uint32_t abi;                       /* out: G18_RN_ABI */
	uint32_t major, minor, patchlevel;  /* out: G18_RN_DRIVER_* */
	uint32_t state, flags;              /* out: G18_STATE_*, G18_INFO_* */
	char name[32];                      /* out: G18_RN_DRIVER_NAME, NUL-terminated */
	char desc[64];                      /* out: driver build string, NUL-terminated */
};

/* GEM_CLOSE: releases the handle. Pages, GPU bindings and CPU maps of the
 * object stay valid (nothing is freed before reboot, G4-PLAN 3.2). */
struct g18_rn_gem_close {
	uint32_t handle;
	uint32_t pad;        /* MBZ */
};

/* GEM_MAP: map a BO into the calling process, CPU read/write,
 * write-combined (G4-PLAN 3.7). size 0 maps the whole BO; otherwise a
 * multiple of 16 KiB, at most the BO size, from offset 0. fixed_va must be 0
 * (no placed maps in G4: EINVAL). A file may hold G18_RN_MAX_MAPS maps; they
 * are unmapped at file cleanup. */
struct g18_rn_gem_map {
	uint32_t handle;
	uint32_t flags;      /* MBZ */
	uint64_t size;       /* in: bytes or 0; out: bytes mapped */
	uint64_t fixed_va;   /* MBZ */
	uint64_t va;         /* out */
};

/* GEM_UNMAP: va is the start of a GEM_MAP mapping of this file; size is 0 or
 * that mapping's size. */
struct g18_rn_gem_unmap {
	uint64_t va;
	uint64_t size;
};

/* ---- asahi structs (layouts of Mesa's asahi_drm.h at 7c34f6f2) ---------
 * Same field order, types and sizes as struct drm_asahi_<name>;
 * uapi/asahi_check.c asserts every offset against Mesa's header. Only the
 * structs the render node uses are mirrored (no render commands in G4). */

#define G18_ASAHI_MAX_CLUSTERS 64u

/* Feature bits (drm_asahi_params_global.features). */
#define G18_ASAHI_FEATURE_SOFT_FAULTS             (1ull << 0)
#define G18_ASAHI_FEATURE_VM_STATUS               (1ull << 1)
#define G18_ASAHI_FEATURE_INDEPENDENT_QUEUES      (1ull << 2)
#define G18_ASAHI_FEATURE_VM_STATUS_MIRROR        (1ull << 11)
#define G18_ASAHI_FEATURE_COMPUTE_RESOURCES       (1ull << 28)
#define G18_ASAHI_FEATURE_G18_GRAPHICS_SCRATCH    (1ull << 31)
#define G18_ASAHI_FEATURE_G18_NATIVE_TILE_SAMPLES (1ull << 32)
#define G18_ASAHI_FEATURE_COMPUTE_TIMEOUT         (1ull << 36)
/* What G4 advertises (G4-PLAN 3.3): VM_STATUS and the two G18 bits of L1.
 * SOFT_FAULTS stays clear (K7: our init data programs no fault_control 0xb);
 * VM_STATUS_MIRROR stays clear (hk falls back to group 1). */
#define G18_RN_FEATURES_G4 \
	(G18_ASAHI_FEATURE_VM_STATUS | G18_ASAHI_FEATURE_G18_GRAPHICS_SCRATCH | \
	 G18_ASAHI_FEATURE_G18_NATIVE_TILE_SAMPLES)

/* GET_PARAMS groups. */
#define G18_ASAHI_PARAM_GROUP_GLOBAL           0u
#define G18_ASAHI_PARAM_GROUP_VM_STATUS        1u
#define G18_ASAHI_PARAM_GROUP_QUEUE_LIMITS     2u
#define G18_ASAHI_PARAM_GROUP_VM_STATUS_MIRROR 3u
/* G6 life (g18 only): struct g18_rn_resources, the engine's resource
 * counters (also answered in state LOST). */
#define G18_RN_PARAM_GROUP_RESOURCES           0x100u

/* GEM flags. */
#define G18_ASAHI_GEM_WRITEBACK         (1u << 0)
#define G18_ASAHI_GEM_VM_PRIVATE        (1u << 1)
#define G18_ASAHI_GEM_USERPTR_READ_ONLY (1u << 2)

/* Bind flags. */
#define G18_ASAHI_BIND_UNBIND      (1u << 0)
#define G18_ASAHI_BIND_READ        (1u << 1)
#define G18_ASAHI_BIND_WRITE       (1u << 2)
#define G18_ASAHI_BIND_SINGLE_PAGE (1u << 3)
#define G18_ASAHI_BIND_SPARSE_TILE (1u << 4) /* not accepted in G4 */

#define G18_ASAHI_GEM_MADV_WILLNEED 0u
#define G18_ASAHI_GEM_MADV_DONTNEED 1u

/* Priorities (QUEUE_CREATE accepts <= 3). */
#define G18_ASAHI_PRIORITY_LOW      0u
#define G18_ASAHI_PRIORITY_MEDIUM   1u
#define G18_ASAHI_PRIORITY_HIGH     2u
#define G18_ASAHI_PRIORITY_REALTIME 3u

/* Command types (g18_asahi_cmd_header.cmd_type). G4 admits
 * SET_COMPUTE_ATTACHMENTS and COMPUTE only. */
#define G18_ASAHI_CMD_RENDER                  0u
#define G18_ASAHI_CMD_COMPUTE                 1u
#define G18_ASAHI_SET_VERTEX_ATTACHMENTS      2u
#define G18_ASAHI_SET_FRAGMENT_ATTACHMENTS    3u
#define G18_ASAHI_SET_COMPUTE_ATTACHMENTS     4u
#define G18_ASAHI_SET_RENDER_RESOURCES        5u
#define G18_ASAHI_SET_COMPUTE_RESOURCES       6u
#define G18_ASAHI_SET_RENDER_RESOURCES_V2     7u
#define G18_ASAHI_BARRIER_NONE                0xFFFFu

/* Compute flags (g18_asahi_cmd_compute.flags). G4 admits the two G18 ones. */
#define G18_ASAHI_COMPUTE_G17P_ADD3_PROOF     (1u << 0)
#define G18_ASAHI_COMPUTE_AS_BUILD            (1u << 2)
#define G18_ASAHI_COMPUTE_USES_MATRIX         (1u << 16)
#define G18_ASAHI_COMPUTE_NO_QUERY_ACCESS     (1u << 17)
#define G18_ASAHI_COMPUTE_USES_TEXTURE_STATE  (1u << 18)
#define G18_ASAHI_COMPUTE_G18_RESOURCES       (1u << 19)
#define G18_ASAHI_COMPUTE_VISIBLE_COMPLETION  (1u << 20)

struct g18_asahi_params_global {
	uint64_t features;
	uint32_t gpu_generation;
	uint32_t gpu_variant;
	uint32_t gpu_revision;
	uint32_t chip_id;
	uint32_t num_dies;
	uint32_t num_clusters_total;
	uint32_t num_cores_per_cluster;
	uint32_t max_frequency_khz;
	uint64_t core_masks[G18_ASAHI_MAX_CLUSTERS];
	uint64_t vm_start;
	uint64_t vm_end;
	uint64_t vm_kernel_min_size;
	uint32_t max_commands_per_submission;
	uint32_t max_attachments;
	uint64_t command_timestamp_frequency_hz;
	uint32_t usc_generation;
	uint32_t gpu_hal_generation;
	uint64_t memory_limit;
	uint64_t client_memory_limit;
};

struct g18_asahi_vm_status_mirror {
	uint32_t vm_id;
	uint32_t handle;
	uint32_t offset;
	uint32_t flags;
};

/* G6 life: GET_PARAMS group G18_RN_PARAM_GROUP_RESOURCES. Engine-wide but
 * the file_* words; the driver copies min(size, 128) bytes. */
struct g18_rn_resources {
	uint64_t vms_live;           /* render-node VMs that exist now */
	uint64_t vms_made;           /* VMs created this boot */
	uint64_t tables_free;        /* page-table pages available (arenas + freed) */
	uint64_t tables_live;        /* page-table pages in use (G2's included) */
	uint64_t bos_live;           /* BOs whose pages are held (zombies included) */
	uint64_t bo_bytes_live;
	uint64_t bo_heap_free;       /* freed BO bytes waiting inside the arena */
	uint64_t bo_arena_used;      /* the replay/BO arena's bump pointer */
	uint64_t contexts_free;      /* context ids available (fresh + reusable) */
	uint64_t quarantined_bytes;  /* BO pages kept out of use (a lost device) */
	uint64_t quarantined_tables;
	uint64_t file_bos;           /* this file: open BO handles */
	uint64_t file_zombies;       /* this file: closed BOs still reached */
	uint64_t file_vms;           /* this file: VMs */
	uint64_t jobs;               /* live jobs run */
	uint64_t lost;               /* 1 once the device is lost */
};

struct g18_asahi_vm_status {
	uint32_t vm_id;
	uint32_t flags;
	int32_t error;
	uint32_t pad;
};

struct g18_asahi_queue_limits {
	uint32_t max_queues;
	uint32_t max_in_flight_per_queue;
	uint32_t flags;
	uint32_t pad;
};

struct g18_asahi_get_params {
	uint32_t param_group;
	uint32_t pad;
	uint64_t pointer;    /* Windows: frame offset of the group's struct */
	uint64_t size;
};

struct g18_asahi_vm_create {
	uint64_t kernel_start;
	uint64_t kernel_end;
	uint32_t vm_id;      /* out */
	uint32_t pad;
};

struct g18_asahi_vm_destroy {
	uint32_t vm_id;
	uint32_t pad;
};

struct g18_asahi_gem_create {
	uint64_t size;
	uint32_t flags;
	uint32_t vm_id;
	uint32_t handle;     /* out */
	uint32_t pad;
};

struct g18_asahi_gem_userptr {
	uint64_t userptr;
	uint64_t size;
	uint32_t flags;
	uint32_t vm_id;
	uint32_t handle;
	uint32_t pad;
};

struct g18_asahi_gem_mmap_offset {
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
};

struct g18_asahi_gem_madvise {
	uint32_t handle;
	uint32_t madv;
	uint32_t retained;   /* out */
	uint32_t pad;
};

struct g18_asahi_gem_residency {
	uint32_t handle;
	uint32_t flags;
};

struct g18_asahi_gem_bind_op {
	uint32_t flags;
	uint32_t handle;
	uint64_t offset;
	uint64_t range;
	uint64_t addr;
};

struct g18_asahi_vm_bind {
	uint32_t vm_id;
	uint32_t num_binds;
	uint32_t stride;
	uint32_t pad;
	uint64_t userptr;    /* Windows: frame offset of the ops */
};

struct g18_asahi_gem_bind_object {
	uint32_t op;
	uint32_t flags;
	uint32_t handle;
	uint32_t vm_id;
	uint64_t offset;
	uint64_t range;
	uint32_t object_handle;
	uint32_t pad;
};

struct g18_asahi_queue_create {
	uint32_t flags;
	uint32_t vm_id;
	uint32_t priority;
	uint32_t queue_id;   /* out */
	uint64_t usc_exec_base;
	uint64_t render_scs_layout[2];
	uint32_t compute_timeout_ms;
	uint32_t pad;
};

struct g18_asahi_queue_destroy {
	uint32_t queue_id;
	uint32_t pad;
};

struct g18_asahi_sync {
	uint32_t sync_type;
	uint32_t handle;
	uint64_t timeline_value;
};

/* G6 perf: g18_asahi_submit.flags. A render returns after its doorbell;
 * the render node retires it at its next call that reaches the engine (any
 * call but VERSION and GEM_UNMAP): a later SUBMIT, VM_BIND or close runs
 * after it, and a synchronization-only SUBMIT (cmdbuf_size 0) waits for it.
 * Compute commands run synchronously either way. A render node without it
 * answers EINVAL. */
#define G18_SUBMIT_ASYNC (1u << 31)

struct g18_asahi_submit {
	uint64_t syncs;      /* Windows: must be 0 */
	uint64_t cmdbuf;     /* Windows: frame offset of the command buffer */
	uint32_t flags;      /* 0 or G18_SUBMIT_ASYNC */
	uint32_t queue_id;
	uint32_t in_sync_count;
	uint32_t out_sync_count;
	uint32_t cmdbuf_size;
	uint32_t pad;
};

struct g18_asahi_cmd_header {
	uint16_t cmd_type;
	uint16_t size;       /* payload bytes after this header */
	uint16_t vdm_barrier;
	uint16_t cdm_barrier;
};

struct g18_asahi_attachment {
	uint64_t pointer;
	uint64_t size;
	uint32_t pad;
	uint32_t flags;
};

struct g18_asahi_timestamp {
	uint32_t handle;
	uint32_t offset;
};

struct g18_asahi_timestamps {
	struct g18_asahi_timestamp start;
	struct g18_asahi_timestamp end;
};

struct g18_asahi_helper_program {
	uint32_t binary;
	uint32_t cfg;
	uint64_t data;
};

struct g18_asahi_compute_program {
	uint32_t primary_bytes;
	uint32_t secondary_bytes;
	uint32_t linked_bytes;
	uint32_t minimum_primary_bytes;
	uint32_t threadgroup_bytes;
	uint32_t threads;
	uint32_t flags;
};

struct g18_asahi_cmd_compute {
	uint32_t flags;
	uint32_t sampler_count;
	uint64_t cdm_ctrl_stream_base;
	uint64_t cdm_ctrl_stream_end;
	uint64_t sampler_heap;
	struct g18_asahi_helper_program helper;
	struct g18_asahi_timestamps ts;
	struct g18_asahi_timestamp ts_end_copies[8];
};

struct g18_asahi_g18_compute_resources {
	uint32_t version;
	uint32_t flags;
	uint32_t registers_16;
	uint32_t scratch_bytes;
	uint32_t spectpr_scratch_bytes;
	uint32_t staging_16;
	uint32_t constant_registers_16;
	uint32_t constant_scratch_bytes;
	uint32_t constant_staging_16;
	uint32_t local_bytes;
	uint32_t imageblock_stride;
	uint32_t variable_shared_bytes;
	uint32_t local_size[3];
	uint32_t reserved;
};

struct g18_asahi_cmd_compute_with_prior {
	struct g18_asahi_cmd_compute legacy;
	uint64_t scs_layout;
	uint64_t prior_vdm;
	uint64_t prior_cdm;
	uint32_t cdm_auxiliary_allocation;
	uint32_t reserved;
	uint64_t intersection_pool;
	uint64_t intersection_pool_bytes;
	uint32_t intersection_first;
	uint32_t intersection_count;
	uint64_t intersection_default_program;
	uint64_t intersection_default_control;
};

/* Payload of a COMPUTE command with G18_ASAHI_COMPUTE_G18_RESOURCES: the
 * 200-byte command, then the resources suffix at +200 (L1 [116]: size 264). */
struct g18_asahi_cmd_compute_g18 {
	struct g18_asahi_cmd_compute_with_prior base;
	struct g18_asahi_g18_compute_resources resources;
};

struct g18_asahi_get_time {
	uint64_t flags;
	uint64_t gpu_timestamp; /* out: ns */
};

/* Layout pins (the Rust mirror asserts the same numbers). */
#ifdef __cplusplus
#define G18_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define G18_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
#define G18_OFFSET_ASSERT(s, f, o) G18_STATIC_ASSERT(offsetof(struct s, f) == (o), #s "." #f)
G18_STATIC_ASSERT(sizeof(struct g18_info) == 360, "g18_info");
G18_STATIC_ASSERT(sizeof(struct g18_load_in) == 560, "g18_load_in");
G18_STATIC_ASSERT(sizeof(struct g18_g1_out) == 240, "g18_g1_out");
G18_STATIC_ASSERT(sizeof(struct g18_job) == 24, "g18_job");
G18_STATIC_ASSERT(sizeof(struct g18_g2_out) == 384, "g18_g2_out");
G18_STATIC_ASSERT(sizeof(struct g18_map_out) == 32, "g18_map_out");
G18_STATIC_ASSERT(sizeof(struct g18_unmap_in) == 16, "g18_unmap_in");
G18_STATIC_ASSERT(sizeof(struct g18_site) == 16, "g18_site");
G18_STATIC_ASSERT(sizeof(struct g18_status) == 1320, "g18_status");
G18_STATIC_ASSERT(sizeof(struct g18_log_in) == 256, "g18_log_in");
G18_STATIC_ASSERT(sizeof(struct g18_live_out) == 160, "g18_live_out");
G18_STATIC_ASSERT(sizeof(struct g18_tel) == 904, "g18_tel");

/* Render node (rev 2). Sizes of the asahi mirrors are Mesa's at 7c34f6f2;
 * uapi/asahi_check.c checks every field offset against Mesa's header. */
G18_STATIC_ASSERT(sizeof(struct g18_rn_hdr) == G18_RN_ARG_OFFSET, "g18_rn_hdr");
G18_STATIC_ASSERT(sizeof(struct g18_rn_version) == 120, "g18_rn_version");
G18_STATIC_ASSERT(sizeof(struct g18_rn_gem_close) == 8, "g18_rn_gem_close");
G18_STATIC_ASSERT(sizeof(struct g18_rn_gem_map) == 32, "g18_rn_gem_map");
G18_STATIC_ASSERT(sizeof(struct g18_rn_gem_unmap) == 16, "g18_rn_gem_unmap");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_params_global) == 616, "params_global (L1 params_bytes)");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_vm_status_mirror) == 16, "vm_status_mirror");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_vm_status) == 16, "vm_status");
G18_STATIC_ASSERT(sizeof(struct g18_rn_resources) == 128, "g18_rn_resources");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_queue_limits) == 16, "queue_limits");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_get_params) == 24, "get_params");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_vm_create) == 24, "vm_create");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_vm_destroy) == 8, "vm_destroy");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_gem_create) == 24, "gem_create");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_gem_userptr) == 32, "gem_userptr");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_gem_mmap_offset) == 16, "gem_mmap_offset");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_gem_madvise) == 16, "gem_madvise");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_gem_residency) == 8, "gem_residency");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_gem_bind_op) == 32, "gem_bind_op");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_vm_bind) == 24, "vm_bind");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_gem_bind_object) == 40, "gem_bind_object");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_queue_create) == 48, "queue_create");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_queue_destroy) == 8, "queue_destroy");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_sync) == 16, "sync");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_submit) == 40, "submit");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_cmd_header) == 8, "cmd_header");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_attachment) == 24, "attachment");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_timestamp) == 8, "timestamp");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_timestamps) == 16, "timestamps");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_helper_program) == 16, "helper_program");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_compute_program) == 28, "compute_program");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_cmd_compute) == 128, "cmd_compute");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_g18_compute_resources) == 64, "g18_compute_resources");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_cmd_compute_with_prior) == 200, "cmd_compute_with_prior");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_cmd_compute_g18) == 264, "cmd_compute_g18 (L1 [116])");
G18_STATIC_ASSERT(sizeof(struct g18_asahi_get_time) == 16, "get_time");
/* The struct-size skews the kernel accepts (min copy, zero fill). */
G18_OFFSET_ASSERT(g18_asahi_queue_create, render_scs_layout, 24);
G18_OFFSET_ASSERT(g18_asahi_queue_create, compute_timeout_ms, 40);
G18_OFFSET_ASSERT(g18_asahi_cmd_compute_with_prior, scs_layout, 128);
G18_OFFSET_ASSERT(g18_asahi_cmd_compute_g18, resources, 200);
G18_OFFSET_ASSERT(g18_asahi_params_global, memory_limit, 600);
G18_STATIC_ASSERT(G18_IOCTL_RN_GET_PARAMS == 0x8A18E800u, "0xA00 code");
G18_STATIC_ASSERT(G18_IOCTL_RN_GEM_UNMAP == 0x8A18E90Cu, "0xA43 code");
G18_STATIC_ASSERT(G18_IOCTL_IS_RN(G18_IOCTL_RN_SUBMIT) && !G18_IOCTL_IS_RN(G18_IOCTL_LOG), "IS_RN");

#endif
