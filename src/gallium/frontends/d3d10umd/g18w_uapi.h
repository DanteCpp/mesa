/*
 * g18w_uapi.h: the g18wddm KMD <-> UMD contract (WDDM 1.3 model, one UMA memory segment).
 * C99, no Windows headers; shared with the UMD (mesa-win g6-umd).
 *
 * Allocations: every DXGK/D3DDDI allocation's private driver data is a g18w_alloc_priv. The KMD places
 * every allocation in segment 1 (the UMA pool; CPU-visible, cache-coherent), so pfnLockCb works on all.
 *
 * Command buffers (pfnRenderCb, and the KMD's own Present/paging buffers) are a sequence of packets,
 * each starting with g18w_hdr { op, bytes } (bytes includes the header, multiple of 8). An allocation is
 * named by a g18w_ref: its index in the allocation list of that submission plus a byte offset; the KMD
 * resolves it at DxgkDdiPatch time (the patch location list is not used, but the UMD must pass at least
 * one patch location per command buffer that references an allocation, pointing at any ref).
 * The software executor (QEMU) bounds-checks every access against the segment; on the M6 the same
 * stream will carry G18 submits.
 */
#ifndef G18W_UAPI_H
#define G18W_UAPI_H

#include <stdint.h>

#define G18W_UAPI_VERSION 1u
#define G18W_ALLOC_MAGIC 0x41573847u /* "G8WA" */

enum g18w_alloc_kind {
	G18W_ALLOC_BUFFER = 1,   /* linear bytes */
	G18W_ALLOC_SURFACE = 2,  /* 2D, pitch-linear */
	G18W_ALLOC_PRIMARY = 3,  /* scan-out capable 2D (flip target) */
	G18W_ALLOC_SHADOW = 4,   /* standard: shadow surface (KMD-filled) */
	G18W_ALLOC_STAGING = 5,  /* standard: staging surface (KMD-filled) */
	G18W_ALLOC_SHAREDPRIMARY = 6, /* standard: shared primary (KMD-filled) */
	G18W_ALLOC_GDI = 7,      /* standard: GDI surface (KMD-filled) */
};

#define G18W_ALLOCF_PRIMARY 0x1u /* scan-out surface: kept in segment 1 */
/* shared resource (D3D MISC_SHARED / keyed mutex / NT handle): VidMm accepts a shareable CPU-visible allocation
 * only when its supported segment set is aperture-only, so the KMD places it in segment 2 (system memory). */
#define G18W_ALLOCF_SHARED 0x2u

/* G6 zero copy (track Z): the GPU renders into it. The UMD keeps it locked and imports the locked range into the
 * render node (g18win GEM_USERPTR), so the zink texture IS the allocation: no shadow, no copy at Present. The KMD
 * keeps it in segment 1 only (the pool's WDDM window, which the GPU may reach; never the aperture), 64 KiB aligned,
 * its size a multiple of 64 KiB, the pitch as given (hk's linear row pitch). Not with G18W_ALLOCF_SHARED. */
#define G18W_ALLOCF_GPU 0x4u
#define G18W_GPU_ALIGN 0x10000u

/* The pool split (the M6): g18wddm's segment 1 is the WDDM window at the top of the carrier's GPU pool; g18win
 * keeps everything below it (BO arena, page tables, firmware structures). The same rule as g18core
 * gpumem::layout::window_off (g18kmd's host test compares): 44 MiB of a 256 MiB pool, half of a pool of 512 MiB
 * or more. Nothing assumes a pool size. */
#define G18W_WINDOW_SMALL 0x2C00000ull
#define G18W_BIG_POOL 0x20000000ull
static inline uint64_t g18w_window_off(uint64_t pool_bytes)
{
	uint64_t w = pool_bytes >= G18W_BIG_POOL ? pool_bytes / 2: G18W_WINDOW_SMALL;
	return pool_bytes > w ? (pool_bytes - w) & ~0xFFFFull: 0;
}

