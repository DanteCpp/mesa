/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * G18G issue 737: a uniform-base device load only reaches the first 128
 * lanes. A workgroup larger than that must copy the base into a GPR.
 */
#include "agx_test.h"

#include <gtest/gtest.h>

namespace {

agx_context *
shader(void *mem, unsigned threads, bool g18g)
{
   agx_builder *b = agx_test_builder(mem);
   b->shader->key = rzalloc(mem, agx_shader_key);
   b->shader->key->dev.encoding = AGX_ISA_ENCODING_AGX3C;
   b->shader->key->dev.emit_agx3d = g18g;
   b->shader->stage = MESA_SHADER_COMPUTE;
   b->shader->nir = rzalloc(mem, nir_shader);
   b->shader->nir->info.stage = MESA_SHADER_COMPUTE;
   b->shader->nir->info.workgroup_size[0] = threads;
   b->shader->nir->info.workgroup_size[1] = 1;
   b->shader->nir->info.workgroup_size[2] = 1;

   agx_index dest = agx_temp(b->shader, AGX_SIZE_32);
   agx_device_load_to(b, dest, agx_uniform(12, AGX_SIZE_64),
                      agx_immediate(0), AGX_FORMAT_I32, 1, 0, false);
   return b->shader;
}

bool
load_base_is_uniform(agx_context *ctx)
{
   bool uniform = false;
   agx_foreach_instr_global(ctx, I) {
      if (I->op == AGX_OPCODE_DEVICE_LOAD)
         uniform = I->src[0].type == AGX_INDEX_UNIFORM;
   }
   return uniform;
}

} // namespace

/* Subgroups 4 and up read 0 from a uniform-base device load. */
TEST(G18GUniformDeviceBase, WideWorkgroupCopiesTheBase)
{
   void *mem = ralloc_context(NULL);
   agx_context *ctx = shader(mem, 256, true);
   agx_lower_uniform_sources(ctx);
   EXPECT_FALSE(load_base_is_uniform(ctx));
   ralloc_free(mem);
}

/* A 128-lane workgroup is one cluster; the native uniform base stays. */
TEST(G18GUniformDeviceBase, NarrowWorkgroupKeepsTheBase)
{
   void *mem = ralloc_context(NULL);
   agx_context *ctx = shader(mem, 128, true);
   agx_lower_uniform_sources(ctx);
   EXPECT_TRUE(load_base_is_uniform(ctx));
   ralloc_free(mem);
}

/* G17G uses the same encoding and is not the M6 fault. */
TEST(G18GUniformDeviceBase, G17GKeepsTheBase)
{
   void *mem = ralloc_context(NULL);
   agx_context *ctx = shader(mem, 256, false);
   agx_lower_uniform_sources(ctx);
   EXPECT_TRUE(load_base_is_uniform(ctx));
   ralloc_free(mem);
}
