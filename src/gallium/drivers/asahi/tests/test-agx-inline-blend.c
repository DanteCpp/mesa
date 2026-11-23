/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

/* Verify Gallium Asahi monolithic inline RGBA8 blend compilation and fallback
 * gating on AGX3B (G17P), AGX3C (M5), and AGX2 control under the compiler-only
 * DRM shim.
 */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/disk_cache.h"
#include "util/driconf.h"
#include "util/hash_table.h"
#include "util/u_inlines.h"
#include "util/u_simple_shaders.h"
#include "util/xmlconfig.h"
#include "compiler/nir/nir_builder.h"
#include "agx_bo.h"
#include "agx_disk_cache.h"
#include "agx_public.h"
#include "agx_state.h"

#define CHECK(expr) do { \
   if (!(expr)) { \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
      exit(1); \
   } \
} while (0)

static bool
contains_bytes(const uint8_t *haystack, size_t hlen,
               const uint8_t *needle, size_t nlen)
{
   if (nlen > hlen)
      return false;
   for (size_t i = 0; i + nlen <= hlen; i += 2) {
      if (!memcmp(haystack + i, needle, nlen))
         return true;
   }
   return false;
}

static void *
create_nir_fs(struct pipe_context *pipe, nir_shader *nir)
{
   struct pipe_shader_state state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = nir,
   };
   return pipe->create_fs_state(pipe, &state);
}

static nir_shader *
make_uniform_color_fs(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &agx_nir_options,
                                                  "uniform_color_fs");
   b.shader->info.num_ubos = 1;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "out_color");
   out->data.location = FRAG_RESULT_DATA0;
   nir_def *c = nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                             .align_mul = 16, .align_offset = 0,
                             .range_base = 0, .range = 16);
   nir_store_var(&b, out, c, 0xf);
   return b.shader;
}

static nir_shader *
make_discard_fs(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &agx_nir_options,
                                                  "discard_fs");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "in_color");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "out_color");
   out->data.location = FRAG_RESULT_DATA0;
   nir_def *c = nir_load_var(&b, in);
   nir_demote_if(&b, nir_flt_imm(&b, nir_channel(&b, c, 3), 0.5f));
   nir_store_var(&b, out, c, 0xf);
   return b.shader;
}

static nir_shader *
make_dual_src_fs(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &agx_nir_options,
                                                  "dual_src_fs");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "in_color");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out0 = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out0");
   out0->data.location = FRAG_RESULT_DATA0;
   nir_variable *out1 = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out1");
   out1->data.location = FRAG_RESULT_DATA0;
   out1->data.index = 1;
   nir_def *c = nir_load_var(&b, in);
   nir_store_var(&b, out0, c, 0xf);
   nir_store_var(&b, out1, c, 0xf);
   return b.shader;
}

static nir_shader *
make_mrt_fs(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                  &agx_nir_options,
                                                  "mrt_fs");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "in_color");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out0 = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out0");
   out0->data.location = FRAG_RESULT_DATA0;
   nir_variable *out1 = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out1");
   out1->data.location = FRAG_RESULT_DATA1;
   nir_def *c = nir_load_var(&b, in);
   nir_store_var(&b, out0, c, 0xf);
   nir_store_var(&b, out1, c, 0xf);
   return b.shader;
}

static struct agx_linked_shader *
draw_once(struct pipe_context *pipe)
{
   struct agx_context *ctx = agx_context(pipe);
   const unsigned refused_before = ctx->skipped_no_program;
   struct pipe_draw_info info = {
      .mode = MESA_PRIM_TRIANGLES,
      .instance_count = 1,
   };
   struct pipe_draw_start_count_bias draw = {.count = 3};
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   CHECK(ctx->skipped_no_program == refused_before);
   CHECK(ctx->linked.fs != NULL);
   CHECK(ctx->linked.fs->bo != NULL);
   return ctx->linked.fs;
}

static void
check_m5_blend_bytes(const char *label, struct agx_linked_shader *linked,
                     bool expect_inline)
{
   static const uint8_t entry[6] = {0x07, 0x30, 0x00, 0x05, 0x00, 0x00};
   static const uint8_t release[6] = {0x67, 0x10, 0x00, 0x04, 0x00, 0x00};
   const uint8_t *code =
      (const uint8_t *)agx_bo_map(linked->bo) + linked->entry_offset;
   CHECK(code != NULL);
   const bool has_entry = contains_bytes(code, linked->size, entry, sizeof(entry));
   const bool has_rel = contains_bytes(code, linked->size, release, sizeof(release));
   const bool has_inline =
      linked->g17p_colour_classes == 1 && has_entry && has_rel;
   if (has_inline != expect_inline) {
      fprintf(stderr,
              "M5 check '%s' failed: expect=%d got=%d (classes=%u entry=%d rel=%d size=%zu)\n",
              label, expect_inline, has_inline, linked->g17p_colour_classes,
              has_entry, has_rel, linked->size);
      exit(1);
   }
}