struct g18w_alloc_priv {
	uint32_t magic;   /* G18W_ALLOC_MAGIC */
	uint32_t version; /* G18W_UAPI_VERSION */
	uint32_t kind;    /* g18w_alloc_kind */
	uint32_t flags;   /* G18W_ALLOCF_* */
	uint32_t width, height; /* texels (0 for a buffer) */
	uint32_t pitch;   /* bytes per row (0: the KMD picks width * bpp, 256-byte aligned) */
	uint32_t format;  /* D3DDDIFORMAT */
	uint32_t bpp;     /* bytes per texel (4 for the 32-bit formats) */
	uint32_t vidpn_source;
	uint32_t refresh_num, refresh_den;
	uint64_t size;    /* bytes (0: pitch * height); the KMD rounds up to 4 KiB */
	uint32_t reserved[4];
};

/* UMDRIVERPRIVATE (D3DKMTQueryAdapterInfo KMTQAITYPE_UMDRIVERPRIVATE) */
struct g18w_adapter_info {
	uint32_t magic;   /* G18W_ALLOC_MAGIC */
	uint32_t version; /* G18W_UAPI_VERSION */
	uint32_t executor; /* 1 = software (QEMU), 2 = G18 */
	uint32_t width, height, format; /* the scan-out mode */
	uint64_t segment_size;
	uint32_t flags; /* G18W_ADAPTERF_* (was reserved[0]) */
	uint32_t reserved[7];
};
/* the KMD takes G18W_ALLOCF_GPU allocations (segment 1 is the pool's WDDM window the render node imports) */
#define G18W_ADAPTERF_GPU_ALLOC 0x1u

enum g18w_op {
	G18W_OP_NOP = 0,
	G18W_OP_FILL = 1,      /* g18w_fill: 32-bit pattern over bytes */
	G18W_OP_COPY = 2,      /* g18w_copy: bytes */
	G18W_OP_BLT = 3,       /* g18w_blt: 2D rect copy, same bpp */
	G18W_OP_FILLRECT = 4,  /* g18w_fillrect */
	G18W_OP_FLIP = 5,      /* g18w_flip: marks a flip (scan-out moves in SetVidPnSourceAddress) */
	/* KMD-only (paging buffers; rejected in UMD command buffers) */
	G18W_OP_PG_XFER = 0x100,
	G18W_OP_PG_FILL = 0x101,
	G18W_OP_PG_MAP = 0x102,   /* aperture: g18w_pg_map */
	G18W_OP_PG_UNMAP = 0x103,
};

struct g18w_hdr {
	uint32_t op;
	uint32_t bytes;
};

struct g18w_ref {
	uint32_t index;  /* allocation list index */
	uint32_t resolved; /* KMD: 1 after DxgkDdiPatch */
	uint64_t offset; /* bytes into the allocation */
	uint64_t addr;   /* KMD: segment address after DxgkDdiPatch */
};

struct g18w_fill {
	struct g18w_hdr hdr;
	struct g18w_ref dst;
	uint64_t bytes;
	uint32_t pattern, flags; /* flags: G18W_FILLF_* (was pad, 0) */
};
/* G6 r7 (A): the pattern is replaced by the outcome of the last G18W_BLTF_IF_CHANGED BLT of the same DMA buffer:
 * G18W_BLT_COPIED or G18W_BLT_UNCHANGED (an older KMD writes the pattern, which the UMD sets to COPIED) */
#define G18W_FILLF_BLT_STATUS 0x1u
#define G18W_BLT_COPIED 1u
#define G18W_BLT_UNCHANGED 2u

struct g18w_copy {
	struct g18w_hdr hdr;
	struct g18w_ref dst, src;
	uint64_t bytes;
};

struct g18w_blt {
	struct g18w_hdr hdr;
	struct g18w_ref dst, src;
	uint32_t dst_pitch, src_pitch, bpp, flags; /* flags: G18W_BLTF_* (was pad, 0) */
	int32_t dx, dy, sx, sy, w, h;
};
/* G6 r6 (H): the destination is written only by BLTs from this source rect (a UMD proxy of a shared surface):
 * the executor hashes the source and skips the write when it equals the last BLT's to the same destination */
#define G18W_BLTF_IF_CHANGED 0x1u

struct g18w_fillrect {
	struct g18w_hdr hdr;
	struct g18w_ref dst;
	uint32_t pitch, bpp;
	int32_t x, y, w, h;
	uint32_t color, pad;
};

