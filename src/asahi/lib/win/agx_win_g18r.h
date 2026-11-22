/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Minimal writer for G18R v1 files (the asahi uAPI capture format of the
 * M6 project's L1 recorder), used by the stub render node to record what
 * Mesa sends, so the Windows port can be compared with a Linux capture.
 *
 * The file is a 64-byte header and 8-byte aligned chunks {type, bytes, seq,
 * crc32c} + payload; the last chunk is END with the SHA-256 of everything
 * before it. Blobs (16 KiB page contents) are deduplicated by SHA-256.
 * The whole file is kept in memory and written by g18r_finish().
 */

#ifndef AGX_WIN_G18R_H
#define AGX_WIN_G18R_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G18R_PAGE 16384u

enum g18r_type {
   G18R_CAPINFO = 0x0001,
   G18R_PARAMS = 0x0002,
   G18R_FILE = 0x0003,
   G18R_VM_NEW = 0x0010,
   G18R_VM_FREE = 0x0011,
   G18R_BO_NEW = 0x0020,
   G18R_BO_FREE = 0x0021,
   G18R_BIND = 0x0030,
   G18R_QUEUE_NEW = 0x0040,
   G18R_QUEUE_FREE = 0x0041,
   G18R_BLOB = 0x0050,
   G18R_PAGE_REC = 0x0051,
   G18R_SUBMIT = 0x0060,
   G18R_MARK = 0x80000001u,
   G18R_RAW_IOCTL = 0x80000002u,
   G18R_MMAP = 0x80000004u,
   G18R_END = 0x0000ffff,
};

#define G18R_FLAG_SYNTHETIC (1u << 2)

/* Start a file at path (written at g18r_finish). capinfo: key=value lines. */
bool g18r_begin(const char *path, const char *capinfo, uint32_t params_bytes);
bool g18r_active(void);
void g18r_chunk(uint32_t type, const void *a, uint32_t na, const void *b,
                uint32_t nb);
/* Blob id of a page's content (0 = all zero), emitting the BLOB if new. */
uint32_t g18r_blob(const void *page);
/* Write the file (header totals and END). Idempotent. */
void g18r_finish(void);

void g18r_sha256(const void *data, uint64_t n, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif
