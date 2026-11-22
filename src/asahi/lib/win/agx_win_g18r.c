/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/* G18R v1 writer (agx_win_g18r.h). Plain C11 and stdio; callers serialize. */

#include "agx_win_g18r.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct g18r_state {
   bool active, done;
   char *path;
   uint8_t *buf;
   size_t len, cap;
   uint32_t seq;
   uint32_t params_bytes;
   uint32_t nblobs, blobs_cap;
   uint8_t (*blob_sha)[32];
};

static struct g18r_state g18r;

/* ---- SHA-256 (FIPS 180-4) ---------------------------------------------- */

static const uint32_t sha_k[64] = {
   0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
   0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
   0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
   0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
   0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
   0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
   0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
   0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
   0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
   0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
   0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha_block(uint32_t h[8], const uint8_t *p)
{
   uint32_t w[64];
   for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
             (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
   for (int i = 16; i < 64; i++) {
      uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
   }
   uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
   uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
   for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
                    ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
      uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
                    ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
   }
   h[0] += a, h[1] += b, h[2] += c, h[3] += d;
   h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void
g18r_sha256(const void *data, uint64_t n, uint8_t out[32])
{
   uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
   const uint8_t *p = data;
   uint64_t left = n;
   while (left >= 64) {
      sha_block(h, p);
      p += 64;
      left -= 64;
   }
   uint8_t tail[128] = {0};
   memcpy(tail, p, left);
   tail[left] = 0x80;
   unsigned tl = left < 56 ? 64 : 128;
   uint64_t bits = n * 8;
   for (int i = 0; i < 8; i++)
      tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
   sha_block(h, tail);
   if (tl == 128)
      sha_block(h, tail + 64);
   for (int i = 0; i < 8; i++) {
      out[4 * i] = h[i] >> 24;
      out[4 * i + 1] = h[i] >> 16;
      out[4 * i + 2] = h[i] >> 8;
      out[4 * i + 3] = h[i];
   }
}

/* ---- CRC-32C (Castagnoli, reflected 0x82F63B78) ------------------------ */

static uint32_t
crc32c(const uint8_t *p, size_t n)
{
   static uint32_t table[256];
   if (!table[1]) {
      for (uint32_t i = 0; i < 256; i++) {
         uint32_t c = i;
         for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
         table[i] = c;
      }
   }
   uint32_t c = 0xFFFFFFFFu;
   for (size_t i = 0; i < n; i++)
      c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
   return c ^ 0xFFFFFFFFu;
}

/* ---- buffer ------------------------------------------------------------ */

static bool
g18r_reserve(size_t more)
{
   if (g18r.len + more <= g18r.cap)
      return true;
   size_t cap = g18r.cap ? g18r.cap : (1u << 20);
   while (cap < g18r.len + more)
      cap *= 2;
   uint8_t *b = realloc(g18r.buf, cap);
   if (!b) {
      g18r.active = false; /* stop recording, keep the process going */
      return false;
   }
   g18r.buf = b;
   g18r.cap = cap;
   return true;
}

static void
put32(uint8_t *p, uint32_t v)
{
   memcpy(p, &v, 4); /* little-endian hosts only (aarch64, x86) */
}

static void
put64(uint8_t *p, uint64_t v)
{
   memcpy(p, &v, 8);
}

bool
g18r_begin(const char *path, const char *capinfo, uint32_t params_bytes)
{
   if (g18r.active || g18r.done)
      return g18r.active;
   g18r.path = strdup(path);
   g18r.params_bytes = params_bytes;
   if (!g18r.path || !g18r_reserve(64))
      return false;
   memset(g18r.buf, 0, 64);
   memcpy(g18r.buf, "G18RPLY1", 8);
   g18r.buf[8] = 1; /* major 1, minor 0 */
   put32(g18r.buf + 12, 64);
   put32(g18r.buf + 16, G18R_FLAG_SYNTHETIC);
   put32(g18r.buf + 20, G18R_PAGE);
   put32(g18r.buf + 36, params_bytes);
   g18r.len = 64;
   g18r.active = true;
   g18r_chunk(G18R_CAPINFO, capinfo, (uint32_t)strlen(capinfo), NULL, 0);
   return true;
}

bool
g18r_active(void)
{
   return g18r.active;
}

void
g18r_chunk(uint32_t type, const void *a, uint32_t na, const void *b,
           uint32_t nb)
{
   if (!g18r.active)
      return;
   uint32_t n = na + nb;
   if (!g18r_reserve(16 + (size_t)n + 8))
      return;
   uint8_t *h = g18r.buf + g18r.len;
   uint8_t *p = h + 16;
   if (na)
      memcpy(p, a, na);
   if (nb)
      memcpy(p + na, b, nb);
   put32(h, type);
   put32(h + 4, n);
   put32(h + 8, g18r.seq++);
   put32(h + 12, crc32c(p, n));
   g18r.len += 16 + n;
   while (g18r.len & 7)
      g18r.buf[g18r.len++] = 0;
}

uint32_t
g18r_blob(const void *page)
{
   static const uint8_t zero[G18R_PAGE];
   if (!memcmp(page, zero, G18R_PAGE))
      return 0;

   uint8_t sha[32];
   g18r_sha256(page, G18R_PAGE, sha);
   for (uint32_t i = 0; i < g18r.nblobs; i++) {
      if (!memcmp(g18r.blob_sha[i], sha, 32))
         return i + 1;
   }
   if (g18r.nblobs == g18r.blobs_cap) {
      uint32_t cap = g18r.blobs_cap ? 2 * g18r.blobs_cap : 64;
      void *n = realloc(g18r.blob_sha, cap * sizeof(*g18r.blob_sha));
      if (!n)
         return 0;
      g18r.blob_sha = n;
      g18r.blobs_cap = cap;
   }
   memcpy(g18r.blob_sha[g18r.nblobs++], sha, 32);
   uint32_t id = g18r.nblobs;

   uint8_t hdr[48] = {0};
   put32(hdr, id);
   put32(hdr + 4, 0); /* raw */
   put32(hdr + 8, G18R_PAGE);
   put32(hdr + 12, G18R_PAGE);
   memcpy(hdr + 16, sha, 32);
   g18r_chunk(G18R_BLOB, hdr, sizeof(hdr), page, G18R_PAGE);
   return id;
}

void
g18r_finish(void)
{
   if (!g18r.active)
      return;
   g18r.active = false;
   g18r.done = true;

   const uint32_t chunks = g18r.seq + 1;
   uint64_t file_bytes = g18r.len + 16 + 40;
   put64(g18r.buf + 24, file_bytes);
   put32(g18r.buf + 32, chunks);

   uint8_t end[40] = {0};
   put32(end, chunks);
   g18r_sha256(g18r.buf, g18r.len, end + 8);
   g18r.active = true;
   g18r_chunk(G18R_END, end, sizeof(end), NULL, 0);
   g18r.active = false;

   FILE *f = fopen(g18r.path, "wb");
   if (f) {
      fwrite(g18r.buf, 1, g18r.len, f);
      fclose(f);
   } else {
      fprintf(stderr, "agx-win-stub: cannot write %s\n", g18r.path);
   }
   free(g18r.buf);
   free(g18r.blob_sha);
   free(g18r.path);
   g18r.buf = NULL;
   g18r.blob_sha = NULL;
   g18r.path = NULL;
}
