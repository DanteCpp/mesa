/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 *
 * Direct3D 10/11 SM4.0 / SM4.1 / SM5.0 DDI Tokenized Bytecode -> Mesa NIR.
 *
 * Consumes the raw DWORD token stream handed by the Windows D3D11 runtime to
 * pfnCreateVertexShader / pfnCreatePixelShader / pfnCreateComputeShader and
 * builds structured, validated NIR directly for Gallium Asahi (M5 / G17G).
 */

#include "sm5_to_nir.h"
#include "sm5_tokens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir_builder.h"
#include "util/bitscan.h"
#include "util/macros.h"

#define SM5_MAX_IO_REGS     32u
#define SM5_MAX_TEMPS       4096u
#define SM5_MAX_IDX_TEMPS   32u
#define SM5_MAX_TGSM        32u
#define SM5_MAX_CF_DEPTH    64u
#define SM5_MAX_SW_CASES    256u

struct sm5_operand;

struct sm5_index {
   enum sm5_index_rep rep;
   uint32_t imm;
   bool has_rel;
   struct sm5_operand *rel;
};

struct sm5_operand {
   enum sm5_operand_type type;
   enum sm5_num_components num_comps;
   enum sm5_sel_mode sel_mode;
   uint8_t write_mask;   /* for SM5_SEL_MASK */
   uint8_t swizzle[4];   /* for SM5_SEL_SWIZZLE / SM5_SEL_SELECT1 */
   enum sm5_operand_mod mod;
   uint32_t imm32[4];
   uint32_t index_dim;
   struct sm5_index idx[3];
};

struct sm5_res_decl {
   bool declared;
   enum sm5_resource_dim dim;
   enum sm5_return_type ret_type[4];
   uint32_t stride; /* bytes for structured buffers */
   uint32_t samples;
   bool is_raw;
   bool is_structured;
};

struct sm5_io_decl {
   bool declared;
   uint8_t mask;
   enum sm5_sysval_name sysval;
   enum sm5_interp_mode interp;
   nir_variable *var;
   nir_variable *shadow; /* function_temp shadow for outputs */
   uint8_t num_comps;
};

struct sm5_tgsm_decl {
   bool declared;
   uint32_t size_bytes;
   uint32_t stride;
   uint32_t offset_bytes; /* offset into shader shared memory */
};

enum sm5_cf_kind {
   SM5_CF_IF = 1,
   SM5_CF_LOOP = 2,
   SM5_CF_SWITCH = 3,
};

struct sm5_cf_frame {
   enum sm5_cf_kind kind;
   nir_if *nif;
   nir_loop *loop;
   /* Switch state: lowered to a single-iteration loop + matched-flag if chain. */
   nir_def *sw_val;
   nir_variable *sw_matched;
   bool sw_in_case;
   nir_if *sw_cur_if;
};

struct sm5_ctx {
   const uint32_t *code;
   size_t num_dwords;
   size_t pos;

   mesa_shader_stage stage;
   nir_builder b;
   nir_shader *nir;
   struct aurora_sm5_info info;

   /* Declarations */
   unsigned num_temps;
   nir_variable **temps; /* [num_temps] of uvec4 */

   nir_variable *idx_temps[SM5_MAX_IDX_TEMPS];
   uint32_t idx_temp_count[SM5_MAX_IDX_TEMPS];

   nir_variable *icb_var;
   const uint32_t *icb_data;
   uint32_t icb_vec4_count;

   struct sm5_io_decl inputs[SM5_MAX_IO_REGS];
   struct sm5_io_decl outputs[SM5_MAX_IO_REGS];
   nir_variable *odepth_var;
   nir_variable *odepth_shadow;
   nir_variable *omask_var;
   nir_variable *omask_shadow;

   struct sm5_res_decl srvs[AURORA_MAX_SRV_SLOTS];
   struct sm5_res_decl uavs[AURORA_MAX_UAV_SLOTS];
   struct sm5_tgsm_decl tgsm[SM5_MAX_TGSM];
   uint32_t total_shared_bytes;

   /* Optional outer loop for early RET / RETC */
   bool has_early_ret;
   nir_loop *ret_loop;

   /* Control-flow stack */
   struct sm5_cf_frame cf[SM5_MAX_CF_DEPTH];
   unsigned cf_depth;

   /* Scratch pool for nested relative operands during a single instruction */
   struct sm5_operand rel_pool[16];
   unsigned rel_pool_used;

   bool failed;
   char err[256];
};

static void
sm5_fail(struct sm5_ctx *c, const char *msg)
{
   if (!c->failed) {
      c->failed = true;
      snprintf(c->err, sizeof(c->err), "%s", msg);
      fprintf(stderr, "aurora_sm5_to_nir: %s (word %zu/%zu)\n",
              msg, c->pos, c->num_dwords);
   }
}

static bool
sm5_read_u32(struct sm5_ctx *c, uint32_t *out)
{
   if (c->pos >= c->num_dwords) {
      sm5_fail(c, "unexpected end of token stream");
      return false;
   }
   *out = c->code[c->pos++];
   return true;
}

static bool
sm5_parse_operand(struct sm5_ctx *c, struct sm5_operand *op)
{
   memset(op, 0, sizeof(*op));
   uint32_t tok0 = 0;
   if (!sm5_read_u32(c, &tok0))
      return false;

   op->num_comps = (enum sm5_num_components)(tok0 & 0x3u);
   op->sel_mode = (enum sm5_sel_mode)((tok0 >> 2) & 0x3u);
   op->type = (enum sm5_operand_type)((tok0 >> 12) & 0xffu);
   op->index_dim = (tok0 >> 20) & 0x3u;

   if (op->num_comps == SM5_COMP_4) {
      if (op->sel_mode == SM5_SEL_MASK) {
         op->write_mask = (uint8_t)((tok0 >> 4) & 0xfu);
         op->swizzle[0] = 0;
         op->swizzle[1] = 1;
         op->swizzle[2] = 2;
         op->swizzle[3] = 3;
      } else if (op->sel_mode == SM5_SEL_SWIZZLE) {
         op->write_mask = 0xfu;
         op->swizzle[0] = (uint8_t)((tok0 >> 4) & 0x3u);
         op->swizzle[1] = (uint8_t)((tok0 >> 6) & 0x3u);
         op->swizzle[2] = (uint8_t)((tok0 >> 8) & 0x3u);
         op->swizzle[3] = (uint8_t)((tok0 >> 10) & 0x3u);
      } else if (op->sel_mode == SM5_SEL_SELECT1) {
         uint8_t s = (uint8_t)((tok0 >> 4) & 0x3u);
         op->write_mask = (uint8_t)(1u << s);
         op->swizzle[0] = s;
         op->swizzle[1] = s;
         op->swizzle[2] = s;
         op->swizzle[3] = s;
      }
   } else if (op->num_comps == SM5_COMP_1) {
      op->write_mask = 0x1u;
      op->swizzle[0] = op->swizzle[1] = op->swizzle[2] = op->swizzle[3] = 0;
   }

   /* Extended operand tokens */
   bool ext = (tok0 >> 31) != 0;
   while (ext) {
      uint32_t etok = 0;
      if (!sm5_read_u32(c, &etok))
         return false;
      uint32_t etype = etok & 0x3fu;
      if (etype == 1u) { /* D3D10_SB_EXTENDED_OPERAND_MODIFIER */
         op->mod = (enum sm5_operand_mod)((etok >> 6) & 0xffu);
      }
      ext = (etok >> 31) != 0;
   }

   if (op->type == SM5_OPERAND_IMMEDIATE32) {
      unsigned n = (op->num_comps == SM5_COMP_4) ? 4u : 1u;
      for (unsigned i = 0; i < n; ++i) {
         if (!sm5_read_u32(c, &op->imm32[i]))
            return false;
      }
      if (n == 1) {
         op->imm32[1] = op->imm32[2] = op->imm32[3] = op->imm32[0];
      }
      return true;
   }

   if (op->type == SM5_OPERAND_IMMEDIATE64) {
      unsigned n = (op->num_comps == SM5_COMP_4) ? 4u : 2u;
      for (unsigned i = 0; i < n; ++i) {
         uint32_t dummy = 0;
         if (!sm5_read_u32(c, &dummy))
            return false;
      }
      return true;
   }

   for (unsigned d = 0; d < op->index_dim; ++d) {
      enum sm5_index_rep rep =
         (enum sm5_index_rep)((tok0 >> (22u + 3u * d)) & 0x7u);
      op->idx[d].rep = rep;
      if (rep == SM5_INDEX_IMM32 || rep == SM5_INDEX_IMM32_PLUS_REL) {
         if (!sm5_read_u32(c, &op->idx[d].imm))
            return false;
      } else if (rep == SM5_INDEX_IMM64 || rep == SM5_INDEX_IMM64_PLUS_REL) {
         uint32_t lo = 0, hi = 0;
         if (!sm5_read_u32(c, &lo) || !sm5_read_u32(c, &hi))
            return false;
         op->idx[d].imm = lo;
      }
      if (rep == SM5_INDEX_REL || rep == SM5_INDEX_IMM32_PLUS_REL ||
          rep == SM5_INDEX_IMM64_PLUS_REL) {
         if (c->rel_pool_used >= ARRAY_SIZE(c->rel_pool)) {
            sm5_fail(c, "too many nested relative index operands");
            return false;
         }
         struct sm5_operand *rel = &c->rel_pool[c->rel_pool_used++];
         if (!sm5_parse_operand(c, rel))
            return false;
         op->idx[d].has_rel = true;
         op->idx[d].rel = rel;
      }
   }
   return true;
}

/* Forward declaration for recursive relative index evaluation. */
static nir_def *sm5_load_src(struct sm5_ctx *c, const struct sm5_operand *op,
                             bool is_int);

static nir_def *
sm5_eval_index(struct sm5_ctx *c, const struct sm5_index *idx)
{
   nir_builder *b = &c->b;
   if (!idx->has_rel)
      return nir_imm_int(b, (int32_t)idx->imm);

   nir_def *rel4 = sm5_load_src(c, idx->rel, true);
   nir_def *rel = nir_channel(b, rel4, 0);
   if (idx->imm != 0)
      return nir_iadd_imm(b, rel, (int32_t)idx->imm);
   return rel;
}

static enum glsl_interp_mode
sm5_to_glsl_interp(enum sm5_interp_mode m)
{
   switch (m) {
   case SM5_INTERP_CONSTANT:
      return INTERP_MODE_FLAT;
   case SM5_INTERP_LINEAR_NOPERSPECTIVE:
   case SM5_INTERP_LINEAR_NOPERSPECTIVE_CENTROID:
   case SM5_INTERP_LINEAR_NOPERSPECTIVE_SAMPLE:
      return INTERP_MODE_NOPERSPECTIVE;
   case SM5_INTERP_LINEAR:
   case SM5_INTERP_LINEAR_CENTROID:
   case SM5_INTERP_LINEAR_SAMPLE:
   default:
      return INTERP_MODE_SMOOTH;
   }
}

static void
sm5_ensure_temp_count(struct sm5_ctx *c, unsigned needed)
{
   if (needed <= c->num_temps)
      return;
   if (needed > SM5_MAX_TEMPS) {
      sm5_fail(c, "temp register count exceeds SM5_MAX_TEMPS");
      return;
   }
   nir_variable **n =
      realloc(c->temps, needed * sizeof(nir_variable *));
   if (!n) {
      sm5_fail(c, "out of memory allocating temps");
      return;
   }
   c->temps = n;
   for (unsigned i = c->num_temps; i < needed; ++i) {
      char name[16];
      snprintf(name, sizeof(name), "r%u", i);
      c->temps[i] =
         nir_local_variable_create(c->b.impl, glsl_uvec4_type(), name);
   }
   c->num_temps = needed;
}

