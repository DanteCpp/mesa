/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

#include "agx_nir.h"
#include "nir.h"
#include "nir_builder.h"
#include "util/u_dynarray.h"

/*
 * General NIR optimizations for 2D image/vision compute idioms:
 *
 * Early (before nir_opt_reassociate_loop / preamble formation) and late (after
 * peephole-select / algebraic passes that may newly expose them):
 *   1. Loop-invariant boolean disjunction reassociation inside loops:
 *      reorders `ior` chains (and `bcsel(a, true, b)` equivalents) so >= 2
 *      loop-invariant predicates combine first and hoist cleanly out of the
 *      inner loop.
 *   2. Two-sided threshold ternary zero-check simplification + sinking:
 *      simplifies `(tri(v1, lo, hi) == 0) && (tri(v2, lo, hi) == 0)` (in
 *      `umax == 0`, `ior == 0`, or `iand(ieq, ieq)` form) into a single range
 *      test `!(ilt(imin(v1, v2), lo) || ilt(hi, imax(v1, v2)))`, and sinks
 *      `tri(v1)` / `tri(v2)` into the dominated block where they are actually
 *      consumed (e.g., past an early-exit branch).
 *   3. Narrow 32-bit signed comparisons against zero-extended 8-bit values to
 *      16-bit signed comparisons when the 32-bit bound is shared across >= 2
 *      comparisons (clamping the bound to [-1, 256] once).
 *
 * Late (after agx_nir_opt_forward_merge_loads):
 *   4. Consecutive side-effect-only guard-`if` coalescing:
 *      wraps >= 3 consecutive empty-else, phi-free, barrier/jump-free
 *      `if (c_k) { ... }` blocks behind a single outer `if (c_0 | ... | c_n)`.
 */

static nir_def *
strip_mov_def(nir_def *d)
{
   while (d && nir_def_instr(d)->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(d));
      if (alu->op == nir_op_mov && alu->src[0].src.ssa->num_components == 1) {
         d = alu->src[0].src.ssa;
         continue;
      }
      break;
   }
   return d;
}

static bool
def_dominates_block(nir_def *def, nir_block *blk)
{
   return nir_def_instr(def)->block != blk &&
          nir_block_dominates(nir_def_instr(def)->block, blk);
}

static unsigned
block_loop_depth(nir_block *blk)
{
   unsigned depth = 0;
   for (nir_cf_node *n = blk->cf_node.parent; n; n = n->parent) {
      if (n->type == nir_cf_node_loop)
         depth++;
   }
   return depth;
}

static bool
def_sole_use_is_instr(nir_def *def, nir_instr *user)
{
   unsigned count = 0;
   nir_foreach_use_including_if(use, def) {
      if (nir_src_is_if(use) || nir_src_use_instr(use) != user)
         return false;
      count++;
   }
   return count == 1;
}

/* ---------------------------------------------------------------------------
 * Opt 1: Loop-invariant boolean disjunction reassociation
 * --------------------------------------------------------------------------- */

static void
collect_ior_terms(nir_def *d, nir_block *loop_header,
                  struct util_dynarray *inv, struct util_dynarray *var,
                  unsigned depth)
{
   if (def_dominates_block(d, loop_header)) {
      util_dynarray_append(inv, d);
      return;
   }
   if (depth > 16) {
      util_dynarray_append(var, d);
      return;
   }

   if (nir_def_instr(d)->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(d));
      if (alu->op == nir_op_ior && d->bit_size == 1 && d->num_components == 1) {
         collect_ior_terms(alu->src[0].src.ssa, loop_header, inv, var,
                           depth + 1);
         collect_ior_terms(alu->src[1].src.ssa, loop_header, inv, var,
                           depth + 1);
         return;
      }
      if (alu->op == nir_op_bcsel && d->bit_size == 1 &&
          d->num_components == 1 && nir_src_is_const(alu->src[1].src) &&
          nir_src_as_bool(alu->src[1].src)) {
         collect_ior_terms(alu->src[0].src.ssa, loop_header, inv, var,
                           depth + 1);
         collect_ior_terms(alu->src[2].src.ssa, loop_header, inv, var,
                           depth + 1);
         return;
      }
   }

   util_dynarray_append(var, d);
}

