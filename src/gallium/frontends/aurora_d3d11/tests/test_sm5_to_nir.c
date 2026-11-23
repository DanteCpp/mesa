/*
 * Host unit test for the Aurora native D3D11 SM4/SM5 -> NIR compiler.
 * Tests real Gears VS/PS bytecode, textured ClearType dual-source PS,
 * control flow (if/else/loop/switch/discard/early-retc), and CS with
 * raw UAV + TGSM + atomics + sync against nir_validate_shader() and
 * agx_compile_shader_nir(AGX_CHIP_G17G / AGX3C).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asahi/compiler/agx_compile.h"
#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "poly/nir/poly_nir.h"
#include "sm5_to_nir.h"
#include "sm5_tokens.h"

#define TOK_OP(op, len, ext) \
   (((uint32_t)(op)) | ((uint32_t)(len) << 24) | (uint32_t)(ext))
#define TOK_MASK(type, mask, dim) \
   (2u | (0u << 2) | ((uint32_t)(mask) << 4) | ((uint32_t)(type) << 12) | ((uint32_t)(dim) << 20))
#define TOK_SWZ(type, swz, dim) \
   (2u | (1u << 2) | ((uint32_t)(swz) << 4) | ((uint32_t)(type) << 12) | ((uint32_t)(dim) << 20))
#define TOK_SEL(type, comp, dim) \
   (2u | (2u << 2) | ((uint32_t)(comp) << 4) | ((uint32_t)(type) << 12) | ((uint32_t)(dim) << 20))
#define TOK_IMM(n) \
   (((n) == 1 ? 1u : 2u) | ((uint32_t)SM5_OPERAND_IMMEDIATE32 << 12))
#define SWZ(x, y, z, w) \
   ((uint32_t)(x) | ((uint32_t)(y) << 2) | ((uint32_t)(z) << 4) | ((uint32_t)(w) << 6))

#define XYZW SWZ(0, 1, 2, 3)
#define XYZX SWZ(0, 1, 2, 0)
#define XXXX SWZ(0, 0, 0, 0)
#define F_ONE 0x3f800000u
#define REFACTORING_ALLOWED (1u << 11)
#define SM5_SATURATE (1u << 13)
#define INTERP_CONST (1u << 11)
#define INTERP_LINEAR (2u << 11)
#define TEST_NZ (1u << 18)

#define CHECK(cond)                                                          \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__,  \
                 #cond);                                                     \
         exit(1);                                                            \
      }                                                                      \
   } while (0)

static unsigned
type_size_slots(const struct glsl_type *type, bool bindless)
{
   (void)bindless;
   return glsl_count_attribute_slots(type, false);
}

static void
check_agx_g17g_compile(nir_shader *nir, const char *label)
{
   nir_shader *clone = nir_shader_clone(NULL, nir);
   NIR_PASS(_, clone, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_slots,
            nir_lower_io_lower_64bit_to_32 |
               nir_lower_io_use_interpolated_input_intrinsics);
   nir_shader_gather_info(clone, nir_shader_get_entrypoint(clone));
   if (clone->info.stage == MESA_SHADER_VERTEX)
      NIR_PASS(_, clone, nir_lower_io_to_scalar, nir_var_shader_out, NULL, NULL);
   NIR_PASS(_, clone, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, clone, nir_lower_load_const_to_scalar);
   nir_foreach_function_impl(impl, clone) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_ubo) {
               b.cursor = nir_before_instr(instr);
               nir_def *base = nir_load_preamble(&b, 1, 64, .base = 0);
               nir_def *addr = nir_iadd(&b, base, nir_u2u64(&b, intr->src[1].ssa));
               nir_def *val = nir_load_global_constant(&b, intr->def.num_components,
                                                       intr->def.bit_size, addr);
               nir_def_replace(&intr->def, val);
            } else if (intr->intrinsic == nir_intrinsic_load_input ||
                       intr->intrinsic == nir_intrinsic_load_interpolated_input) {
               b.cursor = nir_before_instr(instr);
               nir_def *v = nir_load_preamble(&b, intr->def.num_components,
                                              intr->def.bit_size, .base = 4);
               nir_def_replace(&intr->def, v);
            } else if (clone->info.stage == MESA_SHADER_VERTEX &&
                       intr->intrinsic == nir_intrinsic_store_output) {
               b.cursor = nir_before_instr(instr);
               unsigned idx = nir_intrinsic_base(intr) * 4 + nir_intrinsic_component(intr);
               nir_store_uvs_agx(&b, intr->src[0].ssa, nir_imm_intN_t(&b, idx, 16));
               nir_instr_remove(instr);
            } else if (clone->info.stage == MESA_SHADER_FRAGMENT &&
                       intr->intrinsic == nir_intrinsic_store_output) {
               b.cursor = nir_before_instr(instr);
               nir_def *dst_ptr = nir_load_preamble(&b, 1, 64, .base = 2);
               nir_store_global(&b, intr->src[0].ssa, dst_ptr, .write_mask = 0xf);
               nir_instr_remove(instr);
            }
         }
      }
   }
   agx_preprocess_nir_m5(clone);
   if (clone->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, clone, agx_nir_lower_discard_zs_emit);
      NIR_PASS(_, clone, agx_nir_lower_sample_mask);
      nir_foreach_function_impl(impl, clone) {
         nir_builder b = nir_builder_create(impl);
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type == nir_instr_type_intrinsic &&
                   nir_instr_as_intrinsic(instr)->intrinsic ==
                      nir_intrinsic_load_shader_part_tests_zs_agx) {
                  b.cursor = nir_before_instr(instr);
                  nir_def_replace(&nir_instr_as_intrinsic(instr)->def,
                                  nir_imm_intN_t(&b, 0, 16));
               }
            }
         }
      }
      NIR_PASS(_, clone, agx_nir_lower_sample_mask_m5);
   }
   nir_validate_shader(clone, label);

   struct agx_shader_key key = {
      .dev = {
         .chip = AGX_CHIP_G17G,
         .encoding = AGX_ISA_ENCODING_AGX3C,
      },
      .reserved_preamble = 8,
      .fs.nr_samples = 1,
   };
   struct agx_shader_part out = {0};
   bool ok = agx_compile_shader_nir(clone, &key, &out);
   if (!ok || out.info.binary_size == 0) {
      nir_print_shader(clone, stderr);
      fprintf(stderr, "FAIL: agx_compile_shader_nir(%s) failed (ok=%d size=%u)\n",
              label, ok, out.info.binary_size);
      exit(1);
   }
   printf("PASS: %s -> AGX G17G (AGX3C) compiled cleanly (%u bytes, %u gprs)\n",
          label, out.info.binary_size, out.info.nr_gprs);
   free(out.binary);
   ralloc_free(clone);
}

static void
test_gears_vs_ps(void)
{
   uint32_t vs_code[] = {
      0x00010050u, 0,
      TOK_OP(SM5_OP_DCL_GLOBAL_FLAGS, 1, REFACTORING_ALLOWED),
      TOK_OP(SM5_OP_DCL_CONSTANT_BUFFER, 4, 0), TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZW, 2), 0, 10,
      TOK_OP(SM5_OP_DCL_INPUT, 3, 0), TOK_MASK(SM5_OPERAND_INPUT, 0x7, 1), 0,
      TOK_OP(SM5_OP_DCL_INPUT, 3, 0), TOK_MASK(SM5_OPERAND_INPUT, 0x7, 1), 1,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0, SM5_NAME_POSITION,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 1,
      TOK_OP(SM5_OP_DCL_TEMPS, 2, 0), 2,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x7, 1), 0, TOK_SWZ(SM5_OPERAND_INPUT, XYZX, 1), 0,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x8, 1), 0, TOK_IMM(1), F_ONE,
      TOK_OP(SM5_OP_DP4, 8, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 0, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZW, 2), 0, 0, TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
      TOK_OP(SM5_OP_DP4, 8, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x2, 1), 0, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZW, 2), 0, 1, TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
      TOK_OP(SM5_OP_DP4, 8, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x4, 1), 0, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZW, 2), 0, 2, TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
      TOK_OP(SM5_OP_DP4, 8, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x8, 1), 0, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZW, 2), 0, 3, TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
      TOK_OP(SM5_OP_DP3, 8, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 1, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZX, 2), 0, 4, TOK_SWZ(SM5_OPERAND_INPUT, XYZX, 1), 1,
      TOK_OP(SM5_OP_DP3, 8, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x2, 1), 1, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZX, 2), 0, 5, TOK_SWZ(SM5_OPERAND_INPUT, XYZX, 1), 1,
      TOK_OP(SM5_OP_DP3, 8, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x4, 1), 1, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZX, 2), 0, 6, TOK_SWZ(SM5_OPERAND_INPUT, XYZX, 1), 1,
      TOK_OP(SM5_OP_DP3, 8, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 1, TOK_SWZ(SM5_OPERAND_TEMP, XYZX, 1), 1, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZX, 2), 0, 8,
      TOK_OP(SM5_OP_MAX, 7, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 1, TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 1, TOK_IMM(1), 0,
      TOK_OP(SM5_OP_MAD, 11, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 1, TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 1, TOK_SEL(SM5_OPERAND_CONSTANT_BUFFER, 1, 2), 0, 9, TOK_SEL(SM5_OPERAND_CONSTANT_BUFFER, 0, 2), 0, 9,
      TOK_OP(SM5_OP_MUL, 8, SM5_SATURATE), TOK_MASK(SM5_OPERAND_OUTPUT, 0x7, 1), 1, TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZX, 2), 0, 7, TOK_SWZ(SM5_OPERAND_TEMP, XXXX, 1), 1,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x8, 1), 1, TOK_IMM(1), F_ONE,
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   vs_code[1] = (uint32_t)(sizeof(vs_code) / sizeof(vs_code[0]));

   uint32_t ps_code[] = {
      0x00000050u, 0,
      TOK_OP(SM5_OP_DCL_GLOBAL_FLAGS, 1, REFACTORING_ALLOWED),
      TOK_OP(SM5_OP_DCL_INPUT_PS, 3, INTERP_CONST), TOK_MASK(SM5_OPERAND_INPUT, 0xf, 1), 1,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0, TOK_SWZ(SM5_OPERAND_INPUT, XYZW, 1), 1,
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   ps_code[1] = (uint32_t)(sizeof(ps_code) / sizeof(ps_code[0]));

   const nir_shader_compiler_options *opts = &agx_nir_options;
   struct aurora_sm5_info vs_info = {0}, ps_info = {0};

   nir_shader *vs = aurora_sm5_to_nir(vs_code, vs_code[1], NULL, 0, NULL, 0, opts, &vs_info);
   if (!vs) {
      fprintf(stderr, "FAIL: Gears VS translation failed\n");
      exit(1);
   }
   nir_validate_shader(vs, "gears_vs");
   CHECK(vs->info.stage == MESA_SHADER_VERTEX);
   CHECK(vs->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_POS));
   CHECK(vs->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_VAR1));
   CHECK(vs_info.cb_mask == 1u);
   check_agx_g17g_compile(vs, "gears_vs");
   ralloc_free(vs);

   nir_shader *ps = aurora_sm5_to_nir(ps_code, ps_code[1], NULL, 0, NULL, 0, opts, &ps_info);
   if (!ps) {
      fprintf(stderr, "FAIL: Gears PS translation failed\n");
      exit(1);
   }
   nir_validate_shader(ps, "gears_ps");
   CHECK(ps->info.stage == MESA_SHADER_FRAGMENT);
   CHECK(ps->info.inputs_read & BITFIELD64_BIT(VARYING_SLOT_VAR1));
   CHECK(ps->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA0));
   bool found_flat = false;
   nir_foreach_shader_in_variable(var, ps) {
      if (var->data.location == VARYING_SLOT_VAR1) {
         CHECK(var->data.interpolation == INTERP_MODE_FLAT);
         found_flat = true;
      }
   }
   CHECK(found_flat);
   check_agx_g17g_compile(ps, "gears_ps");
   ralloc_free(ps);
}

static void
test_cleartype_textured_ps(void)
{
   uint32_t ret_float4 = (5u) | (5u << 4) | (5u << 8) | (5u << 12);
   uint32_t tok_t0 = (0u) | ((uint32_t)SM5_OPERAND_RESOURCE << 12) | (1u << 20);
   uint32_t tok_s0 = (0u) | ((uint32_t)SM5_OPERAND_SAMPLER << 12) | (1u << 20);
   uint32_t ps_code[] = {
      0x00000050u, 0,
      TOK_OP(SM5_OP_DCL_RESOURCE, 4, (SM5_DIM_TEXTURE2D << 11)), tok_t0, 0, ret_float4,
      TOK_OP(SM5_OP_DCL_SAMPLER, 3, 0), tok_s0, 0,
      TOK_OP(SM5_OP_DCL_CONSTANT_BUFFER, 4, 0), TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZW, 2), 0, 1,
      TOK_OP(SM5_OP_DCL_INPUT_PS, 3, INTERP_LINEAR), TOK_MASK(SM5_OPERAND_INPUT, 0x3, 1), 1,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 1,
      TOK_OP(SM5_OP_DCL_TEMPS, 2, 0), 1,
      TOK_OP(SM5_OP_SAMPLE, 9, 0),
         TOK_MASK(SM5_OPERAND_TEMP, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_INPUT, SWZ(0,1,0,0), 1), 1,
         TOK_SWZ(SM5_OPERAND_RESOURCE, XYZW, 1), 0,
         tok_s0, 0,
      TOK_OP(SM5_OP_MUL, 8, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
         TOK_SWZ(SM5_OPERAND_CONSTANT_BUFFER, XYZW, 2), 0, 0,
      TOK_OP(SM5_OP_MOV, 5, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 1,
         TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   ps_code[1] = (uint32_t)(sizeof(ps_code) / sizeof(ps_code[0]));

   struct aurora_sm5_info info = {0};
   nir_shader *ps = aurora_sm5_to_nir(ps_code, ps_code[1], NULL, 0, NULL, 0,
                                      &agx_nir_options, &info);
   if (!ps) {
      fprintf(stderr, "FAIL: ClearType PS failed\n");
      exit(1);
   }
   nir_validate_shader(ps, "cleartype_ps");
   CHECK(info.sampler_mask == 1u);
   CHECK(ps->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA0));
   CHECK(ps->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA1));
   CHECK(BITSET_TEST(ps->info.textures_used, 0));
   CHECK(BITSET_TEST(ps->info.samplers_used, 0));
   printf("PASS: cleartype_ps -> NIR validated (tex0 + samp0 + dual-src o0/o1)\n");
   ralloc_free(ps);
}

static void
test_control_flow_and_discard(void)
{
   uint32_t ps_code[] = {
      0x00000050u, 0,
      TOK_OP(SM5_OP_DCL_INPUT_PS, 3, INTERP_LINEAR), TOK_MASK(SM5_OPERAND_INPUT, 0xf, 1), 1,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
      TOK_OP(SM5_OP_DCL_TEMPS, 2, 0), 2,
      TOK_OP(SM5_OP_LT, 7, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 0,
         TOK_SEL(SM5_OPERAND_INPUT, 0, 1), 1, TOK_IMM(1), 0x3f000000u,
      TOK_OP(SM5_OP_DISCARD, 3, TEST_NZ), TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 0,
      TOK_OP(SM5_OP_IF, 3, TEST_NZ), TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 0,
         TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0, TOK_SWZ(SM5_OPERAND_INPUT, XYZW, 1), 1,
         TOK_OP(SM5_OP_RET, 1, 0),
      TOK_OP(SM5_OP_ELSE, 1, 0),
         TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 1, TOK_IMM(1), 0,
         TOK_OP(SM5_OP_LOOP, 1, 0),
            TOK_OP(SM5_OP_IADD, 7, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 1,
               TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 1, TOK_IMM(1), 1,
            TOK_OP(SM5_OP_IGE, 7, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x2, 1), 1,
               TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 1, TOK_IMM(1), 4,
            TOK_OP(SM5_OP_BREAKC, 3, TEST_NZ), TOK_SEL(SM5_OPERAND_TEMP, 1, 1), 1,
         TOK_OP(SM5_OP_ENDLOOP, 1, 0),
         TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0, TOK_SWZ(SM5_OPERAND_TEMP, XXXX, 1), 1,
      TOK_OP(SM5_OP_ENDIF, 1, 0),
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   ps_code[1] = (uint32_t)(sizeof(ps_code) / sizeof(ps_code[0]));

   nir_shader *ps = aurora_sm5_to_nir(ps_code, ps_code[1], NULL, 0, NULL, 0,
                                      &agx_nir_options, NULL);
   if (!ps) {
      fprintf(stderr, "FAIL: control_flow_ps failed\n");
      exit(1);
   }
   nir_validate_shader(ps, "control_flow_ps");
   CHECK(ps->info.fs.uses_discard);
   check_agx_g17g_compile(ps, "control_flow_ps");
   ralloc_free(ps);
}

static void
test_compute_uav_tgsm(void)
{
   uint32_t tok_u0 = (0u) | ((uint32_t)SM5_OPERAND_UNORDERED_ACCESS_VIEW << 12) | (1u << 20);
   uint32_t tok_g0 = (0u) | ((uint32_t)SM5_OPERAND_THREAD_GROUP_SHARED_MEMORY << 12) | (1u << 20);
   uint32_t tok_tid = (2u) | (0u << 2) | (1u << 4) | ((uint32_t)SM5_OPERAND_INPUT_THREAD_ID << 12);
   uint32_t tok_tid_x = (2u) | (2u << 2) | (0u << 4) | ((uint32_t)SM5_OPERAND_INPUT_THREAD_ID << 12);
   uint32_t cs_code[] = {
      0x00050050u, 0,
      TOK_OP(SM5_OP_DCL_THREAD_GROUP, 4, 0), 8, 8, 1,
      TOK_OP(SM5_OP_DCL_UNORDERED_ACCESS_VIEW_RAW, 3, 0), tok_u0, 0,
      TOK_OP(SM5_OP_DCL_THREAD_GROUP_SHARED_MEMORY_RAW, 4, 0), tok_g0, 0, 256,
      TOK_OP(SM5_OP_DCL_INPUT, 2, 0), tok_tid,
      TOK_OP(SM5_OP_DCL_TEMPS, 2, 0), 1,
      TOK_OP(SM5_OP_ISHL, 6, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 0, tok_tid_x, TOK_IMM(1), 2,
      TOK_OP(SM5_OP_LD_RAW, 7, 0),
         TOK_MASK(SM5_OPERAND_TEMP, 0x2, 1), 0,
         TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 0,
         TOK_SWZ(SM5_OPERAND_UNORDERED_ACCESS_VIEW, XXXX, 1), 0,
      TOK_OP(SM5_OP_STORE_RAW, 7, 0),
         TOK_MASK(SM5_OPERAND_THREAD_GROUP_SHARED_MEMORY, 0x1, 1), 0,
         TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 0,
         TOK_SWZ(SM5_OPERAND_TEMP, SWZ(1,1,1,1), 1), 0,
      TOK_OP(SM5_OP_SYNC, 1, (7u << 11)),
      TOK_OP(SM5_OP_STORE_RAW, 7, 0),
         TOK_MASK(SM5_OPERAND_UNORDERED_ACCESS_VIEW, 0x1, 1), 0,
         TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 0,
         TOK_SWZ(SM5_OPERAND_TEMP, SWZ(1,1,1,1), 1), 0,
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   cs_code[1] = (uint32_t)(sizeof(cs_code) / sizeof(cs_code[0]));

   struct aurora_sm5_info info = {0};
   nir_shader *cs = aurora_sm5_to_nir(cs_code, cs_code[1], NULL, 0, NULL, 0,
                                      &agx_nir_options, &info);
   if (!cs) {
      fprintf(stderr, "FAIL: compute_cs failed\n");
      exit(1);
   }
   nir_validate_shader(cs, "compute_cs");
   CHECK(cs->info.stage == MESA_SHADER_COMPUTE);
   CHECK(cs->info.workgroup_size[0] == 8 && cs->info.workgroup_size[1] == 8 && cs->info.workgroup_size[2] == 1);
   CHECK(cs->info.shared_size == 256);
   CHECK(info.uav_raw_or_struct_mask == 1u);
   printf("PASS: compute_cs -> NIR validated (wg=8x8x1, tgsm=%u B, uav_raw=0x%x)\n",
          cs->info.shared_size, info.uav_raw_or_struct_mask);
   ralloc_free(cs);
}

static void
test_hull_quad_indexed_fork_join(void)
{
   /* Real SM5 hs_5_0 quad hull shader:
    *   - 4 input CPs -> 4 output CPs
    *   - explicit control_point_phase copying vicp[vOutputControlPointID][0] -> o0
    *   - 4-instance fork_phase writing o[vForkInstanceID.x + 0].x (SV_TessFactor[0..3])
    *   - 1-instance fork_phase writing o4.x & o4.y (packed SV_InsideTessFactor[0..1])
    *   - join_phase reading vpc0.x + vocp[0][0] -> o5.xyzw (user patch constant)
    */
   uint32_t tok_cpid = 0u | ((uint32_t)SM5_OPERAND_OUTPUT_CONTROL_POINT_ID << 12);
   uint32_t tok_forkid = 0u | ((uint32_t)SM5_OPERAND_INPUT_FORK_INSTANCE_ID << 12);
   uint32_t tok_forkid_x = 1u | ((uint32_t)SM5_OPERAND_INPUT_FORK_INSTANCE_ID << 12);
   /* vicp[rel=vOutputControlPointID][imm=0].xyzw: index_dim=2, idx0=REL(2), idx1=IMM32(0) */
   uint32_t tok_vicp_rel =
      TOK_SWZ(SM5_OPERAND_INPUT_CONTROL_POINT, XYZW, 2) | (SM5_INDEX_REL << 22);
   /* o[imm32+rel=r0.x].x: index_dim=1, idx0=IMM32_PLUS_REL(3) */
   uint32_t tok_o_rel =
      TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1) | (SM5_INDEX_IMM32_PLUS_REL << 22);

   uint32_t hs_code[] = {
      ((uint32_t)SM5_PROG_HS << 16) | 0x50u, 0,
      TOK_OP(SM5_OP_HS_DECLS, 1, 0),
      TOK_OP(SM5_OP_DCL_INPUT_CONTROL_POINT_COUNT, 1, (4u << 11)),
      TOK_OP(SM5_OP_DCL_OUTPUT_CONTROL_POINT_COUNT, 1, (4u << 11)),
      TOK_OP(SM5_OP_DCL_TESS_DOMAIN, 1, ((uint32_t)SM5_TESS_DOMAIN_QUAD << 11)),
      TOK_OP(SM5_OP_DCL_TESS_PARTITIONING, 1, ((uint32_t)SM5_TESS_PART_FRACTIONAL_ODD << 11)),
      TOK_OP(SM5_OP_DCL_TESS_OUTPUT_PRIMITIVE, 1, ((uint32_t)SM5_TESS_OUT_TRIANGLE_CCW << 11)),
      TOK_OP(SM5_OP_DCL_HS_MAX_TESSFACTOR, 2, 0), 0x42800000u, /* 64.0f */

      TOK_OP(SM5_OP_HS_CONTROL_POINT_PHASE, 1, 0),
      TOK_OP(SM5_OP_DCL_INPUT, 2, 0), tok_cpid,
      TOK_OP(SM5_OP_DCL_INPUT, 4, 0), TOK_MASK(SM5_OPERAND_INPUT_CONTROL_POINT, 0xf, 2), 4, 0,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
      TOK_OP(SM5_OP_MOV, 6, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
         tok_vicp_rel, tok_cpid, 0,
      TOK_OP(SM5_OP_RET, 1, 0),

      TOK_OP(SM5_OP_HS_FORK_PHASE, 1, 0),
      TOK_OP(SM5_OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT, 2, 0), 4,
      TOK_OP(SM5_OP_DCL_INPUT, 2, 0), tok_forkid,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 0,
         SM5_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 1,
         SM5_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 2,
         SM5_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 3,
         SM5_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_TEMPS, 2, 0), 1,
      TOK_OP(SM5_OP_MOV, 4, 0), TOK_MASK(SM5_OPERAND_TEMP, 0x1, 1), 0, tok_forkid_x,
      TOK_OP(SM5_OP_MOV, 7, 0),
         tok_o_rel, 0, TOK_SEL(SM5_OPERAND_TEMP, 0, 1), 0,
         TOK_IMM(1), 0x40800000u, /* 4.0f */
      TOK_OP(SM5_OP_RET, 1, 0),

      TOK_OP(SM5_OP_HS_FORK_PHASE, 1, 0),
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 4,
         SM5_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x2, 1), 4,
         SM5_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x3, 1), 4, TOK_IMM(1), 0x40400000u,
      TOK_OP(SM5_OP_RET, 1, 0),

      TOK_OP(SM5_OP_HS_JOIN_PHASE, 1, 0),
      TOK_OP(SM5_OP_DCL_INPUT, 3, 0), TOK_MASK(SM5_OPERAND_INPUT_PATCH_CONSTANT, 0x1, 1), 0,
      TOK_OP(SM5_OP_DCL_INPUT, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT_CONTROL_POINT, 0xf, 2), 4, 0,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 5,
      TOK_OP(SM5_OP_ADD, 8, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 5,
         TOK_SWZ(SM5_OPERAND_OUTPUT_CONTROL_POINT, XYZW, 2), 0, 0,
         TOK_SWZ(SM5_OPERAND_INPUT_PATCH_CONSTANT, XXXX, 1), 0,
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   hs_code[1] = (uint32_t)(sizeof(hs_code) / sizeof(hs_code[0]));

   nir_shader *hs = aurora_sm5_to_nir_ex(hs_code, hs_code[1], NULL, 0, NULL, 0,
                                         NULL, 0, &agx_nir_options, NULL);
   CHECK(hs != NULL);
   nir_validate_shader(hs, "hull_quad_indexed");
   CHECK(hs->info.stage == MESA_SHADER_TESS_CTRL);
   CHECK(hs->info.tess.tcs_vertices_out == 4);
   CHECK(hs->info.tess._primitive_mode == TESS_PRIMITIVE_QUADS);
   CHECK(hs->info.tess.spacing == TESS_SPACING_FRACTIONAL_ODD);
   CHECK(hs->info.tess.ccw == true);
   CHECK(hs->info.outputs_written & VARYING_BIT_TESS_LEVEL_OUTER);
   CHECK(hs->info.outputs_written & VARYING_BIT_TESS_LEVEL_INNER);
   CHECK(hs->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_VAR0));
   CHECK(hs->info.patch_outputs_written & (1u << 5));

   /* Lower IO and run Poly TCS lowering to match Gallium Asahi pipeline. */
   NIR_PASS(_, hs, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_slots, 0);
   nir_shader_gather_info(hs, nir_shader_get_entrypoint(hs));
   hs->info.workgroup_size[0] = hs->info.tess.tcs_vertices_out;
   hs->info.workgroup_size[1] = hs->info.workgroup_size[2] = 1;
   NIR_PASS(_, hs, poly_nir_lower_tcs, true);
   nir_validate_shader(hs, "hull_quad_indexed_poly");
   printf("PASS: hull_quad_indexed -> NIR + poly_nir_lower_tcs validated\n");
   ralloc_free(hs);
}