struct g18w_flip {
	struct g18w_hdr hdr;
	struct g18w_ref src;
	uint32_t vidpn_source, pad;
	/* UMD flip (pfnRenderCb): pitch != 0 makes the executor scan out src at the next vsync (pitch bytes per
	 * row, width x height texels, 4 bytes each). The KMD's own Present flips leave these 0. */
	uint32_t pitch, width, height, flags; /* flags: G18W_FLIPF_* */
	/* G6 r7 (A), present when hdr.bytes >= sizeof(struct g18w_flip) (an older UMD sends G18W_FLIP_V1_BYTES): the
	 * UMD's id of this frame with G18W_FLIPF_TAG (see g18w_esc_flipq) */
	uint64_t tag;
};
#define G18W_FLIP_V1_BYTES 56u
/* G6 r7 (A): the UMD did not wait for the GPU. The executor queues the flip and scans it out at the first vsync
 * after the render node finished every job submitted before the executor ran this packet (all of them: the
 * render node runs one job at a time, in order); without a render node (QEMU) at once. Up to 4 such flips wait;
 * a newer signalled one supersedes older ones. */
#define G18W_FLIPF_FENCE 0x1u
#define G18W_FLIPF_TAG 0x2u
/* G6 r7 (H): REGISTER: no flip; the executor remembers src/pitch/size as scan-out slot vidpn_source (0..3) for
 * G18W_ESC_FLIPNOW */
#define G18W_FLIPF_REGISTER 0x4u

/* paging (KMD-built) */
struct g18w_pg_xfer {
	struct g18w_hdr hdr;
	uint64_t bytes;
	uint64_t src_addr, dst_addr; /* segment addresses when the side's segment is 1 */
	uint64_t src_mdl, dst_mdl;   /* PMDL when the side's segment is 0 */
	uint64_t src_mdl_off, dst_mdl_off; /* bytes into the MDL's mapping */
	uint32_t src_seg, dst_seg;
};

struct g18w_pg_fill {
	struct g18w_hdr hdr;
	uint64_t dst_addr, bytes;
	uint32_t pattern, pad;
};

struct g18w_pg_map {
	struct g18w_hdr hdr;
	uint64_t first_page, pages; /* aperture pages */
	uint64_t mdl, mdl_off;      /* PMDL and the byte offset of the first page in its mapping */
};

/* DxgkDdiEscape: stats for the test tools */
#define G18W_ESC_STATS 0x47380001u
struct g18w_esc_stats {
	uint32_t code; /* G18W_ESC_STATS */
	uint32_t version;
	uint64_t vsyncs, flips, submits, packets, bad_packets, fences_done, presents_blt, presents_flip, renders,
		paging_xfer, paging_fill, bytes_moved, scanout_copies;
	uint64_t last_fence;
};

/* G6 r7 (A) DxgkDdiEscape: the UMD flip queue (no wait). Tags are the UMD's (G18W_FLIPF_TAG): a buffer flipped
 * with tag t may be written again once tag_done > t (a newer flip finished its scan-out copy, so neither the panel
 * nor a queued flip reads it). tag_signaled: the newest flip whose fence signalled (the GPU finished every job
 * submitted before it). */
#define G18W_ESC_FLIPQ 0x47380002u
struct g18w_esc_flipq {
	uint32_t code; /* G18W_ESC_FLIPQ */
	uint32_t version; /* KMD: 1 */
	uint64_t tag_signaled, tag_latched, tag_done;
	uint64_t vsyncs, rn_seq, rn_done;
	uint32_t pending, rn; /* rn: 1 when a render node reports progress (the M6) */
	uint64_t fence_flips, fence_waits, fence_timeouts, superseded;
};

/* G6 r7 (H) DxgkDdiEscape: queue a fenced flip of registered scan-out slot `slot` with tag `tag` now (sent by the
 * UMD's helper thread once zink's submit thread handed the frame to the render node, so the fence the executor
 * takes covers it). KMD answers version 1; STATUS_INVALID_PARAMETER for an unregistered slot. */
#define G18W_ESC_FLIPNOW 0x47380003u
struct g18w_esc_flipnow {
	uint32_t code; /* G18W_ESC_FLIPNOW */
	uint32_t slot;
	uint64_t tag;
	uint32_t version, pad;
};

#endif