static nir_variable *
sm5_get_input_var(struct sm5_ctx *c, uint32_t reg)
{
   if (reg >= SM5_MAX_IO_REGS) {
      sm5_fail(c, "input register index out of range");
      return NULL;
   }
   struct sm5_io_decl *d = &c->inputs[reg];
   if (d->var)
      return d->var;

   uint8_t mask = d->mask ? d->mask : 0xfu;
   unsigned comps = util_last_bit(mask);
   if (comps == 0)
      comps = 4;
   d->num_comps = (uint8_t)comps;

   char name[24];
   snprintf(name, sizeof(name), "in_%u", reg);

   int loc = 0;
   const struct glsl_type *type = glsl_vector_type(GLSL_TYPE_FLOAT, comps);
   if (c->stage == MESA_SHADER_VERTEX) {
      loc = VERT_ATTRIB_GENERIC0 + (int)reg;
      type = glsl_uvec4_type();
      d->num_comps = 4;
   } else if (c->stage == MESA_SHADER_FRAGMENT) {
      if (d->sysval == SM5_NAME_POSITION) {
         loc = VARYING_SLOT_POS;
         type = glsl_vec4_type();
         d->num_comps = 4;
      } else if (d->sysval == SM5_NAME_CLIP_DISTANCE) {
         loc = VARYING_SLOT_CLIP_DIST0;
      } else {
         loc = VARYING_SLOT_VAR0 + (int)reg;
      }
   }

   nir_variable *var =
      nir_variable_create(c->nir, nir_var_shader_in, type, name);
   var->data.location = loc;
   var->data.driver_location = reg;
   if (c->stage == MESA_SHADER_FRAGMENT) {
      var->data.interpolation = sm5_to_glsl_interp(d->interp);
      if (d->interp == SM5_INTERP_LINEAR_CENTROID ||
          d->interp == SM5_INTERP_LINEAR_NOPERSPECTIVE_CENTROID)
         var->data.centroid = true;
      if (d->interp == SM5_INTERP_LINEAR_SAMPLE ||
          d->interp == SM5_INTERP_LINEAR_NOPERSPECTIVE_SAMPLE)
         var->data.sample = true;
   }
   d->var = var;
   return var;
}

static nir_variable *
sm5_get_output_shadow(struct sm5_ctx *c, uint32_t reg)
{
   if (reg >= SM5_MAX_IO_REGS) {
      sm5_fail(c, "output register index out of range");
      return NULL;
   }
   struct sm5_io_decl *d = &c->outputs[reg];
   if (!d->shadow) {
      char sname[24];
      snprintf(sname, sizeof(sname), "out_tmp_%u", reg);
      d->shadow =
         nir_local_variable_create(c->b.impl, glsl_uvec4_type(), sname);
   }
   if (!d->var) {
      uint8_t mask = d->mask ? d->mask : 0xfu;
      unsigned comps = util_last_bit(mask);
      if (comps == 0)
         comps = 4;
      char vname[24];
      snprintf(vname, sizeof(vname), "out_%u", reg);

      int loc = 0;
      if (c->stage == MESA_SHADER_VERTEX) {
         if (d->sysval == SM5_NAME_POSITION) {
            loc = VARYING_SLOT_POS;
            comps = 4;
         } else if (d->sysval == SM5_NAME_CLIP_DISTANCE) {
            loc = VARYING_SLOT_CLIP_DIST0;
         } else {
            loc = VARYING_SLOT_VAR0 + (int)reg;
         }
      } else if (c->stage == MESA_SHADER_FRAGMENT) {
         loc = FRAG_RESULT_DATA0 + (int)reg;
         comps = 4;
      }
      d->num_comps = (uint8_t)comps;
      const struct glsl_type *type = glsl_vector_type(GLSL_TYPE_FLOAT, comps);
      nir_variable *var =
         nir_variable_create(c->nir, nir_var_shader_out, type, vname);
      var->data.location = loc;
      var->data.driver_location = reg;
      d->var = var;
   }
   return d->shadow;
}

static nir_def *
sm5_pad_to_4(nir_builder *b, nir_def *v)
{
   if (v->num_components == 4)
      return v;
   nir_def *zero = nir_imm_int(b, 0);
   nir_def *ch[4];
   for (unsigned i = 0; i < 4; ++i)
      ch[i] = (i < v->num_components) ? nir_channel(b, v, i) : zero;
   return nir_vec(b, ch, 4);
}

static nir_def *
sm5_apply_swizzle_and_mod(nir_builder *b, nir_def *raw,
                          const struct sm5_operand *op, bool is_int)
{
   nir_def *v4 = sm5_pad_to_4(b, raw);
   unsigned sw[4] = { op->swizzle[0], op->swizzle[1], op->swizzle[2], op->swizzle[3] };
   nir_def *swz = nir_swizzle(b, v4, sw, 4);

   switch (op->mod) {
   case SM5_MOD_NONE:
      return swz;
   case SM5_MOD_NEG:
      return is_int ? nir_ineg(b, swz) : nir_fneg(b, swz);
   case SM5_MOD_ABS:
      return is_int ? nir_iabs(b, swz) : nir_fabs(b, swz);
   case SM5_MOD_ABSNEG:
      return is_int ? nir_ineg(b, nir_iabs(b, swz))
                    : nir_fneg(b, nir_fabs(b, swz));
   default:
      return swz;
   }
}

static nir_def *
sm5_load_src(struct sm5_ctx *c, const struct sm5_operand *op, bool is_int)
{
   nir_builder *b = &c->b;
   nir_def *raw = NULL;

   switch (op->type) {
   case SM5_OPERAND_TEMP: {
      uint32_t reg = op->idx[0].imm;
      sm5_ensure_temp_count(c, reg + 1);
      if (c->failed)
         return nir_imm_ivec4(b, 0, 0, 0, 0);
      raw = nir_load_var(b, c->temps[reg]);
      break;
   }
   case SM5_OPERAND_INDEXABLE_TEMP: {
      uint32_t slot = op->idx[0].imm;
      if (slot >= SM5_MAX_IDX_TEMPS || !c->idx_temps[slot]) {
         sm5_fail(c, "undeclared indexable temp");
         return nir_imm_ivec4(b, 0, 0, 0, 0);
      }
      nir_def *elem = sm5_eval_index(c, &op->idx[1]);
      nir_deref_instr *deref = nir_build_deref_array(
         b, nir_build_deref_var(b, c->idx_temps[slot]), elem);
      raw = nir_load_deref(b, deref);
      break;
   }
   case SM5_OPERAND_IMMEDIATE32: {
      raw = nir_imm_ivec4(b, (int32_t)op->imm32[0], (int32_t)op->imm32[1],
                          (int32_t)op->imm32[2], (int32_t)op->imm32[3]);
      /* Immediates do not swizzle, only modifiers apply if present. */
      struct sm5_operand id = *op;
      id.swizzle[0] = 0;
      id.swizzle[1] = 1;
      id.swizzle[2] = 2;
      id.swizzle[3] = 3;
      return sm5_apply_swizzle_and_mod(b, raw, &id, is_int);
   }
   case SM5_OPERAND_CONSTANT_BUFFER: {
      uint32_t cb_slot = op->idx[0].imm;
      if (cb_slot < AURORA_MAX_CB_SLOTS)
         c->info.cb_mask |= (1u << cb_slot);
      if (cb_slot + 1 > c->nir->info.num_ubos)
         c->nir->info.num_ubos = cb_slot + 1;
      nir_def *vec_idx = sm5_eval_index(c, &op->idx[1]);
      nir_def *byte_off = nir_ishl_imm(b, vec_idx, 4);
      raw = nir_load_ubo(b, 4, 32, nir_imm_int(b, (int32_t)cb_slot), byte_off,
                         .align_mul = 16, .align_offset = 0,
                         .range_base = 0, .range = ~0u);
      break;
   }
   case SM5_OPERAND_IMMEDIATE_CONSTANT_BUFFER: {
      if (!c->icb_var) {
         sm5_fail(c, "read from undeclared immediate constant buffer");
         return nir_imm_ivec4(b, 0, 0, 0, 0);
      }
      if (!op->idx[0].has_rel && op->idx[0].imm < c->icb_vec4_count) {
         const uint32_t *d = &c->icb_data[op->idx[0].imm * 4u];
         raw = nir_imm_ivec4(b, (int32_t)d[0], (int32_t)d[1],
                             (int32_t)d[2], (int32_t)d[3]);
      } else {
         nir_def *elem = sm5_eval_index(c, &op->idx[0]);
         nir_deref_instr *deref = nir_build_deref_array(
            b, nir_build_deref_var(b, c->icb_var), elem);
         raw = nir_load_deref(b, deref);
      }
      break;
   }
   case SM5_OPERAND_INPUT: {
      uint32_t reg = op->idx[0].imm;
      if (reg >= SM5_MAX_IO_REGS) {
         sm5_fail(c, "input register out of range");
         return nir_imm_ivec4(b, 0, 0, 0, 0);
      }
      enum sm5_sysval_name sv = c->inputs[reg].sysval;
      if (sv == SM5_NAME_VERTEX_ID) {
         raw = nir_load_vertex_id(b);
      } else if (sv == SM5_NAME_INSTANCE_ID) {
         raw = nir_load_instance_id(b);
      } else if (sv == SM5_NAME_PRIMITIVE_ID) {
         raw = nir_load_primitive_id(b);
      } else if (sv == SM5_NAME_IS_FRONT_FACE) {
         nir_def *ff = nir_load_front_face(b, 1);
         raw = nir_bcsel(b, ff, nir_imm_int(b, -1), nir_imm_int(b, 0));
      } else if (sv == SM5_NAME_SAMPLE_INDEX) {
         raw = nir_load_sample_id(b);
      } else if (c->stage == MESA_SHADER_FRAGMENT && sv == SM5_NAME_POSITION) {
         raw = nir_load_frag_coord(b);
      } else {
         nir_variable *var = sm5_get_input_var(c, reg);
         if (!var)
            return nir_imm_ivec4(b, 0, 0, 0, 0);
         raw = nir_load_var(b, var);
      }
      break;
   }
   case SM5_OPERAND_INPUT_PRIMITIVEID:
      raw = nir_load_primitive_id(b);
      break;
   case SM5_OPERAND_INPUT_COVERAGE_MASK:
      raw = nir_load_sample_mask_in(b);
      break;
   case SM5_OPERAND_INPUT_THREAD_ID:
      raw = nir_load_global_invocation_id(b, 32);
      break;
   case SM5_OPERAND_INPUT_THREAD_GROUP_ID:
      raw = nir_load_workgroup_id(b);
      break;
   case SM5_OPERAND_INPUT_THREAD_ID_IN_GROUP:
      raw = nir_load_local_invocation_id(b);
      break;
   case SM5_OPERAND_INPUT_THREAD_ID_IN_GROUP_FLATTENED:
      raw = nir_load_local_invocation_index(b);
      break;
   default:
      sm5_fail(c, "unsupported source operand type");
      return nir_imm_ivec4(b, 0, 0, 0, 0);
   }

   return sm5_apply_swizzle_and_mod(b, raw, op, is_int);
}

static void
sm5_store_dst(struct sm5_ctx *c, const struct sm5_operand *op, nir_def *val,
              bool saturate)
{
   nir_builder *b = &c->b;

   if (op->type == SM5_OPERAND_NULL)
      return;

   nir_def *v4 = sm5_pad_to_4(b, val);
   if (saturate)
      v4 = nir_fsat(b, v4);

   uint8_t mask = op->write_mask ? op->write_mask : 0xfu;

   switch (op->type) {
   case SM5_OPERAND_TEMP: {
      uint32_t reg = op->idx[0].imm;
      sm5_ensure_temp_count(c, reg + 1);
      if (c->failed)
         return;
      nir_store_var(b, c->temps[reg], v4, mask);
      break;
   }
   case SM5_OPERAND_INDEXABLE_TEMP: {
      uint32_t slot = op->idx[0].imm;
      if (slot >= SM5_MAX_IDX_TEMPS || !c->idx_temps[slot]) {
         sm5_fail(c, "store to undeclared indexable temp");
         return;
      }
      nir_def *elem = sm5_eval_index(c, &op->idx[1]);
      nir_deref_instr *deref = nir_build_deref_array(
         b, nir_build_deref_var(b, c->idx_temps[slot]), elem);
      nir_store_deref(b, deref, v4, mask);
      break;
   }
   case SM5_OPERAND_OUTPUT: {
      uint32_t reg = op->idx[0].imm;
      nir_variable *shadow = sm5_get_output_shadow(c, reg);
      if (!shadow)
         return;
      nir_store_var(b, shadow, v4, mask);
      break;
   }
   case SM5_OPERAND_OUTPUT_DEPTH:
   case SM5_OPERAND_OUTPUT_DEPTH_GREATER_EQUAL:
   case SM5_OPERAND_OUTPUT_DEPTH_LESS_EQUAL: {
      if (!c->odepth_shadow) {
         c->odepth_shadow =
            nir_local_variable_create(c->b.impl, glsl_float_type(), "oDepth_tmp");
         c->odepth_var =
            nir_variable_create(c->nir, nir_var_shader_out, glsl_float_type(), "oDepth");
         c->odepth_var->data.location = FRAG_RESULT_DEPTH;
         if (op->type == SM5_OPERAND_OUTPUT_DEPTH_GREATER_EQUAL)
            c->nir->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_GREATER;
         else if (op->type == SM5_OPERAND_OUTPUT_DEPTH_LESS_EQUAL)
            c->nir->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_LESS;
         else
            c->nir->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_ANY;
      }
      nir_store_var(b, c->odepth_shadow, nir_channel(b, v4, 0), 0x1);
      break;
   }
   case SM5_OPERAND_OUTPUT_COVERAGE_MASK: {
      if (!c->omask_shadow) {
         c->omask_shadow =
            nir_local_variable_create(c->b.impl, glsl_uint_type(), "oMask_tmp");
         c->omask_var =
            nir_variable_create(c->nir, nir_var_shader_out, glsl_uint_type(), "oMask");
         c->omask_var->data.location = FRAG_RESULT_SAMPLE_MASK;
      }
      nir_store_var(b, c->omask_shadow, nir_channel(b, v4, 0), 0x1);
      break;
   }
   default:
      sm5_fail(c, "unsupported destination operand type");
      break;
   }
}