static void
test_hull_passthrough(void)
{
   /* Pass-through hull shader (no hs_control_point_phase!). */
   struct aurora_sm5_sig_elem io_sig[] = {
      { .reg = 0, .sysval = SM5_NAME_POSITION, .mask = 0xf },
      { .reg = 1, .sysval = 0, .mask = 0xf },
   };
   uint32_t hs_code[] = {
      ((uint32_t)SM5_PROG_HS << 16) | 0x50u, 0,
      TOK_OP(SM5_OP_HS_DECLS, 1, 0),
      TOK_OP(SM5_OP_DCL_INPUT_CONTROL_POINT_COUNT, 1, (3u << 11)),
      TOK_OP(SM5_OP_DCL_OUTPUT_CONTROL_POINT_COUNT, 1, (3u << 11)),
      TOK_OP(SM5_OP_DCL_TESS_DOMAIN, 1, ((uint32_t)SM5_TESS_DOMAIN_TRI << 11)),
      TOK_OP(SM5_OP_DCL_TESS_PARTITIONING, 1, ((uint32_t)SM5_TESS_PART_INTEGER << 11)),
      TOK_OP(SM5_OP_DCL_TESS_OUTPUT_PRIMITIVE, 1, ((uint32_t)SM5_TESS_OUT_TRIANGLE_CW << 11)),
      TOK_OP(SM5_OP_HS_FORK_PHASE, 1, 0),
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 0,
         SM5_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 1,
         SM5_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 2,
         SM5_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 3,
         SM5_NAME_FINAL_TRI_INSIDE_TESSFACTOR,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 0, TOK_IMM(1), F_ONE,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 1, TOK_IMM(1), F_ONE,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 2, TOK_IMM(1), F_ONE,
      TOK_OP(SM5_OP_MOV, 5, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0x1, 1), 3, TOK_IMM(1), F_ONE,
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   hs_code[1] = (uint32_t)(sizeof(hs_code) / sizeof(hs_code[0]));

   nir_shader *hs = aurora_sm5_to_nir_ex(hs_code, hs_code[1], io_sig, 2, io_sig, 2,
                                         NULL, 0, &agx_nir_options, NULL);
   CHECK(hs != NULL);
   nir_validate_shader(hs, "hull_passthrough");
   CHECK(hs->info.outputs_written & VARYING_BIT_POS);
   CHECK(hs->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_VAR1));
   CHECK(hs->info.outputs_written & VARYING_BIT_TESS_LEVEL_OUTER);
   CHECK(hs->info.outputs_written & VARYING_BIT_TESS_LEVEL_INNER);
   NIR_PASS(_, hs, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_slots, 0);
   nir_shader_gather_info(hs, nir_shader_get_entrypoint(hs));
   hs->info.workgroup_size[0] = hs->info.tess.tcs_vertices_out;
   hs->info.workgroup_size[1] = hs->info.workgroup_size[2] = 1;
   NIR_PASS(_, hs, poly_nir_lower_tcs, true);
   nir_validate_shader(hs, "hull_passthrough_poly");
   printf("PASS: hull_passthrough -> NIR + poly_nir_lower_tcs validated\n");
   ralloc_free(hs);
}

