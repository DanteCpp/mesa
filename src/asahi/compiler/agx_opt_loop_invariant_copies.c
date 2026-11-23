/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "util/bitset.h"
#include "util/sparse_bitset.h"
#include "agx_builder.h"
#include "agx_compiler.h"

/*
 * Hoist loop-invariant immediate moves and uniform-file copies out of loops.
 *
 * Backend lowering materializes an immediate an instruction cannot encode,
 * or a uniform it cannot read, right before that instruction. Inside a loop
 * each one is redone on every iteration, and the same value is often
 * materialized again for the next instruction that needs it. NIR's own loop
 * code motion runs before these copies exist.
 *
 * A copy of an immediate, or of a uniform the program never writes, has the
 * same value wherever it runs. Moving one to the end of the loop's
 * preheader, and replacing identical copies in the loop with it, leaves
 * every use with the same value, and the preheader dominates the loop.
 *
 * The hoisted value stays live across the whole loop. Only hoist into the
 * register headroom the loop already has below the program's peak pressure,
 * so the program's register demand does not grow. Copies used most often go
 * first. Inner loops are visited first; what they hoist into an enclosing
 * loop can then leave that loop too.
 *
 * A program whose peak comes within two register-file alignment units
 * (register allocation rounds demand to eight halves) of the register file
 * is left alone: there allocation splits, rematerializes and spills, and a
 * longer live range costs more instructions than the copies it saves.
 */

#define LICM_FILE_MARGIN_HALVES (2 * 8)

static unsigned
register_file_halves(const agx_context *ctx)
{
   return ctx->key->dev.encoding == AGX_ISA_ENCODING_AGX3B ? AGX3B_NUM_REGS
                                                           : AGX_NUM_REGS;
}

struct licm_loop {
   agx_block *header, *preheader;
   unsigned first, last; /* block indices, inclusive */
};

static bool
writes_uniforms(agx_context *ctx)
{
   agx_foreach_instr_global(ctx, I) {
      if (I->op == AGX_OPCODE_UNIFORM_STORE ||
          !strncmp(agx_opcodes_info[I->op].name, "uniform_", 8))
         return true;
   }
   return false;
}

static bool
is_candidate(const agx_instr *I, bool uniforms_constant)
{
   if (I->nr_dests != 1 || I->dest[0].type != AGX_INDEX_NORMAL)
      return false;

   if (I->op == AGX_OPCODE_MOV_IMM)
      return I->nr_srcs == 0;

   return uniforms_constant && I->op == AGX_OPCODE_MOV && I->nr_srcs == 1 &&
          I->src[0].type == AGX_INDEX_UNIFORM && !I->src[0].memory;
}

static bool
same_value(const agx_instr *A, const agx_instr *B)
{
   if (A->op != B->op || A->dest[0].size != B->dest[0].size ||
       A->dest[0].channels_m1 != B->dest[0].channels_m1)
      return false;

   if (A->op == AGX_OPCODE_MOV_IMM)
      return A->imm == B->imm;

   const agx_index a = A->src[0], b = B->src[0];
   return a.value == b.value && a.size == b.size &&
          a.channels_m1 == b.channels_m1 && a.abs == b.abs && a.neg == b.neg;
}

/* Peak register pressure in halves over a block, from its live-out set. */
static unsigned
block_peak(agx_block *block, const uint8_t *size16, BITSET_WORD *live,
           unsigned nr_words)
{
   memset(live, 0, nr_words * sizeof(BITSET_WORD));
   unsigned pressure = 0;
   U_SPARSE_BITSET_FOREACH_SET(&block->live_out, v) {
      BITSET_SET(live, v);
      pressure += size16[v];
   }

   unsigned peak = pressure;
   agx_foreach_instr_in_block_rev(block, I) {
      agx_foreach_ssa_dest(I, d) {
         const unsigned v = I->dest[d].value;
         if (BITSET_TEST(live, v)) {
            BITSET_CLEAR(live, v);
            pressure -= size16[v];
         }
      }

      /* Phi sources are live out of the predecessors, not here. */
      if (I->op == AGX_OPCODE_PHI)
         continue;

      agx_foreach_ssa_src(I, s) {
         const unsigned v = I->src[s].value;
         if (!BITSET_TEST(live, v)) {
            BITSET_SET(live, v);
            pressure += size16[v];
         }
      }
      peak = MAX2(peak, pressure);
   }
   return peak;
}

static unsigned
live_out_size(agx_block *block, const uint8_t *size16)
{
   unsigned size = 0;
   U_SPARSE_BITSET_FOREACH_SET(&block->live_out, v)
      size += size16[v];
   return size;
}

/* Point every use of a removed copy at the copy that replaced it. */
static void
rewrite_uses(agx_context *ctx, const uint32_t *replace)
{
   agx_foreach_instr_global(ctx, I) {
      agx_foreach_ssa_src(I, s) {
         uint32_t v = I->src[s].value;
         while (replace[v])
            v = replace[v] - 1;
         I->src[s].value = v;
      }
   }
}