static enum glsl_sampler_dim
sm5_dim_to_glsl(enum sm5_resource_dim dim, bool *is_array)
{
   *is_array = false;
   switch (dim) {
   case SM5_DIM_BUFFER:
      return GLSL_SAMPLER_DIM_BUF;
   case SM5_DIM_TEXTURE1D:
      return GLSL_SAMPLER_DIM_1D;
   case SM5_DIM_TEXTURE1DARRAY:
      *is_array = true;
      return GLSL_SAMPLER_DIM_1D;
   case SM5_DIM_TEXTURE2D:
      return GLSL_SAMPLER_DIM_2D;
   case SM5_DIM_TEXTURE2DARRAY:
      *is_array = true;
      return GLSL_SAMPLER_DIM_2D;
   case SM5_DIM_TEXTURE2DMS:
      return GLSL_SAMPLER_DIM_MS;
   case SM5_DIM_TEXTURE2DMSARRAY:
      *is_array = true;
      return GLSL_SAMPLER_DIM_MS;
   case SM5_DIM_TEXTURE3D:
      return GLSL_SAMPLER_DIM_3D;
   case SM5_DIM_TEXTURECUBE:
      return GLSL_SAMPLER_DIM_CUBE;
   case SM5_DIM_TEXTURECUBEARRAY:
      *is_array = true;
      return GLSL_SAMPLER_DIM_CUBE;
   default:
      return GLSL_SAMPLER_DIM_2D;
   }
}

static unsigned
sm5_coord_components(enum glsl_sampler_dim dim, bool is_array)
{
   unsigned base = 2;
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:
   case GLSL_SAMPLER_DIM_BUF:
      base = 1;
      break;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_MS:
      base = 2;
      break;
   case GLSL_SAMPLER_DIM_3D:
   case GLSL_SAMPLER_DIM_CUBE:
      base = 3;
      break;
   default:
      base = 2;
      break;
   }
   return base + (is_array ? 1u : 0u);
}

static nir_alu_type
sm5_ret_to_nir_type(enum sm5_return_type rt)
{
   switch (rt) {
   case SM5_RET_SINT:
      return nir_type_int32;
   case SM5_RET_UINT:
      return nir_type_uint32;
   default:
      return nir_type_float32;
   }
}

static nir_def *
sm5_apply_res_swizzle(nir_builder *b, nir_def *val,
                      const struct sm5_operand *res_op)
{
   nir_def *v4 = sm5_pad_to_4(b, val);
   if (res_op->num_comps == SM5_COMP_4 &&
       (res_op->sel_mode == SM5_SEL_SWIZZLE ||
        res_op->sel_mode == SM5_SEL_SELECT1)) {
      unsigned sw[4] = { res_op->swizzle[0], res_op->swizzle[1],
                         res_op->swizzle[2], res_op->swizzle[3] };
      return nir_swizzle(b, v4, sw, 4);
   }
   return v4;
}

static void
sm5_emit_tex(struct sm5_ctx *c, enum sm5_opcode opcode,
             bool has_offset, const int offset[3],
             const struct sm5_operand *dst,
             const struct sm5_operand *coord_op,
             const struct sm5_operand *res_op,
             const struct sm5_operand *samp_op,
             const struct sm5_operand *extra0,
             const struct sm5_operand *extra1)
{
   nir_builder *b = &c->b;
   uint32_t t_slot = res_op->idx[0].imm;
   if (t_slot >= AURORA_MAX_SRV_SLOTS) {
      sm5_fail(c, "texture slot out of range");
      return;
   }
   struct sm5_res_decl *r = &c->srvs[t_slot];
   bool is_array = false;
   enum glsl_sampler_dim dim = sm5_dim_to_glsl(r->declared ? r->dim : SM5_DIM_TEXTURE2D, &is_array);
   unsigned coord_comps = sm5_coord_components(dim, is_array);

   nir_texop texop = nir_texop_tex;
   bool is_shadow = false;
   bool is_ld = false;
   bool is_gather = false;

   switch (opcode) {
   case SM5_OP_SAMPLE:
      texop = nir_texop_tex;
      break;
   case SM5_OP_SAMPLE_L:
      texop = nir_texop_txl;
      break;
   case SM5_OP_SAMPLE_B:
      texop = nir_texop_txb;
      break;
   case SM5_OP_SAMPLE_D:
      texop = nir_texop_txd;
      break;
   case SM5_OP_SAMPLE_C:
      texop = nir_texop_tex;
      is_shadow = true;
      break;
   case SM5_OP_SAMPLE_C_LZ:
      texop = nir_texop_txl;
      is_shadow = true;
      break;
   case SM5_OP_LD:
      texop = nir_texop_txf;
      is_ld = true;
      break;
   case SM5_OP_LD_MS:
      texop = nir_texop_txf_ms;
      is_ld = true;
      break;
   case SM5_OP_GATHER4:
   case SM5_OP_GATHER4_PO:
      texop = nir_texop_tg4;
      is_gather = true;
      break;
   case SM5_OP_GATHER4_C:
   case SM5_OP_GATHER4_PO_C:
      texop = nir_texop_tg4;
      is_gather = true;
      is_shadow = true;
      break;
   case SM5_OP_LOD:
      texop = nir_texop_lod;
      break;
   default:
      sm5_fail(c, "unexpected texture opcode");
      return;
   }

   /* Count tex sources */
   nir_tex_src srcs[8];
   unsigned nsrc = 0;

   nir_def *coord4 = sm5_load_src(c, coord_op, is_ld);
   nir_def *coord = nir_trim_vector(b, coord4, coord_comps);
   srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);

   if (opcode == SM5_OP_LD && dim != GLSL_SAMPLER_DIM_BUF) {
      /* D3D11 ld places mip level in coord.w (or coord.y for 1D, coord.z for 2D?
       * Spec: ld srcAddress always puts mip level in .w for 1D/2D/3D/arrays;
       * buffer has no mip level). */
      nir_def *lod = nir_channel(b, coord4, 3);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_lod, lod);
   } else if (opcode == SM5_OP_LD_MS) {
      nir_def *ms_idx = nir_channel(b, sm5_load_src(c, extra0, true), 0);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_ms_index, ms_idx);
   } else if (opcode == SM5_OP_SAMPLE_L) {
      nir_def *lod = nir_channel(b, sm5_load_src(c, extra0, false), 0);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_lod, lod);
   } else if (opcode == SM5_OP_SAMPLE_B) {
      nir_def *bias = nir_channel(b, sm5_load_src(c, extra0, false), 0);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_bias, bias);
   } else if (opcode == SM5_OP_SAMPLE_D) {
      unsigned grad_comps = coord_comps - (is_array ? 1u : 0u);
      nir_def *ddx = nir_trim_vector(b, sm5_load_src(c, extra0, false), grad_comps);
      nir_def *ddy = nir_trim_vector(b, sm5_load_src(c, extra1, false), grad_comps);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_ddx, ddx);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_ddy, ddy);
   } else if (opcode == SM5_OP_SAMPLE_C || opcode == SM5_OP_SAMPLE_C_LZ) {
      nir_def *ref = nir_channel(b, sm5_load_src(c, extra0, false), 0);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_comparator, ref);
      if (opcode == SM5_OP_SAMPLE_C_LZ)
         srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_float(b, 0.0f));
   } else if (opcode == SM5_OP_GATHER4_C) {
      nir_def *ref = nir_channel(b, sm5_load_src(c, extra0, false), 0);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_comparator, ref);
   } else if (opcode == SM5_OP_GATHER4_PO) {
      nir_def *po = nir_trim_vector(b, sm5_load_src(c, extra0, true), 2);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_offset, po);
   } else if (opcode == SM5_OP_GATHER4_PO_C) {
      nir_def *po = nir_trim_vector(b, sm5_load_src(c, extra0, true), 2);
      nir_def *ref = nir_channel(b, sm5_load_src(c, extra1, false), 0);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_offset, po);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_comparator, ref);
   }

   if (has_offset && opcode != SM5_OP_GATHER4_PO && opcode != SM5_OP_GATHER4_PO_C) {
      unsigned off_comps = coord_comps - (is_array ? 1u : 0u);
      nir_const_value cv[3];
      for (unsigned i = 0; i < off_comps && i < 3; ++i)
         cv[i] = nir_const_value_for_int(offset[i], 32);
      nir_def *off_def = nir_build_imm(b, off_comps, 32, cv);
      srcs[nsrc++] = nir_tex_src_for_ssa(nir_tex_src_offset, off_def);
   }

   nir_tex_instr *tex = nir_tex_instr_create(c->nir, nsrc);
   tex->op = texop;
   tex->sampler_dim = dim;
   tex->is_array = is_array;
   tex->is_shadow = is_shadow;
   tex->coord_components = coord_comps;
   tex->texture_index = t_slot;
   tex->sampler_index = samp_op ? samp_op->idx[0].imm : 0u;
   tex->dest_type = is_shadow ? nir_type_float32
                              : sm5_ret_to_nir_type(r->declared ? r->ret_type[0] : SM5_RET_FLOAT);
   if (is_gather) {
      /* Component to gather is encoded in the sampler operand's select1/swizzle[0]. */
      tex->component = samp_op ? samp_op->swizzle[0] : 0u;
   }
   for (unsigned i = 0; i < nsrc; ++i)
      tex->src[i] = srcs[i];

   unsigned dest_comps = (is_shadow && !is_gather) ? 1u
                       : (opcode == SM5_OP_LOD) ? 2u
                       : 4u;
   nir_def_init(&tex->instr, &tex->def, dest_comps, 32);
   nir_builder_instr_insert(b, &tex->instr);

   BITSET_SET(c->nir->info.textures_used, t_slot);
   if (samp_op) {
      uint32_t s_slot = samp_op->idx[0].imm;
      BITSET_SET(c->nir->info.samplers_used, s_slot);
      if (s_slot < 32u)
         c->info.sampler_mask |= (1u << s_slot);
   }

   nir_def *res = sm5_apply_res_swizzle(b, &tex->def, res_op);
   sm5_store_dst(c, dst, res, false);
}