static void
check_g17p_blend_bytes(struct agx_linked_shader *linked, bool expect_inline)
{
   static const uint8_t gate[6] = {0x87, 0x02, 0x54, 0x00, 0x06, 0x00};
   const uint8_t *code =
      (const uint8_t *)agx_bo_map(linked->bo) + linked->entry_offset;
   CHECK(code != NULL);
   const bool has_gate = contains_bytes(code, linked->size, gate, sizeof(gate));
   if (expect_inline) {
      CHECK(linked->g17p_colour_classes == 1);
      CHECK(has_gate);
   }
}

static void
remove_tree(const char *path)
{
   struct stat st;
   if (lstat(path, &st) != 0)
      return;
   if (S_ISDIR(st.st_mode)) {
      DIR *dir = opendir(path);
      if (dir) {
         struct dirent *ent;
         while ((ent = readdir(dir)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
               continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            remove_tree(child);
         }
         closedir(dir);
      }
      rmdir(path);
   } else {
      unlink(path);
   }
}

static void
test_warm_disk_cache(bool is_m5, bool is_g17p)
{
#ifdef ENABLE_SHADER_CACHE
   char cache_dir[] = "/tmp/agx-inline-blend-cache-XXXXXX";
   CHECK(mkdtemp(cache_dir) != NULL);

   unsetenv("MESA_SHADER_CACHE_DISABLE");
   setenv("MESA_SHADER_CACHE_DIR", cache_dir, 1);

   static const driOptionDescription descriptions[] = {
#include "driinfo_asahi.h"
   };
   const float vertices[3][8] = {
      {-1, -1, 0, 1, 0.25f, 0.5f, 0.75f, 0.5f},
      { 1, -1, 0, 1, 0.5f, 0.25f, 0.75f, 0.5f},
      { 0,  1, 0, 1, 0.75f, 0.5f, 0.25f, 0.5f},
   };
   const enum tgsi_semantic semantics[] = {TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_GENERIC};
   const unsigned indices[] = {0, 0};
   float ubo_color[4] = {0.25f, 0.5f, 0.75f, 0.5f};

   void *orig_pass_nir = NULL;
   size_t orig_pass_nir_size = 0;
   union asahi_shader_key pass_variant_key = {0};
   void *orig_unif_nir = NULL;
   size_t orig_unif_nir_size = 0;
   union asahi_shader_key unif_variant_key = {0};
   size_t cold_unif_size = 0;
   uint32_t cold_cprog_size = 0;

   /* Pass 1 (cold cache): compile and populate the on-disk shader cache. */
   {
      int fd1 = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      CHECK(fd1 >= 0);
      driOptionCache info, options;
      driParseOptionInfo(&info, descriptions, ARRAY_SIZE(descriptions));
      struct pipe_screen_config config = {.options = &options, .options_info = &info};
      struct pipe_screen *screen = agx_screen_create(fd1, NULL, &config);
      CHECK(screen && agx_screen(screen)->disk_cache != NULL);
      struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
      CHECK(pipe);

      struct pipe_resource tex_tmpl = {
         .target = PIPE_TEXTURE_2D,
         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
         .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
         .bind = PIPE_BIND_RENDER_TARGET,
      };
      struct pipe_resource *rt = screen->resource_create(screen, &tex_tmpl);
      struct pipe_resource *vbuf = pipe_buffer_create(
         screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_DEFAULT, sizeof(vertices));
      CHECK(rt && vbuf);
      pipe_buffer_write(pipe, vbuf, 0, sizeof(vertices), vertices);
      struct pipe_vertex_buffer vb = {.buffer.resource = vbuf};
      pipe->set_vertex_buffers(pipe, 1, &vb);
      struct pipe_vertex_element elems[2] = {
         {.src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
         {.src_offset = 16, .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
      };
      void *velems = pipe->create_vertex_elements_state(pipe, 2, elems);
      pipe->bind_vertex_elements_state(pipe, velems);

      void *vs = util_make_vertex_passthrough_shader(pipe, 2, semantics, indices, false);
      void *fs_pass = util_make_fragment_passthrough_shader(
         pipe, TGSI_SEMANTIC_GENERIC, TGSI_INTERPOLATE_PERSPECTIVE, false);
      void *fs_unif = create_nir_fs(pipe, make_uniform_color_fs());
      CHECK(vs && fs_pass && fs_unif);

      struct pipe_depth_stencil_alpha_state zs = {0};
      void *zs_state = pipe->create_depth_stencil_alpha_state(pipe, &zs);
      struct pipe_rasterizer_state rast = {
         .front_ccw = true, .depth_clip_near = true, .depth_clip_far = true,
         .clip_halfz = true,
      };
      void *rast_state = pipe->create_rasterizer_state(pipe, &rast);
      struct pipe_blend_state b_alpha = {
         .rt[0] = {
            .blend_enable = true,
            .rgb_func = PIPE_BLEND_ADD,
            .rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA,
            .rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
            .alpha_func = PIPE_BLEND_ADD,
            .alpha_src_factor = PIPE_BLENDFACTOR_ONE,
            .alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
            .colormask = PIPE_MASK_RGBA,
         },
      };
      void *bs_alpha = pipe->create_blend_state(pipe, &b_alpha);
      pipe->bind_depth_stencil_alpha_state(pipe, zs_state);
      pipe->bind_rasterizer_state(pipe, rast_state);
      pipe->bind_blend_state(pipe, bs_alpha);

      struct pipe_viewport_state vp = {.scale = {32, 32, 1}, .translate = {32, 32, 0}};
      pipe->set_viewport_states(pipe, 0, 1, &vp);
      struct pipe_framebuffer_state fb = {
         .width = 64, .height = 64, .layers = 1, .nr_cbufs = 1,
         .cbufs[0] = {.texture = rt, .format = PIPE_FORMAT_R8G8B8A8_UNORM},
      };
      pipe->set_framebuffer_state(pipe, &fb);
      pipe->bind_vs_state(pipe, vs);

      pipe->bind_fs_state(pipe, fs_pass);
      struct agx_linked_shader *l_pass = draw_once(pipe);
      struct agx_compiled_shader *compiled_pass = agx_context(pipe)->fs;
      CHECK(compiled_pass != NULL);
      struct agx_uncompiled_shader *so_pass = fs_pass;
      bool found_pass_key = false;
      hash_table_foreach(so_pass->variants, ent) {
         if (ent->data == compiled_pass) {
            memcpy(&pass_variant_key, ent->key, sizeof(pass_variant_key.fs));
            found_pass_key = true;
            break;
         }
      }
      CHECK(found_pass_key);
      if (is_m5 || is_g17p) {
         CHECK(compiled_pass->inline_blend_nir != NULL &&
               compiled_pass->inline_blend_nir_size > 0);
         orig_pass_nir_size = compiled_pass->inline_blend_nir_size;
         orig_pass_nir = malloc(orig_pass_nir_size);
         CHECK(orig_pass_nir != NULL);
         memcpy(orig_pass_nir, compiled_pass->inline_blend_nir, orig_pass_nir_size);
      } else {
         CHECK(compiled_pass->inline_blend_nir == NULL &&
               compiled_pass->inline_blend_nir_size == 0);
      }
      if (is_m5)
         check_m5_blend_bytes("cold_cache_alpha", l_pass, true);
      else if (is_g17p)
         check_g17p_blend_bytes(l_pass, true);

      struct pipe_constant_buffer cb = {
         .user_buffer = ubo_color,
         .buffer_size = sizeof(ubo_color),
      };
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
      pipe->bind_fs_state(pipe, fs_unif);
      struct agx_linked_shader *l_unif = draw_once(pipe);
      struct agx_compiled_shader *compiled_unif = agx_context(pipe)->fs;
      CHECK(compiled_unif != NULL);

      struct agx_uncompiled_shader *so_unif = fs_unif;
      bool found_key = false;
      hash_table_foreach(so_unif->variants, ent) {
         if (ent->data == compiled_unif) {
            memcpy(&unif_variant_key, ent->key, sizeof(unif_variant_key.fs));
            found_key = true;
            break;
         }
      }
      CHECK(found_key);

      if (is_m5) {
         CHECK(compiled_unif->inline_blend_nir != NULL &&
               compiled_unif->inline_blend_nir_size > 0);
         orig_unif_nir_size = compiled_unif->inline_blend_nir_size;
         orig_unif_nir = malloc(orig_unif_nir_size);
         CHECK(orig_unif_nir != NULL);
         memcpy(orig_unif_nir, compiled_unif->inline_blend_nir, orig_unif_nir_size);
         check_m5_blend_bytes("cold_cache_uniform", l_unif, true);
         CHECK(l_unif->cprog_bo != NULL && l_unif->cprog_size > 0);
         cold_unif_size = l_unif->size;
         cold_cprog_size = l_unif->cprog_size;
      } else if (!is_g17p) {
         CHECK(compiled_unif->inline_blend_nir == NULL &&
               compiled_unif->inline_blend_nir_size == 0);
      }

      disk_cache_wait_for_idle(agx_screen(screen)->disk_cache);

      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, NULL);
      pipe->bind_vs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, NULL);
      pipe->delete_vs_state(pipe, vs);
      pipe->delete_fs_state(pipe, fs_pass);
      pipe->delete_fs_state(pipe, fs_unif);
      pipe->bind_vertex_elements_state(pipe, NULL);
      pipe->delete_vertex_elements_state(pipe, velems);
      pipe->bind_blend_state(pipe, NULL);
      pipe->delete_blend_state(pipe, bs_alpha);
      pipe->bind_depth_stencil_alpha_state(pipe, NULL);
      pipe->delete_depth_stencil_alpha_state(pipe, zs_state);
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->delete_rasterizer_state(pipe, rast_state);
      pipe_resource_reference(&rt, NULL);
      pipe_resource_reference(&vbuf, NULL);
      pipe->destroy(pipe);
      screen->destroy(screen);
      driDestroyOptionInfo(&info);
   }

   /* Pass 2 (warm cache): fresh screen/context/shaders must hit disk cache,
    * restore inline_blend_nir + inline_blend_key, and emit inline blend + M5
    * constant program instead of falling back to the split epilog. */
   {
      int fd2 = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      CHECK(fd2 >= 0);
      driOptionCache info, options;
      driParseOptionInfo(&info, descriptions, ARRAY_SIZE(descriptions));
      struct pipe_screen_config config = {.options = &options, .options_info = &info};
      struct pipe_screen *screen = agx_screen_create(fd2, NULL, &config);
      CHECK(screen && agx_screen(screen)->disk_cache != NULL);
      struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
      CHECK(pipe);

      struct pipe_resource tex_tmpl = {
         .target = PIPE_TEXTURE_2D,
         .format = PIPE_FORMAT_R8G8B8A8_UNORM,
         .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
         .bind = PIPE_BIND_RENDER_TARGET,
      };
      struct pipe_resource *rt = screen->resource_create(screen, &tex_tmpl);
      struct pipe_resource *vbuf = pipe_buffer_create(
         screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_DEFAULT, sizeof(vertices));
      CHECK(rt && vbuf);
      pipe_buffer_write(pipe, vbuf, 0, sizeof(vertices), vertices);
      struct pipe_vertex_buffer vb = {.buffer.resource = vbuf};
      pipe->set_vertex_buffers(pipe, 1, &vb);
      struct pipe_vertex_element elems[2] = {
         {.src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
         {.src_offset = 16, .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
      };
      void *velems = pipe->create_vertex_elements_state(pipe, 2, elems);
      pipe->bind_vertex_elements_state(pipe, velems);

      void *vs = util_make_vertex_passthrough_shader(pipe, 2, semantics, indices, false);
      void *fs_pass = util_make_fragment_passthrough_shader(
         pipe, TGSI_SEMANTIC_GENERIC, TGSI_INTERPOLATE_PERSPECTIVE, false);
      void *fs_unif = create_nir_fs(pipe, make_uniform_color_fs());
      CHECK(vs && fs_pass && fs_unif);

      /* Directly verify disk-cache retrieval restores inline_blend_nir + key. */
      struct agx_compiled_shader *reloaded_pass =
         agx_disk_cache_retrieve(agx_screen(screen), fs_pass, &pass_variant_key);
      CHECK(reloaded_pass != NULL);
      if (is_m5 || is_g17p) {
         CHECK(reloaded_pass->inline_blend_nir != NULL);
         CHECK(reloaded_pass->inline_blend_nir_size == orig_pass_nir_size);
         CHECK(memcmp(reloaded_pass->inline_blend_nir, orig_pass_nir,
                      orig_pass_nir_size) == 0);
         CHECK(reloaded_pass->inline_blend_key.g17p_rodata_hint == NULL);
      } else {
         CHECK(reloaded_pass->inline_blend_nir == NULL &&
               reloaded_pass->inline_blend_nir_size == 0);
      }
      free(reloaded_pass->inline_blend_nir);
      free(reloaded_pass->b.binary);
      agx_bo_unreference(&agx_screen(screen)->dev, reloaded_pass->bo);
      FREE(reloaded_pass);

      struct agx_compiled_shader *reloaded =
         agx_disk_cache_retrieve(agx_screen(screen), fs_unif, &unif_variant_key);
      CHECK(reloaded != NULL);
      if (is_m5) {
         CHECK(reloaded->inline_blend_nir != NULL);
         CHECK(reloaded->inline_blend_nir_size == orig_unif_nir_size);
         CHECK(memcmp(reloaded->inline_blend_nir, orig_unif_nir,
                      orig_unif_nir_size) == 0);
         CHECK(reloaded->inline_blend_key.g17p_rodata_hint == NULL);
      } else if (!is_g17p) {
         CHECK(reloaded->inline_blend_nir == NULL &&
               reloaded->inline_blend_nir_size == 0);
      }
      free(reloaded->inline_blend_nir);
      free(reloaded->b.binary);
      agx_bo_unreference(&agx_screen(screen)->dev, reloaded->bo);
      FREE(reloaded);

      struct pipe_depth_stencil_alpha_state zs = {0};
      void *zs_state = pipe->create_depth_stencil_alpha_state(pipe, &zs);
      struct pipe_rasterizer_state rast = {
         .front_ccw = true, .depth_clip_near = true, .depth_clip_far = true,
         .clip_halfz = true,
      };
      void *rast_state = pipe->create_rasterizer_state(pipe, &rast);
      struct pipe_blend_state b_alpha = {
         .rt[0] = {
            .blend_enable = true,
            .rgb_func = PIPE_BLEND_ADD,
            .rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA,
            .rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
            .alpha_func = PIPE_BLEND_ADD,
            .alpha_src_factor = PIPE_BLENDFACTOR_ONE,
            .alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
            .colormask = PIPE_MASK_RGBA,
         },
      };
      void *bs_alpha = pipe->create_blend_state(pipe, &b_alpha);
      pipe->bind_depth_stencil_alpha_state(pipe, zs_state);
      pipe->bind_rasterizer_state(pipe, rast_state);
      pipe->bind_blend_state(pipe, bs_alpha);

      struct pipe_viewport_state vp = {.scale = {32, 32, 1}, .translate = {32, 32, 0}};
      pipe->set_viewport_states(pipe, 0, 1, &vp);
      struct pipe_framebuffer_state fb = {
         .width = 64, .height = 64, .layers = 1, .nr_cbufs = 1,
         .cbufs[0] = {.texture = rt, .format = PIPE_FORMAT_R8G8B8A8_UNORM},
      };
      pipe->set_framebuffer_state(pipe, &fb);
      pipe->bind_vs_state(pipe, vs);

      pipe->bind_fs_state(pipe, fs_pass);
      struct agx_linked_shader *l_warm_pass = draw_once(pipe);
      if (is_m5) {
         CHECK(agx_context(pipe)->fs->inline_blend_nir != NULL);
         check_m5_blend_bytes("warm_cache_alpha", l_warm_pass, true);
      } else if (is_g17p) {
         CHECK(agx_context(pipe)->fs->inline_blend_nir != NULL);
         check_g17p_blend_bytes(l_warm_pass, true);
      }

      struct pipe_constant_buffer cb = {
         .user_buffer = ubo_color,
         .buffer_size = sizeof(ubo_color),
      };
      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
      pipe->bind_fs_state(pipe, fs_unif);
      struct agx_linked_shader *l_warm_unif = draw_once(pipe);
      if (is_m5) {
         CHECK(agx_context(pipe)->fs->inline_blend_nir != NULL);
         check_m5_blend_bytes("warm_cache_uniform", l_warm_unif, true);
         CHECK(l_warm_unif->cprog_bo != NULL &&
               l_warm_unif->cprog_size == cold_cprog_size);
         CHECK(l_warm_unif->size == cold_unif_size);
         CHECK(l_warm_unif->m5_graphics_templates != NULL);
         printf("g17s warm-cache uniform inline blend: main=%zu B, cprog=%u B\n",
                l_warm_unif->size, l_warm_unif->cprog_size);
      } else if (is_g17p) {
         check_g17p_blend_bytes(l_warm_unif, true);
      }

      pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, NULL);
      pipe->bind_vs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, NULL);
      pipe->delete_vs_state(pipe, vs);
      pipe->delete_fs_state(pipe, fs_pass);
      pipe->delete_fs_state(pipe, fs_unif);
      pipe->bind_vertex_elements_state(pipe, NULL);
      pipe->delete_vertex_elements_state(pipe, velems);
      pipe->bind_blend_state(pipe, NULL);
      pipe->delete_blend_state(pipe, bs_alpha);
      pipe->bind_depth_stencil_alpha_state(pipe, NULL);
      pipe->delete_depth_stencil_alpha_state(pipe, zs_state);
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->delete_rasterizer_state(pipe, rast_state);
      pipe_resource_reference(&rt, NULL);
      pipe_resource_reference(&vbuf, NULL);
      pipe->destroy(pipe);
      screen->destroy(screen);
      driDestroyOptionInfo(&info);
   }

   free(orig_pass_nir);
   free(orig_unif_nir);
   remove_tree(cache_dir);
#else
   (void)is_m5;
   (void)is_g17p;
#endif
}

int
main(void)
{
   const char *chip_env = getenv("ASAHI_SHIM_CHIP");
   CHECK(chip_env != NULL);
   const bool is_m5 = !strcmp(chip_env, "g17s");
   const bool is_g17p = !strcmp(chip_env, "g17p");

   int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   CHECK(fd >= 0);
   static const driOptionDescription descriptions[] = {
#include "driinfo_asahi.h"
   };
   driOptionCache info, options;
   driParseOptionInfo(&info, descriptions, ARRAY_SIZE(descriptions));
   struct pipe_screen_config config = {.options = &options, .options_info = &info};
   struct pipe_screen *screen = agx_screen_create(fd, NULL, &config);
   CHECK(screen);
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   CHECK(pipe);

   struct pipe_resource tex_tmpl = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .width0 = 64, .height0 = 64, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
   };
   struct pipe_resource *rt_rgba8 = screen->resource_create(screen, &tex_tmpl);
   struct pipe_resource *rt_rgba8_2 = screen->resource_create(screen, &tex_tmpl);
   struct pipe_resource *sample_tex = screen->resource_create(screen, &tex_tmpl);
   tex_tmpl.format = PIPE_FORMAT_R8G8B8A8_SRGB;
   struct pipe_resource *rt_srgb = screen->resource_create(screen, &tex_tmpl);
   tex_tmpl.format = PIPE_FORMAT_R16G16B16A16_FLOAT;
   struct pipe_resource *rt_fp16 = screen->resource_create(screen, &tex_tmpl);
   tex_tmpl.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   tex_tmpl.nr_samples = 4;
   tex_tmpl.nr_storage_samples = 4;
   struct pipe_resource *rt_msaa4 = screen->resource_create(screen, &tex_tmpl);
   CHECK(rt_rgba8 && rt_rgba8_2 && sample_tex && rt_srgb && rt_fp16 && rt_msaa4);

   const float vertices[3][8] = {
      {-1, -1, 0, 1, 0.25f, 0.5f, 0.75f, 0.5f},
      { 1, -1, 0, 1, 0.5f, 0.25f, 0.75f, 0.5f},
      { 0,  1, 0, 1, 0.75f, 0.5f, 0.25f, 0.5f},
   };
   struct pipe_resource *vbuf = pipe_buffer_create(
      screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_DEFAULT, sizeof(vertices));
   CHECK(vbuf);
   pipe_buffer_write(pipe, vbuf, 0, sizeof(vertices), vertices);
   struct pipe_vertex_buffer vb = {.buffer.resource = vbuf};
   pipe->set_vertex_buffers(pipe, 1, &vb);
   struct pipe_vertex_element elems[2] = {
      {.src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
      {.src_offset = 16, .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 32},
   };
   void *velems = pipe->create_vertex_elements_state(pipe, 2, elems);
   pipe->bind_vertex_elements_state(pipe, velems);

   const enum tgsi_semantic semantics[] = {TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_GENERIC};
   const unsigned indices[] = {0, 0};
   void *vs = util_make_vertex_passthrough_shader(pipe, 2, semantics, indices, false);
   void *fs_pass = util_make_fragment_passthrough_shader(
      pipe, TGSI_SEMANTIC_GENERIC, TGSI_INTERPOLATE_PERSPECTIVE, false);
   void *fs_tex = util_make_fragment_tex_shader(
      pipe, TGSI_TEXTURE_2D, TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
      false, false);
   void *fs_unif = create_nir_fs(pipe, make_uniform_color_fs());
   void *fs_discard = create_nir_fs(pipe, make_discard_fs());
   void *fs_dual = create_nir_fs(pipe, make_dual_src_fs());
   void *fs_mrt = create_nir_fs(pipe, make_mrt_fs());
   CHECK(vs && fs_pass && fs_tex && fs_unif && fs_discard && fs_dual && fs_mrt);

   struct pipe_sampler_state sampler_tmpl = {
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_LINEAR,
      .mag_img_filter = PIPE_TEX_FILTER_LINEAR,
   };
   void *sampler = pipe->create_sampler_state(pipe, &sampler_tmpl);
   struct pipe_sampler_view view_tmpl = {
      .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .target = PIPE_TEXTURE_2D,
      .swizzle_r = PIPE_SWIZZLE_X,
      .swizzle_g = PIPE_SWIZZLE_Y,
      .swizzle_b = PIPE_SWIZZLE_Z,
      .swizzle_a = PIPE_SWIZZLE_W,
   };
   struct pipe_sampler_view *sview =
      pipe->create_sampler_view(pipe, sample_tex, &view_tmpl);
   CHECK(sampler && sview);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1, &sampler);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &sview);

   struct pipe_depth_stencil_alpha_state zs = {0};
   void *zs_state = pipe->create_depth_stencil_alpha_state(pipe, &zs);
   struct pipe_rasterizer_state rast = {
      .front_ccw = true, .depth_clip_near = true, .depth_clip_far = true,
      .clip_halfz = true,
   };
   void *rast_state = pipe->create_rasterizer_state(pipe, &rast);
   pipe->bind_depth_stencil_alpha_state(pipe, zs_state);
   pipe->bind_rasterizer_state(pipe, rast_state);

   struct pipe_viewport_state vp = {.scale = {32, 32, 1}, .translate = {32, 32, 0}};
   pipe->set_viewport_states(pipe, 0, 1, &vp);
   struct pipe_blend_color bcolor = {.color = {0.25f, 0.5f, 0.75f, 1.0f}};
   pipe->set_blend_color(pipe, &bcolor);

   struct pipe_framebuffer_state fb = {
      .width = 64, .height = 64, .layers = 1, .nr_cbufs = 1,
      .cbufs[0] = {.texture = rt_rgba8, .format = PIPE_FORMAT_R8G8B8A8_UNORM},
   };
   pipe->set_framebuffer_state(pipe, &fb);
   pipe->bind_vs_state(pipe, vs);
   pipe->bind_fs_state(pipe, fs_pass);

   /* 1. Standard RGBA8 alpha blend: must inline monolithically on M5 and G17P. */
   struct pipe_blend_state b_alpha = {
      .rt[0] = {
         .blend_enable = true,
         .rgb_func = PIPE_BLEND_ADD,
         .rgb_src_factor = PIPE_BLENDFACTOR_SRC_ALPHA,
         .rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
         .alpha_func = PIPE_BLEND_ADD,
         .alpha_src_factor = PIPE_BLENDFACTOR_ONE,
         .alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
         .colormask = PIPE_MASK_RGBA,
      },
   };
   void *bs_alpha = pipe->create_blend_state(pipe, &b_alpha);
   pipe->bind_blend_state(pipe, bs_alpha);
   struct agx_linked_shader *l_alpha = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("alpha", l_alpha, true);
   else if (is_g17p)
      check_g17p_blend_bytes(l_alpha, true);
   CHECK((l_alpha->fragment_control.opaque[0] >> 29) == AGX_PASS_TYPE_TRANSLUCENT);

   /* 2. Additive blend (different blend state -> distinct cached monolithic shader). */
   struct pipe_blend_state b_add = b_alpha;
   b_add.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_ONE;
   b_add.rt[0].rgb_dst_factor = PIPE_BLENDFACTOR_ONE;
   b_add.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   b_add.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
   void *bs_add = pipe->create_blend_state(pipe, &b_add);
   pipe->bind_blend_state(pipe, bs_add);
   struct agx_linked_shader *l_add = draw_once(pipe);
   CHECK(l_add != l_alpha);
   if (is_m5)
      check_m5_blend_bytes("add", l_add, true);
   else if (is_g17p)
      check_g17p_blend_bytes(l_add, true);

   /* 3. Textured RGBA8 alpha blend: must also inline cleanly without a CProg. */
   pipe->bind_blend_state(pipe, bs_alpha);
   pipe->bind_fs_state(pipe, fs_tex);
   struct agx_linked_shader *l_tex = draw_once(pipe);
   if (is_m5) {
      check_m5_blend_bytes("tex", l_tex, true);
      CHECK(l_tex->cprog_bo == NULL && l_tex->cprog_size == 0);
      CHECK(l_alpha->cprog_bo == NULL && l_alpha->cprog_size == 0);
   } else if (is_g17p) {
      check_g17p_blend_bytes(l_tex, true);
   }

   /* 3b. Uniform-color RGBA8 alpha blend: on M5, must hoist uniform blend setup
    * into a non-empty constant program and emit it through the M5 graphics
    * program builder across consecutive UBO updates. */
   float ubo_color_a[4] = {0.25f, 0.5f, 0.75f, 0.5f};
   float ubo_color_b[4] = {0.8f, 0.2f, 0.1f, 0.4f};
   struct pipe_constant_buffer cb = {
      .user_buffer = ubo_color_a,
      .buffer_size = sizeof(ubo_color_a),
   };
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
   pipe->bind_fs_state(pipe, fs_unif);
   struct agx_linked_shader *l_unif_a = draw_once(pipe);
   cb.user_buffer = ubo_color_b;
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, &cb);
   struct agx_linked_shader *l_unif_b = draw_once(pipe);
   CHECK(l_unif_a == l_unif_b);
   if (is_m5) {
      check_m5_blend_bytes("uniform", l_unif_a, true);
      CHECK(l_unif_a->cprog_bo != NULL && l_unif_a->cprog_size > 0);
      CHECK(l_unif_a->m5_graphics_templates != NULL);
      printf("g17s uniform inline blend: main=%zu B, cprog=%u B\n",
             l_unif_a->size, l_unif_a->cprog_size);
   } else if (is_g17p) {
      check_g17p_blend_bytes(l_unif_a, true);
   }
   pipe->set_constant_buffer(pipe, MESA_SHADER_FRAGMENT, 0, NULL);

   /* 4. Edge cases that MUST fall back cleanly to the split epilog path: */
   pipe->bind_fs_state(pipe, fs_pass);

   /* 4a. Blend constant color. */
   struct pipe_blend_state b_const = b_alpha;
   b_const.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_CONST_COLOR;
   void *bs_const = pipe->create_blend_state(pipe, &b_const);
   pipe->bind_blend_state(pipe, bs_const);
   struct agx_linked_shader *l_const = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("const", l_const, false);

   /* 4b. Partial color write mask. */
   struct pipe_blend_state b_mask = b_alpha;
   b_mask.rt[0].colormask = PIPE_MASK_RGB;
   void *bs_mask = pipe->create_blend_state(pipe, &b_mask);
   pipe->bind_blend_state(pipe, bs_mask);
   struct agx_linked_shader *l_mask = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("mask", l_mask, false);

   /* 4c. LogicOp. */
   struct pipe_blend_state b_logic = {
      .logicop_enable = true,
      .logicop_func = PIPE_LOGICOP_XOR,
      .rt[0] = {.colormask = PIPE_MASK_RGBA},
   };
   void *bs_logic = pipe->create_blend_state(pipe, &b_logic);
   pipe->bind_blend_state(pipe, bs_logic);
   struct agx_linked_shader *l_logic = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("logic", l_logic, false);

   /* 4d. Dual-source blend. */
   struct pipe_blend_state b_dual = b_alpha;
   b_dual.rt[0].rgb_src_factor = PIPE_BLENDFACTOR_SRC1_COLOR;
   void *bs_dual = pipe->create_blend_state(pipe, &b_dual);
   pipe->bind_blend_state(pipe, bs_dual);
   pipe->bind_fs_state(pipe, fs_dual);
   struct agx_linked_shader *l_dual = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("dual", l_dual, false);

   /* 4e. Alpha-to-coverage with multisample enabled. */
   pipe->bind_fs_state(pipe, fs_pass);
   struct pipe_rasterizer_state rast_ms = rast;
   rast_ms.multisample = true;
   void *rast_ms_state = pipe->create_rasterizer_state(pipe, &rast_ms);
   pipe->bind_rasterizer_state(pipe, rast_ms_state);
   struct pipe_blend_state b_a2c = b_alpha;
   b_a2c.alpha_to_coverage = true;
   void *bs_a2c = pipe->create_blend_state(pipe, &b_a2c);
   pipe->bind_blend_state(pipe, bs_a2c);
   struct agx_linked_shader *l_a2c = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("a2c", l_a2c, false);

   /* 4e2. Alpha-to-one (with additive blend so destination read remains). */
   struct pipe_blend_state b_a2o = b_add;
   b_a2o.alpha_to_one = true;
   void *bs_a2o = pipe->create_blend_state(pipe, &b_a2o);
   pipe->bind_blend_state(pipe, bs_a2o);
   struct agx_linked_shader *l_a2o = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("a2o", l_a2o, false);
   pipe->bind_rasterizer_state(pipe, rast_state);

   /* 4e3. Active prolog (polygon stipple). */
   struct pipe_rasterizer_state rast_stipple = rast;
   rast_stipple.poly_stipple_enable = true;
   void *rast_stipple_state = pipe->create_rasterizer_state(pipe, &rast_stipple);
   pipe->bind_rasterizer_state(pipe, rast_stipple_state);
   pipe->bind_blend_state(pipe, bs_alpha);
   struct agx_linked_shader *l_stipple = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("prolog", l_stipple, false);
   pipe->bind_rasterizer_state(pipe, rast_state);

   /* 4f. Discard fragment shader. */
   pipe->bind_blend_state(pipe, bs_alpha);
   pipe->bind_fs_state(pipe, fs_discard);
   struct agx_linked_shader *l_discard = draw_once(pipe);
   CHECK(agx_context(pipe)->fs->inline_blend_nir == NULL);
   if (is_m5)
      check_m5_blend_bytes("discard", l_discard, false);

   /* 4g. sRGB target. */
   pipe->bind_fs_state(pipe, fs_pass);
   fb.cbufs[0].texture = rt_srgb;
   fb.cbufs[0].format = PIPE_FORMAT_R8G8B8A8_SRGB;
   pipe->set_framebuffer_state(pipe, &fb);
   struct agx_linked_shader *l_srgb = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("srgb", l_srgb, false);

   /* 4h. FP16 target. */
   fb.cbufs[0].texture = rt_fp16;
   fb.cbufs[0].format = PIPE_FORMAT_R16G16B16A16_FLOAT;
   pipe->set_framebuffer_state(pipe, &fb);
   struct agx_linked_shader *l_fp16 = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("fp16", l_fp16, false);

   /* 4i. MRT (2 color targets). */
   fb.nr_cbufs = 2;
   fb.cbufs[0].texture = rt_rgba8;
   fb.cbufs[0].format = PIPE_FORMAT_R8G8B8A8_UNORM;
   fb.cbufs[1].texture = rt_rgba8_2;
   fb.cbufs[1].format = PIPE_FORMAT_R8G8B8A8_UNORM;
   pipe->set_framebuffer_state(pipe, &fb);
   pipe->bind_fs_state(pipe, fs_mrt);
   struct agx_linked_shader *l_mrt = draw_once(pipe);
   if (is_m5)
      check_m5_blend_bytes("mrt", l_mrt, false);

   /* 4j. MSAA 4x target: must not capture inline_blend_nir and must link cleanly. */
   fb.nr_cbufs = 1;
   fb.cbufs[0].texture = rt_msaa4;
   fb.cbufs[0].format = PIPE_FORMAT_R8G8B8A8_UNORM;
   fb.cbufs[1].texture = NULL;
   pipe->set_framebuffer_state(pipe, &fb);
   pipe->bind_fs_state(pipe, fs_pass);
   struct agx_linked_shader *l_msaa = draw_once(pipe);
   CHECK(agx_context(pipe)->fs->inline_blend_nir == NULL);
   CHECK(l_msaa != l_alpha);

   /* Cleanup. */
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, NULL);
   pipe_sampler_view_reference(&sview, NULL);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 0, NULL);
   pipe->delete_sampler_state(pipe, sampler);
   pipe->bind_vs_state(pipe, NULL);
   pipe->bind_fs_state(pipe, NULL);
   pipe->delete_vs_state(pipe, vs);
   pipe->delete_fs_state(pipe, fs_pass);
   pipe->delete_fs_state(pipe, fs_tex);
   pipe->delete_fs_state(pipe, fs_unif);
   pipe->delete_fs_state(pipe, fs_discard);
   pipe->delete_fs_state(pipe, fs_dual);
   pipe->delete_fs_state(pipe, fs_mrt);
   pipe->bind_vertex_elements_state(pipe, NULL);
   pipe->delete_vertex_elements_state(pipe, velems);
   pipe->bind_blend_state(pipe, NULL);
   pipe->delete_blend_state(pipe, bs_alpha);
   pipe->delete_blend_state(pipe, bs_add);
   pipe->delete_blend_state(pipe, bs_const);
   pipe->delete_blend_state(pipe, bs_mask);
   pipe->delete_blend_state(pipe, bs_logic);
   pipe->delete_blend_state(pipe, bs_dual);
   pipe->delete_blend_state(pipe, bs_a2c);
   pipe->delete_blend_state(pipe, bs_a2o);
   pipe->bind_depth_stencil_alpha_state(pipe, NULL);
   pipe->delete_depth_stencil_alpha_state(pipe, zs_state);
   pipe->bind_rasterizer_state(pipe, NULL);
   pipe->delete_rasterizer_state(pipe, rast_state);
   pipe->delete_rasterizer_state(pipe, rast_ms_state);
   pipe->delete_rasterizer_state(pipe, rast_stipple_state);
   pipe_resource_reference(&rt_rgba8, NULL);
   pipe_resource_reference(&rt_rgba8_2, NULL);
   pipe_resource_reference(&sample_tex, NULL);
   pipe_resource_reference(&rt_srgb, NULL);
   pipe_resource_reference(&rt_fp16, NULL);
   pipe_resource_reference(&rt_msaa4, NULL);
   pipe_resource_reference(&vbuf, NULL);
   pipe->destroy(pipe);
   screen->destroy(screen);
   driDestroyOptionInfo(&info);
   test_warm_disk_cache(is_m5, is_g17p);
   close(fd);
   printf("%s: inline blend checks passed\n", chip_env);
   return 0;
}