static bool
is_bool_ior_alu(nir_alu_instr *alu)
{
   if (alu->def.bit_size != 1 || alu->def.num_components != 1)
      return false;
   return alu->op == nir_op_ior ||
          (alu->op == nir_op_bcsel && nir_src_is_const(alu->src[1].src) &&
           nir_src_as_bool(alu->src[1].src));
}

static bool
is_root_bool_ior(nir_alu_instr *alu)
{
   if (!is_bool_ior_alu(alu))
      return false;

   /* Only trigger at the root of an ior/bcsel-or chain so we don't rewrite
    * sub-nodes. */
   nir_foreach_use(use, &alu->def) {
      nir_instr *parent = nir_src_use_instr(use);
      if (parent->type == nir_instr_type_alu &&
          is_bool_ior_alu(nir_instr_as_alu(parent)))
         return false;
   }
   return true;
}

static nir_def *
build_ior_tree(nir_builder *b, nir_def *const *defs, unsigned count)
{
   assert(count > 0);
   nir_def *acc = defs[0];
   for (unsigned i = 1; i < count; i++)
      acc = nir_ior(b, acc, defs[i]);
   return acc;
}

static bool
reassoc_loop_invariant_ior_in_loop(nir_builder *b, nir_loop *loop)
{
   bool progress = false;
   nir_block *header = nir_loop_first_block(loop);
   nir_block *preheader =
      nir_cf_node_as_block(nir_cf_node_prev(&loop->cf_node));

   nir_foreach_block_in_cf_node(block, &loop->cf_node) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         if (!is_root_bool_ior(alu))
            continue;

         /* Skip if already split cleanly as ior(inv_only, var_only). */
         if (alu->op == nir_op_ior &&
             (def_dominates_block(alu->src[0].src.ssa, header) ||
              def_dominates_block(alu->src[1].src.ssa, header))) {
            nir_def *other = def_dominates_block(alu->src[0].src.ssa, header)
                                ? alu->src[1].src.ssa
                                : alu->src[0].src.ssa;
            struct util_dynarray sub_inv = UTIL_DYNARRAY_INIT;
            struct util_dynarray sub_var = UTIL_DYNARRAY_INIT;
            collect_ior_terms(other, header, &sub_inv, &sub_var, 0);
            unsigned sub_inv_n = util_dynarray_num_elements(&sub_inv, nir_def *);
            util_dynarray_fini(&sub_inv);
            util_dynarray_fini(&sub_var);
            if (sub_inv_n == 0)
               continue;
         }

         struct util_dynarray inv = UTIL_DYNARRAY_INIT;
         struct util_dynarray var = UTIL_DYNARRAY_INIT;
         collect_ior_terms(&alu->def, header, &inv, &var, 0);

         unsigned n_inv = util_dynarray_num_elements(&inv, nir_def *);
         unsigned n_var = util_dynarray_num_elements(&var, nir_def *);
         if (n_inv >= 2 && n_var >= 1) {
            /* Emit the combined invariant predicate directly in the preheader
             * so it is hoisted immediately before reassociation/LICM. */
            b->cursor = nir_after_block_before_jump(preheader);
            nir_def *inv_or =
               build_ior_tree(b, (nir_def *const *)inv.data, n_inv);

            b->cursor = nir_before_instr(instr);
            nir_def *var_or =
               build_ior_tree(b, (nir_def *const *)var.data, n_var);
            nir_def *combined = nir_ior(b, inv_or, var_or);
            nir_def_rewrite_uses(&alu->def, combined);
            progress = true;
         }

         util_dynarray_fini(&inv);
         util_dynarray_fini(&var);
      }
   }

   return progress;
}