static void
sm5_emit_resinfo(struct sm5_ctx *c, uint32_t tok0,
                 const struct sm5_operand *dst,
                 const struct sm5_operand *lod_op,
                 const struct sm5_operand *res_op)
{
   nir_builder *b = &c->b;
   uint32_t t_slot = res_op->idx[0].imm;
   if (t_slot >= AURORA_MAX_SRV_SLOTS) {
      sm5_fail(c, "resinfo texture slot out of range");
      return;
   }
   struct sm5_res_decl *r = &c->srvs[t_slot];
   bool is_array = false;
   enum glsl_sampler_dim dim =
      sm5_dim_to_glsl(r->declared ? r->dim : SM5_DIM_TEXTURE2D, &is_array);
   unsigned size_comps = sm5_coord_components(dim, is_array);

   nir_def *lod = nir_channel(b, sm5_load_src(c, lod_op, true), 0);

   nir_tex_instr *txs = nir_tex_instr_create(c->nir, dim == GLSL_SAMPLER_DIM_BUF ? 0 : 1);
   txs->op = nir_texop_txs;
   txs->sampler_dim = dim;
   txs->is_array = is_array;
   txs->texture_index = t_slot;
   txs->dest_type = nir_type_int32;
   if (dim != GLSL_SAMPLER_DIM_BUF)
      txs->src[0] = nir_tex_src_for_ssa(nir_tex_src_lod, lod);
   nir_def_init(&txs->instr, &txs->def, size_comps, 32);
   nir_builder_instr_insert(b, &txs->instr);

   nir_tex_instr *lvl = nir_tex_instr_create(c->nir, 0);
   lvl->op = nir_texop_query_levels;
   lvl->sampler_dim = dim;
   lvl->is_array = is_array;
   lvl->texture_index = t_slot;
   lvl->dest_type = nir_type_int32;
   nir_def_init(&lvl->instr, &lvl->def, 1, 32);
   nir_builder_instr_insert(b, &lvl->instr);

   BITSET_SET(c->nir->info.textures_used, t_slot);

   nir_def *zero = nir_imm_int(b, 0);
   nir_def *ch[4] = {
      nir_channel(b, &txs->def, 0),
      size_comps > 1 ? nir_channel(b, &txs->def, 1) : zero,
      size_comps > 2 ? nir_channel(b, &txs->def, 2) : zero,
      dim == GLSL_SAMPLER_DIM_BUF ? nir_imm_int(b, 1) : &lvl->def,
   };
   nir_def *out = nir_vec(b, ch, 4);

   enum sm5_resinfo_ret_type rt = sm5_decode_resinfo_ret(tok0);
   if (rt == SM5_RESINFO_FLOAT) {
      out = nir_u2f32(b, out);
   } else if (rt == SM5_RESINFO_RCPFLOAT) {
      nir_def *f = nir_u2f32(b, out);
      nir_def *rcp = nir_frcp(b, f);
      out = nir_vec4(b, nir_channel(b, rcp, 0), nir_channel(b, rcp, 1),
                     nir_channel(b, rcp, 2), nir_channel(b, f, 3));
   }

   sm5_store_dst(c, dst, sm5_apply_res_swizzle(b, out, res_op), false);
}

static bool
sm5_ssbo_index(struct sm5_ctx *c, const struct sm5_operand *op, uint32_t *ssbo_out,
               uint32_t *stride_out)
{
   if (op->type == SM5_OPERAND_UNORDERED_ACCESS_VIEW) {
      uint32_t u = op->idx[0].imm;
      if (u >= AURORA_MAX_UAV_SLOTS) {
         sm5_fail(c, "UAV slot out of range");
         return false;
      }
      *ssbo_out = u;
      if (stride_out)
         *stride_out = c->uavs[u].stride;
      if (u + 1 > c->nir->info.num_ssbos)
         c->nir->info.num_ssbos = u + 1;
      return true;
   }
   if (op->type == SM5_OPERAND_RESOURCE) {
      uint32_t t = op->idx[0].imm;
      if (t >= 24u) {
         sm5_fail(c, "raw/structured SRV slot >= 24 out of SSBO range");
         return false;
      }
      *ssbo_out = AURORA_SSBO_SRV_BASE + t;
      if (stride_out)
         *stride_out = c->srvs[t].stride;
      if (*ssbo_out + 1 > c->nir->info.num_ssbos)
         c->nir->info.num_ssbos = *ssbo_out + 1;
      return true;
   }
   return false;
}

static void
sm5_emit_raw_struct_load(struct sm5_ctx *c, bool is_structured,
                         const struct sm5_operand *dst,
                         const struct sm5_operand *addr_op,
                         const struct sm5_operand *off_op,
                         const struct sm5_operand *src_op)
{
   nir_builder *b = &c->b;
   nir_def *byte_off = NULL;
   uint32_t comps = util_last_bit(dst->write_mask ? dst->write_mask : 0xfu);
   if (comps == 0)
      comps = 4;

   if (src_op->type == SM5_OPERAND_THREAD_GROUP_SHARED_MEMORY) {
      uint32_t g = src_op->idx[0].imm;
      if (g >= SM5_MAX_TGSM || !c->tgsm[g].declared) {
         sm5_fail(c, "undeclared TGSM in load");
         return;
      }
      if (is_structured) {
         nir_def *idx = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
         nir_def *off = nir_channel(b, sm5_load_src(c, off_op, true), 0);
         byte_off = nir_iadd(b, nir_imul_imm(b, idx, (int32_t)c->tgsm[g].stride), off);
      } else {
         byte_off = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
      }
      byte_off = nir_iadd_imm(b, byte_off, (int32_t)c->tgsm[g].offset_bytes);
      nir_def *val = nir_load_shared(b, comps, 32, byte_off, .align_mul = 4);
      sm5_store_dst(c, dst, sm5_apply_res_swizzle(b, val, src_op), false);
      return;
   }

   uint32_t ssbo = 0, stride = 0;
   if (!sm5_ssbo_index(c, src_op, &ssbo, &stride)) {
      sm5_fail(c, "unsupported source operand in ld_raw/ld_structured");
      return;
   }
   if (is_structured) {
      nir_def *idx = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
      nir_def *off = nir_channel(b, sm5_load_src(c, off_op, true), 0);
      byte_off = nir_iadd(b, nir_imul_imm(b, idx, (int32_t)stride), off);
   } else {
      byte_off = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
   }
   nir_def *val = nir_load_ssbo(b, comps, 32, nir_imm_int(b, (int32_t)ssbo),
                                byte_off, .align_mul = 4);
   sm5_store_dst(c, dst, sm5_apply_res_swizzle(b, val, src_op), false);
}

static void
sm5_emit_raw_struct_store(struct sm5_ctx *c, bool is_structured,
                          const struct sm5_operand *dst_op,
                          const struct sm5_operand *addr_op,
                          const struct sm5_operand *off_op,
                          const struct sm5_operand *val_op)
{
   nir_builder *b = &c->b;
   nir_def *val4 = sm5_load_src(c, val_op, true);
   uint8_t mask = dst_op->write_mask ? dst_op->write_mask : 0xfu;
   unsigned comps = util_last_bit(mask);
   if (comps == 0)
      return;
   nir_def *val = nir_trim_vector(b, val4, comps);

   if (dst_op->type == SM5_OPERAND_THREAD_GROUP_SHARED_MEMORY) {
      uint32_t g = dst_op->idx[0].imm;
      if (g >= SM5_MAX_TGSM || !c->tgsm[g].declared) {
         sm5_fail(c, "undeclared TGSM in store");
         return;
      }
      nir_def *byte_off = NULL;
      if (is_structured) {
         nir_def *idx = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
         nir_def *off = nir_channel(b, sm5_load_src(c, off_op, true), 0);
         byte_off = nir_iadd(b, nir_imul_imm(b, idx, (int32_t)c->tgsm[g].stride), off);
      } else {
         byte_off = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
      }
      byte_off = nir_iadd_imm(b, byte_off, (int32_t)c->tgsm[g].offset_bytes);
      nir_store_shared(b, val, byte_off, .write_mask = mask, .align_mul = 4);
      return;
   }

   uint32_t ssbo = 0, stride = 0;
   if (!sm5_ssbo_index(c, dst_op, &ssbo, &stride)) {
      sm5_fail(c, "unsupported destination in store_raw/store_structured");
      return;
   }
   nir_def *byte_off = NULL;
   if (is_structured) {
      nir_def *idx = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
      nir_def *off = nir_channel(b, sm5_load_src(c, off_op, true), 0);
      byte_off = nir_iadd(b, nir_imul_imm(b, idx, (int32_t)stride), off);
   } else {
      byte_off = nir_channel(b, sm5_load_src(c, addr_op, true), 0);
   }
   nir_store_ssbo(b, val, nir_imm_int(b, (int32_t)ssbo), byte_off,
                  .write_mask = mask, .align_mul = 4);
   c->nir->info.writes_memory = true;
}

static void
sm5_emit_typed_uav(struct sm5_ctx *c, bool is_store,
                   const struct sm5_operand *op0,
                   const struct sm5_operand *op1,
                   const struct sm5_operand *op2)
{
   nir_builder *b = &c->b;
   const struct sm5_operand *uav_op = is_store ? op0 : op2;
   const struct sm5_operand *coord_op = op1;
   uint32_t u = uav_op->idx[0].imm;
   if (u >= AURORA_MAX_UAV_SLOTS) {
      sm5_fail(c, "typed UAV slot out of range");
      return;
   }
   struct sm5_res_decl *d = &c->uavs[u];
   bool is_array = false;
   enum glsl_sampler_dim dim =
      sm5_dim_to_glsl(d->declared ? d->dim : SM5_DIM_TEXTURE2D, &is_array);
   if (u + 1 > c->nir->info.num_images)
      c->nir->info.num_images = u + 1;

   nir_def *coord = sm5_load_src(c, coord_op, true);
   nir_def *idx = nir_imm_int(b, (int32_t)u);
   nir_def *sample = nir_imm_int(b, 0);

   if (!is_store) {
      nir_def *val = nir_image_load(
         b, 4, 32, idx, coord, sample, nir_imm_int(b, 0),
         .image_dim = dim, .image_array = is_array,
         .dest_type = sm5_ret_to_nir_type(d->declared ? d->ret_type[0] : SM5_RET_FLOAT));
      sm5_store_dst(c, op0, sm5_apply_res_swizzle(b, val, uav_op), false);
   } else {
      nir_def *val = sm5_load_src(c, op2, false);
      nir_image_store(
         b, idx, coord, sample, val, nir_imm_int(b, 0),
         .image_dim = dim, .image_array = is_array,
         .src_type = sm5_ret_to_nir_type(d->declared ? d->ret_type[0] : SM5_RET_FLOAT));
      c->nir->info.writes_memory = true;
   }
}

