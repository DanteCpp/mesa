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