static bool
opt_reassoc_loop_ior_cf_list(nir_builder *b, struct exec_list *cf_list)
{
   bool progress = false;
   foreach_list_typed(nir_cf_node, node, node, cf_list) {
      if (node->type == nir_cf_node_loop) {
         nir_loop *loop = nir_cf_node_as_loop(node);
         /* Process inner loops first so innermost loops hoist first. */
         progress |= opt_reassoc_loop_ior_cf_list(b, &loop->body);
         progress |= reassoc_loop_invariant_ior_in_loop(b, loop);
      } else if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);
         progress |= opt_reassoc_loop_ior_cf_list(b, &nif->then_list);
         progress |= opt_reassoc_loop_ior_cf_list(b, &nif->else_list);
      }
   }
   return progress;
}

/* ---------------------------------------------------------------------------
 * Opt 2: Two-sided threshold ternary zero-check simplification + sinking
 * --------------------------------------------------------------------------- */

struct tri_match {
   nir_def *v;
   nir_def *lo;
   nir_def *hi;
   nir_alu_instr *outer_sel;
   nir_alu_instr *inner_sel;
};

static bool
match_signed_lt(nir_def *cond, nir_def **lhs_out, nir_def **rhs_out)
{
   cond = strip_mov_def(cond);
   if (!cond || nir_def_instr(cond)->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(cond));
   unsigned bits = alu->src[0].src.ssa->bit_size;
   if (alu->op == nir_op_ilt && (bits == 16 || bits == 32)) {
      *lhs_out = strip_mov_def(alu->src[0].src.ssa);
      *rhs_out = strip_mov_def(alu->src[1].src.ssa);
      return true;
   }
   return false;
}

/* Match `tri(v, lo, hi)`:
 *   bcsel(v < lo, -1, bcsel(hi < v, 1, 0))
 * or
 *   bcsel(hi < v, 1, bcsel(v < lo, -1, 0))
 * (also accepting +1 / -1 swapped since both are non-zero). */
