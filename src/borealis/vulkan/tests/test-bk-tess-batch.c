/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Batched tessellation stages (bk_tess_batch.c): the real
 * bk_launch_tess_batched(), side streams, splice, flush points and
 * bk_cs_merge_cdm(), with launches, cache flushes, chunks and stream links
 * replaced by words this test decodes again. No GPU or DRM calls.
 */
#include "bk_cmd_buffer.h"
#include "bk_device.h"
#include "bk_tess_batch.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned tests, failures;

#define T(name, cond)                                                          \
   do {                                                                        \
      tests++;                                                                 \
      if (!(cond)) {                                                           \
         failures++;                                                           \
         printf("FAIL %s line %d\n", name, __LINE__);                          \
      } else                                                                   \
         printf("PASS %s\n", name);                                            \
   } while (0)

/* Records, one word each; a link carries its target in the next two. */
enum {
   LAUNCH = 0x10000000, /* | draw << 4 | stage */
   WEAK = 0x20000000,   /* the weak ordering tail of a launch */
   STRONG = 0x30000000, /* a cache flush */
   LINK = 0x40000000,
   OTHER = 0x50000000,  /* unrelated pre-graphics work */
   KIND = 0xf0000000,
};

#define CHUNK 65536
static void *chunks[256];
static unsigned nchunks;

static struct agx_ptr
chunk_alloc(void)
{
   assert(nchunks < ARRAY_SIZE(chunks));
   void *p = calloc(1, CHUNK);
   chunks[nchunks++] = p;
   return (struct agx_ptr){.cpu = p, .gpu = (uintptr_t)p};
}

struct agx_ptr
bk_pool_cs_alloc(struct bk_cmd_buffer *cmd, uint32_t size, uint32_t alignment)
{
   (void)cmd;
   (void)alignment;
   assert(size <= CHUNK);
   return chunk_alloc();
}

void *
bk_cmd_buffer_emit_cs_jump(struct bk_cmd_buffer *cmd, void *current,
                           uint64_t target, bool vdm)
{
   (void)cmd;
   assert(!vdm);
   uint32_t *w = current;
   w[0] = LINK;
   memcpy(&w[1], &target, sizeof(target));
   return (uint8_t *)current + AGX_CDM_STREAM_LINK_LENGTH;
}

/* The streams here record no alias sites (bk_cs::alias_sites stays zero), so
 * the merge never moves any. */
void
bk_cmd_buffer_move_alias_sites(struct bk_cmd_buffer *cmd, const void *from,
                               const void *from_end, uint8_t *to, size_t skip)
{
   assert(!"no alias sites are recorded");
}

/* As the driver's: move to a new chunk through a link when the space does
 * not fit. */
void
bk_ensure_cs_has_space(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                       size_t space)
{
   if (bk_cs_space_fits((uint8_t *)cs->end - (uint8_t *)cs->current, space,
                        AGX_CDM_STREAM_LINK_LENGTH))
      return;

   struct agx_ptr t = chunk_alloc();
   bk_cmd_buffer_emit_cs_jump(cmd, cs->current, t.gpu, false);
   cs->current = t.cpu;
   cs->end = (uint8_t *)t.cpu + CHUNK;
   cs->chunk = t;
   cs->stream_linked = true;
}

static void
put(struct bk_cs *cs, uint32_t word)
{
   *(uint32_t *)cs->current = word;
   cs->current = (uint8_t *)cs->current + 4;
}

/* As bk_cdm_flush(): strengthen a trailing barrier in place, else append. */
void
bk_cdm_cache_flush(struct bk_device *dev, struct bk_cs *cs)
{
   (void)dev;
   if (cs->current == cs->strong_flush_end)
      return;
   if (cs->current == cs->weak_barrier_end)
      ((uint32_t *)cs->current)[-1] = STRONG;
   else
      put(cs, STRONG);
   cs->strong_flush_end = cs->current;
   cs->weak_barrier_end = NULL;
   cs->stats.flushes++;
}

/* A native launch: the record and its weak ordering tail. */
static void
launch(struct bk_cs *cs, unsigned draw, unsigned stage)
{
   bk_ensure_cs_has_space(cs->cmd, cs, 0x2000);
   put(cs, LAUNCH | draw << 4 | stage);
   put(cs, WEAK);
   cs->weak_barrier_end = cs->current;
   cs->stats.cmds++;
}

static unsigned cur_draw;