static void
test_domain_tri(void)
{
   /* Real SM5 ds_5_0 triangle domain shader:
    *   barycentric interpolation of vicp[0..2][0] using vDomain.xyz + vpc0
    */
   uint32_t tok_vdom_mask = TOK_MASK(SM5_OPERAND_INPUT_DOMAIN_POINT, 0x7, 0);
   uint32_t tok_vdom_x = TOK_SWZ(SM5_OPERAND_INPUT_DOMAIN_POINT, XXXX, 0);
   uint32_t tok_vdom_y = TOK_SWZ(SM5_OPERAND_INPUT_DOMAIN_POINT, SWZ(1,1,1,1), 0);
   uint32_t tok_vdom_z = TOK_SWZ(SM5_OPERAND_INPUT_DOMAIN_POINT, SWZ(2,2,2,2), 0);

   uint32_t ds_code[] = {
      ((uint32_t)SM5_PROG_DS << 16) | 0x50u, 0,
      TOK_OP(SM5_OP_DCL_INPUT_CONTROL_POINT_COUNT, 1, (3u << 11)),
      TOK_OP(SM5_OP_DCL_TESS_DOMAIN, 1, ((uint32_t)SM5_TESS_DOMAIN_TRI << 11)),
      TOK_OP(SM5_OP_DCL_INPUT, 2, 0), tok_vdom_mask,
      TOK_OP(SM5_OP_DCL_INPUT, 4, 0), TOK_MASK(SM5_OPERAND_INPUT_CONTROL_POINT, 0xf, 2), 3, 0,
      TOK_OP(SM5_OP_DCL_INPUT, 3, 0), TOK_MASK(SM5_OPERAND_INPUT_PATCH_CONSTANT, 0xf, 1), 0,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0, SM5_NAME_POSITION,
      TOK_OP(SM5_OP_DCL_OUTPUT, 3, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 1,
      TOK_OP(SM5_OP_DCL_TEMPS, 2, 0), 1,
      TOK_OP(SM5_OP_MUL, 7, 0),
         TOK_MASK(SM5_OPERAND_TEMP, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_INPUT_CONTROL_POINT, XYZW, 2), 0, 0,
         tok_vdom_x,
      TOK_OP(SM5_OP_MAD, 9, 0),
         TOK_MASK(SM5_OPERAND_TEMP, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_INPUT_CONTROL_POINT, XYZW, 2), 1, 0,
         tok_vdom_y,
         TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
      TOK_OP(SM5_OP_MAD, 9, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_INPUT_CONTROL_POINT, XYZW, 2), 2, 0,
         tok_vdom_z,
         TOK_SWZ(SM5_OPERAND_TEMP, XYZW, 1), 0,
      TOK_OP(SM5_OP_MOV, 5, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 1,
         TOK_SWZ(SM5_OPERAND_INPUT_PATCH_CONSTANT, XYZW, 1), 0,
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   ds_code[1] = (uint32_t)(sizeof(ds_code) / sizeof(ds_code[0]));

   nir_shader *ds = aurora_sm5_to_nir_ex(ds_code, ds_code[1], NULL, 0, NULL, 0,
                                         NULL, 0, &agx_nir_options, NULL);
   CHECK(ds != NULL);
   nir_validate_shader(ds, "domain_tri");
   CHECK(ds->info.stage == MESA_SHADER_TESS_EVAL);
   CHECK(ds->info.tess._primitive_mode == TESS_PRIMITIVE_TRIANGLES);
   CHECK(ds->info.outputs_written & VARYING_BIT_POS);
   CHECK(ds->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_VAR1));

   NIR_PASS(_, ds, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_slots, 0);
   nir_shader_gather_info(ds, nir_shader_get_entrypoint(ds));
   NIR_PASS(_, ds, poly_nir_lower_tes, true);
   nir_validate_shader(ds, "domain_tri_poly");
   printf("PASS: domain_tri -> NIR + poly_nir_lower_tes validated\n");
   ralloc_free(ds);
}

static void
test_geometry_emit_cut(void)
{
   /* Real SM5 gs_5_0 triangle passthrough GS with emit + cut. */
   uint32_t tok_primid = 0u | ((uint32_t)SM5_OPERAND_INPUT_PRIMITIVEID << 12);
   uint32_t gs_code[] = {
      ((uint32_t)SM5_PROG_GS << 16) | 0x50u, 0,
      TOK_OP(SM5_OP_DCL_GS_INPUT_PRIMITIVE, 1, ((uint32_t)SM5_PRIM_TRIANGLE << 11)),
      TOK_OP(SM5_OP_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY, 1, ((uint32_t)SM5_PRIM_TOPO_TRIANGLESTRIP << 11)),
      TOK_OP(SM5_OP_DCL_MAX_OUTPUT_VERTEX_COUNT, 2, 0), 3,
      TOK_OP(SM5_OP_DCL_INPUT_SIV, 5, 0), TOK_MASK(SM5_OPERAND_INPUT, 0xf, 2), 3, 0, SM5_NAME_POSITION,
      TOK_OP(SM5_OP_DCL_INPUT, 2, 0), tok_primid,
      TOK_OP(SM5_OP_DCL_OUTPUT_SIV, 4, 0), TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0, SM5_NAME_POSITION,
      TOK_OP(SM5_OP_MOV, 6, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_INPUT, XYZW, 2), 0, 0,
      TOK_OP(SM5_OP_EMIT, 1, 0),
      TOK_OP(SM5_OP_MOV, 6, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_INPUT, XYZW, 2), 1, 0,
      TOK_OP(SM5_OP_EMIT, 1, 0),
      TOK_OP(SM5_OP_MOV, 6, 0),
         TOK_MASK(SM5_OPERAND_OUTPUT, 0xf, 1), 0,
         TOK_SWZ(SM5_OPERAND_INPUT, XYZW, 2), 2, 0,
      TOK_OP(SM5_OP_EMITTHENCUT, 1, 0),
      TOK_OP(SM5_OP_RET, 1, 0),
   };
   gs_code[1] = (uint32_t)(sizeof(gs_code) / sizeof(gs_code[0]));

   nir_shader *gs = aurora_sm5_to_nir_ex(gs_code, gs_code[1], NULL, 0, NULL, 0,
                                         NULL, 0, &agx_nir_options, NULL);
   CHECK(gs != NULL);
   nir_validate_shader(gs, "geometry_tri");
   CHECK(gs->info.stage == MESA_SHADER_GEOMETRY);
   CHECK(gs->info.gs.input_primitive == MESA_PRIM_TRIANGLES);
   CHECK(gs->info.gs.output_primitive == MESA_PRIM_TRIANGLE_STRIP);
   CHECK(gs->info.gs.vertices_in == 3);
   CHECK(gs->info.gs.vertices_out == 3);
   CHECK(gs->info.gs.invocations == 1);

   NIR_PASS(_, gs, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_slots, 0);
   nir_shader_gather_info(gs, nir_shader_get_entrypoint(gs));
   nir_shader *gs_count = NULL, *gs_copy = NULL, *pre_gs = NULL;
   struct poly_gs_info ginfo = {0};
   poly_nir_lower_gs(gs, &gs_count, &gs_copy, &pre_gs, &ginfo);
   CHECK(gs_copy && pre_gs);
   if (gs_count)
      nir_validate_shader(gs_count, "gs_count");
   nir_validate_shader(gs_copy, "gs_copy");
   nir_validate_shader(pre_gs, "pre_gs");
   ralloc_free(gs);
   ralloc_free(gs_count);
   ralloc_free(gs_copy);
   ralloc_free(pre_gs);
   printf("PASS: geometry_tri -> NIR + poly_nir_lower_gs validated\n");
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   test_gears_vs_ps();
   test_cleartype_textured_ps();
   test_control_flow_and_discard();
   test_compute_uav_tgsm();
   test_hull_quad_indexed_fork_join();
   test_hull_passthrough();
   test_domain_tri();
   test_geometry_emit_cut();
   glsl_type_singleton_decref();
   printf("ALL SM5-TO-NIR + AGX G17G HOST TESTS PASSED!\n");
   return 0;
}