static bool
match_tri_expr(nir_def *d, struct tri_match *m)
{
   d = strip_mov_def(d);
   if (!d || d->bit_size != 32 || d->num_components != 1 ||
       nir_def_instr(d)->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *outer = nir_instr_as_alu(nir_def_instr(d));
   if (outer->op != nir_op_bcsel)
      return false;

   /* Outer selects between non-zero const and inner bcsel. */
   nir_def *c_outer = outer->src[0].src.ssa;
   nir_def *t_outer = outer->src[1].src.ssa;
   nir_def *e_outer = outer->src[2].src.ssa;

   if (!nir_src_is_const(nir_src_for_ssa(t_outer)))
      return false;
   int32_t k1 = (int32_t)nir_src_as_int(nir_src_for_ssa(t_outer));
   if (k1 == 0)
      return false;

   e_outer = strip_mov_def(e_outer);
   if (!e_outer || nir_def_instr(e_outer)->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *inner = nir_instr_as_alu(nir_def_instr(e_outer));
   if (inner->op != nir_op_bcsel)
      return false;

   nir_def *c_inner = inner->src[0].src.ssa;
   nir_def *t_inner = inner->src[1].src.ssa;
   nir_def *e_inner = inner->src[2].src.ssa;

   if (!nir_src_is_const(nir_src_for_ssa(t_inner)) ||
       !nir_src_is_const(nir_src_for_ssa(e_inner)))
      return false;
   int32_t k2 = (int32_t)nir_src_as_int(nir_src_for_ssa(t_inner));
   int32_t k0 = (int32_t)nir_src_as_int(nir_src_for_ssa(e_inner));
   if (k2 == 0 || k0 != 0)
      return false;

   nir_def *a1 = NULL, *b1 = NULL, *a2 = NULL, *b2 = NULL;
   if (!match_signed_lt(c_outer, &a1, &b1) ||
       !match_signed_lt(c_inner, &a2, &b2))
      return false;

   /* Case 1: c_outer is (v < lo), c_inner is (hi < v) -> a1 == b2 == v. */
   if (a1 == b2) {
      m->v = a1;
      m->lo = b1;
      m->hi = a2;
      m->outer_sel = outer;
      m->inner_sel = inner;
      return true;
   }
   /* Case 2: c_outer is (hi < v), c_inner is (v < lo) -> b1 == a2 == v. */
   if (b1 == a2) {
      m->v = b1;
      m->lo = b2;
      m->hi = a1;
      m->outer_sel = outer;
      m->inner_sel = inner;
      return true;
   }

   return false;
}

static bool
match_tri_eq_zero(nir_def *cond, struct tri_match *m, bool *is_ne_out)
{
   cond = strip_mov_def(cond);
   if (!cond || nir_def_instr(cond)->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(cond));
   if (alu->op != nir_op_ieq && alu->op != nir_op_ine)
      return false;

   for (unsigned s = 0; s < 2; s++) {
      if (nir_src_is_const(alu->src[1 - s].src) &&
          nir_src_as_uint(alu->src[1 - s].src) == 0 &&
          match_tri_expr(alu->src[s].src.ssa, m)) {
         *is_ne_out = (alu->op == nir_op_ine);
         return true;
      }
   }
   return false;
}

/* Sink a matched `tri(v, lo, hi)` chain (`c_outer`, `c_inner`, `inner_sel`,
 * `outer_sel`) to the lowest common dominator of its remaining uses when that
 * block is strictly dominated by `outer_sel->block` and not deeper in a loop
 * (e.g. past an early-exit branch into the rare full-evaluation arm). */
static void
try_sink_tri_to_uses(const struct tri_match *m)
{
   nir_block *def_blk = m->outer_sel->instr.block;
   nir_block *lca = NULL;

   nir_foreach_use_including_if(use, &m->outer_sel->def) {
      if (!nir_src_is_if(use) &&
          nir_src_use_instr(use)->type == nir_instr_type_phi)
         return;
      nir_block *ub =
         nir_src_is_if(use)
            ? nir_cf_node_as_block(
                 nir_cf_node_prev(&nir_src_use_if(use)->cf_node))
            : nir_src_use_instr(use)->block;
      lca = lca ? nir_dominance_lca(lca, ub) : ub;
   }

   if (!lca || lca == def_blk || !nir_block_dominates(def_blk, lca))
      return;
   if (block_loop_depth(lca) > block_loop_depth(def_blk))
      return;

   nir_instr *outer_i = &m->outer_sel->instr;
   nir_instr *inner_i = &m->inner_sel->instr;
   nir_instr *c_outer_i = nir_def_instr(m->outer_sel->src[0].src.ssa);
   nir_instr *c_inner_i = nir_def_instr(m->inner_sel->src[0].src.ssa);

   bool move_c_outer = c_outer_i->block == def_blk &&
                       def_sole_use_is_instr(m->outer_sel->src[0].src.ssa, outer_i);
   bool move_inner = inner_i->block == def_blk &&
                     def_sole_use_is_instr(&m->inner_sel->def, outer_i);
   bool move_c_inner = move_inner && c_inner_i->block == def_blk &&
                       def_sole_use_is_instr(m->inner_sel->src[0].src.ssa, inner_i);

   nir_instr_move(nir_before_block_after_phis(lca), outer_i);
   if (move_c_outer)
      nir_instr_move(nir_before_instr(outer_i), c_outer_i);
   if (move_inner) {
      nir_instr_move(nir_before_instr(outer_i), inner_i);
      if (move_c_inner)
         nir_instr_move(nir_before_instr(inner_i), c_inner_i);
   }
}

static bool
opt_tri_zero_pair(nir_builder *b, nir_function_impl *impl)
{
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         if (alu->def.bit_size != 1 || alu->def.num_components != 1)
            continue;

         struct tri_match m1 = {0}, m2 = {0};
         bool want_any_nonzero = false;
         bool matched = false;
         nir_instr *dead_in[2] = {NULL, NULL};

         if (alu->op == nir_op_ieq || alu->op == nir_op_ine) {
            /* ieq/ine(umax(tri1, tri2), 0) or ieq/ine(ior(tri1, tri2), 0) */
            for (unsigned s = 0; s < 2; s++) {
               if (!nir_src_is_const(alu->src[1 - s].src) ||
                   nir_src_as_uint(alu->src[1 - s].src) != 0)
                  continue;
               nir_def *comb = strip_mov_def(alu->src[s].src.ssa);
               if (!comb || nir_def_instr(comb)->type != nir_instr_type_alu)
                  continue;
               nir_alu_instr *ca = nir_instr_as_alu(nir_def_instr(comb));
               if (ca->op == nir_op_umax || ca->op == nir_op_ior) {
                  if (match_tri_expr(ca->src[0].src.ssa, &m1) &&
                      match_tri_expr(ca->src[1].src.ssa, &m2)) {
                     want_any_nonzero = (alu->op == nir_op_ine);
                     dead_in[0] = &ca->instr;
                     matched = true;
                     break;
                  }
               }
            }
         } else if (alu->op == nir_op_iand || alu->op == nir_op_ior) {
            bool ne1 = false, ne2 = false;
            if (match_tri_eq_zero(alu->src[0].src.ssa, &m1, &ne1) &&
                match_tri_eq_zero(alu->src[1].src.ssa, &m2, &ne2) &&
                ne1 == ne2) {
               if (alu->op == nir_op_iand && !ne1) {
                  want_any_nonzero = false;
                  dead_in[0] = nir_def_instr(strip_mov_def(alu->src[0].src.ssa));
                  dead_in[1] = nir_def_instr(strip_mov_def(alu->src[1].src.ssa));
                  matched = true;
               } else if (alu->op == nir_op_ior && ne1) {
                  want_any_nonzero = true;
                  dead_in[0] = nir_def_instr(strip_mov_def(alu->src[0].src.ssa));
                  dead_in[1] = nir_def_instr(strip_mov_def(alu->src[1].src.ssa));
                  matched = true;
               }
            }
         }

         if (!matched || m1.lo != m2.lo || m1.hi != m2.hi || m1.v == m2.v)
            continue;

         /* Both tri expressions share (lo, hi):
          * any_nonzero <=> (imin(v1, v2) < lo) || (hi < imax(v1, v2)). */
         b->cursor = nir_before_instr(instr);
         nir_def *vmin = nir_imin(b, m1.v, m2.v);
         nir_def *vmax = nir_imax(b, m1.v, m2.v);
         nir_def *any_nz =
            nir_ior(b, nir_ilt(b, vmin, m1.lo), nir_ilt(b, m1.hi, vmax));
         nir_def *res = want_any_nonzero ? any_nz : nir_inot(b, any_nz);
         nir_def_rewrite_uses(&alu->def, res);

         /* Remove dead combining ops immediately so remaining uses of
          * m1/m2.outer_sel reflect only downstream consumers, then sink them
          * past early exits if possible. */
         nir_instr_remove(instr);
         for (unsigned d = 0; d < 2; d++) {
            if (dead_in[d] && dead_in[d]->type == nir_instr_type_alu &&
                nir_def_is_unused(&nir_instr_as_alu(dead_in[d])->def))
               nir_instr_remove(dead_in[d]);
         }

         try_sink_tri_to_uses(&m1);
         try_sink_tri_to_uses(&m2);
         progress = true;
      }
   }

   return progress;
}