static unsigned
stage_of(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   for (unsigned i = 0; i < BK_TESS_STAGES; ++i) {
      if (cmd->tess_batch.stages[i] == cs)
         return i;
   }
   abort();
}

/* The VS and TCS launches. */
void
bk_dispatch_g17p_native(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                        struct bk_shader *s, struct agx_grid grid,
                        struct agx_workgroup local_size)
{
   (void)s;
   (void)local_size;
   if (grid.count[0])
      launch(cs, cur_draw, stage_of(cmd, cs));
}

/* The precompiled kernels, through the pre-graphics getter as the driver's
 * bk_dispatch_precomp_g17p(); a batched launch asks for no flush. */
void
bk_dispatch_precomp(struct bk_cmd_buffer *cmd, struct agx_grid grid,
                    enum agx_barrier barrier, enum libagx_program idx,
                    void *data, size_t data_size)
{
   (void)idx;
   (void)data;
   (void)data_size;
   assert(barrier == AGX_PREGFX);
   if (!grid.count[0])
      return;
   struct bk_cs *cs =
      bk_cmd_buffer_get_cs_general(cmd, &cmd->current_cs.pre_gfx, true);
   launch(cs, cur_draw, stage_of(cmd, cs));
}

/* bk_launch_tess_batched() for one draw. `skip` stages dispatch nothing (an
 * empty grid); the count and tessellate stages share one grid, and the prefix
 * sum always runs. */
#define SKIP_TESS                                                              \
   (BITFIELD_BIT(BK_TESS_STAGE_COUNT) | BITFIELD_BIT(BK_TESS_STAGE_TESSELLATE))
static void
record_draw(struct bk_cmd_buffer *cmd, unsigned draw, unsigned skip)
{
   static struct bk_shader vs, tcs;
   vs.b.info.stats.instrs = tcs.b.info.stats.instrs = 2;
   tcs.info.tess.tcs_output_patch_size = 3;

   const struct agx_grid none = agx_1d(0);
   cur_draw = draw;
   bk_launch_tess_batched(
      cmd, &vs, &tcs, skip & BITFIELD_BIT(BK_TESS_STAGE_VS) ? none : agx_1d(3),
      skip & BITFIELD_BIT(BK_TESS_STAGE_TCS) ? none : agx_1d(3),
      skip & SKIP_TESS ? none : agx_1d(1),
      (struct bk_tess_info){.mode = TESS_PRIMITIVE_TRIANGLES}, 0);
}

/* Other work for the pre-graphics stream, ending in a flush. */
static void
other_work(struct bk_cmd_buffer *cmd)
{
   struct bk_cs *cs =
      bk_cmd_buffer_get_cs_general(cmd, &cmd->current_cs.pre_gfx, true);
   bk_ensure_cs_has_space(cmd, cs, 0x2000);
   put(cs, OTHER);
   cs->stats.cmds++;
   bk_cdm_cache_flush(NULL, cs);
}

/* The records of a stream in execution order, following its links. A link
 * must follow a barrier, or start the stream; returns the record count. */
static unsigned
decode(const struct bk_cs *cs, uint32_t *out, unsigned max, bool *link_ok)
{
   const uint32_t *w = cs->start;
   unsigned n = 0;
   *link_ok = true;
   while ((const void *)w != cs->current) {
      if ((*w & KIND) == LINK) {
         uint64_t target;
         memcpy(&target, &w[1], sizeof(target));
         if (n && (out[n - 1] & KIND) != WEAK && (out[n - 1] & KIND) != STRONG)
            *link_ok = false;
         w = (const uint32_t *)(uintptr_t)target;
         continue;
      }
      assert(n < max);
      out[n++] = *w++;
   }
   return n;
}

static unsigned
count(const uint32_t *r, unsigned n, uint32_t kind)
{
   unsigned c = 0;
   for (unsigned i = 0; i < n; ++i)
      c += (r[i] & KIND) == kind;
   return c;
}

/* Whether r is `draws` launches of each stage not in `skip`, stage by stage
 * in draw order, with only a flush after each stage. */
static bool
stage_major(const uint32_t *r, unsigned n, unsigned first, unsigned draws,
            unsigned skip)
{
   unsigned i = 0;
   for (unsigned stage = 0; stage < BK_TESS_STAGES; ++stage) {
      if (skip & BITFIELD_BIT(stage))
         continue;
      for (unsigned d = first; d < first + draws; ++d) {
         if (i >= n || r[i++] != (LAUNCH | d << 4 | stage))
            return false;
      }
      if (i >= n || r[i++] != STRONG)
         return false;
   }
   return i == n;
}

