/* SPDX-License-Identifier: MIT
 * Per-command, negotiated WDDM primary-write declaration. No firmware ABI. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AGX_PRIMARY_WRITE_RANGES 20u

/* Why a set is incomplete (a command the driver refuses to submit under the
 * version-6 contract). Bits accumulate; the log names them. */
#define AGX_PRIMARY_WRITE_CAUSE_RANGES           (1u << 0) /* a bad range, or more than 20 */
#define AGX_PRIMARY_WRITE_CAUSE_ATTACHMENT       (1u << 1) /* a render attachment without a plain VA span */
#define AGX_PRIMARY_WRITE_CAUSE_SHADER_WRITES    (1u << 2) /* draw shaders write memory (storage/UAV) */
#define AGX_PRIMARY_WRITE_CAUSE_XFB              (1u << 3) /* transform feedback */
#define AGX_PRIMARY_WRITE_CAUSE_COMPUTE_NO_SCOPE (1u << 4) /* a dispatch outside any transfer or helper scope */
#define AGX_PRIMARY_WRITE_CAUSE_TRANSFER_DST     (1u << 5) /* a transfer whose destination span is unknown */
#define AGX_PRIMARY_WRITE_CAUSE_HELPER_UNPROVEN  (1u << 6) /* a helper write not proven to hit a never-shared driver BO */
#define AGX_PRIMARY_WRITE_CAUSE_NOT_OBSERVED     (1u << 7) /* a stream with no noted work */

struct agx_primary_write_range { uint64_t address, size; };
struct agx_primary_write_set {
   struct agx_primary_write_range ranges[AGX_PRIMARY_WRITE_RANGES];
   uint32_t count;
   bool observed, incomplete;
   uint32_t causes; /* AGX_PRIMARY_WRITE_CAUSE_*, nonzero iff a refusal reason was recorded */
};

static inline void
agx_primary_write_refuse(struct agx_primary_write_set *s, uint32_t cause)
{
   s->observed = true;
   s->incomplete = true;
   s->causes |= cause;
}

static inline void
agx_primary_write_add(struct agx_primary_write_set *s, uint64_t start,
                      uint64_t size)
{
   s->observed = true;
   if (!start || !size || size > UINT64_MAX - start) {
      agx_primary_write_refuse(s, AGX_PRIMARY_WRITE_CAUSE_RANGES);
      return;
   }
   uint64_t end = start + size;
   uint32_t first = 0, last;
   while (first < s->count &&
          s->ranges[first].address + s->ranges[first].size < start) first++;
   last = first;
   while (last < s->count && s->ranges[last].address <= end) {
      if (s->ranges[last].address < start) start = s->ranges[last].address;
      uint64_t tail = s->ranges[last].address + s->ranges[last].size;
      if (tail > end) end = tail;
      last++;
   }
   if (first == last && s->count == AGX_PRIMARY_WRITE_RANGES) {
      agx_primary_write_refuse(s, AGX_PRIMARY_WRITE_CAUSE_RANGES);
      return;
   }
   memmove(&s->ranges[first + 1], &s->ranges[last],
           (s->count - last) * sizeof(s->ranges[0]));
   s->ranges[first] = (struct agx_primary_write_range){start, end - start};
   s->count = s->count - (last - first) + 1;
}

/* A helper write (an event or query word). proven: the caller checked that
 * the destination lies inside one of the driver's own BOs that is neither
 * shared nor shareable, so it can never be a primary: it is observed and
 * declares nothing. Not proven: refused, by name. */
static inline void
agx_primary_write_note_private(struct agx_primary_write_set *s, bool proven)
{
   if (proven)
      s->observed = true;
   else
      agx_primary_write_refuse(s, AGX_PRIMARY_WRITE_CAUSE_HELPER_UNPROVEN);
}

static inline void
agx_primary_write_merge(struct agx_primary_write_set *a,
                        const struct agx_primary_write_set *b)
{
   a->observed |= b->observed;
   a->incomplete |= b->incomplete;
   a->causes |= b->causes;
   for (uint32_t i = 0; i < b->count; i++)
      agx_primary_write_add(a, b->ranges[i].address, b->ranges[i].size);
}