/* ---------------------------------------------------------------------------
 * Opt 3: Narrow shared 32-bit bounds compared against u2u32(v8) to 16-bit
 * --------------------------------------------------------------------------- */

/* Match a 32-bit value guaranteed in [0, 255] and return its 16-bit equivalent
 * (u2u16 of an 8-bit value, or imin/imax/umin/umax of two such values). */
static nir_def *
try_narrow_u8_val_to_u16(nir_builder *b, nir_def *d, unsigned depth)
{
   if (depth > 2)
      return NULL;
   d = strip_mov_def(d);
   if (!d || d->bit_size != 32 || d->num_components != 1 ||
       nir_def_instr(d)->type != nir_instr_type_alu)
      return NULL;

   nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(d));
   if (alu->op == nir_op_u2u32 && alu->src[0].src.ssa->bit_size == 8)
      return nir_u2u16(b, alu->src[0].src.ssa);

   if (alu->op == nir_op_imin || alu->op == nir_op_imax ||
       alu->op == nir_op_umin || alu->op == nir_op_umax) {
      nir_def *a = try_narrow_u8_val_to_u16(b, alu->src[0].src.ssa, depth + 1);
      nir_def *c = try_narrow_u8_val_to_u16(b, alu->src[1].src.ssa, depth + 1);
      if (a && c) {
         if (alu->op == nir_op_imin || alu->op == nir_op_umin)
            return nir_umin(b, a, c);
         return nir_umax(b, a, c);
      }
   }

   return NULL;
}

