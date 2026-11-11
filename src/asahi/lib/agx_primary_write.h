/* SPDX-License-Identifier: MIT
 * Per-command, negotiated WDDM primary-write declaration. No firmware ABI. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AGX_PRIMARY_WRITE_RANGES 20u
struct agx_primary_write_range { uint64_t address, size; };
struct agx_primary_write_set {
   struct agx_primary_write_range ranges[AGX_PRIMARY_WRITE_RANGES];
   uint32_t count;
   bool observed, incomplete;
};

static inline void
agx_primary_write_add(struct agx_primary_write_set *s, uint64_t start,
                      uint64_t size)
{
   s->observed = true;
   if (!start || !size || size > UINT64_MAX - start) {
      s->incomplete = true;
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
      s->incomplete = true;
      return;
   }
   memmove(&s->ranges[first + 1], &s->ranges[last],
           (s->count - last) * sizeof(s->ranges[0]));
   s->ranges[first] = (struct agx_primary_write_range){start, end - start};
   s->count = s->count - (last - first) + 1;
}

static inline void
agx_primary_write_merge(struct agx_primary_write_set *a,
                        const struct agx_primary_write_set *b)
{
   a->observed |= b->observed;
   a->incomplete |= b->incomplete;
   for (uint32_t i = 0; i < b->count; i++)
      agx_primary_write_add(a, b->ranges[i].address, b->ranges[i].size);
}
