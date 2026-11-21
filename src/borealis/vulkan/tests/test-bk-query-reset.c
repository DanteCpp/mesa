/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "bk_cmd_buffer.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned flushes, reservations;

/* Exercise the production drain hook with command emission replaced by a
 * recorder. No device or BO allocation is needed to close an existing stream.
 */
void
bk_ensure_cs_has_space(struct bk_cmd_buffer *cmd, struct bk_cs *cs, size_t size)
{
   assert(cmd == cs->cmd);
   assert(size == AGX3_CDM_BARRIER_SIZE);
   reservations++;
}

void
bk_cdm_cache_flush(struct bk_device *dev, struct bk_cs *cs)
{
   (void)dev;
   assert(cs->query_reset_pending);
   assert(reservations == flushes + 1);
   flushes++;
}

/* All fixtures reuse a nonempty, already allocated general stream. These
 * paths must not allocate, change dependencies, discard a stream or emit
 * unrelated work while draining query resets. */
void
bk_flush_deferred_clears_why(struct bk_cmd_buffer *cmd, const char *why)
{
   (void)cmd;
   (void)why;
   abort();
}

void
bk_sync_check_cs(struct bk_cmd_buffer *cmd)
{
   (void)cmd;
   abort();
}

struct agx_ptr
bk_pool_cs_alloc(struct bk_cmd_buffer *cmd, uint32_t size, uint32_t alignment)
{
   (void)cmd;
   (void)size;
   (void)alignment;
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
bk_drm_attachment_set_init(struct bk_drm_attachment_set *set)
{
   (void)set;
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

/* No tessellation is batched here. */
void
bk_tess_batch_flush(struct bk_cmd_buffer *cmd)
{
   (void)cmd;
   abort();
}

/* Query resets are not transfers. */
void
bk_sync_note_xfer_write(struct bk_cmd_buffer *cmd, struct bk_cs *cs)
{
   (void)cmd;
   (void)cs;
   abort();
}

int
main(void)
{
   struct bk_cmd_buffer *cmd = calloc(1, sizeof(*cmd));
   assert(cmd);
   struct bk_cs cs = {.cmd = cmd, .type = BK_CS_CDM};

   bk_flush_query_resets(cmd, &cs);
   assert(flushes == 0 && reservations == 0);

   /* The real getter defers drains during consecutive reset recording. */
   cs.query_reset_pending = true;
   cs.stats.query_reset_dispatches = 3;
   cs.stats.cmds = 3;
   cs.sync.kind = BK_SYNC_ALL;
   cmd->current_cs.cs = &cs;
   cmd->state.query_reset_recording = true;
   assert(bk_cmd_buffer_get_cs(cmd, true) == &cs);
   assert(cs.query_reset_pending && flushes == 0);

   /* The next consumer goes through that same getter and drains once. */
   cmd->state.query_reset_recording = false;
   assert(bk_cmd_buffer_get_cs(cmd, true) == &cs);
   assert(!cs.query_reset_pending);
   assert(flushes == 1 && cs.stats.query_reset_flushes == 1);
   assert(bk_cmd_buffer_get_cs(cmd, true) == &cs);
   assert(flushes == 1);

   /* Closing a stream also drains, even inside reset recording. */
   cmd->state.query_reset_recording = true;
   cs.query_reset_pending = true;
   bk_cmd_buffer_end_compute(cmd);
   assert(!cs.query_reset_pending && cmd->current_cs.cs == NULL);
   assert(flushes == 2 && cs.stats.query_reset_flushes == 2);
   assert(cs.stats.query_reset_dispatches == 3);
   bk_cmd_buffer_end_compute(cmd);
   assert(flushes == 2);
   free(cmd);
   puts(
      "query reset drains: empty, reset recording, consumer and stream end passed");
   return 0;
}