static bool
is_u8_range_32(nir_def *d, unsigned depth)
{
   if (depth > 2)
      return false;
   d = strip_mov_def(d);
   if (!d || d->bit_size != 32 || d->num_components != 1 ||
       nir_def_instr(d)->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(d));
   if (alu->op == nir_op_u2u32 && alu->src[0].src.ssa->bit_size == 8)
      return true;
   if (alu->op == nir_op_imin || alu->op == nir_op_imax ||
       alu->op == nir_op_umin || alu->op == nir_op_umax) {
      return is_u8_range_32(alu->src[0].src.ssa, depth + 1) &&
             is_u8_range_32(alu->src[1].src.ssa, depth + 1);
   }
   return false;
}

static bool
opt_narrow_u8_bound_cmps(nir_builder *b, nir_function_impl *impl)
{
   bool progress = false;

   /* Find 32-bit bounds compared against >= 2 u8-range values. */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *bound_alu = nir_instr_as_alu(instr);
         if (bound_alu->def.bit_size != 32 ||
             bound_alu->def.num_components != 1)
            continue;

         unsigned cmp_uses = 0;
         nir_foreach_use(use, &bound_alu->def) {
            nir_instr *u = nir_src_use_instr(use);
            if (u->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *ca = nir_instr_as_alu(u);
            if (ca->op != nir_op_ilt && ca->op != nir_op_ige &&
                ca->op != nir_op_ieq && ca->op != nir_op_ine)
               continue;
            nir_def *other = (ca->src[0].src.ssa == &bound_alu->def)
                                ? ca->src[1].src.ssa
                                : ca->src[0].src.ssa;
            if (is_u8_range_32(other, 0))
               cmp_uses++;
         }

         if (cmp_uses < 2)
            continue;

         /* Clamp bound32 to [-1, 256] and narrow to i16 once right after its
          * definition; every comparison against [0, 255] is preserved verbatim. */
         b->cursor = nir_after_instr(instr);
         nir_def *clamped =
            nir_imin(b, nir_imax(b, &bound_alu->def, nir_imm_int(b, -1)),
                     nir_imm_int(b, 256));
         nir_def *bound16 = nir_i2i16(b, clamped);

         nir_foreach_use_safe(use, &bound_alu->def) {
            nir_instr *u = nir_src_use_instr(use);
            if (u->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *ca = nir_instr_as_alu(u);
            if (ca->op != nir_op_ilt && ca->op != nir_op_ige &&
                ca->op != nir_op_ieq && ca->op != nir_op_ine)
               continue;

            bool bound_is_lhs = (ca->src[0].src.ssa == &bound_alu->def);
            nir_def *other =
               bound_is_lhs ? ca->src[1].src.ssa : ca->src[0].src.ssa;
            if (!is_u8_range_32(other, 0))
               continue;

            b->cursor = nir_before_instr(u);
            nir_def *other16 = try_narrow_u8_val_to_u16(b, other, 0);
            if (!other16)
               continue;

            nir_def *lhs16 = bound_is_lhs ? bound16 : other16;
            nir_def *rhs16 = bound_is_lhs ? other16 : bound16;
            nir_def *new_cmp = nir_build_alu2(b, ca->op, lhs16, rhs16);
            nir_def_rewrite_uses(&ca->def, new_cmp);
            progress = true;
         }
      }
   }

   return progress;
}

