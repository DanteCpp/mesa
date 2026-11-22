/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "util/sparse_bitset.h"
#include "agx_nir.h"
#include "compiler/nir/nir.h"

/*
 * Loop-invariant code motion that keeps register demand in check.
 *
 * nir_opt_licm hoists every invariant instruction of a loop, and each hoisted
 * result that the loop still reads stays live across the whole loop. Shaders
 * that address hundreds of memory locations from one loop-invariant base, as
 * DXVK compute shaders do for their shared-memory taps, then carry hundreds of
 * hoisted addresses through a loop that already fills the register file, and
 * fill them from spill slots on every iteration, which costs more than the
 * additions hoisting saved. Estimate each loop's peak register demand and the
 * demand hoisting would add, and only hoist if every loop still fits.
 *
 * The estimate counts the 16-bit halves of SSA values live at each point,
 * leaving out constants and preamble values, which immediates and uniforms
 * hold rather than registers.
 */

struct licm_estimate {
   uint8_t *halves;
   BITSET_WORD *invariant;
   void *mem_ctx;

   struct u_sparse_bitset live;
   unsigned live_halves;
};

static unsigned
def_halves(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);

   if (instr->type == nir_instr_type_load_const ||
       instr->type == nir_instr_type_undef ||
       (instr->type == nir_instr_type_intrinsic &&
        nir_instr_as_intrinsic(instr)->intrinsic ==
           nir_intrinsic_load_preamble))
      return 0;

   return def->num_components * DIV_ROUND_UP(def->bit_size, 16);
}

static bool
record_halves(nir_def *def, void *data)
{
   struct licm_estimate *est = data;
   est->halves[def->index] = MIN2(def_halves(def), UINT8_MAX);
   return true;
}

static void
set_live(struct licm_estimate *est, unsigned index)
{
   if (!u_sparse_bitset_test(&est->live, index)) {
      u_sparse_bitset_set(&est->live, index);
      est->live_halves += est->halves[index];
   }
}

static bool
kill_def(nir_def *def, void *data)
{
   struct licm_estimate *est = data;

   if (u_sparse_bitset_test(&est->live, def->index)) {
      u_sparse_bitset_clear(&est->live, def->index);
      est->live_halves -= est->halves[def->index];
   }

   return true;
}

static bool
gen_src(nir_src *src, void *data)
{
   set_live(data, src->ssa->index);
   return true;
}

/* The most halves live at any point of the loop */
static unsigned
loop_peak(struct licm_estimate *est, nir_loop *loop)
{
   unsigned peak = 0;

   nir_foreach_block_in_cf_node(block, &loop->cf_node) {
      u_sparse_bitset_dup_with_ctx(&est->live, &block->live_out, est->mem_ctx);
      est->live_halves = 0;
      U_SPARSE_BITSET_FOREACH_SET(&est->live, i)
         est->live_halves += est->halves[i];

      nir_if *following_if = nir_block_get_following_if(block);
      if (following_if)
         gen_src(&following_if->condition, est);

      peak = MAX2(peak, est->live_halves);

      nir_foreach_instr_reverse(instr, block) {
         if (instr->type == nir_instr_type_phi)
            break;

         /* Results are written while the sources are still read */
         nir_foreach_src(instr, gen_src, est);
         peak = MAX2(peak, est->live_halves);
         nir_foreach_def(instr, kill_def, est);
      }
   }

   return peak;
}

struct invariance {
   struct licm_estimate *est;
   nir_loop *loop;
};

static bool
src_invariant(nir_src *src, void *data)
{
   struct invariance *inv = data;
   return nir_def_block(src->ssa)->index <=
             nir_loop_predecessor_block(inv->loop)->index ||
          BITSET_TEST(inv->est->invariant, src->ssa->index);
}

/* What nir_opt_licm without a filter hoists out of the loop */
static bool
instr_invariant(nir_instr *instr, struct invariance *inv)
{
   switch (instr->type) {
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
      return true;

   case nir_instr_type_intrinsic:
      if (!nir_intrinsic_can_reorder(nir_instr_as_intrinsic(instr)))
         return false;
      FALLTHROUGH;

   case nir_instr_type_alu:
   case nir_instr_type_tex:
   case nir_instr_type_deref:
      return nir_foreach_src(instr, src_invariant, inv);

   default:
      return false;
   }
}

/* Pure, side-effect-free instructions safe to hoist speculatively out of
 * loops whose body follows a header exit test (e.g. `if (i >= n) break;`).
 */
static bool
instr_speculatable(nir_instr *instr, nir_loop *loop)
{
   /* Only speculatively hoist from top-level loop blocks (e.g. loop body
    * following a header break), not from inside conditional branches.
    */
   if (instr->block->cf_node.parent != &loop->cf_node)
      return false;

   if (instr->type == nir_instr_type_intrinsic)
      return nir_instr_as_intrinsic(instr)->intrinsic ==
             nir_intrinsic_load_preamble;

   if (instr->type != nir_instr_type_alu)
      return false;

   switch (nir_instr_as_alu(instr)->op) {
   case nir_op_idiv:
   case nir_op_udiv:
   case nir_op_imod:
   case nir_op_umod:
   case nir_op_irem:
      return false;
   default:
      return true;
   }
}