/* Hoist from one loop. Returns whether anything moved. */
static bool
hoist_loop(agx_context *ctx, const struct licm_loop *loop, agx_block **blocks,
           unsigned cap, bool uniforms_constant, uint32_t *replace)
{
   agx_compute_liveness(ctx);

   uint8_t *size16 = calloc(ctx->alloc, sizeof(uint8_t));
   const unsigned nr_words = BITSET_WORDS(ctx->alloc);
   BITSET_WORD *live = calloc(nr_words, sizeof(BITSET_WORD));
   if (!size16 || !live) {
      free(size16);
      free(live);
      return false;
   }

   agx_foreach_instr_global(ctx, I) {
      agx_foreach_ssa_dest(I, d)
         size16[I->dest[d].value] = agx_index_size_16(I->dest[d]);
   }

   unsigned peak = live_out_size(loop->preheader, size16);
   for (unsigned i = loop->first; i <= loop->last; ++i)
      peak = MAX2(peak, block_peak(blocks[i], size16, live, nr_words));
   free(live);

   unsigned headroom = cap > peak ? cap - peak : 0;

   /* Group identical copies in the loop, counting uses of each group. */
   struct group {
      agx_instr *rep;
      unsigned count;
   };
   struct util_dynarray groups;
   util_dynarray_init(&groups, NULL);

   for (unsigned i = loop->first; i <= loop->last; ++i) {
      agx_foreach_instr_in_block(blocks[i], I) {
         if (!is_candidate(I, uniforms_constant))
            continue;

         bool found = false;
         util_dynarray_foreach(&groups, struct group, g) {
            if (same_value(g->rep, I)) {
               g->count++;
               found = true;
               break;
            }
         }
         if (!found) {
            struct group g = {.rep = I, .count = 1};
            util_dynarray_append(&groups, g);
         }
      }
   }

   bool progress = false;
   agx_builder b = agx_init_builder(ctx, agx_after_block_logical(loop->preheader));

   for (;;) {
      /* The most-used group that still fits. */
      struct group *best = NULL;
      util_dynarray_foreach(&groups, struct group, g) {
         if (g->rep && size16[g->rep->dest[0].value] <= headroom &&
             (!best || g->count > best->count))
            best = g;
      }
      if (!best)
         break;

      agx_instr *rep = best->rep;
      headroom -= size16[rep->dest[0].value];

      for (unsigned i = loop->first; i <= loop->last; ++i) {
         agx_foreach_instr_in_block_safe(blocks[i], I) {
            if (I == rep || !is_candidate(I, uniforms_constant) ||
                !same_value(rep, I))
               continue;
            replace[I->dest[0].value] = rep->dest[0].value + 1;
            agx_remove_instruction(I);
         }
      }

      agx_remove_instruction(rep);
      agx_builder_insert(&b.cursor, rep);
      best->rep = NULL;
      progress = true;
   }

   util_dynarray_fini(&groups);
   free(size16);
   return progress;
}

bool
agx_opt_loop_invariant_copies(agx_context *ctx)
{
   unsigned nr_blocks = 0;
   agx_foreach_block(ctx, block)
      nr_blocks = MAX2(nr_blocks, block->index + 1);
   if (!nr_blocks)
      return false;

   agx_block **blocks = calloc(nr_blocks, sizeof(*blocks));
   struct licm_loop *loops = calloc(nr_blocks, sizeof(*loops));
   if (!blocks || !loops) {
      free(blocks);
      free(loops);
      return false;
   }
   agx_foreach_block(ctx, block)
      blocks[block->index] = block;

   /* A loop spans the source-order interval from its header to the block
    * holding its back edge, entered from the one predecessor before it. */
   unsigned nr_loops = 0;
   agx_foreach_block(ctx, header) {
      if (!header->loop_header)
         continue;

      agx_block *preheader = NULL;
      unsigned last = header->index, outside = 0;
      bool back_edge = false;
      agx_foreach_predecessor(header, pred) {
         if ((*pred)->index >= header->index) {
            last = MAX2(last, (*pred)->index);
            back_edge = true;
         } else {
            preheader = *pred;
            outside++;
         }
      }
      if (outside != 1 || !back_edge)
         continue;

      loops[nr_loops++] = (struct licm_loop){
         .header = header, .preheader = preheader,
         .first = header->index, .last = last,
      };
   }

   /* Innermost first: a nested loop is a shorter interval. */
   for (unsigned i = 1; i < nr_loops; ++i) {
      struct licm_loop l = loops[i];
      unsigned j = i;
      for (; j > 0 && loops[j - 1].last - loops[j - 1].first > l.last - l.first;
           --j)
         loops[j] = loops[j - 1];
      loops[j] = l;
   }

   bool progress = false;
   if (nr_loops) {
      const bool uniforms_constant = !writes_uniforms(ctx);
      uint32_t *replace = calloc(ctx->alloc, sizeof(uint32_t));

      /* The program's peak pressure before hoisting caps every loop. */
      unsigned cap = 0;
      if (replace) {
         agx_compute_liveness(ctx);
         uint8_t *size16 = calloc(ctx->alloc, sizeof(uint8_t));
         const unsigned nr_words = BITSET_WORDS(ctx->alloc);
         BITSET_WORD *live = calloc(nr_words, sizeof(BITSET_WORD));
         if (size16 && live) {
            agx_foreach_instr_global(ctx, I) {
               agx_foreach_ssa_dest(I, d)
                  size16[I->dest[d].value] = agx_index_size_16(I->dest[d]);
            }
            agx_foreach_block(ctx, block)
               cap = MAX2(cap, block_peak(block, size16, live, nr_words));
         }
         free(size16);
         free(live);
      }

      if (cap + LICM_FILE_MARGIN_HALVES > register_file_halves(ctx))
         cap = 0;

      /* Each loop's liveness must see the copies the previous one moved. */
      for (unsigned i = 0; replace && cap && i < nr_loops; ++i) {
         if (hoist_loop(ctx, &loops[i], blocks, cap, uniforms_constant,
                        replace)) {
            rewrite_uses(ctx, replace);
            progress = true;
         }
      }
      free(replace);

      /* Liveness marked last uses; later passes compute their own. */
      agx_foreach_instr_global(ctx, I) {
         agx_foreach_ssa_src(I, s)
            I->src[s].kill = false;
      }
   }

   free(blocks);
   free(loops);
   return progress;
}