static void
sm5_emit_atomic(struct sm5_ctx *c, enum sm5_opcode opcode, bool has_ret)
{
   nir_builder *b = &c->b;
   struct sm5_operand dst_ret = {0}, target = {0}, addr = {0}, src0 = {0}, src1 = {0};
   bool is_cmp = (opcode == SM5_OP_ATOMIC_CMP_STORE ||
                  opcode == SM5_OP_IMM_ATOMIC_CMP_EXCH);

   if (has_ret && !sm5_parse_operand(c, &dst_ret))
      return;
   if (!sm5_parse_operand(c, &target) || !sm5_parse_operand(c, &addr) ||
       !sm5_parse_operand(c, &src0))
      return;
   if (is_cmp && !sm5_parse_operand(c, &src1))
      return;

   nir_atomic_op aop = nir_atomic_op_iadd;
   switch (opcode) {
   case SM5_OP_ATOMIC_IADD:
   case SM5_OP_IMM_ATOMIC_IADD: aop = nir_atomic_op_iadd; break;
   case SM5_OP_ATOMIC_AND:
   case SM5_OP_IMM_ATOMIC_AND:  aop = nir_atomic_op_iand; break;
   case SM5_OP_ATOMIC_OR:
   case SM5_OP_IMM_ATOMIC_OR:   aop = nir_atomic_op_ior;  break;
   case SM5_OP_ATOMIC_XOR:
   case SM5_OP_IMM_ATOMIC_XOR:  aop = nir_atomic_op_ixor; break;
   case SM5_OP_ATOMIC_IMAX:
   case SM5_OP_IMM_ATOMIC_IMAX: aop = nir_atomic_op_imax; break;
   case SM5_OP_ATOMIC_IMIN:
   case SM5_OP_IMM_ATOMIC_IMIN: aop = nir_atomic_op_imin; break;
   case SM5_OP_ATOMIC_UMAX:
   case SM5_OP_IMM_ATOMIC_UMAX: aop = nir_atomic_op_umax; break;
   case SM5_OP_ATOMIC_UMIN:
   case SM5_OP_IMM_ATOMIC_UMIN: aop = nir_atomic_op_umin; break;
   case SM5_OP_IMM_ATOMIC_EXCH: aop = nir_atomic_op_xchg; break;
   case SM5_OP_ATOMIC_CMP_STORE:
   case SM5_OP_IMM_ATOMIC_CMP_EXCH: aop = nir_atomic_op_cmpxchg; break;
   default:
      sm5_fail(c, "unsupported atomic opcode");
      return;
   }

   nir_def *a4 = sm5_load_src(c, &addr, true);
   nir_def *v0 = nir_channel(b, sm5_load_src(c, &src0, true), 0);
   nir_def *v1 = is_cmp ? nir_channel(b, sm5_load_src(c, &src1, true), 0) : NULL;
   nir_def *res = NULL;

   if (target.type == SM5_OPERAND_THREAD_GROUP_SHARED_MEMORY) {
      uint32_t g = target.idx[0].imm;
      if (g >= SM5_MAX_TGSM || !c->tgsm[g].declared) {
         sm5_fail(c, "undeclared TGSM in atomic");
         return;
      }
      nir_def *off = c->tgsm[g].stride
                        ? nir_iadd(b,
                                   nir_imul_imm(b, nir_channel(b, a4, 0),
                                                (int32_t)c->tgsm[g].stride),
                                   nir_channel(b, a4, 1))
                        : nir_channel(b, a4, 0);
      off = nir_iadd_imm(b, off, (int32_t)c->tgsm[g].offset_bytes);
      res = is_cmp ? nir_shared_atomic_swap(b, 32, off, v0, v1, .atomic_op = aop)
                   : nir_shared_atomic(b, 32, off, v0, .atomic_op = aop);
   } else if (target.type == SM5_OPERAND_UNORDERED_ACCESS_VIEW) {
      uint32_t u = target.idx[0].imm;
      if (u >= AURORA_MAX_UAV_SLOTS) {
         sm5_fail(c, "UAV atomic slot out of range");
         return;
      }
      if (c->uavs[u].is_raw || c->uavs[u].is_structured) {
         uint32_t ssbo = u;
         if (u + 1 > c->nir->info.num_ssbos)
            c->nir->info.num_ssbos = u + 1;
         nir_def *off = c->uavs[u].is_structured
                           ? nir_iadd(b,
                                      nir_imul_imm(b, nir_channel(b, a4, 0),
                                                   (int32_t)c->uavs[u].stride),
                                      nir_channel(b, a4, 1))
                           : nir_channel(b, a4, 0);
         res = is_cmp
                  ? nir_ssbo_atomic_swap(b, 32, nir_imm_int(b, (int32_t)ssbo),
                                         off, v0, v1, .atomic_op = aop)
                  : nir_ssbo_atomic(b, 32, nir_imm_int(b, (int32_t)ssbo),
                                    off, v0, .atomic_op = aop);
      } else {
         bool is_array = false;
         enum glsl_sampler_dim dim = sm5_dim_to_glsl(c->uavs[u].dim, &is_array);
         if (u + 1 > c->nir->info.num_images)
            c->nir->info.num_images = u + 1;
         res = is_cmp
                  ? nir_image_atomic_swap(b, 32, nir_imm_int(b, (int32_t)u), a4,
                                          nir_imm_int(b, 0), v0, v1,
                                          .image_dim = dim, .image_array = is_array,
                                          .atomic_op = aop)
                  : nir_image_atomic(b, 32, nir_imm_int(b, (int32_t)u), a4,
                                     nir_imm_int(b, 0), v0,
                                     .image_dim = dim, .image_array = is_array,
                                     .atomic_op = aop);
      }
      c->nir->info.writes_memory = true;
   } else {
      sm5_fail(c, "unsupported target in atomic");
      return;
   }

   if (has_ret && res)
      sm5_store_dst(c, &dst_ret, nir_replicate(b, res, 4), false);
}

/* Pre-scan declarations and detect whether any non-final RET/RETC exists. */
static bool
sm5_prescan(struct sm5_ctx *c)
{
   size_t pos = 2;
   while (pos < c->num_dwords) {
      uint32_t tok0 = c->code[pos];
      enum sm5_opcode op = sm5_decode_opcode(tok0);
      size_t inst_len = 0;

      if (op == SM5_OP_CUSTOMDATA) {
         if (pos + 1 >= c->num_dwords) {
            sm5_fail(c, "truncated customdata block");
            return false;
         }
         inst_len = c->code[pos + 1];
         if (inst_len < 2 || pos + inst_len > c->num_dwords) {
            sm5_fail(c, "invalid customdata length");
            return false;
         }
         enum sm5_customdata_class cls =
            (enum sm5_customdata_class)((tok0 >> 11) & 0x1fffffu);
         if (cls == SM5_CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER) {
            size_t payload = inst_len - 2;
            c->icb_data = &c->code[pos + 2];
            c->icb_vec4_count = (uint32_t)(payload / 4u);
         }
         pos += inst_len;
         continue;
      }

      inst_len = sm5_decode_inst_len(tok0);
      if (inst_len == 0 || pos + inst_len > c->num_dwords) {
         sm5_fail(c, "invalid instruction length in prescan");
         return false;
      }

      if (op == SM5_OP_RETC || (op == SM5_OP_RET && pos + inst_len < c->num_dwords))
         c->has_early_ret = true;

      pos += inst_len;
   }
   return true;
}

static void
sm5_close_switch_case(struct sm5_ctx *c, struct sm5_cf_frame *f)
{
   if (f->sw_in_case) {
      nir_pop_if(&c->b, f->sw_cur_if);
      f->sw_in_case = false;
      f->sw_cur_if = NULL;
   }
}

