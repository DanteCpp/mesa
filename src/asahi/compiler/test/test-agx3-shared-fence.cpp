/* SPDX-License-Identifier: MIT */
/*
 * A shared-memory fence without workgroup execution scope must not become the
 * AGX3 workgroup rendezvous. memoryBarrierShared() in divergent control flow
 * otherwise counts arrivals the rest of the workgroup never makes. Each case
 * compiles a compute shader through selection, scheduling and encoding, and
 * counts the rendezvous encodings in its binary.
 */
#include <cstring>
#include <gtest/gtest.h>
#include "agx3_pack.h"
#include "agx_test.h"
extern "C" {
#include "compiler/nir/nir_builder.h"
#include "agx_nir.h"
}

namespace {

struct counts {
   int threadgroup, execution, device_fence;
};

enum shape {
   /* if (i < 7) { shared[i] = i; fence; shared[64 + i] = 1; } */
   DIVERGENT_FENCE,
   /* shared[i] = i; barrier(); shared[64 + i] = shared[i ^ 1]; */
   WORKGROUP_BARRIER,
};

int
occurrences(const agx_shader_part &part, enum agx_isa_encoding encoding,
            enum agx3_barrier_scope scope)
{
   struct agx3_encoded_scoped_barrier bar;
   if (!agx3_pack_scoped_barrier(encoding, scope, &bar) || !bar.length)
      return -1;
   const uint8_t *code = static_cast<const uint8_t *>(part.binary);
   int n = 0;
   for (unsigned i = part.info.main_offset;
        i + bar.length <= part.info.main_offset + part.info.main_size; ++i)
      n += !memcmp(code + i, bar.bytes, bar.length);
   return n;
}

counts
compile(enum agx_chip chip, enum agx_isa_encoding encoding, enum shape shape,
        mesa_scope execution, mesa_scope memory, nir_variable_mode modes)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "shared fence");
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.shared_size = 512;

   nir_def *i = nir_load_local_invocation_index(&b);
   nir_def *offset = nir_ishl_imm(&b, i, 2);
   nir_if *nif = NULL;
   if (shape == DIVERGENT_FENCE)
      nif = nir_push_if(&b, nir_ult_imm(&b, i, 7));

   nir_store_shared(&b, i, offset, .align_mul = 4);
   nir_barrier(&b, .execution_scope = execution, .memory_scope = memory,
               .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = modes);
   nir_def *value =
      shape == DIVERGENT_FENCE
         ? nir_imm_int(&b, 1)
         : nir_load_shared(&b, 1, 32,
                           nir_ishl_imm(&b, nir_ixor(&b, i, nir_imm_int(&b, 1)), 2),
                           .align_mul = 4);
   nir_store_shared(&b, value, nir_iadd_imm(&b, offset, 256), .align_mul = 4);

   if (nif)
      nir_pop_if(&b, nif);

   agx_preprocess_nir_native(b.shader, chip);
   agx_shader_key key = {};
   key.dev.chip = chip;
   key.dev.encoding = encoding;
   key.reserved_preamble = 8;
   key.no_preamble = true;
   agx_shader_part part = {};
   counts c = {-1, -1, -1};
   if (agx_compile_shader_nir(b.shader, &key, &part) && part.binary) {
      c.threadgroup =
         occurrences(part, encoding, AGX3_BARRIER_SCOPE_THREADGROUP);
      c.execution = occurrences(part, encoding, AGX3_BARRIER_SCOPE_EXECUTION);
      c.device_fence =
         occurrences(part, encoding, AGX3_BARRIER_SCOPE_DEVICE_FENCE);
   }
   free(part.binary);
   ralloc_free(b.shader);
   return c;
}

const struct {
   enum agx_chip chip;
   enum agx_isa_encoding encoding;
} targets[] = {
   {AGX_CHIP_G16G, AGX_ISA_ENCODING_AGX3B},
   {AGX_CHIP_G17P, AGX_ISA_ENCODING_AGX3B},
   {AGX_CHIP_G17S, AGX_ISA_ENCODING_AGX3C},
};

} /* namespace */

/* memoryBarrierShared(): no execution scope, workgroup memory scope. At any
 * memory scope it orders the invocation's own shared accesses only. */
TEST(AGX3SharedFence, MemoryOnlyFenceIsNoRendezvous)
{
   for (const auto &t : targets) {
      for (mesa_scope memory : {SCOPE_SUBGROUP, SCOPE_WORKGROUP,
                                SCOPE_QUEUE_FAMILY, SCOPE_DEVICE}) {
         SCOPED_TRACE(::testing::Message()
                      << "chip=" << t.chip << " memory=" << memory);
         const counts c = compile(t.chip, t.encoding, DIVERGENT_FENCE,
                                  SCOPE_NONE, memory, nir_var_mem_shared);
         EXPECT_EQ(c.threadgroup, 0);
         EXPECT_EQ(c.execution, 0);
      }
   }
}

/* memoryBarrier(): every mode, no execution scope. The device and texture
 * parts keep their memory-only fences; the shared part adds no rendezvous. */
TEST(AGX3SharedFence, MemoryBarrierAllModesIsNoRendezvous)
{
   const nir_variable_mode all = (nir_variable_mode)(
      nir_var_mem_shared | nir_var_mem_global | nir_var_image);
   for (const auto &t : targets) {
      SCOPED_TRACE(::testing::Message() << "chip=" << t.chip);
      const counts c = compile(t.chip, t.encoding, DIVERGENT_FENCE,
                               SCOPE_NONE, SCOPE_DEVICE, all);
      EXPECT_EQ(c.threadgroup, 0);
      EXPECT_EQ(c.execution, 0);
      EXPECT_GE(c.device_fence, 1);
   }
}

/* barrier() keeps the workgroup rendezvous with its shared-memory class. */
TEST(AGX3SharedFence, WorkgroupBarrierKeepsRendezvous)
{
   for (const auto &t : targets) {
      SCOPED_TRACE(::testing::Message() << "chip=" << t.chip);
      const counts c = compile(t.chip, t.encoding, WORKGROUP_BARRIER,
                               SCOPE_WORKGROUP, SCOPE_WORKGROUP,
                               nir_var_mem_shared);
      EXPECT_EQ(c.threadgroup, 1);
      EXPECT_EQ(c.execution, 0);
   }
}

/* The zero-byte ordering scope packs to nothing on every AGX3 encoding. */
TEST(AGX3SharedFence, SharedOrderPacksToNothing)
{
   for (auto encoding : {AGX_ISA_ENCODING_AGX3B, AGX_ISA_ENCODING_AGX3C}) {
      struct agx3_encoded_scoped_barrier bar;
      memset(&bar, 0xa5, sizeof(bar));
      ASSERT_TRUE(agx3_pack_scoped_barrier(
         encoding, AGX3_BARRIER_SCOPE_SHARED_ORDER, &bar));
      EXPECT_EQ(bar.length, 0u);
   }
}