bool
agx_nir_opt_image_idioms_early(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      nir_metadata_require(impl, nir_metadata_dominance);
      nir_builder b = nir_builder_create(impl);
      bool impl_progress = false;

      impl_progress |= opt_reassoc_loop_ior_cf_list(&b, &impl->body);
      impl_progress |= opt_tri_zero_pair(&b, impl);
      impl_progress |= opt_narrow_u8_bound_cmps(&b, impl);

      if (impl_progress) {
         nir_progress(true, impl, nir_metadata_control_flow);
         progress = true;
      } else {
         nir_progress(false, impl, nir_metadata_all);
      }
   }

   return progress;
}

/* ---------------------------------------------------------------------------
 * Opt 4: Consecutive side-effect-only guard-`if` coalescing
 * --------------------------------------------------------------------------- */

static bool
is_pure_cheap_alu(nir_instr *instr)
{
   return instr->type == nir_instr_type_load_const ||
          instr->type == nir_instr_type_alu;
}

static bool
if_is_coalesceable_guard(nir_if *nif)
{
   /* Must have an empty else branch. */
   nir_block *ef = nir_if_first_else_block(nif);
   nir_block *el = nir_if_last_else_block(nif);
   if (ef != el || !exec_list_is_empty(&ef->instr_list))
      return false;

   /* Merge block right after `nif` must have no phis. */
   nir_block *after = nir_cf_node_as_block(nir_cf_node_next(&nif->cf_node));
   if (!exec_list_is_empty(&after->instr_list) &&
       nir_block_first_instr(after)->type == nir_instr_type_phi)
      return false;

   /* Then branch must contain no jumps, calls, or barriers. */
   bool ok = true;
   nir_foreach_block_in_cf_node(blk, &nif->cf_node) {
      nir_foreach_instr(instr, blk) {
         if (instr->type == nir_instr_type_jump ||
             instr->type == nir_instr_type_call) {
            ok = false;
            break;
         }
         if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
            if (op == nir_intrinsic_barrier ||
                op == nir_intrinsic_demote ||
                op == nir_intrinsic_demote_if ||
                op == nir_intrinsic_terminate ||
                op == nir_intrinsic_terminate_if) {
               ok = false;
               break;
            }
         }
      }
      if (!ok)
         break;
   }

   return ok;
}

static bool
block_only_pure_alu(nir_block *blk, unsigned max_instrs)
{
   unsigned n = 0;
   nir_foreach_instr(instr, blk) {
      if (!is_pure_cheap_alu(instr))
         return false;
      if (++n > max_instrs)
         return false;
   }
   return true;
}