static bool
agx_licm_filter(nir_instr *instr, nir_loop *loop, bool block_dominates_exit)
{
   return block_dominates_exit || instr_speculatable(instr, loop);
}

static bool
loop_hoists(nir_loop *loop, bool *has_terminate)
{
   *has_terminate = false;

   if (!nir_loop_has_back_edge(loop))
      return false;

   nir_foreach_block_in_cf_node(block, &loop->cf_node) {
      if (nir_block_ends_in_return_or_halt(block))
         return false;

      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic &&
             (nir_instr_as_intrinsic(instr)->intrinsic ==
                 nir_intrinsic_terminate ||
              nir_instr_as_intrinsic(instr)->intrinsic ==
                 nir_intrinsic_terminate_if))
            *has_terminate = true;
      }
   }

   return true;
}

static bool
block_in_loop(nir_block *block, nir_loop *loop)
{
   for (nir_cf_node *node = block->cf_node.parent; node; node = node->parent) {
      if (node == &loop->cf_node)
         return true;
   }

   return false;
}

/* Whether a use keeps a hoisted result live in the loop */
static bool
use_stays_in_loop(struct licm_estimate *est, nir_src *use, nir_loop *loop)
{
   if (!block_in_loop(nir_src_get_block(use), loop))
      return false;

   if (nir_src_is_if(use))
      return true;

   nir_def *user = nir_instr_def(nir_src_use_instr(use));
   return !user || !BITSET_TEST(est->invariant, user->index);
}

/* Halves of hoisted results that instructions staying in the loop read */
static unsigned
loop_hoist_cost(struct licm_estimate *est, unsigned ssa_alloc, nir_loop *loop,
                bool has_terminate, bool speculative)
{
   struct invariance inv = {est, loop};
   nir_block *exit = nir_loop_successor_block(loop);
   unsigned cost = 0;

   memset(est->invariant, 0, BITSET_BYTES(ssa_alloc));

   nir_foreach_block_in_cf_node(block, &loop->cf_node) {
      bool dom_exit = !has_terminate && nir_block_dominates(block, exit);

      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (def && instr_invariant(instr, &inv) &&
             (dom_exit || (speculative && instr_speculatable(instr, loop))))
            BITSET_SET(est->invariant, def->index);
      }
   }

   nir_foreach_block_in_cf_node(block, &loop->cf_node) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         if (!def || !BITSET_TEST(est->invariant, def->index))
            continue;

         nir_foreach_use_including_if(use, def) {
            if (use_stays_in_loop(est, use, loop)) {
               cost += est->halves[def->index];
               break;
            }
         }
      }
   }

   return cost;
}

static bool
loops_fit(struct licm_estimate *est, unsigned ssa_alloc,
          struct exec_list *list, unsigned budget, bool speculative)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         if (!loops_fit(est, ssa_alloc, &nif->then_list, budget, speculative) ||
             !loops_fit(est, ssa_alloc, &nif->else_list, budget, speculative))
            return false;
         break;
      }

      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(node);
         bool has_terminate = false;
         if (loop_hoists(loop, &has_terminate) && (!has_terminate || speculative)) {
            unsigned cost =
               loop_hoist_cost(est, ssa_alloc, loop, has_terminate, speculative);
            if (cost && loop_peak(est, loop) + cost > budget)
               return false;
         }

         if (!loops_fit(est, ssa_alloc, &loop->body, budget, speculative))
            return false;
         break;
      }

      default:
         break;
      }
   }

   return true;
}

/*
 * Run nir_opt_licm if no loop's estimated register demand would exceed budget
 * halves with the invariants hoisted.
 */
bool
agx_nir_opt_licm(nir_shader *shader, unsigned budget, bool speculative)
{
   nir_foreach_function_impl(impl, shader) {
      nir_metadata_require(impl, nir_metadata_block_index |
                                    nir_metadata_dominance |
                                    nir_metadata_live_defs);

      void *mem_ctx = ralloc_context(NULL);
      struct licm_estimate est = {
         .halves = rzalloc_array(mem_ctx, uint8_t, impl->ssa_alloc),
         .invariant =
            rzalloc_array(mem_ctx, BITSET_WORD, BITSET_WORDS(impl->ssa_alloc)),
         .mem_ctx = mem_ctx,
      };

      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block)
            nir_foreach_def(instr, record_halves, &est);
      }

      bool fits = loops_fit(&est, impl->ssa_alloc, &impl->body, budget, speculative);
      ralloc_free(mem_ctx);

      if (!fits)
         return false;
   }

   return nir_opt_licm(shader, speculative ? agx_licm_filter : NULL);
}