/* One cause bit's name for the log ("unknown" for anything else). */
static inline const char *
agx_primary_write_cause_name(uint32_t cause)
{
   switch (cause) {
   case AGX_PRIMARY_WRITE_CAUSE_RANGES: return "ranges";
   case AGX_PRIMARY_WRITE_CAUSE_ATTACHMENT: return "attachment";
   case AGX_PRIMARY_WRITE_CAUSE_SHADER_WRITES: return "shader-writes-memory";
   case AGX_PRIMARY_WRITE_CAUSE_XFB: return "xfb";
   case AGX_PRIMARY_WRITE_CAUSE_COMPUTE_NO_SCOPE: return "compute-no-scope";
   case AGX_PRIMARY_WRITE_CAUSE_TRANSFER_DST: return "transfer-dst-unknown";
   case AGX_PRIMARY_WRITE_CAUSE_HELPER_UNPROVEN: return "helper-unproven";
   case AGX_PRIMARY_WRITE_CAUSE_NOT_OBSERVED: return "not-observed";
   default: return "unknown";
   }
}

/* Tight byte range [*lo_out, *hi_out) written in a destination buffer by one
 * VkBufferImageCopy2 region, clamped to buffer_size. Returns false when the
 * region writes no bytes. Intermediate overflow saturates to buffer_size. */
static inline bool
agx_copy_image_to_buffer_bounds(uint64_t buffer_offset,
                                uint32_t buffer_row_length,
                                uint32_t buffer_image_height,
                                uint32_t extent_width,
                                uint32_t extent_height,
                                uint32_t extent_depth,
                                uint32_t layer_count,
                                uint32_t block_w,
                                uint32_t block_h,
                                uint32_t block_d,
                                uint32_t blocksize_B,
                                uint64_t buffer_size,
                                uint64_t *lo_out,
                                uint64_t *hi_out)
{
   if (!extent_width || !extent_height || !extent_depth || !layer_count ||
       !block_w || !block_h || !block_d || !blocksize_B ||
       buffer_offset >= buffer_size)
      return false;

   const uint64_t bw = block_w;
   const uint64_t bh = block_h;
   const uint64_t bd = block_d;
   const uint64_t bs = blocksize_B;
   const uint64_t row_texels =
      buffer_row_length > extent_width ? buffer_row_length : extent_width;
   const uint64_t slice_texels =
      buffer_image_height > extent_height ? buffer_image_height : extent_height;

   const uint64_t width_blocks = ((uint64_t)extent_width + bw - 1u) / bw;
   const uint64_t height_blocks = ((uint64_t)extent_height + bh - 1u) / bh;
   const uint64_t depth_blocks = ((uint64_t)extent_depth + bd - 1u) / bd;
   const uint64_t row_blocks = (row_texels + bw - 1u) / bw;
   const uint64_t slice_rows = (slice_texels + bh - 1u) / bh;
   const uint64_t slices =
      depth_blocks > (uint64_t)layer_count ? depth_blocks : (uint64_t)layer_count;

   const uint64_t max_span = buffer_size - buffer_offset;
   uint64_t span = max_span;

   if (width_blocks <= max_span / bs && row_blocks <= UINT64_MAX / bs) {
      const uint64_t row_bytes = width_blocks * bs;
      const uint64_t row_stride = row_blocks * bs;
      bool overflow = false;
      uint64_t total = row_bytes;

      if (height_blocks > 1u) {
         const uint64_t extra_rows = height_blocks - 1u;
         if (row_stride > (max_span - total) / extra_rows)
            overflow = true;
         else
            total += extra_rows * row_stride;
      }

      if (!overflow && slices > 1u) {
         const uint64_t extra_slices = slices - 1u;
         if (row_stride > UINT64_MAX / slice_rows) {
            overflow = true;
         } else {
            const uint64_t slice_stride = slice_rows * row_stride;
            if (slice_stride > (max_span - total) / extra_slices)
               overflow = true;
            else
               total += extra_slices * slice_stride;
         }
      }

      if (!overflow)
         span = total;
   }

   *lo_out = buffer_offset;
   *hi_out = buffer_offset + span;
   return true;
}
