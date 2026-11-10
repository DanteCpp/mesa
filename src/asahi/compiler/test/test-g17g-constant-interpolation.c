/* SPDX-License-Identifier: MIT
 * Actual Honeykrisp source-placement replay plus full G17G AGX compilation.
 * Coefficient values are explicit modeled native marker/ordinary-plane inputs;
 * this executable does not claim to execute GPU ISA or a Windows Vulkan device.
 */
int existing_graphics_main(int argc, char **argv);
#define main existing_graphics_main
#include "test-agx3-graphics-compile.c"
#undef main
#include "asahi/lib/agx_chip.h"
#include "constant-interpolation-e2e/placement.inc"

struct values { uint32_t a; bool marker; };
static bool
materialize(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   const struct values *v = data;
   b->cursor = nir_before_instr(&in->instr);
   if (in->intrinsic == nir_intrinsic_load_coefficients_agx) {
      nir_def *cf = nir_vec3(b, nir_imm_int(b, v->a),
                            nir_imm_float(b, 0.0), nir_imm_float(b, 0.25));
      nir_def_rewrite_uses(&in->def, cf);
      nir_instr_remove(&in->instr); return true;
   }
   if (in->intrinsic == nir_intrinsic_load_interpolated_input) {
      nir_def *c = nir_imm_floatN_t(b, v->marker ? 1.25 : 0.4,
                                    in->def.bit_size);
      nir_def *cs[4] = {c,c,c,c};
      nir_def_rewrite_uses(&in->def, nir_vec(b, cs, in->def.num_components));
      nir_instr_remove(&in->instr); return true;
   }
   return false;
}

static nir_shader *build_input(unsigned bits, bool centroid)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                  &agx_nir_options, "actual127 smooth varying path");
   nir_def *bary = centroid ? nir_load_barycentric_centroid(&b,32,.interp_mode=INTERP_MODE_SMOOTH)
                            : nir_load_barycentric_pixel(&b,32,.interp_mode=INTERP_MODE_SMOOTH);
   nir_def *color = nir_load_interpolated_input(&b,4,bits,bary,nir_imm_int(&b,0),
                       .base=0,.component=0,.io_semantics.location=VARYING_SLOT_VAR0,
                       .io_semantics.num_slots=1);
   fs_store_colour(&b, color);
   b.shader->info.inputs_read=BITFIELD64_BIT(VARYING_SLOT_VAR0);
   return fs_finish(&b);
}

int main(int argc, char **argv)
{
   if(argc==2 && !strcmp(argv[1],"--legacy-compiler"))
      return existing_graphics_main(1,argv);
   if (argc!=2) return 2;
   const struct values cases[] = {
      {0x00000001,true},{0x00000002,true},{0x00800002,true},
      {0x00000000,false},{0x3f800000,false},{0x3f800001,false},
   };
   unsigned checks=0,failures=0;
   const enum agx_chip unaffected[]={AGX_CHIP_G17P,AGX_CHIP_G18G};
   for(unsigned i=0;i<ARRAY_SIZE(unaffected)+1;i++) {
      nir_shader*s=build_input(32,false);
      if(i==ARRAY_SIZE(unaffected)) {
         nir_foreach_function_impl(impl,s) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
            if(instr->type==nir_instr_type_intrinsic) {
               nir_intrinsic_instr*in=nir_instr_as_intrinsic(instr);
               if(in->intrinsic==nir_intrinsic_load_barycentric_pixel)
                  nir_intrinsic_set_interp_mode(in,INTERP_MODE_NOPERSPECTIVE);
            }
         }
      }
      production_interpolation(s,i<ARRAY_SIZE(unaffected)?unaffected[i]:AGX_CHIP_G17G);
      unsigned coefficients=0;
      nir_foreach_function_impl(impl,s) nir_foreach_block(block,impl) nir_foreach_instr(instr,block)
         if(instr->type==nir_instr_type_intrinsic && nir_instr_as_intrinsic(instr)->intrinsic==nir_intrinsic_load_coefficients_agx)coefficients++;
      checks++;failures+=coefficients!=0;
      printf("unaffected scope=%u coefficient_loads=%u passed=%u\n",i,coefficients,coefficients==0);
      ralloc_free(s);
   }
   for(unsigned bits=16;bits<=32;bits+=16) for(unsigned center=0;center<2;center++) {
      nir_shader *live=build_input(bits,center);
      production_interpolation(live,AGX_CHIP_G17G);
      nir_validate_shader(live,"Honeykrisp G17G interpolation placement");
      struct agx_shader_key key={.dev={.chip=AGX_CHIP_G17G,.encoding=AGX_ISA_ENCODING_AGX3C},
                                .fs.ignore_tib_dependencies=true,.no_preamble=true};
      struct agx_shader_part part={0};
      bool compiled=agx_compile_shader_nir(live,&key,&part) && part.binary;
      /* Compilation is required for the final lowering. This does not assert
       * hardware support for centroid; baseline refusal is retained separately. */
      bool expected_compiled=true;
      checks++; failures+=(compiled!=expected_compiled);
      printf("compile bits=%u centroid=%u chip=G17G compiled=%u expected=%u passed=%u bytes=%u\n",bits,center,compiled,expected_compiled,compiled==expected_compiled,part.info.binary_size);
      if(compiled){char path[4096];snprintf(path,sizeof(path),"%s/fs-%u-%u.bin",argv[1],bits,center);FILE*f=fopen(path,"wb");if(!f)return 2;fwrite(part.binary,1,part.info.binary_size,f);fclose(f);}
      free(part.binary);ralloc_free(live);
      for(unsigned i=0;i<ARRAY_SIZE(cases);i++) {
         nir_shader *s=build_input(bits,center);production_interpolation(s,AGX_CHIP_G17G);
         NIR_PASS(_,s,nir_shader_intrinsics_pass,materialize,nir_metadata_control_flow,(void*)&cases[i]);
         for(unsigned j=0;j<8;j++){NIR_PASS(_,s,nir_opt_constant_folding);NIR_PASS(_,s,nir_opt_algebraic);NIR_PASS(_,s,nir_opt_dce);}
         bool ok=true;unsigned stores=0;
         nir_foreach_function_impl(impl,s) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
            if(instr->type!=nir_instr_type_intrinsic)continue;
            nir_intrinsic_instr*in=nir_instr_as_intrinsic(instr);
            if(in->intrinsic!=nir_intrinsic_store_local_pixel_agx)continue;
            stores++;ok &= nir_src_is_const(in->src[0]);
            if(nir_src_is_const(in->src[0]))for(unsigned c=0;c<4;c++){
               float got=nir_src_comp_as_float(in->src[0],c);float want=cases[i].marker?0.25f:0.4f;
               ok &= fabsf(got-want)<0.0005f;
            }
         }
         ok &= stores==1;checks++;failures+=!ok;
         printf("coefficient a=%08x bits=%u centroid=%u marker=%u passed=%u\n",cases[i].a,bits,center,cases[i].marker,ok);
         ralloc_free(s);
      }
   }
   printf("RESULT checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