static bool
sm5_translate_inst(struct sm5_ctx *c)
{
   nir_builder *b = &c->b;
   c->rel_pool_used = 0;

   size_t inst_start = c->pos;
   uint32_t tok0 = 0;
   if (!sm5_read_u32(c, &tok0))
      return false;

   enum sm5_opcode op = sm5_decode_opcode(tok0);
   if (op == SM5_OP_CUSTOMDATA) {
      uint32_t len = 0;
      if (!sm5_read_u32(c, &len) || len < 2 || inst_start + len > c->num_dwords) {
         sm5_fail(c, "invalid customdata block");
         return false;
      }
      c->pos = inst_start + len;
      return true;
   }

   uint32_t inst_len = sm5_decode_inst_len(tok0);
   if (inst_len == 0 || inst_start + inst_len > c->num_dwords) {
      sm5_fail(c, "invalid instruction length");
      return false;
   }
   size_t inst_end = inst_start + inst_len;

   /* Extended opcode tokens (sample offsets, resource dim/return overrides) */
   bool has_tex_offset = false;
   int tex_offset[3] = {0, 0, 0};
   bool ext = sm5_decode_extended(tok0);
   while (ext) {
      uint32_t etok = 0;
      if (!sm5_read_u32(c, &etok))
         return false;
      enum sm5_ext_opcode_type etype = (enum sm5_ext_opcode_type)(etok & 0x3fu);
      if (etype == SM5_EXT_OP_SAMPLE_CONTROLS) {
         has_tex_offset = true;
         tex_offset[0] = sm5_sign_extend_4(etok >> 9);
         tex_offset[1] = sm5_sign_extend_4(etok >> 13);
         tex_offset[2] = sm5_sign_extend_4(etok >> 17);
      }
      ext = (etok >> 31) != 0;
   }

   bool sat = sm5_decode_saturate(tok0);
   struct sm5_operand d0, d1, s0, s1, s2, s3, s4;

   switch (op) {
   /* --- Declarations --- */
   case SM5_OP_DCL_GLOBAL_FLAGS:
   case SM5_OP_DCL_INDEX_RANGE:
   case SM5_OP_NOP:
   case SM5_OP_ABORT:
   case SM5_OP_DEBUG_BREAK:
      break;

   case SM5_OP_DCL_TEMPS: {
      uint32_t count = 0;
      if (!sm5_read_u32(c, &count))
         return false;
      sm5_ensure_temp_count(c, count);
      break;
   }

   case SM5_OP_DCL_INDEXABLE_TEMP: {
      uint32_t idx = 0, count = 0, comps = 0;
      if (!sm5_read_u32(c, &idx) || !sm5_read_u32(c, &count) ||
          !sm5_read_u32(c, &comps))
         return false;
      if (idx >= SM5_MAX_IDX_TEMPS || count == 0) {
         sm5_fail(c, "invalid indexable temp declaration");
         return false;
      }
      char name[20];
      snprintf(name, sizeof(name), "x%u", idx);
      c->idx_temps[idx] = nir_local_variable_create(
         c->b.impl, glsl_array_type(glsl_uvec4_type(), count, 0), name);
      c->idx_temp_count[idx] = count;
      break;
   }

   case SM5_OP_DCL_CONSTANT_BUFFER: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < AURORA_MAX_CB_SLOTS)
         c->info.cb_mask |= (1u << slot);
      if (slot + 1 > c->nir->info.num_ubos)
         c->nir->info.num_ubos = slot + 1;
      break;
   }

   case SM5_OP_DCL_SAMPLER: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < 32u)
         c->info.sampler_mask |= (1u << slot);
      break;
   }

   case SM5_OP_DCL_RESOURCE: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      uint32_t rtt = 0;
      if (!sm5_read_u32(c, &rtt))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < AURORA_MAX_SRV_SLOTS) {
         c->srvs[slot].declared = true;
         c->srvs[slot].dim = sm5_decode_dcl_res_dim(tok0);
         c->srvs[slot].samples = (tok0 >> 16) & 0x7fu;
         for (unsigned k = 0; k < 4; ++k)
            c->srvs[slot].ret_type[k] = (enum sm5_return_type)((rtt >> (4u * k)) & 0xfu);
      }
      break;
   }

   case SM5_OP_DCL_RESOURCE_RAW: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < 32u) {
         c->srvs[slot].declared = true;
         c->srvs[slot].dim = SM5_DIM_RAW_BUFFER;
         c->srvs[slot].is_raw = true;
         c->info.srv_raw_or_struct_mask |= (1u << slot);
      }
      break;
   }

   case SM5_OP_DCL_RESOURCE_STRUCTURED: {
      uint32_t stride = 0;
      if (!sm5_parse_operand(c, &s0) || !sm5_read_u32(c, &stride))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < 32u) {
         c->srvs[slot].declared = true;
         c->srvs[slot].dim = SM5_DIM_STRUCTURED_BUFFER;
         c->srvs[slot].is_structured = true;
         c->srvs[slot].stride = stride;
         c->info.srv_raw_or_struct_mask |= (1u << slot);
         c->info.srv_stride[slot] = stride;
      }
      break;
   }

   case SM5_OP_DCL_UNORDERED_ACCESS_VIEW_TYPED: {
      uint32_t rtt = 0;
      if (!sm5_parse_operand(c, &s0) || !sm5_read_u32(c, &rtt))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < AURORA_MAX_UAV_SLOTS) {
         c->uavs[slot].declared = true;
         c->uavs[slot].dim = sm5_decode_dcl_res_dim(tok0);
         for (unsigned k = 0; k < 4; ++k)
            c->uavs[slot].ret_type[k] = (enum sm5_return_type)((rtt >> (4u * k)) & 0xfu);
         c->info.uav_typed_mask |= (1u << slot);
      }
      break;
   }

   case SM5_OP_DCL_UNORDERED_ACCESS_VIEW_RAW: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < AURORA_MAX_UAV_SLOTS) {
         c->uavs[slot].declared = true;
         c->uavs[slot].dim = SM5_DIM_RAW_BUFFER;
         c->uavs[slot].is_raw = true;
         c->info.uav_raw_or_struct_mask |= (1u << slot);
      }
      break;
   }

   case SM5_OP_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED: {
      uint32_t stride = 0;
      if (!sm5_parse_operand(c, &s0) || !sm5_read_u32(c, &stride))
         return false;
      uint32_t slot = s0.idx[0].imm;
      if (slot < AURORA_MAX_UAV_SLOTS) {
         c->uavs[slot].declared = true;
         c->uavs[slot].dim = SM5_DIM_STRUCTURED_BUFFER;
         c->uavs[slot].is_structured = true;
         c->uavs[slot].stride = stride;
         c->info.uav_raw_or_struct_mask |= (1u << slot);
         c->info.uav_stride[slot] = stride;
      }
      break;
   }

   case SM5_OP_DCL_THREAD_GROUP: {
      uint32_t x = 1, y = 1, z = 1;
      if (!sm5_read_u32(c, &x) || !sm5_read_u32(c, &y) || !sm5_read_u32(c, &z))
         return false;
      c->nir->info.workgroup_size[0] = (uint16_t)x;
      c->nir->info.workgroup_size[1] = (uint16_t)y;
      c->nir->info.workgroup_size[2] = (uint16_t)z;
      c->info.workgroup_size[0] = x;
      c->info.workgroup_size[1] = y;
      c->info.workgroup_size[2] = z;
      break;
   }

   case SM5_OP_DCL_THREAD_GROUP_SHARED_MEMORY_RAW: {
      uint32_t bytes = 0;
      if (!sm5_parse_operand(c, &s0) || !sm5_read_u32(c, &bytes))
         return false;
      uint32_t g = s0.idx[0].imm;
      if (g < SM5_MAX_TGSM) {
         c->tgsm[g].declared = true;
         c->tgsm[g].size_bytes = bytes;
         c->tgsm[g].stride = 0;
         c->tgsm[g].offset_bytes = c->total_shared_bytes;
         c->total_shared_bytes = ALIGN_POT(c->total_shared_bytes + bytes, 16u);
         c->nir->info.shared_size = c->total_shared_bytes;
      }
      break;
   }

   case SM5_OP_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED: {
      uint32_t stride = 0, count = 0;
      if (!sm5_parse_operand(c, &s0) || !sm5_read_u32(c, &stride) ||
          !sm5_read_u32(c, &count))
         return false;
      uint32_t g = s0.idx[0].imm;
      if (g < SM5_MAX_TGSM) {
         uint32_t bytes = stride * count;
         c->tgsm[g].declared = true;
         c->tgsm[g].size_bytes = bytes;
         c->tgsm[g].stride = stride;
         c->tgsm[g].offset_bytes = c->total_shared_bytes;
         c->total_shared_bytes = ALIGN_POT(c->total_shared_bytes + bytes, 16u);
         c->nir->info.shared_size = c->total_shared_bytes;
      }
      break;
   }

   case SM5_OP_DCL_INPUT:
   case SM5_OP_DCL_INPUT_SGV:
   case SM5_OP_DCL_INPUT_SIV:
   case SM5_OP_DCL_INPUT_PS:
   case SM5_OP_DCL_INPUT_PS_SGV:
   case SM5_OP_DCL_INPUT_PS_SIV: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      uint32_t sv_tok = 0;
      if (op == SM5_OP_DCL_INPUT_SGV || op == SM5_OP_DCL_INPUT_SIV ||
          op == SM5_OP_DCL_INPUT_PS_SGV || op == SM5_OP_DCL_INPUT_PS_SIV) {
         if (!sm5_read_u32(c, &sv_tok))
            return false;
      }
      if (s0.type == SM5_OPERAND_INPUT && s0.idx[0].imm < SM5_MAX_IO_REGS) {
         uint32_t reg = s0.idx[0].imm;
         c->inputs[reg].declared = true;
         c->inputs[reg].mask |= s0.write_mask;
         if (sv_tok)
            c->inputs[reg].sysval = (enum sm5_sysval_name)(sv_tok & 0xffffu);
         if (op == SM5_OP_DCL_INPUT_PS || op == SM5_OP_DCL_INPUT_PS_SGV ||
             op == SM5_OP_DCL_INPUT_PS_SIV)
            c->inputs[reg].interp = sm5_decode_interp_mode(tok0);
      }
      break;
   }

   case SM5_OP_DCL_OUTPUT:
   case SM5_OP_DCL_OUTPUT_SGV:
   case SM5_OP_DCL_OUTPUT_SIV: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      uint32_t sv_tok = 0;
      if (op == SM5_OP_DCL_OUTPUT_SGV || op == SM5_OP_DCL_OUTPUT_SIV) {
         if (!sm5_read_u32(c, &sv_tok))
            return false;
      }
      if (s0.type == SM5_OPERAND_OUTPUT && s0.idx[0].imm < SM5_MAX_IO_REGS) {
         uint32_t reg = s0.idx[0].imm;
         c->outputs[reg].declared = true;
         c->outputs[reg].mask |= s0.write_mask;
         if (sv_tok)
            c->outputs[reg].sysval = (enum sm5_sysval_name)(sv_tok & 0xffffu);
      }
      break;
   }

   /* --- Floating-Point ALU --- */
   case SM5_OP_MOV: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, sm5_load_src(c, &s0, false), sat);
      break;
   }
   case SM5_OP_MOVC: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      nir_def *cond = nir_ine_imm(b, sm5_load_src(c, &s0, true), 0);
      nir_def *a = sm5_load_src(c, &s1, false);
      nir_def *bv = sm5_load_src(c, &s2, false);
      sm5_store_dst(c, &d0, nir_bcsel(b, cond, a, bv), sat);
      break;
   }
   case SM5_OP_SWAPC: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &d1) ||
          !sm5_parse_operand(c, &s0) || !sm5_parse_operand(c, &s1) ||
          !sm5_parse_operand(c, &s2))
         return false;
      nir_def *cond = nir_ine_imm(b, sm5_load_src(c, &s0, true), 0);
      nir_def *a = sm5_load_src(c, &s1, false);
      nir_def *bv = sm5_load_src(c, &s2, false);
      sm5_store_dst(c, &d0, nir_bcsel(b, cond, bv, a), sat);
      sm5_store_dst(c, &d1, nir_bcsel(b, cond, a, bv), sat);
      break;
   }
   case SM5_OP_ADD: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_store_dst(c, &d0,
                    nir_fadd(b, sm5_load_src(c, &s0, false),
                             sm5_load_src(c, &s1, false)),
                    sat);
      break;
   }
   case SM5_OP_MUL: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_store_dst(c, &d0,
                    nir_fmul(b, sm5_load_src(c, &s0, false),
                             sm5_load_src(c, &s1, false)),
                    sat);
      break;
   }
   case SM5_OP_MAD: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      sm5_store_dst(c, &d0,
                    nir_ffma(b, sm5_load_src(c, &s0, false),
                             sm5_load_src(c, &s1, false),
                             sm5_load_src(c, &s2, false)),
                    sat);
      break;
   }
   case SM5_OP_DIV: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_store_dst(c, &d0,
                    nir_fdiv(b, sm5_load_src(c, &s0, false),
                             sm5_load_src(c, &s1, false)),
                    sat);
      break;
   }
   case SM5_OP_RCP: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_frcp(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_DP2:
   case SM5_OP_DP3:
   case SM5_OP_DP4: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      unsigned n = (op == SM5_OP_DP2) ? 2u : (op == SM5_OP_DP3) ? 3u : 4u;
      nir_def *a = nir_trim_vector(b, sm5_load_src(c, &s0, false), n);
      nir_def *bv = nir_trim_vector(b, sm5_load_src(c, &s1, false), n);
      nir_def *dot = nir_fdot(b, a, bv);
      sm5_store_dst(c, &d0, nir_replicate(b, dot, 4), sat);
      break;
   }
   case SM5_OP_MIN: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_store_dst(c, &d0,
                    nir_fmin(b, sm5_load_src(c, &s0, false),
                             sm5_load_src(c, &s1, false)),
                    sat);
      break;
   }
   case SM5_OP_MAX: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_store_dst(c, &d0,
                    nir_fmax(b, sm5_load_src(c, &s0, false),
                             sm5_load_src(c, &s1, false)),
                    sat);
      break;
   }
   case SM5_OP_SQRT: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_fsqrt(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_RSQ: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_frsq(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_EXP: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_fexp2(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_LOG: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_flog2(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_FRC: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ffract(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_ROUND_NE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_fround_even(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_ROUND_NI: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ffloor(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_ROUND_PI: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_fceil(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_ROUND_Z: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ftrunc(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_SINCOS: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &d1) ||
          !sm5_parse_operand(c, &s0))
         return false;
      nir_def *a = sm5_load_src(c, &s0, false);
      if (d0.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d0, nir_fsin(b, a), sat);
      if (d1.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d1, nir_fcos(b, a), sat);
      break;
   }
   case SM5_OP_DERIV_RTX:
   case SM5_OP_DERIV_RTX_COARSE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ddx_coarse(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_DERIV_RTX_FINE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ddx_fine(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_DERIV_RTY:
   case SM5_OP_DERIV_RTY_COARSE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ddy_coarse(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }
   case SM5_OP_DERIV_RTY_FINE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ddy_fine(b, sm5_load_src(c, &s0, false)), sat);
      break;
   }

   /* --- Comparisons (produce 0xFFFFFFFF / 0x00000000) --- */
   case SM5_OP_EQ:
   case SM5_OP_NE:
   case SM5_OP_LT:
   case SM5_OP_GE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, false);
      nir_def *bv = sm5_load_src(c, &s1, false);
      nir_def *cmp = (op == SM5_OP_EQ)   ? nir_feq(b, a, bv)
                     : (op == SM5_OP_NE) ? nir_fneu(b, a, bv)
                     : (op == SM5_OP_LT) ? nir_flt(b, a, bv)
                                         : nir_fge(b, a, bv);
      sm5_store_dst(c, &d0,
                    nir_bcsel(b, cmp, nir_imm_int(b, -1), nir_imm_int(b, 0)),
                    false);
      break;
   }
   case SM5_OP_IEQ:
   case SM5_OP_INE:
   case SM5_OP_ILT:
   case SM5_OP_IGE:
   case SM5_OP_ULT:
   case SM5_OP_UGE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *bv = sm5_load_src(c, &s1, true);
      nir_def *cmp = (op == SM5_OP_IEQ)   ? nir_ieq(b, a, bv)
                     : (op == SM5_OP_INE) ? nir_ine(b, a, bv)
                     : (op == SM5_OP_ILT) ? nir_ilt(b, a, bv)
                     : (op == SM5_OP_IGE) ? nir_ige(b, a, bv)
                     : (op == SM5_OP_ULT) ? nir_ult(b, a, bv)
                                          : nir_uge(b, a, bv);
      sm5_store_dst(c, &d0,
                    nir_bcsel(b, cmp, nir_imm_int(b, -1), nir_imm_int(b, 0)),
                    false);
      break;
   }

   /* --- Conversions --- */
   case SM5_OP_FTOI: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_f2i32(b, sm5_load_src(c, &s0, false)), false);
      break;
   }
   case SM5_OP_FTOU: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_f2u32(b, sm5_load_src(c, &s0, false)), false);
      break;
   }
   case SM5_OP_ITOF: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_i2f32(b, sm5_load_src(c, &s0, true)), sat);
      break;
   }
   case SM5_OP_UTOF: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_u2f32(b, sm5_load_src(c, &s0, true)), sat);
      break;
   }
   case SM5_OP_F32TOF16: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      nir_def *h = nir_f2f16(b, sm5_load_src(c, &s0, false));
      sm5_store_dst(c, &d0, nir_u2u32(b, h), false);
      break;
   }
   case SM5_OP_F16TOF32: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      nir_def *h = nir_u2u16(b, sm5_load_src(c, &s0, true));
      sm5_store_dst(c, &d0, nir_f2f32(b, h), sat);
      break;
   }

   /* --- Integer & Bitwise ALU --- */
   case SM5_OP_IADD: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_store_dst(c, &d0,
                    nir_iadd(b, sm5_load_src(c, &s0, true),
                             sm5_load_src(c, &s1, true)),
                    false);
      break;
   }
   case SM5_OP_INEG: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_ineg(b, sm5_load_src(c, &s0, true)), false);
      break;
   }
   case SM5_OP_IMAD:
   case SM5_OP_UMAD: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      nir_def *m = nir_imul(b, sm5_load_src(c, &s0, true),
                            sm5_load_src(c, &s1, true));
      sm5_store_dst(c, &d0, nir_iadd(b, m, sm5_load_src(c, &s2, true)), false);
      break;
   }
   case SM5_OP_IMUL:
   case SM5_OP_UMUL: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &d1) ||
          !sm5_parse_operand(c, &s0) || !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *bv = sm5_load_src(c, &s1, true);
      if (d0.type != SM5_OPERAND_NULL) {
         sm5_store_dst(c, &d0,
                       (op == SM5_OP_IMUL) ? nir_imul_high(b, a, bv)
                                           : nir_umul_high(b, a, bv),
                       false);
      }
      if (d1.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d1, nir_imul(b, a, bv), false);
      break;
   }
   case SM5_OP_UDIV: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &d1) ||
          !sm5_parse_operand(c, &s0) || !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *bv = sm5_load_src(c, &s1, true);
      if (d0.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d0, nir_udiv(b, a, bv), false);
      if (d1.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d1, nir_umod(b, a, bv), false);
      break;
   }
   case SM5_OP_UADDC: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &d1) ||
          !sm5_parse_operand(c, &s0) || !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *bv = sm5_load_src(c, &s1, true);
      if (d0.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d0, nir_iadd(b, a, bv), false);
      if (d1.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d1, nir_uadd_carry(b, a, bv), false);
      break;
   }
   case SM5_OP_USUBB: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &d1) ||
          !sm5_parse_operand(c, &s0) || !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *bv = sm5_load_src(c, &s1, true);
      if (d0.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d0, nir_isub(b, a, bv), false);
      if (d1.type != SM5_OPERAND_NULL)
         sm5_store_dst(c, &d1, nir_usub_borrow(b, a, bv), false);
      break;
   }
   case SM5_OP_IMIN:
   case SM5_OP_IMAX:
   case SM5_OP_UMIN:
   case SM5_OP_UMAX: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *bv = sm5_load_src(c, &s1, true);
      nir_def *r = (op == SM5_OP_IMIN)   ? nir_imin(b, a, bv)
                   : (op == SM5_OP_IMAX) ? nir_imax(b, a, bv)
                   : (op == SM5_OP_UMIN) ? nir_umin(b, a, bv)
                                         : nir_umax(b, a, bv);
      sm5_store_dst(c, &d0, r, false);
      break;
   }
   case SM5_OP_AND:
   case SM5_OP_OR:
   case SM5_OP_XOR: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *bv = sm5_load_src(c, &s1, true);
      nir_def *r = (op == SM5_OP_AND)  ? nir_iand(b, a, bv)
                   : (op == SM5_OP_OR) ? nir_ior(b, a, bv)
                                       : nir_ixor(b, a, bv);
      sm5_store_dst(c, &d0, r, false);
      break;
   }
   case SM5_OP_NOT: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_inot(b, sm5_load_src(c, &s0, true)), false);
      break;
   }
   case SM5_OP_ISHL:
   case SM5_OP_ISHR:
   case SM5_OP_USHR: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      nir_def *a = sm5_load_src(c, &s0, true);
      nir_def *sh = nir_iand_imm(b, sm5_load_src(c, &s1, true), 0x1f);
      nir_def *r = (op == SM5_OP_ISHL)   ? nir_ishl(b, a, sh)
                   : (op == SM5_OP_ISHR) ? nir_ishr(b, a, sh)
                                         : nir_ushr(b, a, sh);
      sm5_store_dst(c, &d0, r, false);
      break;
   }
   case SM5_OP_COUNTBITS: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_bit_count(b, sm5_load_src(c, &s0, true)), false);
      break;
   }
   case SM5_OP_BFREV: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_bitfield_reverse(b, sm5_load_src(c, &s0, true)),
                    false);
      break;
   }
   case SM5_OP_FIRSTBIT_LO: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      sm5_store_dst(c, &d0, nir_find_lsb(b, sm5_load_src(c, &s0, true)), false);
      break;
   }
   case SM5_OP_FIRSTBIT_HI: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      /* D3D11 firstbit_hi counts from MSB (31 - find_msb), returning ~0u for 0. */
      nir_def *v = sm5_load_src(c, &s0, true);
      nir_def *msb = nir_ufind_msb(b, v);
      nir_def *res = nir_bcsel(b, nir_ieq_imm(b, v, 0), nir_imm_int(b, -1),
                               nir_isub(b, nir_imm_int(b, 31), msb));
      sm5_store_dst(c, &d0, res, false);
      break;
   }
   case SM5_OP_FIRSTBIT_SHI: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      nir_def *v = sm5_load_src(c, &s0, true);
      nir_def *msb = nir_ifind_msb(b, v);
      nir_def *res = nir_bcsel(b, nir_ieq_imm(b, msb, -1), nir_imm_int(b, -1),
                               nir_isub(b, nir_imm_int(b, 31), msb));
      sm5_store_dst(c, &d0, res, false);
      break;
   }
   case SM5_OP_UBFE:
   case SM5_OP_IBFE: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      nir_def *width = nir_iand_imm(b, sm5_load_src(c, &s0, true), 0x1f);
      nir_def *off = nir_iand_imm(b, sm5_load_src(c, &s1, true), 0x1f);
      nir_def *val = sm5_load_src(c, &s2, true);
      nir_def *r = (op == SM5_OP_UBFE) ? nir_ubfe(b, val, off, width)
                                       : nir_ibfe(b, val, off, width);
      sm5_store_dst(c, &d0, r, false);
      break;
   }
   case SM5_OP_BFI: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2) ||
          !sm5_parse_operand(c, &s3))
         return false;
      nir_def *width = nir_iand_imm(b, sm5_load_src(c, &s0, true), 0x1f);
      nir_def *off = nir_iand_imm(b, sm5_load_src(c, &s1, true), 0x1f);
      nir_def *ins = sm5_load_src(c, &s2, true);
      nir_def *base = sm5_load_src(c, &s3, true);
      sm5_store_dst(c, &d0, nir_bitfield_insert(b, base, ins, off, width), false);
      break;
   }

   /* --- Texture / Sampling / Resource Info --- */
   case SM5_OP_SAMPLE:
   case SM5_OP_GATHER4:
   case SM5_OP_LOD: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      sm5_emit_tex(c, op, has_tex_offset, tex_offset, &d0, &s0, &s1, &s2, NULL, NULL);
      break;
   }
   case SM5_OP_SAMPLE_L:
   case SM5_OP_SAMPLE_B:
   case SM5_OP_SAMPLE_C:
   case SM5_OP_SAMPLE_C_LZ:
   case SM5_OP_GATHER4_C: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2) ||
          !sm5_parse_operand(c, &s3))
         return false;
      sm5_emit_tex(c, op, has_tex_offset, tex_offset, &d0, &s0, &s1, &s2, &s3, NULL);
      break;
   }
   case SM5_OP_SAMPLE_D: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2) ||
          !sm5_parse_operand(c, &s3) || !sm5_parse_operand(c, &s4))
         return false;
      sm5_emit_tex(c, op, has_tex_offset, tex_offset, &d0, &s0, &s1, &s2, &s3, &s4);
      break;
   }
   case SM5_OP_GATHER4_PO: {
      /* gather4_po dst, srcCoord, srcOffset, srcResource, srcSampler */
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2) ||
          !sm5_parse_operand(c, &s3))
         return false;
      sm5_emit_tex(c, op, false, tex_offset, &d0, &s0, &s2, &s3, &s1, NULL);
      break;
   }
   case SM5_OP_GATHER4_PO_C: {
      /* gather4_po_c dst, srcCoord, srcOffset, srcResource, srcSampler, srcRef */
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2) ||
          !sm5_parse_operand(c, &s3) || !sm5_parse_operand(c, &s4))
         return false;
      sm5_emit_tex(c, op, false, tex_offset, &d0, &s0, &s2, &s3, &s1, &s4);
      break;
   }
   case SM5_OP_LD: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_emit_tex(c, op, has_tex_offset, tex_offset, &d0, &s0, &s1, NULL, NULL, NULL);
      break;
   }
   case SM5_OP_LD_MS: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      sm5_emit_tex(c, op, has_tex_offset, tex_offset, &d0, &s0, &s1, NULL, &s2, NULL);
      break;
   }
   case SM5_OP_RESINFO: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_emit_resinfo(c, tok0, &d0, &s0, &s1);
      break;
   }
   case SM5_OP_BUFINFO: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0))
         return false;
      uint32_t ssbo = 0, stride = 0;
      if (sm5_ssbo_index(c, &s0, &ssbo, &stride)) {
         nir_def *sz = nir_get_ssbo_size(b, 32, nir_imm_int(b, (int32_t)ssbo));
         if (stride > 0)
            sz = nir_udiv_imm(b, sz, stride);
         sm5_store_dst(c, &d0, nir_replicate(b, sz, 4), false);
      } else if (s0.type == SM5_OPERAND_RESOURCE) {
         struct sm5_operand lod0 = {
            .type = SM5_OPERAND_IMMEDIATE32, .num_comps = SM5_COMP_1,
         };
         sm5_emit_resinfo(c, 2u << 11, &d0, &lod0, &s0);
      } else {
         sm5_store_dst(c, &d0, nir_imm_ivec4(b, 0, 0, 0, 0), false);
      }
      break;
   }

   /* --- Raw / Structured / Typed UAV / TGSM / Atomics / Sync --- */
   case SM5_OP_LD_RAW: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_emit_raw_struct_load(c, false, &d0, &s0, NULL, &s1);
      break;
   }
   case SM5_OP_LD_STRUCTURED: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      sm5_emit_raw_struct_load(c, true, &d0, &s0, &s1, &s2);
      break;
   }
   case SM5_OP_STORE_RAW: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_emit_raw_struct_store(c, false, &d0, &s0, NULL, &s1);
      break;
   }
   case SM5_OP_STORE_STRUCTURED: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1) || !sm5_parse_operand(c, &s2))
         return false;
      sm5_emit_raw_struct_store(c, true, &d0, &s0, &s1, &s2);
      break;
   }
   case SM5_OP_LD_UAV_TYPED: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_emit_typed_uav(c, false, &d0, &s0, &s1);
      break;
   }
   case SM5_OP_STORE_UAV_TYPED: {
      if (!sm5_parse_operand(c, &d0) || !sm5_parse_operand(c, &s0) ||
          !sm5_parse_operand(c, &s1))
         return false;
      sm5_emit_typed_uav(c, true, &d0, &s0, &s1);
      break;
   }
   case SM5_OP_ATOMIC_AND:
   case SM5_OP_ATOMIC_OR:
   case SM5_OP_ATOMIC_XOR:
   case SM5_OP_ATOMIC_CMP_STORE:
   case SM5_OP_ATOMIC_IADD:
   case SM5_OP_ATOMIC_IMAX:
   case SM5_OP_ATOMIC_IMIN:
   case SM5_OP_ATOMIC_UMAX:
   case SM5_OP_ATOMIC_UMIN:
      sm5_emit_atomic(c, op, false);
      break;
   case SM5_OP_IMM_ATOMIC_IADD:
   case SM5_OP_IMM_ATOMIC_AND:
   case SM5_OP_IMM_ATOMIC_OR:
   case SM5_OP_IMM_ATOMIC_XOR:
   case SM5_OP_IMM_ATOMIC_EXCH:
   case SM5_OP_IMM_ATOMIC_CMP_EXCH:
   case SM5_OP_IMM_ATOMIC_IMAX:
   case SM5_OP_IMM_ATOMIC_IMIN:
   case SM5_OP_IMM_ATOMIC_UMAX:
   case SM5_OP_IMM_ATOMIC_UMIN:
      sm5_emit_atomic(c, op, true);
      break;
   case SM5_OP_SYNC: {
      mesa_scope exec_scope =
         (tok0 & SM5_SYNC_THREADS_IN_GROUP) ? SCOPE_WORKGROUP : SCOPE_NONE;
      mesa_scope mem_scope =
         (tok0 & SM5_SYNC_UAV_GLOBAL) ? SCOPE_DEVICE
         : ((tok0 & (SM5_SYNC_TGSM | SM5_SYNC_UAV_GROUP)) ? SCOPE_WORKGROUP
                                                          : SCOPE_NONE);
      nir_variable_mode modes = 0;
      if (tok0 & SM5_SYNC_TGSM)
         modes |= nir_var_mem_shared;
      if (tok0 & (SM5_SYNC_UAV_GROUP | SM5_SYNC_UAV_GLOBAL))
         modes |= nir_var_mem_ssbo | nir_var_image;
      nir_barrier(b, .execution_scope = exec_scope, .memory_scope = mem_scope,
                  .memory_semantics = mem_scope != SCOPE_NONE
                                         ? (NIR_MEMORY_ACQ_REL | NIR_MEMORY_MAKE_AVAILABLE |
                                            NIR_MEMORY_MAKE_VISIBLE)
                                         : 0,
                  .memory_modes = modes);
      break;
   }

   /* --- Control Flow --- */
   case SM5_OP_DISCARD: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      nir_def *v = nir_channel(b, sm5_load_src(c, &s0, true), 0);
      nir_def *cond = sm5_decode_test_nz(tok0) ? nir_ine_imm(b, v, 0)
                                               : nir_ieq_imm(b, v, 0);
      nir_discard_if(b, cond);
      c->nir->info.fs.uses_discard = true;
      break;
   }
   case SM5_OP_IF: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      if (c->cf_depth >= SM5_MAX_CF_DEPTH) {
         sm5_fail(c, "control flow nesting exceeded");
         return false;
      }
      nir_def *v = nir_channel(b, sm5_load_src(c, &s0, true), 0);
      nir_def *cond = sm5_decode_test_nz(tok0) ? nir_ine_imm(b, v, 0)
                                               : nir_ieq_imm(b, v, 0);
      nir_if *nif = nir_push_if(b, cond);
      c->cf[c->cf_depth++] = (struct sm5_cf_frame){.kind = SM5_CF_IF, .nif = nif};
      break;
   }
   case SM5_OP_ELSE: {
      if (c->cf_depth == 0 || c->cf[c->cf_depth - 1].kind != SM5_CF_IF) {
         sm5_fail(c, "unmatched else");
         return false;
      }
      nir_push_else(b, c->cf[c->cf_depth - 1].nif);
      break;
   }
   case SM5_OP_ENDIF: {
      if (c->cf_depth == 0 || c->cf[c->cf_depth - 1].kind != SM5_CF_IF) {
         sm5_fail(c, "unmatched endif");
         return false;
      }
      nir_pop_if(b, c->cf[--c->cf_depth].nif);
      break;
   }
   case SM5_OP_LOOP: {
      if (c->cf_depth >= SM5_MAX_CF_DEPTH) {
         sm5_fail(c, "control flow nesting exceeded");
         return false;
      }
      nir_loop *loop = nir_push_loop(b);
      c->cf[c->cf_depth++] = (struct sm5_cf_frame){.kind = SM5_CF_LOOP, .loop = loop};
      break;
   }
   case SM5_OP_ENDLOOP: {
      if (c->cf_depth == 0 || c->cf[c->cf_depth - 1].kind != SM5_CF_LOOP) {
         sm5_fail(c, "unmatched endloop");
         return false;
      }
      nir_pop_loop(b, c->cf[--c->cf_depth].loop);
      break;
   }
   case SM5_OP_BREAK: {
      nir_jump(b, nir_jump_break);
      break;
   }
   case SM5_OP_BREAKC: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      nir_def *v = nir_channel(b, sm5_load_src(c, &s0, true), 0);
      nir_def *cond = sm5_decode_test_nz(tok0) ? nir_ine_imm(b, v, 0)
                                               : nir_ieq_imm(b, v, 0);
      nir_push_if(b, cond);
      nir_jump(b, nir_jump_break);
      nir_pop_if(b, NULL);
      break;
   }
   case SM5_OP_CONTINUE: {
      nir_jump(b, nir_jump_continue);
      break;
   }
   case SM5_OP_CONTINUEC: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      nir_def *v = nir_channel(b, sm5_load_src(c, &s0, true), 0);
      nir_def *cond = sm5_decode_test_nz(tok0) ? nir_ine_imm(b, v, 0)
                                               : nir_ieq_imm(b, v, 0);
      nir_push_if(b, cond);
      nir_jump(b, nir_jump_continue);
      nir_pop_if(b, NULL);
      break;
   }
   case SM5_OP_SWITCH: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      if (c->cf_depth >= SM5_MAX_CF_DEPTH) {
         sm5_fail(c, "control flow nesting exceeded");
         return false;
      }
      nir_def *sv = nir_channel(b, sm5_load_src(c, &s0, true), 0);
      nir_variable *matched =
         nir_local_variable_create(b->impl, glsl_bool_type(), "sw_matched");
      nir_store_var(b, matched, nir_imm_false(b), 1);
      nir_loop *loop = nir_push_loop(b);
      c->cf[c->cf_depth++] = (struct sm5_cf_frame){
         .kind = SM5_CF_SWITCH,
         .loop = loop,
         .sw_val = sv,
         .sw_matched = matched,
      };
      break;
   }
   case SM5_OP_CASE: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      if (c->cf_depth == 0 || c->cf[c->cf_depth - 1].kind != SM5_CF_SWITCH) {
         sm5_fail(c, "case outside switch");
         return false;
      }
      struct sm5_cf_frame *f = &c->cf[c->cf_depth - 1];
      sm5_close_switch_case(c, f);
      nir_def *cv = nir_channel(b, sm5_load_src(c, &s0, true), 0);
      nir_def *cond = nir_ior(b, nir_load_var(b, f->sw_matched),
                              nir_ieq(b, f->sw_val, cv));
      f->sw_cur_if = nir_push_if(b, cond);
      nir_store_var(b, f->sw_matched, nir_imm_true(b), 1);
      f->sw_in_case = true;
      break;
   }
   case SM5_OP_DEFAULT: {
      if (c->cf_depth == 0 || c->cf[c->cf_depth - 1].kind != SM5_CF_SWITCH) {
         sm5_fail(c, "default outside switch");
         return false;
      }
      struct sm5_cf_frame *f = &c->cf[c->cf_depth - 1];
      sm5_close_switch_case(c, f);
      f->sw_cur_if = nir_push_if(b, nir_imm_true(b));
      nir_store_var(b, f->sw_matched, nir_imm_true(b), 1);
      f->sw_in_case = true;
      break;
   }
   case SM5_OP_ENDSWITCH: {
      if (c->cf_depth == 0 || c->cf[c->cf_depth - 1].kind != SM5_CF_SWITCH) {
         sm5_fail(c, "unmatched endswitch");
         return false;
      }
      struct sm5_cf_frame *f = &c->cf[--c->cf_depth];
      sm5_close_switch_case(c, f);
      nir_jump(b, nir_jump_break);
      nir_pop_loop(b, f->loop);
      break;
   }
   case SM5_OP_RET: {
      if (inst_end < c->num_dwords) {
         /* Early return: flush accumulated outputs first, then jump return.
          * nir_lower_returns() lowers this cleanly at the end of translation. */
         for (unsigned r = 0; r < SM5_MAX_IO_REGS; ++r) {
            if (c->outputs[r].shadow && c->outputs[r].var) {
               nir_def *v = nir_load_var(b, c->outputs[r].shadow);
               nir_store_var(b, c->outputs[r].var,
                             nir_trim_vector(b, v, c->outputs[r].num_comps),
                             BITFIELD_MASK(c->outputs[r].num_comps));
            }
         }
         if (c->odepth_shadow && c->odepth_var)
            nir_store_var(b, c->odepth_var, nir_load_var(b, c->odepth_shadow), 1);
         if (c->omask_shadow && c->omask_var)
            nir_store_var(b, c->omask_var, nir_load_var(b, c->omask_shadow), 1);
         nir_jump(b, nir_jump_return);
      }
      break;
   }
   case SM5_OP_RETC: {
      if (!sm5_parse_operand(c, &s0))
         return false;
      nir_def *v = nir_channel(b, sm5_load_src(c, &s0, true), 0);
      nir_def *cond = sm5_decode_test_nz(tok0) ? nir_ine_imm(b, v, 0)
                                               : nir_ieq_imm(b, v, 0);
      nir_push_if(b, cond);
      for (unsigned r = 0; r < SM5_MAX_IO_REGS; ++r) {
         if (c->outputs[r].shadow && c->outputs[r].var) {
            nir_def *ov = nir_load_var(b, c->outputs[r].shadow);
            nir_store_var(b, c->outputs[r].var,
                          nir_trim_vector(b, ov, c->outputs[r].num_comps),
                          BITFIELD_MASK(c->outputs[r].num_comps));
         }
      }
      if (c->odepth_shadow && c->odepth_var)
         nir_store_var(b, c->odepth_var, nir_load_var(b, c->odepth_shadow), 1);
      if (c->omask_shadow && c->omask_var)
         nir_store_var(b, c->omask_var, nir_load_var(b, c->omask_shadow), 1);
      nir_jump(b, nir_jump_return);
      nir_pop_if(b, NULL);
      break;
   }

   default: {
      char msg[128];
      snprintf(msg, sizeof(msg), "unsupported SM5 opcode %u", (unsigned)op);
      sm5_fail(c, msg);
      return false;
   }
   }

   /* Always advance to exact instruction boundary so any trailing tokens stay aligned. */
   c->pos = inst_end;
   return !c->failed;
}