static bool
opt_coalesce_guard_ifs_list(nir_builder *b, struct exec_list *cf_list)
{
   bool progress = false;

   /* Recurse into nested CF nodes first. */
   foreach_list_typed(nir_cf_node, node, node, cf_list) {
      if (node->type == nir_cf_node_loop) {
         progress |=
            opt_coalesce_guard_ifs_list(b, &nir_cf_node_as_loop(node)->body);
      } else if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);
         progress |= opt_coalesce_guard_ifs_list(b, &nif->then_list);
         progress |= opt_coalesce_guard_ifs_list(b, &nif->else_list);
      }
   }

   /* Scan for runs of >= 3 consecutive guard-`if`s separated only by empty or
    * tiny pure-ALU blocks. */
   nir_cf_node *node = exec_node_data(nir_cf_node, exec_list_get_head(cf_list),
                                      node);
   while (node) {
      if (node->type != nir_cf_node_if ||
          !if_is_coalesceable_guard(nir_cf_node_as_if(node))) {
         node = nir_cf_node_next(node);
         continue;
      }

      /* Skip if this run is ALREADY the sole body of an outer guard `if`. */
      if (node->parent && node->parent->type == nir_cf_node_if) {
         nir_if *parent_if = nir_cf_node_as_if(node->parent);
         nir_cf_node *prev = nir_cf_node_prev(node);
         if (prev && prev->type == nir_cf_node_block &&
             prev == &nir_if_first_then_block(parent_if)->cf_node &&
             exec_list_is_empty(&nir_cf_node_as_block(prev)->instr_list)) {
            nir_cf_node *p_after = node;
            while (p_after) {
               nir_cf_node *nxt = nir_cf_node_next(p_after);
               if (!nxt)
                  break;
               p_after = nxt;
            }
            if (p_after->type == nir_cf_node_block &&
                exec_list_is_empty(&nir_cf_node_as_block(p_after)->instr_list) &&
                if_is_coalesceable_guard(parent_if)) {
               break;
            }
         }
      }

      nir_if *run[8];
      unsigned run_len = 0;
      run[run_len++] = nir_cf_node_as_if(node);

      nir_cf_node *scan = nir_cf_node_next(node);
      while (scan && run_len < ARRAY_SIZE(run)) {
         if (scan->type != nir_cf_node_block)
            break;
         nir_block *sep = nir_cf_node_as_block(scan);
         nir_cf_node *next_if_cf = nir_cf_node_next(scan);
         if (!next_if_cf || next_if_cf->type != nir_cf_node_if)
            break;
         nir_if *next_if = nir_cf_node_as_if(next_if_cf);
         if (!if_is_coalesceable_guard(next_if) ||
             !block_only_pure_alu(sep, 4))
            break;

         run[run_len++] = next_if;
         scan = nir_cf_node_next(next_if_cf);
      }

      if (run_len < 3) {
         node = nir_cf_node_next(&run[run_len - 1]->cf_node);
         continue;
      }

      /* Hoist any pure-ALU instructions from separating blocks right before
       * `run[0]` so every `run[k]->condition` dominates the outer `if`. */
      nir_block *before_blk =
         nir_cf_node_as_block(nir_cf_node_prev(&run[0]->cf_node));
      for (unsigned k = 0; k + 1 < run_len; k++) {
         nir_block *sep =
            nir_cf_node_as_block(nir_cf_node_next(&run[k]->cf_node));
         nir_foreach_instr_safe(instr, sep) {
            nir_instr_move(nir_after_block(before_blk), instr);
         }
      }

      /* Build `any_c = c_0 | c_1 | ... | c_{n-1}` and wrap `run[0..n-1]`. */
      b->cursor = nir_after_block(before_blk);
      nir_def *any_c = run[0]->condition.ssa;
      for (unsigned k = 1; k < run_len; k++)
         any_c = nir_ior(b, any_c, run[k]->condition.ssa);

      nir_cf_list extracted;
      nir_cf_extract(&extracted, nir_before_cf_node(&run[0]->cf_node),
                     nir_after_cf_node(&run[run_len - 1]->cf_node));

      b->cursor = nir_after_block(before_blk);
      nir_if *outer = nir_push_if(b, any_c);
      nir_cf_reinsert(&extracted, nir_before_block(nir_if_first_then_block(outer)));
      nir_pop_if(b, outer);

      progress = true;
      node = nir_cf_node_next(&outer->cf_node);
   }

   return progress;
}

bool
agx_nir_opt_image_idioms_late(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      nir_metadata_require(impl, nir_metadata_dominance);
      nir_builder b = nir_builder_create(impl);
      bool impl_progress = false;

      /* Re-run loop-invariant disjunction hoisting and ternary zero-pair
       * simplification + sinking after peephole-select / algebraic passes that
       * may newly expose bcsel-or chains and umax(tri, tri) == 0 checks. */
      impl_progress |= opt_reassoc_loop_ior_cf_list(&b, &impl->body);
      impl_progress |= opt_tri_zero_pair(&b, impl);

      /* Coalesce runs of consecutive side-effect-only guard ifs. */
      impl_progress |= opt_coalesce_guard_ifs_list(&b, &impl->body);

      if (impl_progress) {
         nir_progress(true, impl, nir_metadata_none);
         progress = true;
      } else {
         nir_progress(false, impl, nir_metadata_all);
      }
   }

   return progress;
}