static struct bk_cmd_buffer *
new_cmd(void)
{
   struct bk_device *dev = calloc(1, sizeof(*dev));
   dev->dev.chip = AGX_CHIP_G17P;
   struct bk_cmd_buffer *cmd = calloc(1, sizeof(*cmd));
   cmd->vk.base.device = &dev->vk;
   list_inithead(&cmd->control_streams);
   return cmd;
}

static struct bk_cs *
open_pre_gfx(struct bk_cmd_buffer *cmd)
{
   return bk_cmd_buffer_get_cs_general(cmd, &cmd->current_cs.pre_gfx, true);
}

#define MAX_RECORDS (16384 * BK_TESS_STAGES + 64)
static uint32_t r[MAX_RECORDS];

int
main(void)
{
   bool link_ok;
   unsigned n;

   /* Three draws: five flushes, not fifteen, and no weak barrier between
    * the launches of a stage. */
   struct bk_cmd_buffer *cmd = new_cmd();
   struct bk_cs *pre = open_pre_gfx(cmd);
   for (unsigned d = 0; d < 3; ++d)
      record_draw(cmd, d, 0);
   T("pending until a flush point",
     pre->current == pre->start && cmd->tess_batch.draws == 3);
   void *roots[BK_TESS_STAGES];
   for (unsigned i = 0; i < BK_TESS_STAGES; ++i)
      roots[i] = cmd->tess_batch.stages[i]->start;
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("3 draws: stage-major", stage_major(r, n, 0, 3, 0));
   T("3 draws: 5 barriers", count(r, n, STRONG) == 5 && count(r, n, WEAK) == 0);
   T("3 draws: stats", pre->stats.cmds == 15 && cmd->tess_batch.draws == 0);
   bool kept = true;
   for (unsigned i = 0; i < BK_TESS_STAGES; ++i) {
      const struct bk_cs *stage = cmd->tess_batch.stages[i];
      kept &= stage && stage->start == roots[i] && stage->current == roots[i] &&
              !stage->stats.cmds;
   }
   T("copied side streams keep their roots, empty", kept);

   /* Alternating with other pre-graphics work reuses the five roots: no new
    * chunks beyond what the pre-graphics stream itself fills. */
   const unsigned chunks_before = nchunks;
   for (unsigned d = 3; d < 2003; ++d) {
      record_draw(cmd, d, 0);
      other_work(cmd);
   }
   T("alternating: roots reused", nchunks - chunks_before <= 2 &&
                                     cmd->tess_batch.stages[0]->start == roots[0]);

   /* 4096 draws: stages that no longer fit the pre-graphics root go to a
    * new chunk of it, still copied. */
   cmd = new_cmd();
   pre = open_pre_gfx(cmd);
   for (unsigned d = 0; d < 4096; ++d)
      record_draw(cmd, d, 0);
   for (unsigned i = 0; i < BK_TESS_STAGES; ++i)
      roots[i] = cmd->tess_batch.stages[i]->start;
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("4096 draws: stage-major", stage_major(r, n, 0, 4096, 0));
   T("4096 draws: 5 barriers", count(r, n, STRONG) == 5 && count(r, n, WEAK) == 0);
   T("4096 draws: linked", pre->stream_linked);
   T("4096 draws: links follow barriers", link_ok);
   T("4096 draws: copied into the linked stream",
     cmd->tess_batch.stages[0]->start == roots[0] &&
        cmd->tess_batch.stages[4]->start == roots[4]);

   /* 16384 draws: side streams of more than one chunk are linked to instead
    * of copied, and become part of the pre-graphics stream. */
   cmd = new_cmd();
   pre = open_pre_gfx(cmd);
   for (unsigned d = 0; d < 16384; ++d)
      record_draw(cmd, d, 0);
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("16384 draws: links follow barriers", link_ok);
   /* A side stream's own chunk links keep the weak barrier before them. */
   unsigned weak = 0, m = 0;
   for (unsigned i = 0; i < n; ++i) {
      if ((r[i] & KIND) == WEAK)
         weak++;
      else
         r[m++] = r[i];
   }
   T("16384 draws: stage-major, weak barriers only at chunk links",
     stage_major(r, m, 0, 16384, 0) && weak && weak < 5 * 4);
   T("16384 draws: 5 flushes", count(r, m, STRONG) == 5);
   T("16384 draws: linked side streams handed over",
     !cmd->tess_batch.stages[0] && !cmd->tess_batch.stages[4]);

   /* A small batch after that is still copied, into the linked stream. */
   const unsigned linked_before = n;
   for (unsigned d = 0; d < 3; ++d)
      record_draw(cmd, d, 0);
   for (unsigned i = 0; i < BK_TESS_STAGES; ++i)
      roots[i] = cmd->tess_batch.stages[i]->start;
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("into a linked stream: copied",
     stage_major(r + linked_before, n - linked_before, 0, 3, 0) && link_ok &&
        cmd->tess_batch.stages[0]->start == roots[0] &&
        cmd->tess_batch.stages[4]->start == roots[4]);

   /* A stage that exactly fills what is left of the pre-graphics chunk is
    * still copied whole, its flush in place, the link to the next chunk
    * after that flush. */
   cmd = new_cmd();
   pre = open_pre_gfx(cmd);
   for (unsigned d = 0; d < 3; ++d)
      record_draw(cmd, d, 0);
   const struct bk_cs *vs0 = cmd->tess_batch.stages[BK_TESS_STAGE_VS];
   pre->end = (uint8_t *)pre->current +
              ((uint8_t *)vs0->current - (uint8_t *)vs0->start) +
              AGX_CDM_STREAM_LINK_LENGTH + BK_CS_SPACE_PADDING;
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("exact fit: flush before the link",
     stage_major(r, n, 0, 3, 0) && count(r, n, WEAK) == 0 && link_ok &&
        pre->stream_linked);

   /* Other pre-graphics work splits the batch: it follows the tessellation
    * of the draws before it and precedes that of the draws after it. */
   cmd = new_cmd();
   pre = open_pre_gfx(cmd);
   record_draw(cmd, 0, 0);
   record_draw(cmd, 1, 0);
   other_work(cmd);
   T("other work flushes the batch", cmd->tess_batch.draws == 0);
   record_draw(cmd, 2, 0);
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   const unsigned first = 2 * BK_TESS_STAGES + BK_TESS_STAGES;
   T("split: first batch", stage_major(r, first, 0, 2, 0));
   T("split: other work after it", n > first && r[first] == OTHER &&
                                      r[first + 1] == STRONG);
   T("split: second batch",
     stage_major(r + first + 2, n - first - 2, 2, 1, 0));

   /* The batch's own fetch of the pre-graphics stream does not flush it. */
   record_draw(cmd, 3, 0);
   cmd->tess_batch.recording = true;
   T("recording: kept", open_pre_gfx(cmd) == pre && cmd->tess_batch.draws == 1);
   cmd->tess_batch.recording = false;

   /* The end of the render splices what is pending. */
   const unsigned before = n;
   bk_cmd_buffer_end_graphics(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("end of render flushes", cmd->current_cs.pre_gfx == NULL &&
                                 cmd->tess_batch.draws == 0 &&
                                 stage_major(r + before, n - before, 3, 1, 0));

   /* Stages without launches add no barrier: no two barriers adjacent. */
   cmd = new_cmd();
   pre = open_pre_gfx(cmd);
   const unsigned skip = BITFIELD_BIT(BK_TESS_STAGE_TCS);
   for (unsigned d = 0; d < 4; ++d)
      record_draw(cmd, d, skip);
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("empty stage: no barrier for it",
     stage_major(r, n, 0, 4, skip) && count(r, n, STRONG) == 4);

   /* A launch that a stage then skips (an empty grid after its tail was
    * dropped) still gets a barrier after it. */
   cmd = new_cmd();
   pre = open_pre_gfx(cmd);
   record_draw(cmd, 0, 0);
   record_draw(cmd, 1, ~BITFIELD_BIT(BK_TESS_STAGE_PREFIX_SUM));
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   const uint32_t skipped[] = {
      LAUNCH | BK_TESS_STAGE_VS, STRONG, LAUNCH | BK_TESS_STAGE_TCS, STRONG,
      LAUNCH | BK_TESS_STAGE_COUNT, STRONG, LAUNCH | BK_TESS_STAGE_PREFIX_SUM,
      LAUNCH | 1 << 4 | BK_TESS_STAGE_PREFIX_SUM, STRONG,
      LAUNCH | BK_TESS_STAGE_TESSELLATE, STRONG,
   };
   T("skipped last launch: flushed",
     n == ARRAY_SIZE(skipped) && !memcmp(r, skipped, sizeof(skipped)));

   /* ... and a chunk link the next draw needs comes after that barrier, which
    * is put back, not right after the launch. */
   cmd = new_cmd();
   pre = open_pre_gfx(cmd);
   record_draw(cmd, 0, 0);
   record_draw(cmd, 1, BITFIELD_BIT(BK_TESS_STAGE_VS));
   struct bk_cs *vs = cmd->tess_batch.stages[BK_TESS_STAGE_VS];
   vs->end = (uint8_t *)vs->current + 0x100;
   record_draw(cmd, 2, 0);
   T("skipped launch, then a link: chunk linked", vs->stream_linked);
   bk_tess_batch_flush(cmd);
   n = decode(pre, r, MAX_RECORDS, &link_ok);
   T("skipped launch, then a link: link after a barrier",
     link_ok && n > 3 && r[0] == (LAUNCH | BK_TESS_STAGE_VS) && r[1] == WEAK &&
        r[2] == (LAUNCH | 2 << 4 | BK_TESS_STAGE_VS) && r[3] == STRONG);

   /* Nothing pending: no records. */
   bk_tess_batch_flush(cmd);
   T("empty flush", decode(pre, r, MAX_RECORDS, &link_ok) == n);

   /* Reset drops unspliced side streams. */
   record_draw(cmd, 2, 0);
   bk_tess_batch_reset(cmd);
   T("reset", cmd->tess_batch.draws == 0 && !cmd->tess_batch.stages[0]);

   /* Which draws may join a batch. */
   struct agx_draw direct = {.b = agx_3d(3, 1, 1)};
   struct agx_draw indirect = {.b = agx_grid_indirect(0x10000)};
   T("batchable: direct", bk_tess_batchable(AGX_CHIP_G17P, direct, false, false));
   T("batchable: not indirect",
     !bk_tess_batchable(AGX_CHIP_G17P, indirect, false, false));
   T("batchable: not with a geometry shader",
     !bk_tess_batchable(AGX_CHIP_G17P, direct, true, false));
   T("batchable: not with queries counting",
     !bk_tess_batchable(AGX_CHIP_G17P, direct, false, true));
   T("batchable: not on other chips",
     !bk_tess_batchable(AGX_CHIP_G13G, direct, false, false));

   for (unsigned i = 0; i < nchunks; ++i)
      free(chunks[i]);
   printf("%u/%u passed\n", tests - failures, tests);
   return failures ? 1 : 0;
}

/* Paths these fixtures never take. */
void
bk_flush_deferred_clears_why(struct bk_cmd_buffer *cmd, const char *why)
{
   (void)cmd;
   (void)why;
   abort();
}

void
bk_reserve_scratch_slow(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                        struct bk_shader *s)
{
   (void)cmd;
   (void)cs;
   (void)s;
   abort();
}

void
bk_sync_check_cs(struct bk_cmd_buffer *cmd)
{
   (void)cmd;
   abort();
}

bool
bk_cs_init_graphics(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

void
bk_sync_stream_created(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                       struct bk_cs **slot, bool compute)
{
   (void)cmd;
   (void)cs;
   (void)slot;
   (void)compute;
}

void
bk_flush_query_resets(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

void
bk_sync_note_xfer_write(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

void
bk_dispatch_imm_writes(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

bool
bk_defer_oq_available(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

void
bk_sync_forget_stream(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

/* These devices never map memory at a second address, so the real
 * function adds no flush either.
 */
void
bk_cdm_end_stream_flush(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
}

bool
bk_optimize_empty_vdm(struct bk_cmd_buffer *cmd)
{
   (void)cmd;
   abort();
}

void *
bk_cmd_buffer_emit_vdm_terminate(struct bk_cmd_buffer *cmd, void *current)
{
   (void)cmd;
   (void)current;
   abort();
}

uint64_t
bk_pool_upload(struct bk_cmd_buffer *cmd, const void *data, uint32_t size,
               uint32_t alignment)
{
   (void)cmd;
   (void)data;
   (void)size;
   (void)alignment;
   abort();
}

uint64_t
bk_pool_usc_upload(struct bk_cmd_buffer *cmd, const void *data, uint32_t size,
                   uint32_t alignment)
{
   (void)cmd;
   (void)data;
   (void)size;
   (void)alignment;
   abort();
}

/* Only bk_tess_batch_open() sets an error, on allocation failure. */
VkResult
__vk_errorf(const void *obj, VkResult error, const char *file, int line,
            const char *format, ...)
{
   (void)obj;
   (void)file;
   (void)line;
   (void)format;
   abort();
   return error;
}