nir_shader *
aurora_sm5_to_nir(const uint32_t *code, size_t max_dwords,
                  const struct aurora_sm5_sig_elem *in_sig,
                  unsigned num_in_sig,
                  const struct aurora_sm5_sig_elem *out_sig,
                  unsigned num_out_sig,
                  const nir_shader_compiler_options *options,
                  struct aurora_sm5_info *info_out)
{
   if (!code || max_dwords < 2 || !options)
      return NULL;

   uint32_t ver_tok = code[0];
   uint32_t len_tok = code[1];
   if (len_tok < 2 || len_tok > max_dwords)
      return NULL;

   mesa_shader_stage stage;
   switch (sm5_decode_prog_type(ver_tok)) {
   case SM5_PROG_VS: stage = MESA_SHADER_VERTEX;   break;
   case SM5_PROG_PS: stage = MESA_SHADER_FRAGMENT; break;
   case SM5_PROG_CS: stage = MESA_SHADER_COMPUTE;  break;
   default:
      fprintf(stderr, "aurora_sm5_to_nir: unsupported shader type %u\n",
              (unsigned)sm5_decode_prog_type(ver_tok));
      return NULL;
   }

   struct sm5_ctx *c = calloc(1, sizeof(*c));
   if (!c)
      return NULL;

   c->code = code;
   c->num_dwords = len_tok;
   c->pos = 2;
   c->stage = stage;
   c->info.stage = stage;
   c->b = nir_builder_init_simple_shader(stage, options, "aurora_sm5_%s",
                                         mesa_shader_stage_name(stage));
   c->nir = c->b.shader;
   c->nir->info.internal = false;
   if (stage == MESA_SHADER_FRAGMENT) {
      c->nir->info.fs.origin_upper_left = true;
      c->nir->info.fs.pixel_center_integer = false;
   }

   /* Seed input/output signatures so sysvals & masks are known even before DCLs. */
   for (unsigned i = 0; i < num_in_sig; ++i) {
      uint32_t reg = in_sig[i].reg;
      if (reg < SM5_MAX_IO_REGS) {
         c->inputs[reg].mask |= in_sig[i].mask;
         if (in_sig[i].sysval)
            c->inputs[reg].sysval = (enum sm5_sysval_name)in_sig[i].sysval;
      }
   }
   for (unsigned i = 0; i < num_out_sig; ++i) {
      uint32_t reg = out_sig[i].reg;
      if (reg < SM5_MAX_IO_REGS) {
         c->outputs[reg].mask |= out_sig[i].mask;
         if (out_sig[i].sysval)
            c->outputs[reg].sysval = (enum sm5_sysval_name)out_sig[i].sysval;
      }
   }

   if (!sm5_prescan(c))
      goto fail;

   /* Materialize immediate constant buffer (if any) in entry block. */
   if (c->icb_vec4_count > 0 && c->icb_data) {
      c->icb_var = nir_local_variable_create(
         c->b.impl, glsl_array_type(glsl_uvec4_type(), c->icb_vec4_count, 0),
         "icb");
      for (uint32_t i = 0; i < c->icb_vec4_count; ++i) {
         const uint32_t *d = &c->icb_data[i * 4u];
         nir_deref_instr *deref = nir_build_deref_array_imm(
            &c->b, nir_build_deref_var(&c->b, c->icb_var), i);
         nir_store_deref(&c->b, deref,
                         nir_imm_ivec4(&c->b, (int32_t)d[0], (int32_t)d[1],
                                       (int32_t)d[2], (int32_t)d[3]),
                         0xf);
      }
   }

   while (c->pos < c->num_dwords && !c->failed) {
      if (!sm5_translate_inst(c))
         goto fail;
   }

   if (c->cf_depth != 0) {
      sm5_fail(c, "unclosed control flow block at end of shader");
      goto fail;
   }

   /* Flush accumulated shadow outputs to shader output variables. */
   if (!nir_block_ends_in_jump(nir_cursor_current_block(c->b.cursor))) {
      for (unsigned r = 0; r < SM5_MAX_IO_REGS; ++r) {
         if (c->outputs[r].shadow && c->outputs[r].var) {
            nir_def *v = nir_load_var(&c->b, c->outputs[r].shadow);
            nir_store_var(&c->b, c->outputs[r].var,
                          nir_trim_vector(&c->b, v, c->outputs[r].num_comps),
                          BITFIELD_MASK(c->outputs[r].num_comps));
         }
      }
      if (c->odepth_shadow && c->odepth_var)
         nir_store_var(&c->b, c->odepth_var, nir_load_var(&c->b, c->odepth_shadow), 1);
      if (c->omask_shadow && c->omask_var)
         nir_store_var(&c->b, c->omask_var, nir_load_var(&c->b, c->omask_shadow), 1);
   }

   /* Clean up NIR into SSA ready for Gallium Asahi (agx_shader_initialize runs
    * nir_lower_io itself with nir_lower_io_use_interpolated_input_intrinsics). */
   nir_shader *nir = c->nir;
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   if (info_out)
      *info_out = c->info;

   free(c->temps);
   free(c);
   return nir;

fail:
   ralloc_free(c->nir);
   free(c->temps);
   free(c);
   return NULL;
}
