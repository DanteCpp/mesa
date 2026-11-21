/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "util/macros.h"

#include "util/list.h"
#include "agx_abi.h"
#include "agx_helpers.h"
#include "agx_linker.h"
#include "agx_pack.h"
#include "agx_tilebuffer.h"
#include "agx_uvs.h"
#include "libagx_dgc.h"
#include "pool.h"
#include "shader_enums.h"

#include "asahi/compiler/agx3_ioto_uniform_program.h"
#include "bk_agx3_ioto_program_core.h"
#include "asahi/genxml/agx3_cmdstream.h"
#include "asahi/lib/agx3_compute_program_core.h"
#include "asahi/lib/agx_ppp_hal200_graphics_core.h"
#include "asahi/lib/agx_ppp_hal300_graphics_core.h"
#include "asahi/lib/agx_ppp_viewport_payload_core.h"
#include "asahi/lib/agx_vdm_hal200_bind_tail_core.h"
#include "asahi/lib/agx_va_runtime_hole.h"

#include "bk_private.h"
#include "bk_barrier.h"
#include "bk_conditional_render_core.h"
#include "bk_shader.h"
#include "bk_sync.h"
#include "bk_tess_batch.h"

#include "bk_cmd_pool.h"
#include "bk_cs_root_size.h"
#include "bk_descriptor_set.h"
#include "bk_drm_render_core.h"
#include "bk_ioto_arena_core.h"
#include "bk_pipeline_statistics_core.h"

#include "asahi/lib/agx_nir_lower_vbo.h"
#include "util/hash_table.h"
#include "util/u_dynarray.h"
#include "vulkan/vulkan_core.h"

#include "vk_command_buffer.h"

#include <stdio.h>

void *bk_cmd_buffer_emit_cdm_terminate(struct bk_cmd_buffer *cmd,
                                       void *current);
void *bk_cmd_buffer_emit_vdm_terminate(struct bk_cmd_buffer *cmd,
                                       void *current);

struct set;
struct bk_buffer;
struct bk_cmd_bo;
struct bk_cmd_pool;
struct bk_image_view;
struct bk_push_descriptor_set;
struct bk_ray_tracing_pipeline;
struct bk_api_shader;
struct bk_shader;
struct bk_linked_shader;
struct agx_usc_builder;
struct vk_shader;

/* A complete but unpublished HAL200 state update.  The referenced VDM state
 * is command-buffer-owned.  The scissor/depth records stay inline until the
 * draw chooses its live stream, so preparation cannot perturb stream-local
 * indices or make firmware-visible bytes reachable early. */
struct bk_g17p_prepared_bind_tail {
   struct agx_vdm_hal200_bind_tail tail;
   uint8_t scissors[AGX_PPP_VIEWPORT_PAYLOAD_MAX_VIEWPORTS]
                   [AGX_SCISSOR_LENGTH];
   uint8_t depth_bias[AGX_PPP_VIEWPORT_PAYLOAD_MAX_VIEWPORTS]
                    [AGX_DEPTH_BIAS_LENGTH];
   uint64_t dynamic_generation;
   uint64_t descriptor_generation;
   uint64_t vertex_buffer_generation;
   uint64_t root;
   /* The fragment shader variant the tail launches and the uniform-DMA set
    * base its program was built with (bk_g17p_dma_set_base()). */
   const struct bk_shader *fragment_shader;
   uint64_t fragment_dma_set_base;
   struct bk_image_view *render_color0;
   struct bk_image_view *render_depth;
   struct bk_image_view *render_stencil;
   VkRect2D render_area;
   VkFormat render_depth_format;
   uint32_t render_width;
   uint32_t render_height;
   uint32_t render_layer_count;
   uint32_t render_view_mask;
   uint8_t render_color_count;
   uint8_t render_samples;
   struct bk_api_shader *shaders[MESA_SHADER_MESH + 1];
   struct bk_linked_shader *linked_vs;
   struct bk_linked_shader *linked_fs;
   uint64_t varyings;
   uint32_t varying_counts_32;
   uint32_t varying_counts_16;
   uint32_t occlusion_index;
   uint8_t topology;
   uint8_t object_type;
   uint8_t provoking;
   uint8_t occlusion_mode;
   uint8_t viewport_count;
   bool generate_primitive_id;
   bool force_never_in_shader;
   bool in_meta;
   bool valid;
};

/** Root descriptor table. */
struct bk_root_descriptor_table {
   /* Address of this descriptor itself. Must be first for reflection. */
   uint64_t root_desc_addr;

   /* Descriptor set base addresses. Must follow root_desc_addr to match our
    * push layout.
    */
   uint64_t sets[BK_MAX_SETS];

   union {
      struct {
         uint32_t view_index;
         uint32_t ppp_multisamplectl;

         /* Vertex input state, indexed by compacted attribute slot. The
          * bases live at the end of this arm (attrib_base below): the clamp
          * and stride tables at 32 entries exactly fill the 256 bytes the
          * three 16-entry tables used to, which keeps every later field --
          * notably the pinned pipeline_stats offsets -- where it was.
          */
         uint32_t attrib_clamps[AGX_MAX_VBUFS];
         uint32_t attrib_strides[AGX_MAX_VBUFS];

         /* Mask of outputs flowing VS->TCS, VS->GS, or TES->GS . */
         uint64_t vertex_outputs;

         /* Address of vertex param buffer if geom/tess is used, else 0 */
         uint64_t vertex_params;

         /* Address of tessellation param buffer if tessellation used, else 0 */
         uint64_t tess_params;

         /* Address of geometry param buffer if GS is used, else 0 */
         uint64_t geometry_params;

         /* Pipeline statistics queries. This is a base address with flags. */
         uint64_t pipeline_stats;
         VkQueryPipelineStatisticFlags pipeline_stats_flags;

         float blend_constant[4];
         uint16_t no_epilog_discard;
         uint16_t _pad1;
         uint16_t api_sample_mask;
         uint16_t _pad2;
         uint16_t force_never_in_shader;
         uint16_t _pad3;
         uint16_t provoking;
         uint16_t _pad4;

         /* True if there is an API geometry shader. If false, there may still
          * be a geometry shader in use (notably for transform feedback) but it
          * should not contribute to pipeline statistics.
          */
         uint16_t api_gs;
         uint16_t _pad5;

         uint16_t rasterization_stream;
         uint16_t _pad6;

         /* Alpha-to-coverage enable for fragment shaders that write the
          * sample mask; they apply it before their own coverage update.
          * Sysval fields must sit at 4-byte offsets (root table loads). */
         uint16_t alpha_to_coverage;

         /* The pass has no attachments, so no storage backs its layers: the
          * hardware sees layer zero, while a fragment shader reading the layer
          * still gets what the vertex stage wrote. */
         uint16_t no_attachments;

         /* Mapping from varying slots written by the last vertex stage to UVS
          * indices. This mapping must be compatible with the fragment shader.
          */
         uint8_t uvs_index[VARYING_SLOT_MAX];

         /* GPU addresses of the immutable NIR constant-data blobs of the
          * bound vertex and fragment shaders, or zero if they have none. */
         uint64_t vs_constant_data_addr;
         uint64_t fs_constant_data_addr;

         /* Vertex attribute base addresses, indexed by compacted slot. */
         uint64_t attrib_base[AGX_MAX_VBUFS];
      } draw;
      struct {
         /* GPU address of the immutable NIR constant-data blob for the
          * currently bound compute shader. */
         uint64_t constant_data_addr;
         uint64_t group_count_addr;
         uint32_t base_group[3];

         /* Native G17P indirect dispatch only.  The grid helper reads the
          * threadgroup counts at group_count_addr, multiplies them by
          * grid_helper_local_size, and publishes the six-u32 launch geometry
          * at grid_helper_geometry_addr for the main record to consume.  For
          * an indirect-local grid, group_count_addr instead holds a launch
          * geometry, which the geometry copy helper publishes as it is.  Both
          * are zero for every direct dispatch and for every other chip. */
         uint64_t grid_helper_geometry_addr;

         /* Native G17P indirect dispatch whose argument buffer is a sparse
          * residency buffer: the base of that buffer's residency bitmap (a
          * struct bk_sparse_buffer_map_header followed by one bit per page),
          * and zero otherwise.  When it is set, group_count_addr names the
          * PRIMARY mapping rather than the read-only shadow, and the grid
          * helper reads the bitmap to decide whether the arguments are
          * resident, publishing a zero grid when they are not.  See
          * bk_CmdDispatchIndirect() for why the shadow cannot be read
          * directly here. */
         uint64_t grid_helper_sparse_map_addr;

         uint32_t grid_helper_local_size[3];
         uint32_t _pad_grid_helper;

         /* VK_KHR_ray_tracing_pipeline: device address of the
          * VkTraceRaysIndirectCommand2KHR record the megakernel reads its
          * shader binding tables and launch size from.  Zero for every
          * ordinary compute dispatch; the ray-tracing megakernel is the only
          * shader that loads it. */
         uint64_t trace_rays_addr;
      } cs;
   };

   /* Client push constants */
   uint8_t push[BK_MAX_PUSH_SIZE];

   /* Dynamic buffer bindings */
   struct bk_buffer_address dynamic_buffers[BK_MAX_DYNAMIC_BUFFERS];

   /* Start index in dynamic_buffers where each set starts */
   uint8_t set_dynamic_buffer_start[BK_MAX_SETS];

   /* Process-local on G17P; loaded through the root instead of cached code. */
   uint64_t printf_buffer_address;
   uint32_t mesh_draw_id;
   uint64_t mesh_grid_addr;
   uint64_t mesh_prims_query;
   uint64_t sampler_heap_address;
   uint64_t physical_sparse_map;
   uint64_t physical_sparse_base;
   uint64_t physical_sparse_size;

   /* Upper four sample positions on M5. It follows every agx3b root field,
    * so the draw arm, and with it every field after the union, keeps the
    * agx3b layout. */
   uint32_t ppp_multisamplectl_hi;

   /* M5 task draws whose grid, count or predicate live in GPU memory:
    * address of the word libagx_m5_task_dispatch writes (nonzero = execute);
    * zero for every other draw. Appended after every agx3b field. */
   uint64_t mesh_task_flag;

   /* VK_EXT_depth_clip_control: 1/2 maps clip z from [-w, w] to [0, w] in the
    * last pre-rasterization stage, 0 leaves it (agx_nir_lower_clip_m1_1). */
   float clip_z_coeff;

   /* G17P: which launch of a mesh draw this is (BK_MESH_LAUNCH_WGS). */
   uint32_t mesh_launch;

   /* G17P and M5 mesh lines: 1/64 pixel along +y in NDC for viewport 0,
    * signed by its y direction (agx_nir_lower_gen1_mesh, m5_mesh_line_bias). */
   float line_y_bias;

   /* G17P mesh-only draw: nonzero when the draw's grid is known at recording
    * and fits one launch as it is (bk_mesh_launch_whole()), so the mesh
    * workgroup IDs are the API's without reading the grid. */
   uint32_t mesh_whole;
};

/* The graphics arm is the larger of the two, so everything after the union --
 * push constants, dynamic buffers, the printf address -- keeps its offset when
 * a compute field is added. Growing the compute arm past it would move all of
 * them at once, which no single change should do silently. */
static_assert(sizeof(((struct bk_root_descriptor_table *)0)->cs) <=
                 sizeof(((struct bk_root_descriptor_table *)0)->draw),
              "compute root fields must not grow the root descriptor table");

/* helper macro for computing root descriptor byte offsets */
#define bk_root_descriptor_offset(member)                                      \
   offsetof(struct bk_root_descriptor_table, member)

/* Counts only these CPU upload paths, enabled by ASAHI_MESA_DEBUG=perf. */
struct bk_upload_stats {
   uint64_t draws, descriptor_binds, descriptor_noops;
   uint64_t root_uploads, root_bytes, root_reuses;
   uint64_t draw_param_uploads, draw_param_bytes, draw_param_reuses;
   uint64_t draw_id_uploads, draw_id_bytes, draw_id_reuses;
   uint64_t constant_uploads, constant_bytes, constant_reuses;
};

#define bk_upload_stat(cmd, field, value)                                      \
   do {                                                                       \
      if (unlikely(bk_cmd_buffer_device(cmd)->dev.debug & AGX_DBG_PERF))         \
         (cmd)->state.upload_stats.field += (value);                            \
   } while (0)

/* Root bytes the fragment stage reads only through these classes of access,
 * recorded per shader in bk_shader_info::agx3_root_reads. */
enum bk_root_reads {
   /* The root's own address: the shader can then reach any root byte. */
   BK_ROOT_READS_ADDRESS = BITFIELD_BIT(0),
   /* Vertex attribute bases, clamps and strides, and the vertex shader's
    * constant data. */
   BK_ROOT_READS_VERTEX_INPUT = BITFIELD_BIT(1),
   BK_ROOT_READS_DYNAMIC_BUFFERS = BITFIELD_BIT(2),
};

/* Every root byte a draw may republish without the fragment stage observing
 * it unless the fragment shader reads that class (bk_m5_fragment_root()), as
 * X(from, to, class) spans in ascending order. The vertex-input spans hold
 * just their fields: the clamp and stride tables are adjacent and followed by
 * vertex_outputs, and attrib_base ends the graphics arm. */
#define BK_ROOT_FRAGMENT_HIDDEN_SPANS(X)                                       \
   X(root_desc_addr, sets, BK_ROOT_READS_ADDRESS)                              \
   X(draw.attrib_clamps, draw.vertex_outputs, BK_ROOT_READS_VERTEX_INPUT)      \
   X(draw.vs_constant_data_addr, draw.fs_constant_data_addr,                   \
     BK_ROOT_READS_VERTEX_INPUT)                                               \
   X(draw.attrib_base, push, BK_ROOT_READS_VERTEX_INPUT)                       \
   X(dynamic_buffers, set_dynamic_buffer_start, BK_ROOT_READS_DYNAMIC_BUFFERS)

/* The BK_ROOT_READS_* classes a read of root bytes [start_B, end_B) falls
 * in. */
static inline uint8_t
bk_root_reads_of(uint64_t start_B, uint64_t end_B)
{
   uint8_t reads = 0;
#define BK_ROOT_CLASSIFY(from, to, class)                                      \
   if (start_B < bk_root_descriptor_offset(to) &&                              \
       end_B > bk_root_descriptor_offset(from))                                \
      reads |= (class);
   BK_ROOT_FRAGMENT_HIDDEN_SPANS(BK_ROOT_CLASSIFY)
#undef BK_ROOT_CLASSIFY
   return reads;
}

/* Whether two roots agree on every byte outside the hidden spans. */
static inline bool
bk_root_matches_for_fragment(const struct bk_root_descriptor_table *a,
                             const struct bk_root_descriptor_table *b)
{
   size_t at = 0;
#define BK_ROOT_COMPARE_BEFORE(from, to, class)                                \
   if (memcmp((const uint8_t *)a + at, (const uint8_t *)b + at,                \
              bk_root_descriptor_offset(from) - at))                           \
      return false;                                                            \
   at = bk_root_descriptor_offset(to);
   BK_ROOT_FRAGMENT_HIDDEN_SPANS(BK_ROOT_COMPARE_BEFORE)
#undef BK_ROOT_COMPARE_BEFORE
   return !memcmp((const uint8_t *)a + at, (const uint8_t *)b + at,
                  sizeof(*a) - at);
}

/* Build the same visible-byte sequence as bk_root_matches_for_fragment().
 * Keeping this derived from the shared span list includes future root fields
 * and padding without duplicating the root ABI. */
#define BK_ROOT_FRAGMENT_SUBTRACT(from, to, class)                             \
   -(bk_root_descriptor_offset(to) - bk_root_descriptor_offset(from))
enum {
   BK_ROOT_FRAGMENT_VISIBLE_SIZE = sizeof(struct bk_root_descriptor_table)
      BK_ROOT_FRAGMENT_HIDDEN_SPANS(BK_ROOT_FRAGMENT_SUBTRACT),
};
#undef BK_ROOT_FRAGMENT_SUBTRACT

struct bk_root_fragment_snapshot {
   uint8_t bytes[BK_ROOT_FRAGMENT_VISIBLE_SIZE];
};

static inline void
bk_root_fragment_snapshot_store(struct bk_root_fragment_snapshot *snapshot,
                                const struct bk_root_descriptor_table *root)
{
   size_t at = 0, out = 0;
#define BK_ROOT_FRAGMENT_COPY(from, to, class)                                 \
   do {                                                                        \
      const size_t size = bk_root_descriptor_offset(from) - at;                \
      memcpy(snapshot->bytes + out, (const uint8_t *)root + at, size);         \
      out += size;                                                             \
      at = bk_root_descriptor_offset(to);                                      \
   } while (0);
   BK_ROOT_FRAGMENT_HIDDEN_SPANS(BK_ROOT_FRAGMENT_COPY)
#undef BK_ROOT_FRAGMENT_COPY
   memcpy(snapshot->bytes + out, (const uint8_t *)root + at,
          sizeof(*root) - at);
   assert(out + sizeof(*root) - at == sizeof(snapshot->bytes));
}

static_assert(bk_root_descriptor_offset(draw.attrib_strides) ==
                    bk_root_descriptor_offset(draw.attrib_clamps) +
                       sizeof(((struct bk_root_descriptor_table *)0)
                                 ->draw.attrib_clamps) &&
                 bk_root_descriptor_offset(draw.vertex_outputs) ==
                    bk_root_descriptor_offset(draw.attrib_strides) +
                       sizeof(((struct bk_root_descriptor_table *)0)
                                 ->draw.attrib_strides),
              "only the clamp and stride tables precede vertex_outputs");
static_assert(bk_root_descriptor_offset(draw.fs_constant_data_addr) ==
                 bk_root_descriptor_offset(draw.vs_constant_data_addr) +
                    sizeof(uint64_t),
              "the fragment constant data follows the vertex constant data");
static_assert(bk_root_descriptor_offset(push) ==
                 bk_root_descriptor_offset(draw.attrib_base) +
                    sizeof(((struct bk_root_descriptor_table *)0)
                              ->draw.attrib_base),
              "attrib_base ends the graphics arm");
static_assert(bk_root_descriptor_offset(set_dynamic_buffer_start) ==
                 bk_root_descriptor_offset(dynamic_buffers) +
                    sizeof(((struct bk_root_descriptor_table *)0)
                              ->dynamic_buffers),
              "the dynamic buffer table is followed by its set starts");

struct bk_descriptor_state {
   bool root_dirty;
   uint64_t generation;
   struct bk_root_descriptor_table root;
   /* Immutable command-buffer storage; reset with descriptor state. Only the
    * uploaded head and tail are compared, excluding unused dynamic slots. */
   struct agx_ptr uploaded_root;
   uint32_t uploaded_root_size;
   uint64_t uploaded_root_generation;
   uint8_t uploaded_dynamic_buffer_end;

   /* One past the highest root.dynamic_buffers entry any bind wrote. Later
    * entries are unread by every bound layout and stay out of uploads. */
   uint8_t dynamic_buffer_end;

   uint32_t set_sizes[BK_MAX_SETS];
   struct bk_descriptor_set *sets[BK_MAX_SETS];
   uint32_t sets_dirty;

   struct bk_push_descriptor_set *push[BK_MAX_SETS];
   uint32_t push_dirty;
   uint32_t push_active;

   /* Sets bound from a descriptor buffer (or embedded immutable samplers):
    * root.sets[] holds their address, and their contents may change until
    * the commands execute, so recording never reads them. */
   uint32_t buffer_active;
};

struct bk_attachment {
   VkRenderingAttachmentFlagBitsKHR flags;

   VkFormat vk_format;
   struct bk_image_view *iview;

   /* The API load and store operations and the layout the pass was begun
    * with. */
   VkAttachmentLoadOp load_op;
   VkAttachmentStoreOp store_op;
   VkImageLayout layout;

   VkResolveModeFlagBits resolve_mode;
   struct bk_image_view *resolve_iview;

   bool clear;
   uint32_t clear_colour[4];
};

struct bk_bg_eot {
   uint64_t usc;
   uint32_t primary_storage_B;
   uint32_t staging_storage_B;
   union {
      struct agx_counts_packed counts;
      uint64_t rsrc_spec;
   };
   /* Frame anatomy profiler (bk_frame_anatomy.h): per-RT op summary of the
    * program. Written only while HK_FRAME_ANATOMY is set; otherwise zero and
    * never read. Not part of any hardware or kernel structure. */
   uint8_t anatomy_ops[BK_MAX_RTS];
};

/* BG/EOT programs are self-addressed and may reference command-buffer pool
 * uploads, so reuse them only inside the command buffer which owns both. One
 * exact render-pass key owns its main/partial BG/EOT variants; hash collisions
 * remain harmless misses. */
#define BK_G17P_BG_EOT_CACHE_ENTRIES 16
#define BK_G17P_BG_EOT_CACHE_KEY_MAX 768
struct bk_g17p_bg_eot_dev_entry;

struct bk_g17p_bg_eot_cache_entry {
   uint8_t key[BK_G17P_BG_EOT_CACHE_KEY_MAX];
   struct bk_bg_eot program[4];
   /* frg-codegen v4: the device-lifetime entry this pass fills or reuses,
    * or NULL. */
   struct bk_g17p_bg_eot_dev_entry *dev_entry;
   uint64_t hash;
   uint16_t key_size;
   uint8_t valid_variants;
   uint8_t direct_color_resolve_variants;
   bool valid;
};

struct bk_render_registers {
   uint32_t width, height, layers;
   uint32_t zls_width, zls_height;
   uint32_t isp_bgobjdepth;
   uint32_t isp_bgobjvals;
   struct agx_zls_control_packed zls_control, zls_control_partial;
   uint32_t depth_dimensions;
   bool process_empty_tiles;
   /* The render area is every attachment's full extent: the tile stores
    * write no pixel outside it. */
   bool whole_attachments;
   enum u_tristate dbias_is_int;

   struct {
      uint32_t dimensions;
      uint64_t buffer, meta;
      uint32_t stride, meta_stride;
   } depth;

   struct {
      uint64_t buffer, meta;
      uint32_t stride, meta_stride;
   } stencil;

   struct {
      struct bk_bg_eot main;
      struct bk_bg_eot partial;
   } bg;

   struct {
      struct bk_bg_eot main;
      struct bk_bg_eot partial;
   } eot;

   /* Frame anatomy profiler only: bit i set when colour attachment i's image
    * layout is compressed. Zero unless HK_FRAME_ANATOMY is set. */
   uint8_t color_compressed_mask;
};

struct bk_rendering_state {
   VkRenderingFlagBits flags;

   /* May have written storage memory/XFB anywhere in this API pass, including
    * before a physical render split. Used only for conservative barriers. */
   bool barrier_shader_writes;

   VkRect2D area;
   uint32_t layer_count;
   uint32_t view_mask;

   /* The main G17P EOT writes the single-sample resolve target directly. */
   bool g17p_direct_color_resolve;

   /* Exact admission result for the current physical tilebuffer layout.
    * The layout is immutable after BeginRendering except for the one
    * attachmentless late-sample transition, which refreshes this value. */
   bool g17p_rt0_layout_supported;

   uint32_t color_att_count;
   struct bk_attachment color_att[BK_MAX_RTS];
   struct bk_attachment depth_att;
   struct bk_attachment stencil_att;

   struct agx_tilebuffer_layout tilebuffer;
   struct bk_render_registers cr;

   /* G17P precise depth store: the ZLS words for the
    * stream that ends the pass, when it is also the first stream (only) or
    * a later one (last). Depth honours storeOp there; stencil keeps the
    * forced store. zls_first_cs is the stream BeginRendering created. */
   bool zls_precise;
   struct agx_zls_control_packed zls_control_only, zls_control_last;
   const void *zls_first_cs;

   /* Track depth and stencil independently: an untouched aspect can be
    * removed from every stream even when the other aspect is cleared or
    * used. NONE/NONE aspects must retain their memory contents; eliding
    * untouched LOAD/STORE aspects also avoids redundant traffic.
    */
   VkImageAspectFlags g17p_zls_elide_mask;
   VkImageAspectFlags g17p_zls_touched;

   /* Of the touched aspects, those a draw or clear may have changed. A pass
    * that only tests an elidable aspect leaves its memory contents as they
    * were, so it keeps the load and drops the store
    * (bk_g17p_zls_elide_store()).
    */
   VkImageAspectFlags g17p_zls_written;

   /* M5: an attachment of the pass can alias another resource (aliased,
    * sparse or linear image), so its stores count as memory writes for
    * dependency tracking (BK_SYNC_GFX_MEM). */
   bool m5_attachments_reach_memory;

   /* AGX3 dead depth store: the "only" ZLS word with the depth store
    * dropped, and whether the pass processes empty tiles without that store,
    * for a later pass that makes the store dead (bk_take_pending_z_store()).
    * Valid when no_z_store_ok. */
   bool no_z_store_ok;
   bool process_empty_tiles_without_z;
   struct agx_zls_control_packed zls_control_only_no_z_store;

   /* G17P: colour attachments a draw or clear of the pass may write, and
    * whether a fragment shader reads the tilebuffer. See
    * bk_g17p_elide_untouched_color(). */
   uint8_t color_written;
   bool color_read;

   /* G17P: the pass's first draw was seen, and the colour attachments it
    * overwrote at every sample before anything could read them. See
    * bk_g17p_note_first_draw(). */
   bool first_draw_seen;
   uint8_t color_covered;
};

struct bk_index_buffer_state {
   struct bk_addr_range buffer;
   enum agx_index_size size;
};

/* Dirty tracking bits for state not tracked by vk_dynamic_graphics_state or
 * shaders_dirty.
 */
enum bk_dirty {
   BK_DIRTY_VB = BITFIELD_BIT(0),
   BK_DIRTY_OCCLUSION = BITFIELD_BIT(1),
   BK_DIRTY_PROVOKING = BITFIELD_BIT(2),
   BK_DIRTY_VARYINGS = BITFIELD_BIT(3),
};

/* Where a G17P graphics uniform range's source address comes from. */
enum bk_g17p_range_source {
   /* Fixed for the shader and device (heap pointers, promoted constants). */
   BK_G17P_RANGE_SOURCE_CONSTANT = 0,
   /* The graphics root plus a fixed offset. */
   BK_G17P_RANGE_SOURCE_ROOT = 1,
   BK_G17P_RANGE_SOURCE_DRAW_PARAMS = 2,
   BK_G17P_RANGE_SOURCE_DRAW_ID = 3,
   /* A bound descriptor set's address plus a fixed offset, or the zero page
    * when the set is not bound (uniform DMA spans). */
   BK_G17P_RANGE_SOURCE_SET = 4,
   /* A whole uniform DMA buffer span: the buffer whose descriptor the root
    * (dynamic buffers) or a static descriptor set holds at a fixed byte, plus
    * a fixed offset, while that buffer covers the span. */
   BK_G17P_RANGE_SOURCE_BUFFER = 5,
   /* Uploaded while collecting the ranges (combined-sampler pointers,
    * spilled render-target descriptors), or a buffer span the buffer does
    * not wholly cover: not re-derivable. */
   BK_G17P_RANGE_SOURCE_UPLOADED,
};

struct bk_g17p_gfx_loader;

/* How a stage's uniform ranges derive from the draw state, recorded from a
 * collection by bk_g17p_graphics_uniform_ranges(), and the loader they were
 * published with. A draw with the same shader, linked executable, imageblock,
 * range-selecting state (bk_g17p_launch_flags()) and, for a fragment shader,
 * admission inputs derives its ranges from it. Every BO the loader names was
 * retained when it was recorded. */
#define BK_G17P_LAUNCH_SLOTS 8
#define BK_G17P_LAUNCH_ROOT_SET 0xff

struct bk_g17p_launch {
   const struct bk_shader *s;
   const struct bk_linked_shader *linked;
   const struct bk_g17p_gfx_loader *loader;
   const struct bk_shader *hw_vs;
   uint16_t tile_size_px;
   uint8_t sample_size_B;
   uint8_t nr_samples;
   uint8_t topology;
   uint8_t provoking;
   uint8_t flags;
   uint8_t count;
   bool valid;
   uint8_t source[BK_AGX3_G17P_GRAPHICS_MAX_RANGES];
   /* Descriptor set of a SET or BUFFER range; BK_G17P_LAUNCH_ROOT_SET for a
    * BUFFER range whose descriptor is in the root. */
   uint8_t set[BK_AGX3_G17P_GRAPHICS_MAX_RANGES];
   uint16_t start_halfs[BK_AGX3_G17P_GRAPHICS_MAX_RANGES];
   uint16_t size_halfs[BK_AGX3_G17P_GRAPHICS_MAX_RANGES];
   /* Byte of a BUFFER range's descriptor in its root or set. */
   uint16_t descriptor_B[BK_AGX3_G17P_GRAPHICS_MAX_RANGES];
   /* Root/set/buffer offset, or the address of a constant range. */
   uint64_t offset[BK_AGX3_G17P_GRAPHICS_MAX_RANGES];
};

struct bk_g17p_graphics_ioto_cache_entry {
   /* The loader of the last draw, the program published from it and the
    * range addresses written into it (bk_g17p_publish_gfx_loader()). */
   const struct bk_g17p_gfx_loader *loader;
   const struct bk_linked_shader *loader_linked;
   uint64_t loader_program;
   struct agx3_ioto_uniform_range
      loader_ranges[BK_AGX3_G17P_GRAPHICS_MAX_RANGES];

   /* Inputs of the last admitted program (bk_upload_usc_words_internal()). */
   struct {
      const struct bk_shader *s;
      const struct bk_linked_shader *linked;
      const struct bk_shader *hw_vs;
      bool spills;
      uint8_t topology;
      uint8_t provoking;
   } admit;
};

/* The inputs of a G17P VertexProgramVariant state update other than the
 * pipeline address (bk_agx3_publish_vertex_state()). */
struct bk_g17p_vdm_update_key {
   const struct bk_shader *hw_vs;
   uint32_t selector;
   uint8_t draw_path;
   uint8_t provoking;
   bool rasterization_enabled;
   bool primitive_id;
   bool ordinary;
   bool selector_valid;
};

struct bk_graphics_state {
   /* A bound graphics program writes external memory. */
   bool shaders_write_memory;

   struct bk_rendering_state render;
   struct bk_descriptor_state descriptors;

   enum bk_dirty dirty;
   /* CmdBindVertexBuffers is not vk_dynamic_graphics_state and therefore does
    * not advance its generation.  Prepared G17P roots snapshot this separate
    * identity so a post-BeginRendering bind cannot publish stale attrib VAs. */
   uint64_t vertex_buffer_generation;
   /* The vertex_buffer_generation that descriptors.root.draw.attrib_* were
    * last derived from (bk_flush_dynamic_state_slow()'s vertex-buffer
    * block).  A prepared bind tail records this, not the live generation:
    * a tail rebuilt outside a full flush (vkCmdBindIndexBuffer) must not
    * vouch for a root whose attrib VAs predate the latest vertex binding. */
   uint64_t root_vertex_buffer_generation;

   uint64_t root;
   /* CPU copy of the root uploaded at root (bk_cmd_buffer_upload_root()). */
   const struct bk_root_descriptor_table *root_cpu;
   /* The root the fragment program was last published with, and its CPU
    * copy (bk_m5_fragment_root()). */
   uint64_t fs_root;
   const struct bk_root_descriptor_table *fs_root_cpu;

   /* A successful equality check between the immutable root snapshots at
    * fs_root and this address. Cleared whenever fs_root is replaced. */
   uint64_t fs_root_checked_root;

   /* CPU-cached copies of exactly the fragment-visible published bytes. The
    * pointer keys name the immutable GPU-mapped copies, never their contents. */
   struct bk_root_fragment_snapshot root_fragment_snapshot;
   struct bk_root_fragment_snapshot fs_root_fragment_snapshot;
   const struct bk_root_descriptor_table *root_fragment_snapshot_cpu;
   const struct bk_root_descriptor_table *fs_root_fragment_snapshot_cpu;

   uint64_t draw_params;
   uint64_t draw_id_ptr;
   uint32_t g17p_draw_params_value[2];
   uint32_t g17p_draw_id_value;
   bool g17p_draw_params_host_valid;
   bool g17p_draw_id_host_valid;

   struct bk_g17p_prepared_bind_tail g17p_bind_tail;
   /* The last complete build of g17p_bind_tail, whose other fields still
    * describe its inputs. A rebuild whose only changed inputs are the root,
    * descriptor or vertex-buffer identity (a descriptor push or vertex-buffer
    * bind) reads exactly the same state for every record except the fragment
    * record's stage program, which is recomputed from fs_fields; see
    * bk_g17p_build_native_bind_tail(). */
   struct {
      struct agx_ppp_hal200_fragment_fields fs_fields;
      /* The state block of the last build or memo rebuild: CPU copy, its
       * compact address and size. */
      const uint8_t *state_cpu;
      uint32_t state_compact;
      uint32_t state_size;
      const struct bk_shader *fs;
      const struct bk_shader *hw_vs;
      bool valid;
   } g17p_bind_tail_memo;

   /* Per vertex-attribute slot: the last API format seen and the pipe format
    * and interchange block size bk_flush_dynamic_state_slow() derives from
    * it, which are pure functions of the format. */
   struct {
      VkFormat vk_format;
      enum pipe_format format;
      unsigned interchange_align;
   } vb_format_memo[AGX_MAX_VBUFS];

   /* The last VertexProgramVariant state update this command buffer encoded,
    * keyed by every encoder input except the pipeline address; see
    * bk_agx3_publish_vertex_state(). */
   struct {
      struct bk_g17p_vdm_update_key key;
      struct agx_vdm_state_update encoded;
      bool valid;
   } g17p_vdm_update_memo;

   /* The last M5 HAL300 graphics core agx_ppp_hal300_graphics_core_pack()
    * packed, a pure function of its fields (zero-initialised, so they compare
    * bytewise); see bk_flush_ppp_state(). */
   struct {
      struct agx_ppp_hal300_graphics_fields fields;
      struct agx_ppp_hal300_graphics_core state;
      bool valid;
   } m5_ppp_pack_memo;

   uint32_t shaders_dirty;
   struct bk_api_shader *shaders[MESA_SHADER_MESH + 1];

   /* Stages bound to a shader compiled without custom-border emulation that
    * has an emulating twin to switch to once the device needs it. */
   uint32_t border_free_stages;

   /* G17P: the twin of the bound fragment shader that reads its per-primitive
    * inputs from the bound mesh shader's records (bk_mesh_fs_twin()), or
    * NULL to draw with the bound fragment shader itself. */
   struct bk_api_shader *mesh_fs;

   /* The zs_self_dep twin of the bound fragment shader while a dynamic
    * depth/stencil feedback loop is enabled (bk_fbl_twin()), or NULL. */
   struct bk_api_shader *fbl_fs;

   /* Vertex buffers */
   struct bk_addr_range vb[AGX_MAX_VBUFS];

   /* Transform feedback buffers */
   struct bk_addr_range xfb[4];

   /* Is transform feedback enabled? */
   bool xfb_enabled;

   /* Internal transform feedback offset vec4.
    *
    * TODO: Strictly could be global.
    */
   uint64_t xfb_offsets;

   /* Pointer to the GPU memory backing active transform feedback queries,
    * per-stream. Zero if no query is bound.
    */
   uint64_t xfb_query[4];
   /* Active VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT reports, per stream. */
   uint64_t prims_generated_query[4];

   struct bk_index_buffer_state index;
   enum agx_primitive topology;
   enum agx_object_type object_type;

   /* Provoking vertex 0, 1, or 2. Usually 0 or 2 for FIRST/LAST. 1 can only be
    * set for tri fans.
    */
   uint8_t provoking;

   struct {
      enum agx_visibility_mode mode;

      /* If enabled, index of the current occlusion query in the occlusion heap.
       * There can only be one active at a time (hardware contraint).
       */
      uint16_t index;

      /* On M5, an empty query needs only its logical availability word.
       * Assign its physical counter when the first draw needs it. */
      struct bk_query_pool *pending_pool;
      uint32_t pending_query;

      /* Recording order (bk_sync_stream::seq) of the render the active query
       * began in, 0 if none: only a query that also ends there may become
       * available with that render's retirement. */
      uint32_t begin_render_seq;
   } occlusion;

   /* Fast linked shader data structures */
   uint64_t varyings;
   struct agx_varyings_vs linked_varyings;
   /* bk_g17p_smooth_varyings_supported() of the last (linked FS, hardware
    * VS, topology, provoking) it evaluated against the linked_varyings
    * above. Cleared whenever linked_varyings is reassigned. */
   struct {
      const void *linked;
      const void *vs;
      uint32_t topology;
      uint32_t provoking;
      bool valid;
      bool result;
   } g17p_smooth_memo;

   uint32_t linked_dirty;
   struct bk_linked_shader *linked[MESA_SHADER_STAGES];

   /* Last complete fast-link selection for this command buffer. The shader
    * identity is part of the cache key: identical bytes owned by a different
    * shader may have a different lifetime. Graphics-state invalidation clears
    * these records with the rest of gfx state. */
   struct {
      struct bk_shader *shader;
      struct bk_fast_link_key_vs key;
      bool valid;
   } fast_link_vs;
   struct {
      struct bk_shader *shader;
      struct bk_fast_link_key_fs key;
      bool valid;
      /* Colour attachments the linked fragment program writes, and whether
       * it reads the tilebuffer. */
      uint8_t color_written;
      bool reads_tib;
   } fast_link_fs;

   /* Keep these four native graphics caches contiguous: AGX2 resets leave
    * their zero-initialized storage untouched.
    *
    * Exact last VS/FS loader identities. These are recording-only selectors;
    * the published programs and every referenced BO remain command-buffer-
    * owned. Submission never mutates or consults this cache. */
   struct bk_g17p_graphics_ioto_cache_entry g17p_ioto[2];
   struct bk_g17p_launch g17p_launch[2][BK_G17P_LAUNCH_SLOTS];
   /* Exact command-buffer-owned uploads for combined texture/sampler pointer
    * pairs.  Shader/material switches may revisit the same pair thousands of
    * times; a fresh upload address would otherwise force a fresh Ioto program
    * even though the GPU-visible values are identical. */
   struct {
      /* Key: descriptor-set texture word address and sampler state address. */
      uint64_t pointers[2];
      /* The uploaded pair; its texture pointer is 32-byte aligned
       * (bk_g17p_texture_state_pointer). */
      uint64_t published[2];
      uint64_t address;
      bool valid;
   } g17p_combined_sampler_pointers[32];

   /* Exact immutable eMRT descriptor-table upload used by every fragment
    * program in one spilled render configuration. */
   struct {
      uint8_t entries[BK_MAX_RTS][AGX_G17P_EMRT_DESCRIPTOR_SIZE];
      uint64_t address;
      uint64_t pointer;
      uint8_t color_att_count;
      bool valid;
   } g17p_spilled_rt_descs;

   bool generate_primitive_id;

   /* Tessellation state */
   struct {
      uint64_t out_draws;
      uint64_t grids;
      /* Direct thread count bounding the TES-before-GS kernel of this draw,
       * or 0 to use the indirect grid (bk_tes_direct_query_threads()). */
      uint32_t tes_direct_threads;
      struct bk_tess_info info;
      enum mesa_prim prim;
   } tess;

   /* Native mesh draw state (VK_EXT_mesh_shader, native TA path). */
   struct {
      struct agx_ptr params;
      uint64_t grid, object_program, object_params;
      /* Direct-draw API task grid, used for the object dispatch when an
       * task shader is bound. */
      uint32_t task_grid[3];
   } mesh;

   /* Needed by vk_command_buffer::dynamic_graphics_state */
   struct vk_vertex_input_state _dynamic_vi;
   struct vk_sample_locations_state _dynamic_sl;
};

/* The CPU source is mutable. Snapshot its visible bytes at publication,
 * before copying into the immutable GPU-mapped root. */
static inline void
bk_root_fragment_snapshot_publish(struct bk_graphics_state *gfx,
                                  const struct bk_root_descriptor_table *cpu,
                                  const struct bk_root_descriptor_table *source)
{
   bk_root_fragment_snapshot_store(&gfx->root_fragment_snapshot, source);
   gfx->root_fragment_snapshot_cpu = cpu;
}

/* Select the same immutable root as the fresh byte comparison. Hidden reads
 * always use the current root; snapshots and the positive identity memo only
 * accelerate the equality check for shaders that cannot observe those bytes. */
static inline uint64_t
bk_m5_fragment_root_select(struct bk_graphics_state *gfx, uint8_t root_reads,
                           bool use_memo, bool use_shadow)
{
   const uint8_t hidden = BK_ROOT_READS_ADDRESS | BK_ROOT_READS_VERTEX_INPUT |
                          BK_ROOT_READS_DYNAMIC_BUFFERS;

   const bool shadow = use_shadow &&
                       gfx->root_fragment_snapshot_cpu == gfx->root_cpu &&
                       gfx->fs_root_fragment_snapshot_cpu == gfx->fs_root_cpu;

   if (gfx->fs_root && gfx->fs_root != gfx->root && gfx->root_cpu &&
       !(root_reads & hidden) &&
       ((use_memo && gfx->fs_root_checked_root &&
         gfx->fs_root_checked_root == gfx->root) ||
        (shadow
            ? !memcmp(&gfx->fs_root_fragment_snapshot,
                      &gfx->root_fragment_snapshot,
                      sizeof(gfx->root_fragment_snapshot))
            : bk_root_matches_for_fragment(gfx->fs_root_cpu, gfx->root_cpu)))) {
      if (use_memo)
         gfx->fs_root_checked_root = gfx->root;
      return gfx->fs_root;
   }

   if (use_shadow && gfx->root_cpu &&
       gfx->root_fragment_snapshot_cpu == gfx->root_cpu) {
      gfx->fs_root_fragment_snapshot = gfx->root_fragment_snapshot;
      gfx->fs_root_fragment_snapshot_cpu = gfx->root_cpu;
   } else {
      gfx->fs_root_fragment_snapshot_cpu = NULL;
   }

   gfx->fs_root_checked_root = 0;
   gfx->fs_root = gfx->root;
   gfx->fs_root_cpu = gfx->root_cpu;
   return gfx->root;
}

struct bk_compute_state {
   struct bk_descriptor_state descriptors;
   struct bk_api_shader *shader;
};

/*
 * VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR state.  Descriptor sets and push
 * constants bound to the ray-tracing bind point are separate from the compute
 * ones, even though the pipeline executes as a compute shader: an application
 * may bind a compute pipeline's sets between vkCmdBindPipeline and
 * vkCmdTraceRaysKHR and neither may disturb the other.
 */
struct bk_ray_tracing_state {
   struct bk_descriptor_state descriptors;
   struct bk_ray_tracing_pipeline *pipeline;
};

#define BK_RT_SHADER_STAGES                                                    \
   (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |         \
    VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |       \
    VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR)

struct bk_cmd_push {
   void *map;
   uint64_t addr;
   uint32_t range;
   bool no_prefetch;
};

struct bk_scratch_req {
   bool main;
   bool preamble;
};

/* struct agx_timestamp_req lives in bk_sync.h. */

/*
 * bk_cs represents a single control stream, to be enqueued either to the
 * CDM or VDM for compute/3D respectively.
 */
enum bk_cs_type {
   BK_CS_CDM,
   BK_CS_VDM,
};

/* Fragment programs a control stream records for the frame anatomy
 * profiler (bk_frame_anatomy.h). */
#define BK_FA_FS_MAX 4

struct bk_cs {
   struct list_head node;

   /* Parent command buffer. Convenience. */
   struct bk_cmd_buffer *cmd;

   /* Data master */
   enum bk_cs_type type;

   /* Address of the root control stream for the job */
   uint64_t addr;

   /* Fat pointer to the start of the current chunk of the control stream */
   struct agx_ptr chunk;

   /* Start pointer of the root control stream */
   void *start;

   /* Size of the root control stream allocation */
   uint32_t root_size;

   /* Current pointer within the control stream */
   void *current;

   /* End pointer of the current chunk of the control stream */
   void *end;

   /* AGX3 CDM: the value of current just after a strong cache flush, so a
    * flush requested with nothing recorded since it can be elided. */
   void *strong_flush_end;

   /* Likewise for the weak ordering barrier attached to a native launch. A
    * later cache-flush request upgrades this record in place instead of
    * emitting two adjacent barriers. */
   void *weak_barrier_end;

   /* AGX3 CDM: the value of current just after a strong flush that also has
    * memory scope (AGX3_CDM_BARRIER_FLUSH_MEMORY): on G17P and M5 the
    * flushes bk_cdm_visibility_flush() asks for, and on M5 also the driver's
    * own (bk_cdm_cache_flush()). A patch site written later
    * (struct bk_alias_site) does not move it. */
   void *memory_flush_end;

   /* M5: patch sites recorded in this stream (bk_cmd_buffer::alias_sites),
    * which a merge moves with its bytes. */
   uint32_t alias_sites;

   /* AGX3 CDM: the value of current just after a standalone barrier that is
    * the stream's *first* record. Apple's emitter only opens a stream with a
    * barrier (the strong flush that makes a preceding render visible), so a
    * stream is canonical on its own; but concatenating it after a stream that
    * already ends in a barrier leaves two adjacent barriers with no launch
    * between them, which Apple never emits. bk_cs_merge_cdm() uses this to
    * fold the leading record into the destination's trailing one. */
   void *leading_barrier_end;

   /* AGX3 CDM: weak_barrier_end when that barrier ends an application
    * dispatch. Where agx_chip_elide_app_dispatch_barrier() holds, a following
    * application dispatch overwrites it, so consecutive application dispatches
    * carry no barrier between them; any other record still follows it. */
   void *app_dispatch_end;

   /* Whether there is more than just the root chunk */
   bool stream_linked;

   /* Active-context selector consumed by AGX3 VertexProgramVariant. */
   bool agx3_active_context_selector_valid;
   uint32_t agx3_raw_active_context_selector;
   /* G18G: a terminated M5 dispatch owns this stream; no record may follow
    * it, and the submission carries the stream's compute resources. */
   bool g18_dispatch_sealed;
   struct drm_asahi_g18_compute_resources g18_compute_resources;

   /* Whether the sampler heap is required. Although we always must maintain the
    * heap for correctness, it's often not necessary since we can push lots of
    * samplers (especially for GL/DX11-era engines).
    */
   bool uses_sampler_heap;
   bool gen1_native_rq;
   bool gen1_native_fragment_rq;
   bool gen1_native_vertex_rq;
   bool m5_matrix_ops;

   /* Threads of the application's direct dispatches in this stream, and
    * whether it holds an indirect one (bk_queue::sib only takes streams with
    * enough work to overlap). */
   uint64_t sib_threads;
   bool sib_indirect;

   /* Submit packing (bk_pack_runs()): how many of the stream's launches are
    * direct application dispatches, their threads, and the sum over them of
    * threads times static native instructions. These bound the work one
    * packed command may hold. The sums saturate at UINT64_MAX. */
   uint32_t pack_dispatches;
   uint64_t pack_threads;
   uint64_t pack_static_work;
   /* A dispatch in this stream runs a program with global atomics. */
   bool uses_global_atomics;
   /* A dispatch in this stream runs a program with a loop: pack_static_work
    * does not bound its run time. */
   bool pack_loops;
   /* The stream calls another stream through a link with return; its bytes
    * must stay where they were recorded. */
   bool calls_stream;

   /* At least one AS-build dispatch or compute transfer is in this stream. */
   bool as_build;
   /* M5: a pre-rasterization program recorded in this CS keeps state in
    * thread-private storage (a spill frame or constant-program inputs), so
    * its tiling must not overlap earlier renders' fragment work
    * (bk_sync_resolve_submit()). */
   bool m5_vertex_tps;

   /* M5: a later render pass instance continues in this stream
    * (bk_m5_pass_merge_begin()); its draws may not share the stream's
    * committed sample locations (bk_flush_gfx_state()). */
   bool m5_continued_pass;

   /* A graphics program recorded in this stream writes external memory. */
   bool m5_shader_writes_memory;

   /* M5: the continued instance was ordered after this stream's earlier work
    * by a dependency that is not framebuffer-local (bk_m5_pass_merge_begin()).
    * While this is the open render stream, every draw recorded into it must
    * leave the merge memory-equivalent to the two separate instances, or the
    * stream ends first (bk_m5_ordered_draw_hazard()). */
   bool m5_ordered;

   /* M5: every attachment of the continued instances is in an
    * attachment-only layout, so no descriptor can access one of them and
    * descriptor sets bound for graphics need not end an ordered continuation
    * (bk_m5_attachments_exclude_descriptors()). */
   bool m5_layout_proof;

   /* Scratch requirements */
   struct {
      union {
         struct bk_scratch_req vs;
         struct bk_scratch_req cs;
      };

      struct bk_scratch_req fs;
   } scratch;

   /* Immediate writes, type libagx_imm_write. These happen at the end of the
    * control stream (parallel on AGX2, native ordered stores on G17P). This
    * accelerates queries. Implies CDM.
    */
   struct util_dynarray imm_writes;

   /* Native private-memory profiles for every dispatch in this stream. */
   struct util_dynarray compute_resources;
   /* Fields of the M5 HAL300 graphics core this stream last emitted, and the
    * records they packed to. The PPP latches each record for the following
    * draws of the stream, so an identical core is not emitted again and a
    * changed one emits only its changed records; any other PPP state emitted
    * into the stream clears m5_ppp_emitted_valid. See bk_flush_ppp_state(). */
   struct agx_ppp_hal300_graphics_fields m5_ppp_emitted;
   struct agx_ppp_hal300_graphics_core m5_ppp_emitted_state;
   bool m5_ppp_emitted_valid;

   struct util_dynarray render_resources;
   /* The render_resources entry the last draw matched or appended, see
    * bk_m5_note_render_resources(). */
   uint8_t m5_render_resources_last[3 * sizeof(struct drm_asahi_render_program_v2)];
   size_t m5_render_resources_last_size;
   struct drm_asahi_render_program tile_resources;
   /* Native AGX3: struct bk_oq_stamp for each immediate write that makes an
    * occlusion query available and could instead ride on the retirement
    * timestamp of the render that ended the query. While oq_avail_deferred,
    * the writes are not emitted yet: the end of the command buffer decides
    * (bk_resolve_deferred_oq_available()). Implies CDM. */
   struct util_dynarray oq_stamps;
   bool oq_avail_deferred;

   /* Number of successfully emitted draws whose last raster producer is
    * positively known to export only the default Layer=0, without multiview.
    * Compare with stats.cmds: untracked emitters conservatively disqualify.
    * The wider proof count does not wrap with stats.cmds.
    */
   uint64_t g17p_layer_zero_draws;
   bool g17p_layer_zero_unknown;

   /* An upper bound of the primitives every draw recorded into this G17P
    * render stream submits (bk_draw_primitive_bound()), saturating at
    * UINT64_MAX, which also stands for "not bounded when recorded". Secondary
    * command buffers replay into the primary's stream and add to it there. */
   uint64_t g17p_primitive_bound;

   /* This stream begins (vkCmdBeginRendering without RESUMING) and ends
    * (vkCmdEndRendering without SUSPENDING) its render pass: it is the whole
    * pass, not a part split off or continued. */
   bool g17p_pass_begins, g17p_pass_ends;

   /* A G17P mesh launch is recorded into this render stream. The A18 Pro
    * loses the vertex output of the mesh workgroup in flight whenever the
    * tiler heap runs out mid-draw, so the submission asks the kernel to size
    * the heap for mesh output first (DRM_ASAHI_RENDER_MESH_OUTPUT). */
   bool g17p_mesh_output;

   /* Consecutive GPU query resets share one strong flush, at the first
    * consumer or the end of the stream. G13 never sets this. */
   bool query_reset_pending;

   /* A query copy or reset was recorded into this stream. */
   bool query_access;

   /* The stream ends with a strong, memory-scoped flush
    * (DRM_ASAHI_COMPUTE_VISIBLE_COMPLETION).
    */
   bool visible_completion;

   /* Statistics */
   struct {
      uint32_t calls, cmds, flushes, merged;
      uint64_t query_copy_dispatches, query_copy_queries, query_copy_counters;
      uint64_t query_reset_dispatches, query_reset_queries, query_reset_flushes;
      uint32_t robust_index_draws, robust_index_dispatches,
         robust_index_batches;
   } stats;

   /* Frame anatomy profiler only: the first fragment programs this stream
    * draws with and their draw counts; later programs are only counted.
    * Zero unless HK_FRAME_ANATOMY is set. */
   struct {
      const struct bk_shader *fs;
      uint32_t draws;
      /* Compute streams: total threads and workgroups of the direct
       * dispatches, and how many were indirect. */
      uint64_t threads, groups;
      uint32_t indirect;
   } anatomy_fs[BK_FA_FS_MAX];
   uint32_t anatomy_fs_other;

   /* Likewise for the hardware vertex programs. */
   struct {
      const struct bk_shader *vs;
      uint32_t draws;
   } anatomy_vs[BK_FA_FS_MAX];
   uint32_t anatomy_vs_other;

   /* Frame anatomy profiler only: driver-internal (precompiled) kernels this
    * stream dispatches, with counts and total threads (UINT64_MAX once any
    * dispatch is indirect). */
   struct {
      uint16_t program;
      uint16_t dispatches;
      uint64_t threads;
   } anatomy_precomp[BK_FA_FS_MAX];
   uint32_t anatomy_precomp_other;

   /* Frame anatomy profiler only: the API pipeline barriers recorded between
    * the previous stream's creation and this one's (count, and the union of
    * their source and destination stage masks). */
   uint32_t anatomy_api_barriers;
   uint16_t anatomy_api_mem, anatomy_api_buf, anatomy_api_img;
   /* Frame anatomy profiler only: draws that republished render state,
    * vertex-program state updates and how many named a program other than
    * the stream's previous one, and PPP state records emitted. */
   uint32_t anatomy_republish, anatomy_vdm, anatomy_vdm_new, anatomy_ppp;
   /* Colour attachments whose load the first draw made unnecessary. */
   uint8_t anatomy_covered;
   /* Frame anatomy profiler only, M5: draws by the latched fragment pass type
    * (opaque, translucent, punch-through, translucent punch-through), and
    * draws with triangle merging disabled, with depth writes, and with a
    * depth mode other than UNCHANGED (shader depth or side effects). */
   uint32_t anatomy_pass[4], anatomy_nomerge, anatomy_zwrite, anatomy_late_z;
   /* Frame anatomy profiler only: the render area of a pass that does not
    * cover its whole framebuffer (zero extent otherwise). */
   VkRect2D anatomy_area;
   uint64_t anatomy_last_program;
   uint64_t anatomy_api_src, anatomy_api_dst;
   uint64_t anatomy_api_src_access, anatomy_api_dst_access;

   /* Timestamp writes: the start of the command (compute start, render
    * vertex start) and its end (compute end, render fragment end). The
    * kernel writes both from the firmware's native pass record.
    */
   struct {
      struct agx_timestamp_req start;
      struct agx_timestamp_req end;
   } timestamp;

   /* Render only: occlusion availability slots written by the kernel at
    * retirement, indexed vertex start, vertex end, fragment start, fragment
    * end. Slots 0 and 3 are only used when timestamp.start/end are not. */
   struct agx_timestamp_req oq_stamp[4];

   /* G17P query commands observe timestamp writes from earlier streams.
    * Firmware start timestamps reach query memory only at retirement, so a
    * producer cannot be merged across this consumer boundary.
    */
   bool g17p_timestamp_query;

   /* Recorded dependencies, see bk_sync.h. */
   struct bk_sync_stream sync;

   /* Writable destinations known while recording meta compute dispatches. */
   struct bk_drm_attachment_set compute_attachments;

   /* Remaining state is for graphics only, ignored for compute */
   struct agx_tilebuffer_layout tib;

   /* Bottom-right corner of the render area, or 0 where it is not known (a
    * secondary command buffer continuing a render pass). Tiles past it hold
    * nothing the render may change, so the render need not visit them. */
   uint32_t area_x1, area_y1;

   struct util_dynarray scissor, depth_bias;
   uint64_t uploaded_scissor, uploaded_zbias;

   /* G17P: the last uploaded draw-state selector record and the scissor /
    * depth-bias array sizes and viewport count it selects for. */
   struct {
      uint32_t offset;
      uint32_t scissor_size, depth_bias_size;
      uint8_t viewport_count;
   } g17p_draw_state;

   /* We can only set ppp_multisamplectl once per batch. has_sample_locations
    * tracks if we've committed to a set of sample locations yet. vk_meta
    * operations do not set has_sample_locations since they don't care and it
    * would interfere with the app-provided samples.
    *
    */
   bool has_sample_locations;
   uint64_t ppp_multisamplectl;

   struct bk_render_registers cr;

   /* Snapshot of the writable image ranges for this exact VDM command. Render
    * state may change before queue submission, so the synthetic fragment
    * attachment command must travel with the control stream it describes. */
   struct bk_drm_attachment_set fragment_attachments;

   /* The restart index last emitted to this stream, valid once
    * restart_index_set. VK_EXT_primitive_restart_index makes every value,
    * zero included, a legal custom index, so no value can mean "unset".
    * This is used in place of dirty tracking, because dirty tracking
    * restart indices is complicated and just checking the saved value is cheap.
    */
   uint32_t restart_index;
   bool restart_index_set;
};

static inline unsigned
bk_sync_sq(const struct bk_cs *cs)
{
   return cs->type == BK_CS_VDM ? BK_SYNC_VDM : BK_SYNC_CDM;
}

/* The later of two streams in list order; NULL is earlier than anything. */
static inline struct bk_cs *
bk_sync_later(struct bk_cs *a, struct bk_cs *b)
{
   if (!a)
      return b;
   if (!b)
      return a;
   return b->sync.seq > a->sync.seq ? b : a;
}

void *bk_cmd_buffer_emit_cs_jump(struct bk_cmd_buffer *cmd, void *current,
                                 uint64_t target, bool vdm);

/* Moves the patch sites of cmd in [from, from_end), copied to `to` without
 * their first `skip` bytes, which were folded into the record just before
 * `to` (bk_cs_merge_cdm_until()). */
void bk_cmd_buffer_move_alias_sites(struct bk_cmd_buffer *cmd,
                                    const void *from, const void *from_end,
                                    uint8_t *to, size_t skip);

/*
 * Helper to merge two compute control streams, concatenating the second control
 * stream, up to b_current, to the first one. Must sync with bk_cs.
 *
 * Submit packing (bk_pack_runs()) merges streams of other command buffers
 * into a submission-owned stream, stopping before their terminate; it only
 * merges inline, so the link path, which allocates from a's command buffer,
 * is never taken for it.
 */
static inline void
bk_cs_merge_cdm_until(struct bk_cs *a, const struct bk_cs *b, void *b_current)
{
   assert(a->type == BK_CS_CDM && b->type == BK_CS_CDM);
   assert(!a->timestamp.end.handle);
   a->m5_matrix_ops |= b->m5_matrix_ops;

   const size_t b_size = (uint8_t *)b_current - (uint8_t *)b->start;
   const size_t remaining = (uint8_t *)a->end - (uint8_t *)a->current;
   const bool inline_b = !a->stream_linked && !b->stream_linked &&
                         bk_cs_space_fits(remaining, b_size,
                                          AGX_CDM_STREAM_LINK_LENGTH);

   if (inline_b) {
      uint8_t *dst = a->current;

      /* Two barriers with no launch between them is not a shape Apple's CDM
       * emitter produces, and the G17P stream oracle rejects it. Within one
       * stream bk_cdm_cache_flush() already strengthens the trailing record
       * in place; do the same across a concatenation. b's leading barrier is
       * the strong flush, so copying its word over a's trailing barrier is
       * exactly that upgrade -- no encoder needed, and it removes a record
       * rather than adding one. */
      const bool b_opens_with_barrier =
         b->leading_barrier_end &&
         (uint8_t *)b->leading_barrier_end ==
            (uint8_t *)b->start + AGX3_CDM_BARRIER_SIZE;
      const bool a_ends_with_barrier =
         ((uint8_t *)a->current == (uint8_t *)a->strong_flush_end ||
          (uint8_t *)a->current == (uint8_t *)a->weak_barrier_end) &&
         (uint8_t *)a->current - (uint8_t *)a->start >= AGX3_CDM_BARRIER_SIZE;
      const size_t b_skip = (b_opens_with_barrier && a_ends_with_barrier)
                               ? AGX3_CDM_BARRIER_SIZE
                               : 0;
      const bool a_memory = (uint8_t *)a->current == (uint8_t *)a->memory_flush_end;
      if (b_skip) {
         /* b's strong flush must not drop the memory scope a's trailing
          * record carried for the work before it. */
         uint32_t word = agx3_cmdstream_load_le32(b->start);
         if (a_memory)
            word |= AGX3_CDM_BARRIER_FLUSH_MEMORY;
         agx3_cmdstream_store_le32(dst - AGX3_CDM_BARRIER_SIZE, word);
      }

      memcpy(dst, (const uint8_t *)b->start + b_skip, b_size - b_skip);
      a->current = dst + (b_size - b_skip);
      /* b's patch sites are in the copy now. Submit packing copies other
       * command buffers' streams, whose sites stay with their recording. */
      if (b->alias_sites && a->cmd == b->cmd)
         bk_cmd_buffer_move_alias_sites(a->cmd, b->start, b_current, dst,
                                        b_skip);

      /* These markers point just past an emitted barrier. Preserve them in
       * the destination root so another adjacent merge still has an exact
       * view of the stream tail. A marker on the folded record maps onto the
       * trailing record that absorbed it. */
      a->strong_flush_end =
         b->strong_flush_end
            ? dst + ((uint8_t *)b->strong_flush_end - (uint8_t *)b->start) -
                 b_skip
            : NULL;
      a->weak_barrier_end =
         b->weak_barrier_end
            ? dst + ((uint8_t *)b->weak_barrier_end - (uint8_t *)b->start) -
                 b_skip
            : NULL;
      a->memory_flush_end =
         b->memory_flush_end
            ? dst + ((uint8_t *)b->memory_flush_end - (uint8_t *)b->start) -
                 b_skip
         : (a_memory && b_skip && a->current == dst) ? dst
                                                     : NULL;
      a->app_dispatch_end = NULL;
   } else {
      void *link_end =
         bk_cmd_buffer_emit_cs_jump(a->cmd, a->current, b->addr, false);
      const bool linked = link_end != a->current;
      a->current = b_current;
      a->end = b->end;
      a->chunk = b->chunk;
      a->stream_linked |= linked || b->stream_linked;
      a->strong_flush_end = b->strong_flush_end;
      a->weak_barrier_end = b->weak_barrier_end;
      a->memory_flush_end = b->memory_flush_end;
      a->app_dispatch_end = NULL;
   }

   a->alias_sites += b->alias_sites;
   a->uses_sampler_heap |= b->uses_sampler_heap;
   a->gen1_native_rq |= b->gen1_native_rq;
   a->as_build |= b->as_build;
   a->uses_global_atomics |= b->uses_global_atomics;
   a->pack_loops |= b->pack_loops;
   a->calls_stream |= b->calls_stream;
   a->pack_dispatches += b->pack_dispatches;
   a->pack_threads = b->pack_threads > UINT64_MAX - a->pack_threads
                        ? UINT64_MAX : a->pack_threads + b->pack_threads;
   a->pack_static_work =
      b->pack_static_work > UINT64_MAX - a->pack_static_work
         ? UINT64_MAX : a->pack_static_work + b->pack_static_work;
   a->scratch.cs.main |= b->scratch.cs.main;
   a->scratch.cs.preamble |= b->scratch.cs.preamble;

   /* Submission sees only the merged root. Keep the destinations recorded
    * by both streams without changing their inline/link batching policy. */
   for (unsigned i = 0; i < b->compute_attachments.count; ++i) {
      const struct drm_asahi_attachment *attachment =
         &b->compute_attachments.list[i];
      bk_drm_attachment_set_add(&a->compute_attachments,
                                attachment->pointer, attachment->size);
   }
   a->compute_attachments.complete &= b->compute_attachments.complete;

   /* The merged command ends where b ended. It starts where a started; a
    * start stamp b carried was a top-of-pipe write, which any earlier value
    * also satisfies. The caller refuses the merge when both have one. */
   a->timestamp.end = b->timestamp.end;
   if (!a->timestamp.start.handle)
      a->timestamp.start = b->timestamp.start;

   util_dynarray_append_array(&a->compute_resources, uint8_t,
                             b->compute_resources.data, b->compute_resources.size);

   a->g17p_timestamp_query |= b->g17p_timestamp_query;

   a->stats.calls += b->stats.calls;
   a->stats.cmds += b->stats.cmds;
   a->stats.flushes += b->stats.flushes;
   a->stats.robust_index_draws += b->stats.robust_index_draws;
   a->stats.robust_index_dispatches += b->stats.robust_index_dispatches;
   a->stats.robust_index_batches += b->stats.robust_index_batches;
   a->stats.merged++;
   a->stats.query_copy_dispatches += b->stats.query_copy_dispatches;
   a->stats.query_copy_queries += b->stats.query_copy_queries;
   a->stats.query_copy_counters += b->stats.query_copy_counters;
   a->stats.query_reset_dispatches += b->stats.query_reset_dispatches;
   a->stats.query_reset_queries += b->stats.query_reset_queries;
   a->stats.query_reset_flushes += b->stats.query_reset_flushes;
   a->query_access |= b->query_access;
   assert(!a->query_reset_pending && !b->query_reset_pending);

   /* Frame anatomy: the merged command runs b's programs too. Without them
    * its whole duration is charged to a's programs alone. */
   for (unsigned j = 0; j < BK_FA_FS_MAX && b->anatomy_fs[j].fs; ++j) {
      unsigned i = 0;
      while (i < BK_FA_FS_MAX && a->anatomy_fs[i].fs &&
             a->anatomy_fs[i].fs != b->anatomy_fs[j].fs)
         ++i;
      if (i == BK_FA_FS_MAX) {
         a->anatomy_fs_other += b->anatomy_fs[j].draws;
         continue;
      }
      a->anatomy_fs[i].fs = b->anatomy_fs[j].fs;
      a->anatomy_fs[i].draws += b->anatomy_fs[j].draws;
      a->anatomy_fs[i].threads += b->anatomy_fs[j].threads;
      a->anatomy_fs[i].groups += b->anatomy_fs[j].groups;
      a->anatomy_fs[i].indirect += b->anatomy_fs[j].indirect;
   }
   a->anatomy_fs_other += b->anatomy_fs_other;

   for (unsigned j = 0; j < BK_FA_FS_MAX && b->anatomy_precomp[j].dispatches;
        ++j) {
      unsigned i = 0;
      while (i < BK_FA_FS_MAX && a->anatomy_precomp[i].dispatches &&
             a->anatomy_precomp[i].program != b->anatomy_precomp[j].program)
         ++i;
      if (i == BK_FA_FS_MAX) {
         a->anatomy_precomp_other += b->anatomy_precomp[j].dispatches;
         continue;
      }
      const uint64_t t = b->anatomy_precomp[j].threads;
      a->anatomy_precomp[i].threads =
         a->anatomy_precomp[i].dispatches == 0 ? t
         : (t == UINT64_MAX || a->anatomy_precomp[i].threads == UINT64_MAX)
            ? UINT64_MAX
            : a->anatomy_precomp[i].threads + t;
      a->anatomy_precomp[i].program = b->anatomy_precomp[j].program;
      a->anatomy_precomp[i].dispatches += b->anatomy_precomp[j].dispatches;
   }
   a->anatomy_precomp_other += b->anatomy_precomp_other;
}

static inline void
bk_cs_merge_cdm(struct bk_cs *a, const struct bk_cs *b)
{
   assert(a->cmd == b->cmd);
   bk_cs_merge_cdm_until(a, b, b->current);
}

static inline uint64_t
bk_cs_current_addr(struct bk_cs *cs)
{
   return cs->chunk.gpu + ((uint8_t *)cs->current - (uint8_t *)cs->chunk.cpu);
}

struct bk_uploader {
   /** List of bk_cmd_bo */
   struct list_head bos;

   /* Current addresses */
   uint8_t *map;
   uint64_t base;
   uint32_t offset;
   uint32_t capacity;

   /* VDM uploader only: low alias of base (its bk_vdm_allocation's alias). */
   uint64_t vdm_alias;
};

/*
 * Bump arena for the G17P Ioto programs one command buffer records. Semantic
 * program reuse happens before reservation; resource programs embed their
 * reserved address, so post-composition byte hashing cannot deduplicate them.
 * See bk_ioto_arena_core.h for why programs cannot each have a BO.
 */
struct bk_ioto_arena {
   /* Chunk currently being filled. Owned by bk_cmd_buffer::large_bos, unless
    * it is the span below, which has no bo of its own. */
   struct agx_bo *bo;
   struct bk_vdm_allocation *span;
   uint8_t *map;
   uint64_t base;
   uint32_t cursor;
   uint32_t capacity;

   /* Recording statistics, reported when the arena cannot grow. */
   uint32_t programs;
   uint64_t program_bytes;
   uint64_t chunk_bytes;
};

#define BK_MAX_DEFERRED_CLEARS 8

struct bk_cmd_buffer {
   struct vk_command_buffer vk;

   /* Frame anatomy profiler only: API pipeline barriers recorded since the
    * last stream was created (see bk_cs::anatomy_api_barriers). */
   struct {
      uint32_t count;
      uint16_t mem, buf, img;
      uint64_t src, dst, src_access, dst_access;
   } anatomy_bar;

   /* frg-codegen v4: the per-recording BG/EOT cache (13.5 KiB) lives outside
    * `state` so the full-state memset of every reset no longer clears it;
    * bk_reset_cmd_buffer() invalidates the entries instead. Entries are only
    * read when valid, so this is the same cache contract. */
   struct bk_g17p_bg_eot_cache_entry
      g17p_bg_eot_cache[BK_G17P_BG_EOT_CACHE_ENTRIES];
   uint8_t g17p_bg_eot_cache_next;

   /* Set by bk_reset_cmd_buffer, cleared by vkBeginCommandBuffer. */
   bool g17p_pristine;

   struct {
      struct bk_upload_stats upload_stats;
      struct {
         uint64_t id, addr;
      } constant_data[MESA_SHADER_STAGES];
      /* vkCmdBindDescriptorBuffersEXT() */
      uint64_t descriptor_buffers[BK_MAX_DESCRIPTOR_BUFFERS];
      struct bk_graphics_state gfx;
      struct bk_compute_state cs;
      struct bk_ray_tracing_state rt;
      struct bk_conditional_render_state conditional;

      /* Whole-image clears not yet recorded, see
       * bk_flush_deferred_clears(). */
      struct bk_deferred_clear {
         struct bk_image *image;
         VkImageAspectFlags aspects;
         VkClearValue value;
      } deferred_clear[BK_MAX_DEFERRED_CLEARS];
      uint8_t deferred_clear_count;
      bool deferred_clear_flushing;
      /* Recording a buffer-only command that does not touch their memory. */
      bool deferred_clear_hold;

      /* Suppress consumption of pending resets while recording another reset. */
      bool query_reset_recording;

      /* An event is set, reset or waited on: the host or another queue can
       * observe this recording before its submission completes. */
      bool uses_events;

      /* Scoped around direct/indirect AS builds, including their meta blits. */
      bool as_build_recording;

      /* AGX3: the last render pass instance stored a whole depth-only image
       * that nothing recorded since may have read. A later instance that
       * clears the whole image, or does not care about its contents, makes
       * that store dead (bk_take_pending_z_store()). Kept across exactly the
       * commands deferred clears survive (deferred_clear_hold). */
      struct bk_pending_z_store {
         struct bk_cs *cs;
         const struct bk_image *image;
         struct agx_zls_control_packed zls_control;
         bool process_empty_tiles;
      } pending_z_store;
      /* M5: the render pass instance that ended last, while the next one may
       * continue in its render stream instead of storing and reloading the
       * attachments (bk_m5_pass_merge_begin()). */
      struct bk_pass_merge_candidate {
         bool valid;
         /* The instance's single render stream, where its draws ended and
          * where its terminator left the stream and chunk. */
         struct bk_cs *cs;
         void *resume_current;
         void *after_current;
         void *end;
         /* The tracked state when the instance ended; the next instance may
          * continue only if none of it moved. */
         uint32_t next_seq;
         uint32_t sync_epoch;
         struct bk_cs *open_cs;
         void *open_cs_current;
         /* A recorded dependency orders the next instance after this one
          * beyond attachment operations; continuing then needs the stronger
          * conditions of bk_m5_ordered_refusal(). */
         bool ordered;
         /* The render state the instance ended with. */
         struct bk_rendering_state render;
      } pass_merge;

      /* M5: the open render stream is an ordered continuation (bk_cs::
       * m5_ordered) and a command recorded since may read the instance's
       * attachments through memory; the next draw ends the stream first. */
      bool m5_ordered_split;

      /* Per recording; reported at submission so reused command buffers are
       * counted each time. Reasons overlap, total counts physical splits. */
      struct {
         uint64_t kept, splits;
         uint64_t reason[BK_BARRIER_SPLIT_COUNT];
      } barrier_stats;
   } state;

   struct {
      struct bk_uploader main, usc, root, vdm, streams;
   } uploader;

   /* Command-pool slab spans leased by this command buffer's VDM uploader. */
   struct list_head vdm_allocations;
   /* Most recently addressed span. Recording often references the current
    * 4 KiB VDM chunk many times before moving to the next one. */
   struct bk_vdm_allocation *g17p_vdm_last_allocation;

   /* List of all recorded control streams */
   struct list_head control_streams;

   /* Current recorded control stream */
   struct {
      /* VDM stream for 3D */
      struct bk_cs *gfx;

      /* CDM stream for compute */
      struct bk_cs *cs;

      /* CDM stream that executes immediately before the current graphics
       * control stream. Used for geometry shading, tessellation, etc.
       */
      struct bk_cs *pre_gfx;

      /* CDM stream that will execute after the current graphics control stream
       * finishes. Used for queries.
       */
      struct bk_cs *post_gfx;
   } current_cs;

   /* Tessellation stages of the current render's draws, not yet in pre_gfx */
   struct bk_tess_batch tess_batch;

   /* Recorded dependencies between control streams, see bk_sync.h. */
   struct bk_sync_state sync;

   /* Tile hazards: struct bk_sync_pass_record for every recorded pass
    * instance footprint, in recording order. Cleared by bk_sync_reset. */
   struct util_dynarray sync_pass_records;

   /* struct bk_oq_end for every occlusion query this command buffer ends. */
   struct util_dynarray oq_ends;

   /* Are we currently inside a vk_meta operation? This alters sample location
    * behaviour.
    */
   bool in_meta;

   /* CmdClearAttachments is the sole vk_meta operation affected by
    * conditional rendering. */
   bool conditional_meta;

   /* Single-kick vkCmdTraceRays* on M5: while set, bk_dispatch_m5_native
    * writes the kernel's launch record (40-byte direct launch + barrier) here
    * instead of into the control stream. */
   uint8_t *m5_rt_capture;
   bool m5_rt_captured;

   /* XXX: move me?
    *
    * Indirect draw generated by the indirect GS translator.
    */
   uint64_t geom_index_buffer;
   uint32_t geom_index_count;
   uint32_t geom_instance_count;

   /* Does the command buffer use the geometry heap? */
   bool uses_heap;

   /* Owned BOs and retained references needed by recorded commands. */
   struct util_dynarray large_bos;
   /* Direct-mapped temporal cache avoids rescanning large_bos for the linked
    * vertex/fragment shader, setup and rodata BOs repeated by every draw. */
   struct agx_bo *recent_retained_bos[16];
   /* Open-addressed pointer set of the BOs bk_cmd_buffer_retain_bo() has
    * appended to large_bos (capacity a power of two, or zero). A recording
    * retains one BO per distinct shader/setup/program it publishes, so a
    * linear scan of large_bos on every temporal-cache miss grew quadratically
    * with the number of pipelines a command buffer uses. */
   struct agx_bo **retained_set;
   uint32_t retained_set_capacity;
   uint32_t retained_set_count;

   /* M5: every attachment-capable image (bk_image) a graphics push descriptor
    * has referenced since recording began. Push descriptors stay valid until
    * overwritten, so an image recorded here may still be readable by the
    * bound shaders (bk_m5_ordered_refusal()). Created on first use. */
   struct set *m5_pushed_images;

   /* Executable USC-window arena the G17P Ioto programs recorded by this
    * command buffer are bump-allocated from. Its chunks are owned by
    * large_bos, so it dies with the recording like every other program
    * reference. */
   struct bk_ioto_arena ioto;

   /* M5 with bk_device::host_alias.dynamic: the application dependencies
    * recorded without memory scope (struct bk_alias_site), written once an
    * import may alias (bk_cdm_alias_scope()). alias_patched (atomic) once
    * they are; alias_registered (atomic) while alias_link is in
    * bk_device::host_alias.cmd_buffers. */
   struct util_dynarray alias_sites;
   uint32_t alias_patched;
   uint32_t alias_registered;
   struct list_head alias_link;
};

/* A patch site: the stream word of a barrier record an application
 * dependency was recorded with, without memory scope, because no host import
 * could alias then (bk_cdm_alias_scope()). `value` is the memory-scoped
 * strong flush (bk_device::host_alias.flush_word), which only strengthens
 * the record: a weak ordering record, or a strong flush, becomes the strong
 * flush with memory scope, as bk_cdm_flush() upgrades in place. A
 * site stays a barrier record for the life of the recording: an application
 * dispatch after it no longer drops it, and a merge that folds a leading
 * barrier over it writes a barrier. */
struct bk_alias_site {
   uint32_t *word;
   uint32_t value;
};

/* Whether an application dependency recorded now needs memory scope: as
 * bk_cdm_alias_scope(), and also, with host_alias.dynamic, in a command
 * buffer that uses events. */
bool bk_cmd_alias_scope(struct bk_cmd_buffer *cmd);

/* After an application dependency was recorded into cs without memory scope
 * because bk_cmd_alias_scope() was false: with host_alias.dynamic, records
 * the stream's last record as a patch site, or, without such a record,
 * flushes with memory scope now. */
void bk_cdm_note_alias_site(struct bk_cmd_buffer *cmd, struct bk_cs *cs);

/* Writes the command buffer's patch sites, once. Callers ensure no reset
 * runs concurrently; two writers write the same words. */
void bk_cmd_buffer_patch_alias_sites(struct bk_cmd_buffer *cmd);

/* Transfer entry points bracket their recording so the streams they create
 * belong to the transfer stage class (VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT and
 * its sub-stages). Nesting is fine: meta helpers re-enter through the
 * dispatch table. */
static inline void
bk_sync_xfer_begin(struct bk_cmd_buffer *cmd)
{
   /* An outermost transfer without a known destination may write anything
    * (bk_sync_note_xfer_write()). */
   if (cmd->sync.xfer_depth++ == 0)
      cmd->sync.xfer_dst = (struct bk_sync_xfer_dst){0};
}

/* As bk_sync_xfer_begin(), for a transfer whose writes stay inside the GPU
 * VA range [lo, hi). device_address says shaders may also reach the range
 * through a buffer device address. A nested transfer widens the range of the
 * outermost one. */
static inline void
bk_sync_xfer_begin_dst(struct bk_cmd_buffer *cmd, uint64_t lo, uint64_t hi,
                       bool device_address)
{
   struct bk_sync_xfer_dst *dst = &cmd->sync.xfer_dst;
   if (cmd->sync.xfer_depth++ == 0) {
      *dst = (struct bk_sync_xfer_dst){
         .lo = lo,
         .hi = hi,
         .valid = lo < hi,
         .device_address = device_address,
      };
   } else if (dst->valid) {
      dst->lo = MIN2(dst->lo, lo);
      dst->hi = MAX2(dst->hi, hi);
      dst->device_address |= device_address;
   }
}

static inline void
bk_sync_xfer_end(struct bk_cmd_buffer *cmd)
{
   assert(cmd->sync.xfer_depth > 0);
   if (--cmd->sync.xfer_depth == 0)
      cmd->sync.xfer_dst = (struct bk_sync_xfer_dst){0};
}

/*
 * One reservation inside the Ioto arena: where the program is to be composed,
 * and the address the composer must be given for it.
 */
struct bk_ioto_reservation {
   void *map;
   uint64_t va;
};

/*
 * Reserve `size` bytes of executable USC-window space for one Ioto program.
 * On failure the command buffer error is already set.
 */
bool bk_cmd_reserve_ioto_program_slow(struct bk_cmd_buffer *cmd, size_t size,
                                      const char *label,
                                      struct bk_ioto_reservation *reservation);

/* AGX_IOTO_PROGRAM_ALIGNMENT, checked in bk_ioto_arena.c. */
#define BK_IOTO_PROGRAM_ALIGNMENT 64u

static inline bool
bk_cmd_reserve_ioto_program(struct bk_cmd_buffer *cmd, size_t size,
                            const char *label,
                            struct bk_ioto_reservation *reservation)
{
   /* The placement bk_ioto_arena_place() computes, in the current chunk. */
   struct bk_ioto_arena *arena = &cmd->ioto;
   const uint32_t mask = BK_IOTO_PROGRAM_ALIGNMENT - 1;
   if (likely(reservation && size != 0 && size <= UINT32_MAX && arena->map &&
              arena->cursor <= arena->capacity &&
              arena->cursor <= UINT32_MAX - mask)) {
      const uint32_t offset = (arena->cursor + mask) & ~mask;
      if (likely(offset <= arena->capacity &&
                 (uint32_t)size <= arena->capacity - offset)) {
         *reservation = (struct bk_ioto_reservation){
            .map = arena->map + offset,
            .va = arena->base + offset,
         };
         arena->cursor = offset + (uint32_t)size;
         return true;
      }
   }

   return bk_cmd_reserve_ioto_program_slow(cmd, size, label, reservation);
}

/*
 * Publish the `length` bytes composed at reservation->map and return the
 * reserved address for the launch record.
 */
uint64_t bk_cmd_publish_ioto_program(struct bk_cmd_buffer *cmd,
                                     const struct bk_ioto_reservation *
                                        reservation,
                                     size_t length);

/* Clean the current chunk's programs from the CPU cache (cached command
 * memory); bk_cmd_buffer_publish_cached_uploads() fences it. */
void bk_ioto_arena_clean(struct bk_cmd_buffer *cmd);

/* Release the arena's bookkeeping. The chunks themselves are in large_bos. */
void bk_ioto_arena_finish(struct bk_cmd_buffer *cmd);

VK_DEFINE_HANDLE_CASTS(bk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

extern const struct vk_command_buffer_ops bk_cmd_buffer_ops;

static inline struct bk_cmd_pool *
bk_cmd_buffer_pool(struct bk_cmd_buffer *cmd)
{
   return (struct bk_cmd_pool *)cmd->vk.pool;
}

/* Whether the current blend state reads the blend constants. Derived from
 * the dynamic graphics state at each use rather than cached: the fragment
 * stage program (its blend-constant uniform range) is built by bind-time and
 * BeginRendering preparation as well as by the draw, and that preparation
 * runs before the draw-time dirty-state flush. A cached copy refreshed only
 * by that flush was stale there, so the first draw after a blend-equation
 * change published a fragment program without the blend-constant range and
 * the epilog read unwritten uniforms. Prepared programs are keyed by
 * dynamic_graphics_state.generation, which covers every input here. */
static inline bool
bk_uses_blend_constant(const struct vk_color_blend_state *cb)
{
   static_assert(VK_BLEND_FACTOR_CONSTANT_COLOR + 3 ==
                    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
                 "constant factors are contiguous");

   for (unsigned i = 0; i < cb->attachment_count; ++i) {
      const unsigned factors[] = {
         cb->attachments[i].src_color_blend_factor,
         cb->attachments[i].src_alpha_blend_factor,
         cb->attachments[i].dst_color_blend_factor,
         cb->attachments[i].dst_alpha_blend_factor,
      };

      for (unsigned j = 0; j < ARRAY_SIZE(factors); ++j) {
         if (factors[j] >= VK_BLEND_FACTOR_CONSTANT_COLOR &&
             factors[j] <= VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA)
            return true;
      }
   }

   return false;
}

static inline bool
bk_cmd_uses_blend_constant(const struct bk_cmd_buffer *cmd)
{
   return bk_uses_blend_constant(&cmd->vk.dynamic_graphics_state.cb);
}

/*
 * The hardware vertex shader is supplied by the last geometry stage. The
 * geometry pipeline is vertex->tess->geometry so we search backwards.
 */
static inline struct bk_shader *
bk_bound_hw_vs(struct bk_graphics_state *gfx)
{
   struct bk_api_shader *vs = gfx->shaders[MESA_SHADER_VERTEX];
   struct bk_api_shader *tes = gfx->shaders[MESA_SHADER_TESS_EVAL];
   struct bk_api_shader *gs = gfx->shaders[MESA_SHADER_GEOMETRY];
   struct bk_api_shader *mesh = gfx->shaders[MESA_SHADER_MESH];

   /* A mesh pipeline has no vertex, tessellation or geometry stage; its
    * native mesh variant is the hardware vertex program. */
   if (mesh)
      return &mesh->variants[BK_MESH_VARIANT_MAIN];
   else if (gs)
      return &gs->variants[BK_GS_VARIANT_RAST];
   else if (tes)
      return &tes->variants[BK_VS_VARIANT_HW];
   else
      return &vs->variants[BK_VS_VARIANT_HW];
}

static inline struct bk_shader *
bk_bound_sw_vs(struct bk_graphics_state *gfx)
{
   struct bk_api_shader *vs = gfx->shaders[MESA_SHADER_VERTEX];
   struct bk_shader *hw_vs = bk_bound_hw_vs(gfx);

   /* Mesh pipelines: the raster variant is both the hardware and the only
    * vertex program (no attributes, no prolog remap). */
   if (!vs)
      return hw_vs;

   if (hw_vs == &vs->variants[BK_VS_VARIANT_HW])
      return hw_vs;
   else
      return &vs->variants[BK_VS_VARIANT_SW];
}

static inline struct bk_shader *
bk_bound_sw_vs_before_gs(struct bk_graphics_state *gfx)
{
   struct bk_api_shader *vs = gfx->shaders[MESA_SHADER_VERTEX];
   struct bk_api_shader *tes = gfx->shaders[MESA_SHADER_TESS_EVAL];
   struct bk_api_shader *api = tes ?: vs;

   /* Mesh pipelines have no software vertex stage. */
   if (!api)
      return NULL;

   return &api->variants[BK_VS_VARIANT_SW];
}

struct agx_ptr bk_pool_alloc_from_slow(struct bk_cmd_buffer *cmd,
                                       struct bk_uploader *uploader,
                                       uint32_t size, uint32_t alignment,
                                       enum bk_cmd_bo_type type);

/* Bump allocation from the uploader's current BO; a new BO or a large
 * allocation takes bk_pool_alloc_from_slow(). */
static inline struct agx_ptr
bk_pool_alloc_from(struct bk_cmd_buffer *cmd, struct bk_uploader *uploader,
                   uint32_t size, uint32_t alignment,
                   enum bk_cmd_bo_type type)
{
   assert(alignment > 0);
   const uint32_t offset = align(uploader->offset, alignment);
   if (likely(uploader->map != NULL && offset <= uploader->capacity &&
              size <= uploader->capacity - offset)) {
      uploader->offset = offset + size;
      return (struct agx_ptr){
         .gpu = uploader->base + offset,
         .cpu = uploader->map + offset,
      };
   }

   return bk_pool_alloc_from_slow(cmd, uploader, size, alignment, type);
}

struct agx_ptr bk_pool_cs_alloc(struct bk_cmd_buffer *cmd, uint32_t size,
                                uint32_t alignment);
struct agx_ptr bk_pool_vdm_primary_alloc(struct bk_cmd_buffer *cmd,
                                         uint32_t size, uint32_t alignment);

uint64_t bk_g17p_vdm_compact(struct bk_cmd_buffer *cmd, uint64_t primary);
uint64_t bk_g17p_vdm_alias(struct bk_cmd_buffer *cmd, uint64_t primary);

uint64_t bk_pool_upload(struct bk_cmd_buffer *cmd, const void *data,
                        uint32_t size, uint32_t alignment);

/*
 * G17P does not honour the low five bits of the texture-table pointer that
 * the combined-sampler ABIs publish in uniform halves 0..3: the 32-byte
 * texture state is read as if the pointer were 32-byte aligned.  Sampled-image
 * descriptors only have 8-byte alignment and a 72-byte stride in a descriptor
 * set, so set address + binding offset is generally not a valid table
 * pointer.  Measured on G17P with vkCmdBlitImage of D32_SFLOAT_S8_UINT, whose
 * pushed set holds the depth and stencil texture words at offsets 72 and 144:
 * every depth and stencil texel read back wrong, and when the qword before
 * the stencil texture word (the depth descriptor's last qword) was nonzero the
 * texture unit's read faulted (VM error at 0x40000000000, VK_ERROR_DEVICE_LOST
 * in Pokemon Scarlet).  The same descriptor bytes published at a 32-byte
 * aligned address -- including 32 mod 64 -- read back exactly and never
 * faulted, while 8 and 16 mod 32 still read back wrong.
 *
 * Returns texture_va when it is already aligned, otherwise the address of a
 * 32-byte-aligned command-buffer copy of the descriptor record (texture word
 * plus the software fields after it, which descriptor crawls through the
 * same pointer may read), or 0 on allocation failure.  The ABIs using this
 * that need a copy are not selected when sampled-image update-after-bind is
 * enabled.  The direct txf ABI may still publish an already aligned live set
 * pointer in that case.  The combined-sampler ABIs also snapshot the
 * descriptor's sampler index at record time, so their copies hold what the set
 * holds when the commands execute.
 */
#define BK_G17P_TEXTURE_STATE_ALIGN 32

uint64_t bk_g17p_texture_state_pointer(struct bk_cmd_buffer *cmd,
                                       uint64_t texture_va,
                                       const void *descriptor,
                                       uint32_t available_B);
uint64_t bk_pool_usc_upload(struct bk_cmd_buffer *cmd, const void *data,
                            uint32_t size, uint32_t alignment);

/* Keep externally owned storage referenced by recorded GPU commands alive for
 * the command-buffer lifetime. Repeated uses of the same BO are deduplicated.
 */
void bk_cmd_buffer_retain_bo(struct bk_cmd_buffer *cmd, struct agx_bo *bo);
void bk_cmd_buffer_release_bos(struct bk_cmd_buffer *cmd,
                               struct agx_device *dev);

static inline struct agx_ptr
bk_pool_alloc(struct bk_cmd_buffer *cmd, uint32_t size, uint32_t alignment)
{
   return bk_pool_alloc_from(cmd, &cmd->uploader.main, size, alignment,
                             BK_CMD_BO_MAIN);
}

static inline struct agx_ptr
bk_pool_usc_alloc(struct bk_cmd_buffer *cmd, uint32_t size, uint32_t alignment)
{
   return bk_pool_alloc_from(cmd, &cmd->uploader.usc, size, alignment,
                             BK_CMD_BO_USC);
}

/*
 * Allocate a payload that a VDM record references through its 40-bit pointer.
 * HAL200 draw-state fields are narrower than the VDM control-stream pointer,
 * so those payloads retain a device-global low alias. Control-stream roots
 * themselves use the constrained main heap directly.
 */
static inline struct agx_ptr
bk_pool_vdm_alloc(struct bk_cmd_buffer *cmd, uint32_t size, uint32_t alignment)
{
   struct bk_device *dev = bk_cmd_buffer_device(cmd);

   /* Bump-allocate from the current VDM chunk and derive its compact pointer
    * from the chunk's alias.  Allocations never overlap, so this is the
    * address bk_g17p_vdm_compact() resolves through the chunk's
    * bk_vdm_allocation; anything else takes the general path below. */
   struct bk_uploader *vdm = &cmd->uploader.vdm;
   const uint32_t vdm_offset = align(vdm->offset, alignment);
   uint32_t vdm_compact;
   if (likely(vdm->map && vdm_offset <= vdm->capacity &&
              size <= vdm->capacity - vdm_offset &&
              bk_g17p_vdm_low_window_enabled(&dev->dev) &&
              agx_g17p_vdm_alias_compact_inline(vdm->vdm_alias + vdm_offset,
                                                &vdm_compact) &&
              vdm_compact != 0)) {
      vdm->offset = vdm_offset + size;
      return (struct agx_ptr){
         .gpu = vdm_compact,
         .cpu = vdm->map + vdm_offset,
      };
   }

   struct agx_ptr T = bk_pool_vdm_primary_alloc(cmd, size, alignment);

   if (T.gpu && bk_g17p_vdm_low_window_enabled(&dev->dev)) {
      T.gpu = bk_g17p_vdm_compact(cmd, T.gpu);
      if (!T.gpu) {
         vk_command_buffer_set_error(&cmd->vk,
                                     VK_ERROR_OUT_OF_DEVICE_MEMORY);
         T.cpu = NULL;
      }
   }

   return T;
}

bool bk_cs_init_graphics(struct bk_cmd_buffer *cmd, struct bk_cs *cs);
/* A depth/stencil clear touched the pass's attachment (ZLS elision). */
void bk_g17p_note_depth_clear(struct bk_cmd_buffer *cmd,
                              VkImageAspectFlags aspects);

uint64_t bk_default_sample_positions(unsigned nr_samples);

void bk_flush_deferred_clears_why(struct bk_cmd_buffer *cmd, const char *why);
#define bk_flush_deferred_clears(cmd) bk_flush_deferred_clears_why(cmd, __func__)

/* The depth image of the pending store is overwritten as a whole before
 * anything read it: the store is dead (see bk_take_pending_z_store()). */
static inline void
bk_kill_pending_z_store(struct bk_cmd_buffer *cmd)
{
   struct bk_pending_z_store *p = &cmd->state.pending_z_store;
   p->cs->cr.zls_control = p->zls_control;
   p->cs->cr.process_empty_tiles = p->process_empty_tiles;
   p->cs = NULL;
}
void bk_deferred_clears_hold_buffer(struct bk_cmd_buffer *cmd, uint64_t addr,
                                    uint64_t size);
bool bk_defer_clear(struct bk_cmd_buffer *cmd, struct bk_image *image,
                    VkImageAspectFlags aspects, const VkClearValue *value,
                    uint32_t range_count, const VkImageSubresourceRange *ranges);
const VkRenderingInfo *
bk_take_deferred_clears(struct bk_cmd_buffer *cmd, const VkRenderingInfo *info,
                        VkRenderingInfo *copy,
                        VkRenderingAttachmentInfo *color,
                        VkRenderingAttachmentInfo *depth,
                        VkRenderingAttachmentInfo *stencil);

void bk_flush_query_resets(struct bk_cmd_buffer *cmd, struct bk_cs *cs);

/* A control stream outside the command buffer's list of streams, on a root
 * of bk_cs_root_size() bytes. */
static inline struct bk_cs *
bk_cs_create_at(struct bk_cmd_buffer *cmd, bool compute, struct agx_ptr root)
{
   struct bk_device *dev = bk_cmd_buffer_device(cmd);
   size_t initial_size = bk_cs_root_size(dev->dev.chip, compute);

   /* Allocate bk_cs for the new stream */
   struct bk_cs *cs = malloc(sizeof(*cs));
   *cs = (struct bk_cs){
      .cmd = cmd,
      .type = compute ? BK_CS_CDM : BK_CS_VDM,
      .addr = root.gpu,
      .start = root.cpu,
      .root_size = initial_size,
      .chunk = root,
      .current = root.cpu,
      .end = root.cpu + initial_size,
   };

   util_dynarray_init(&cs->compute_resources, NULL);
   util_dynarray_init(&cs->render_resources, NULL);
   list_inithead(&cs->node);
   bk_drm_attachment_set_init(&cs->compute_attachments);
   /* Only known meta destinations are collected so far. */
   cs->compute_attachments.complete = false;
   return cs;
}

static inline struct bk_cs *
bk_cs_create(struct bk_cmd_buffer *cmd, bool compute)
{
   /* Allocate root control stream */
   struct bk_device *dev = bk_cmd_buffer_device(cmd);
   struct agx_ptr root =
      bk_pool_cs_alloc(cmd, bk_cs_root_size(dev->dev.chip, compute), 1024);
   return root.cpu ? bk_cs_create_at(cmd, compute, root) : NULL;
}

static inline struct bk_cs *
bk_cmd_buffer_get_cs_general(struct bk_cmd_buffer *cmd, struct bk_cs **ptr,
                             bool compute)
{
   /* Any recorded work may read an image with a deferred clear, or the
    * depth image of a pending store. */
   if (unlikely(cmd->state.deferred_clear_count) &&
       !cmd->state.deferred_clear_flushing && !cmd->state.deferred_clear_hold)
      bk_flush_deferred_clears(cmd);
   if (unlikely(cmd->state.pending_z_store.cs) &&
       !cmd->state.deferred_clear_hold)
      cmd->state.pending_z_store.cs = NULL;

   if (vk_command_buffer_has_error(&cmd->vk))
      return NULL;

   if (*ptr && (*ptr)->g18_dispatch_sealed)
      *ptr = NULL;

   /* A recorded dependency may have ordered the open general compute stream
    * after work that follows it in the list. It is then ended, and the work
    * goes into a stream positioned after that requirement. Work of a new
    * stage class must also update the reused stream's kind and dependencies,
    * so later barriers include transfers and meta graphics emitted there. */
   if (*ptr && ptr == &cmd->current_cs.cs &&
       (cmd->sync.cs_check_pending ||
        (bk_sync_general_cs_kind(cmd) & ~(*ptr)->sync.kind)))
      bk_sync_check_cs(cmd);

   /* Work for the pre-graphics stream follows the batched tessellation of the
    * draws recorded before it. */
   if (ptr == &cmd->current_cs.pre_gfx && cmd->tess_batch.draws &&
       !cmd->tess_batch.recording)
      bk_tess_batch_flush(cmd);

   if ((*ptr) == NULL) {
      struct bk_device *dev = bk_cmd_buffer_device(cmd);
      struct bk_cs *cs = bk_cs_create(cmd, compute);
      if (!cs)
         return NULL;

      bool before_gfx = (ptr == &cmd->current_cs.pre_gfx);

      if (!compute) {
         const bool initialized = bk_cs_init_graphics(cmd, cs);
         if (!bk_graphics_cs_publishable(
                dev->dev.chip, initialized,
                cs->agx3_active_context_selector_valid)) {
            free(cs);
            return NULL;
         }
      }

      /* Insert into the command buffer. We usually append to the end of the
       * command buffer, except for pre-graphics streams which go right before
       * the graphics workload. (This implies a level of out-of-order processing
       * that's allowed by Vulkan and required for efficient
       * geometry/tessellation shaders.)
       */
      if (before_gfx && cmd->current_cs.gfx) {
         list_addtail(&cs->node, &cmd->current_cs.gfx->node);
      } else {
         list_addtail(&cs->node, &cmd->control_streams);
      }

      *ptr = cs;

      bk_sync_stream_created(cmd, cs, ptr, compute);
   }

   assert(*ptr != NULL);
   if (compute && unlikely((*ptr)->query_reset_pending) &&
       !cmd->state.query_reset_recording)
      bk_flush_query_resets(cmd, *ptr);
   if (compute && cmd->state.as_build_recording)
      (*ptr)->as_build = true;
   /* Transfer entry points also reuse an open CDM stream, so note each use,
    * not just stream creation (bk_sync_note_xfer_write()). */
   if (compute && unlikely(cmd->sync.xfer_depth) && !cmd->sync.in_pass &&
       bk_cmd_buffer_device(cmd)->dev.chip == AGX_CHIP_G17P)
      bk_sync_note_xfer_write(cmd, *ptr);
   return *ptr;
}

static inline struct bk_cs *
bk_cmd_buffer_get_cs(struct bk_cmd_buffer *cmd, bool compute)
{
   struct bk_cs **ptr = compute ? &cmd->current_cs.cs : &cmd->current_cs.gfx;
   return bk_cmd_buffer_get_cs_general(cmd, ptr, compute);
}

void bk_ensure_cs_has_space(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                            size_t space);

static inline uint64_t
bk_cs_alloc_for_indirect(struct bk_cs *cs, size_t size_B)
{
   bk_ensure_cs_has_space(cs->cmd, cs, size_B);

   uint64_t addr = bk_cs_current_addr(cs);
   cs->current += size_B;
   return addr;
}

static void
bk_cmd_buffer_dirty_all(struct bk_cmd_buffer *cmd)
{
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct bk_graphics_state *gfx = &cmd->state.gfx;

   vk_dynamic_graphics_state_dirty_all(dyn);
   gfx->dirty = ~0;
   gfx->shaders_dirty = ~0;
   gfx->linked_dirty = ~0;
   gfx->descriptors.root_dirty = true;
}

static inline void
bk_cs_destroy(struct bk_cs *cs)
{
   util_dynarray_fini(&cs->compute_resources);
   util_dynarray_fini(&cs->render_resources);
   if (cs->type == BK_CS_VDM) {
      util_dynarray_fini(&cs->scissor);
      util_dynarray_fini(&cs->depth_bias);
   } else {
      util_dynarray_fini(&cs->imm_writes);
      util_dynarray_fini(&cs->oq_stamps);
   }

   free(cs);
}

void bk_dispatch_imm_writes(struct bk_cmd_buffer *cmd, struct bk_cs *cs);
bool bk_defer_oq_available(struct bk_cmd_buffer *cmd, struct bk_cs *cs);
void bk_resolve_deferred_oq_available(struct bk_cmd_buffer *cmd);

bool bk_g17p_smooth_varyings_supported(
   struct bk_cmd_buffer *cmd, const struct bk_linked_shader *linked);

void *bk_cmd_buffer_emit_vdm_terminate(struct bk_cmd_buffer *cmd,
                                       void *current);

/* Ends a compute stream holding launches with a flush that has memory scope
 * while bk_cmd_alias_scope() asks for it; otherwise its last record is a
 * patch site (bk_cdm_note_alias_site()). */
void bk_cdm_end_stream_flush(struct bk_cmd_buffer *cmd, struct bk_cs *cs);

static void
bk_cmd_buffer_end_compute_internal(struct bk_cmd_buffer *cmd,
                                   struct bk_cs **ptr)
{
   if (*ptr) {
      struct bk_cs *cs = *ptr;
      if (unlikely(cs->query_reset_pending))
         bk_flush_query_resets(cmd, cs);

      /* This control stream may write immediates as it ends. Queue the writes
       * now that we're done emitting everything else. Occlusion availability
       * alone may instead wait for the end of the command buffer, which
       * keeps the (still empty) stream in the list.
       */
      if (cs->imm_writes.size) {
         if (bk_defer_oq_available(cmd, cs)) {
            *ptr = NULL;
            return;
         }
         bk_dispatch_imm_writes(cmd, cs);
      }

      bk_cdm_end_stream_flush(cmd, cs);

      /* A transformed draw may reserve a pre-graphics stream and then prove
       * that it has no vertices to process. Do not leave an empty CDM job in
       * the submission list. A stream that only carries a timestamp stays:
       * that is the one job the stamp can ride on. */
      if (cs->stats.cmds == 0 && cs->stats.flushes == 0 &&
          !cs->timestamp.end.handle && !cs->timestamp.start.handle) {
         bk_sync_forget_stream(cmd, cs);
         list_del(&cs->node);
         bk_cs_destroy(cs);
      }
   }

   *ptr = NULL;
}

static void
bk_cmd_buffer_end_compute(struct bk_cmd_buffer *cmd)
{
   bk_cmd_buffer_end_compute_internal(cmd, &cmd->current_cs.cs);
}

/* Return true when the VDM was replaced/removed, false when it needs normal
 * graphics finalization (for example, a native background clear). */
bool bk_optimize_empty_vdm(struct bk_cmd_buffer *cmd);

/* Return whether a physical render was finalized (empty renders can vanish). */
static bool
bk_cmd_buffer_end_graphics(struct bk_cmd_buffer *cmd)
{
   bk_tess_batch_flush(cmd);

   struct bk_cs *cs = cmd->current_cs.gfx;

   if (cs && cs->stats.cmds == 0 && bk_optimize_empty_vdm(cmd))
      cs = NULL;

   if (cs) {
      struct bk_device *dev = bk_cmd_buffer_device(cmd);
      const bool full_usc_state = agx_native_graphics(&dev->dev);
      /* Scissor and depth bias arrays are staged to dynamic arrays on the
       * CPU. When we end the control stream, they're done growing and are
       * ready for upload. G17P's ISP consumes these as full bit-40-set GART
       * addresses, matching native's USC-window allocations; a main-heap
       * address has bit 39 set and incorrectly selects the upper root.
       */
      cs->uploaded_scissor =
         full_usc_state
            ? bk_pool_usc_upload(cmd, cs->scissor.data, cs->scissor.size, 64)
            : bk_pool_upload(cmd, cs->scissor.data, cs->scissor.size, 64);

      cs->uploaded_zbias =
         full_usc_state
            ? bk_pool_usc_upload(cmd, cs->depth_bias.data,
                                 cs->depth_bias.size, 64)
            : bk_pool_upload(cmd, cs->depth_bias.data,
                             cs->depth_bias.size, 64);

      /* TODO: maybe free scissor/depth_bias now? */
      cs->current = bk_cmd_buffer_emit_vdm_terminate(cmd, cs->current);
   }

   cmd->current_cs.gfx = NULL;

   bk_cmd_buffer_end_compute_internal(cmd, &cmd->current_cs.pre_gfx);
   bk_cmd_buffer_end_compute_internal(cmd, &cmd->current_cs.post_gfx);

   assert(cmd->current_cs.pre_gfx == NULL);
   assert(cmd->current_cs.post_gfx == NULL);

   /* We just flushed out the heap use. If we want to use it again, we'll need
    * to queue a free for it again.
    */
   cmd->uses_heap = false;
   return cs != NULL;
}

static inline uint64_t
bk_pipeline_stat_addr(struct bk_cmd_buffer *cmd,
                      VkQueryPipelineStatisticFlagBits stat)
{
   struct bk_root_descriptor_table *root = &cmd->state.gfx.descriptors.root;
   VkQueryPipelineStatisticFlags flags = root->draw.pipeline_stats_flags;

   if (flags & stat) {
      assert(!cmd->in_meta && "queries paused for meta");
      assert(util_bitcount(stat) == 1 && "by construction");

      uint32_t index;
      const bool present =
         bk_pipeline_statistics_report_index(flags, stat, &index);
      assert(present && "enabled statistic has a compact report index");

      return root->draw.pipeline_stats + (sizeof(uint64_t) * index);
   } else {
      /* Query disabled */
      return AGX_SCRATCH_PAGE_ADDRESS;
   }
}

static inline bool
bk_stat_enabled(uint64_t addr)
{
   return addr != AGX_SCRATCH_PAGE_ADDRESS;
}

/* AGX3 render pass continuation (bk_cmd_draw.c): a pipeline barrier recorded
 * while an ended instance is still continuable decides whether the next one
 * may continue in its stream. */
void bk_m5_pass_merge_barrier(struct bk_cmd_buffer *cmd,
                              const VkDependencyInfo *info);

/* An ordered continuation (bk_cs::m5_ordered) ends at a command that may
 * break its memory equivalence (reason names the command). */
void bk_m5_pass_merge_end_ordered(struct bk_cmd_buffer *cmd,
                                  const char *reason);

/* Graphics descriptor updates while an ordered continuation is open, and
 * bookkeeping of the images graphics push descriptors reference. */
void bk_m5_pass_merge_note_push(struct bk_cmd_buffer *cmd,
                                struct bk_image_view *iview);
void bk_m5_pass_merge_note_set_bind(struct bk_cmd_buffer *cmd);

/* The vertex stage program of record draw_id > 0 of a multi-draw
 * (bk_cmd_buffer.c): only the draw parameters and draw index may differ from
 * the program the previous record of the same multi-draw published. */
uint64_t bk_m5_multi_draw_vertex_program(struct bk_cmd_buffer *cmd,
                                         struct bk_shader *s,
                                         struct bk_linked_shader *linked);

void bk_cmd_buffer_begin_graphics(struct bk_cmd_buffer *cmd,
                                  const VkCommandBufferBeginInfo *pBeginInfo);
void bk_cmd_buffer_begin_compute(struct bk_cmd_buffer *cmd,
                                 const VkCommandBufferBeginInfo *pBeginInfo);

void bk_cmd_invalidate_graphics_state(struct bk_cmd_buffer *cmd);
void bk_cmd_invalidate_compute_state(struct bk_cmd_buffer *cmd);

void bk_cmd_bind_shaders(struct vk_command_buffer *vk_cmd, uint32_t stage_count,
                         const mesa_shader_stage *stages,
                         struct vk_shader **const shaders);

void bk_cmd_bind_graphics_shader(struct bk_cmd_buffer *cmd,
                                 const mesa_shader_stage stage,
                                 struct bk_api_shader *shader);

void bk_cmd_bind_compute_shader(struct bk_cmd_buffer *cmd,
                                struct bk_api_shader *shader);

void bk_cmd_bind_vertex_buffer(struct bk_cmd_buffer *cmd, uint32_t vb_idx,
                               struct bk_addr_range addr_range);

static inline struct bk_descriptor_state *
bk_get_descriptors_state(struct bk_cmd_buffer *cmd,
                         VkPipelineBindPoint bind_point)
{
   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      return &cmd->state.gfx.descriptors;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmd->state.cs.descriptors;
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
      return &cmd->state.rt.descriptors;
   default:
      UNREACHABLE("Unhandled bind point");
   }
};

void bk_cmd_buffer_flush_push_descriptors(struct bk_cmd_buffer *cmd,
                                          struct bk_descriptor_state *desc);

void bk_meta_resolve_rendering(struct bk_cmd_buffer *cmd,
                               const VkRenderingInfo *pRenderingInfo);

void bk_cmd_upload_draw_params(struct bk_cmd_buffer *cmd, uint32_t first,
                               uint32_t first_instance);
void bk_cmd_upload_draw_id(struct bk_cmd_buffer *cmd, uint32_t draw_id);
uint64_t bk_cmd_upload_constants(struct bk_cmd_buffer *cmd,
                                 const struct bk_shader *shader,
                                 mesa_shader_stage stage);
void bk_cmd_print_upload_stats(struct bk_cmd_buffer *cmd);

uint64_t bk_cmd_buffer_upload_root(struct bk_cmd_buffer *cmd,
                                   VkPipelineBindPoint bind_point);

void bk_reserve_scratch_slow(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                             struct bk_shader *s);

static inline void
bk_reserve_scratch(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                   struct bk_shader *s)
{
   /* Not scratch but this is the most convenient place for this... */
   cs->uses_sampler_heap |= s->b.info.uses_sampler_heap;

   /* G17P and M5 publish a shader's spill frame as thread-private storage in their
    * SPECTPR. There is no helper-program stack, and the kernel rejects a
    * command that names one. */
   if ((s->b.info.scratch_size == 0 && s->b.info.preamble_scratch_size == 0) ||
       agx_native_graphics(&bk_cmd_buffer_device(cmd)->dev) ||
       agx_chip_g17_m5(bk_cmd_buffer_device(cmd)->dev.chip))
      return;

   bk_reserve_scratch_slow(cmd, cs, s);
}

/* Whether image barriers of the command buffer order only the vertex work
 * that reads images (bk_sync.h, BK_SYNC_GFX_IMG). */
static inline bool
bk_sync_image_class(const struct bk_cmd_buffer *cmd)
{
   const enum agx_chip chip = bk_cmd_buffer_device(cmd)->dev.chip;
   return chip == AGX_CHIP_G17P || agx_chip_g17_m5(chip);
}

/* AGX2 returns its shader-base-relative USC offset.  G17P returns the absolute
 * 48-bit Ioto program VA consumed by HAL200 VDM state. */
struct agx3_ioto_uniform_range;

bool bk_m5_compute_uniform_dma_ranges(
   struct bk_cmd_buffer *cmd, const struct bk_shader *s, uint64_t root_ptr,
   struct agx3_ioto_uniform_range *ranges, size_t capacity, size_t *count);

uint64_t bk_g17p_dma_set_base(const struct bk_cmd_buffer *cmd,
                              const struct bk_shader *s);

struct agx3_ioto_uniform_range;
bool bk_g17p_dma_ranges(const struct bk_descriptor_state *desc,
                        const struct bk_shader *s, uint64_t root_ptr,
                        struct agx3_ioto_uniform_range *ranges,
                        size_t max_ranges, size_t *count_io,
                        uint8_t *sources);

uint64_t bk_upload_native_mesh_program(struct bk_cmd_buffer *cmd,
                                      struct bk_shader *s,
                                      struct bk_linked_shader *linked, bool object,
                                      bool task);
uint64_t bk_upload_usc_words(struct bk_cmd_buffer *cmd, struct bk_shader *s,
                             struct bk_linked_shader *linked);

void bk_usc_upload_spilled_rt_descs(struct agx_usc_builder *b,
                                    struct bk_cmd_buffer *cmd);

void bk_cdm_cache_flush(struct bk_device *dev, struct bk_cs *cs);

/* As bk_cdm_cache_flush(), for a flush an application dependency asks for.
 * With `memory`, the flush also has memory scope on G17P and M5: needed for
 * host availability, and for device dependencies once the same memory may be
 * reached through more than one virtual address (bk_cdm_alias_scope()). A
 * trailing strong flush without memory scope is upgraded in place. The caller
 * ensures space for one barrier record. */
void bk_cdm_visibility_flush(struct bk_device *dev, struct bk_cs *cs,
                             bool memory);

/* Whether bk_cdm_visibility_flush(dev, cs, memory) would record anything. */
bool bk_cdm_visibility_flush_needed(const struct bk_device *dev,
                                    const struct bk_cs *cs, bool memory);

void bk_dispatch_with_usc_launch(struct bk_device *dev, struct bk_cs *cs,
                                 struct agx_cdm_launch_word_0_packed launch,
                                 uint32_t usc, struct agx_grid grid,
                                 struct agx_workgroup local_size);

void bk_dispatch_with_usc(struct bk_device *dev, struct bk_cs *cs,
                          struct agx_shader_info *info, uint32_t usc,
                          struct agx_grid grid,
                          struct agx_workgroup local_size);

void bk_dispatch_m5_native(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                            struct bk_shader *s, struct agx_grid grid,
                            struct agx_workgroup local_size);

void bk_dispatch_g17p_native(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                             struct bk_shader *s, struct agx_grid grid,
                             struct agx_workgroup local_size);
void bk_dispatch_g17g_native(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                             struct bk_shader *s, struct agx_grid grid,
                             struct agx_workgroup local_size);

bool bk_gen1_upload_fragment_query_state(struct bk_cmd_buffer *cmd, bool vertex_stage,
   struct agx3_ioto_uniform_range *range);
bool bk_gen1_upload_rq_uniforms(struct bk_cmd_buffer *cmd, uint32_t *context,
                                unsigned context_words, bool profile,
                                struct agx3_ioto_uniform_range *range);

void bk_g17p_prewarm_linked_compute_shader(struct bk_device *dev,
                                            struct bk_shader *s,
                                            struct bk_linked_shader *linked);
void bk_g17p_linked_compute_cache_finish(struct bk_linked_shader *linked);
void bk_g17p_linked_graphics_cache_finish(struct bk_device *dev,
                                          struct bk_linked_shader *linked);

/*
 * Saved application state around a driver-internal meta operation.  The
 * acceleration-structure builder needs this as much as a blit does: the
 * common build shaders bind their own compute pipeline and push constants,
 * so a build recorded between two application dispatches would otherwise
 * silently clobber them.
 */
struct bk_meta_save {
   struct vk_vertex_input_state _dynamic_vi;
   struct vk_sample_locations_state _dynamic_sl;
   struct vk_dynamic_graphics_state dynamic;
   struct bk_api_shader *shaders[MESA_SHADER_MESH + 1];
   struct bk_addr_range vb0;
   struct bk_descriptor_set *desc0;
   /* Nested meta must retain the outer query/conditional-rendering scope. */
   bool in_meta;
   bool has_push_desc0;
   bool push_active0;
   /* Set 0 bound from a descriptor buffer */
   bool buffer_active0;
   uint64_t buffer_set0;
   uint32_t buffer_set_size0;
   enum agx_visibility_mode occlusion;
   struct bk_push_descriptor_set push_desc0;
   VkQueryPipelineStatisticFlags pipeline_stats_flags;
   uint8_t push[BK_MAX_PUSH_SIZE];
   /* The runtime's record of stages bound from a pipeline. */
   VkShaderStageFlags pipeline_shader_stages;
};

void bk_meta_begin(struct bk_cmd_buffer *cmd, struct bk_meta_save *save,
                   VkPipelineBindPoint bind_point);
void bk_meta_end(struct bk_cmd_buffer *cmd, struct bk_meta_save *save,
                 VkPipelineBindPoint bind_point);

void bk_cmd_dispatch_rays(struct bk_cmd_buffer *cmd,
                          struct bk_api_shader *shader, uint64_t record_addr,
                          uint64_t group_count_addr, struct agx_grid grid);

void bk_cmd_dispatch_unaligned(VkCommandBuffer commandBuffer,
                               uint32_t invocations_x, uint32_t invocations_y,
                               uint32_t invocations_z);

/* Driver-owned, resident indirect arguments addressed through their primary
 * GPU VA. Sparse application buffers must use the public entrypoint. */
void bk_cmd_dispatch_indirect_addr(VkCommandBuffer command_buffer,
                                   VkDeviceAddress address);

static inline void
bk_dispatch_with_local_size(struct bk_cmd_buffer *cmd, struct bk_cs *cs,
                            struct bk_shader *s, struct agx_grid grid,
                            struct agx_workgroup local_size)
{
   if (agx_is_shader_empty(&s->b))
      return;

   struct bk_device *dev = bk_cmd_buffer_device(cmd);
   if (dev->dev.chip == AGX_CHIP_G18G) {
      bk_dispatch_g17g_native(cmd, cs, s, grid, local_size);
      return;
   }
   if (agx_chip_g17_m5(dev->dev.chip)) {
      bk_dispatch_m5_native(cmd, cs, s, grid, local_size);
      return;
   }
   if (agx_native_graphics(&dev->dev)) {
      bk_reserve_scratch(cmd, cs, s);
      bk_dispatch_g17p_native(cmd, cs, s, grid, local_size);
      /* Software graphics stages communicate through memory: VS outputs,
       * geometry counts and XFB bases are consumed by the next dispatch.
       * The application cannot supply barriers between these internal
       * stages. Preserve the AGX2 flush-after-launch contract, including
       * indirect launches, by strengthening the native ordering barrier. */
      if (s->info.stage != MESA_SHADER_COMPUTE) {
         bk_ensure_cs_has_space(cmd, cs, AGX3_CDM_BARRIER_SIZE);
         bk_cdm_cache_flush(dev, cs);
      }
      return;
   }

   uint64_t uploaded = bk_upload_usc_words(cmd, s, s->only_linked);
   if (uploaded > UINT32_MAX) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }
   uint32_t usc = uploaded;

   bk_reserve_scratch(cmd, cs, s);
   bk_dispatch_with_usc(dev, cs, &s->b.info, usc, grid, local_size);
}

void bk_dispatch_precomp(struct bk_cmd_buffer *cmd, struct agx_grid grid,
                         enum agx_barrier barrier, enum libagx_program idx,
                         void *data, size_t data_size);
void bk_dispatch_precomp_m5(struct bk_cmd_buffer *cmd, struct agx_grid grid,
                            enum agx_barrier barrier, enum libagx_program idx,
                            const void *data, size_t data_size);
void bk_dispatch_precomp_g17p(struct bk_cmd_buffer *cmd, struct agx_grid grid,
                              enum agx_barrier barrier,
                              enum libagx_program idx, const void *data,
                              size_t data_size);

/* Turn a three/four/five-word indirect argument record into an execution-time
 * conditional record. `barrier` selects the ordered compute stream which must
 * precede its consumer. */
uint64_t bk_apply_conditional_rendering(struct bk_cmd_buffer *cmd,
                                        uint64_t source, uint32_t words,
                                        enum agx_barrier barrier);

#define MESA_DISPATCH_PRECOMP bk_dispatch_precomp

void bk_queue_write(struct bk_cmd_buffer *cmd, uint64_t address, uint32_t value,
                    bool after_gfx);
bool bk_ensure_occlusion_query_for_draw(struct bk_cmd_buffer *cmd);

/* Dispatch the native G17P raw-address fill meta shader into an explicitly
 * selected compute stream.  The target may be the ordinary or post-graphics
 * stream; the latter preserves query-availability ordering. */
void bk_g17p_meta_fill_addr(struct bk_cmd_buffer *cmd, struct bk_cs **target,
                            uint64_t address, uint64_t size, uint32_t value);

/* Poll a naturally aligned 32-bit device address from a native G17P compute
 * stream until it reaches value. */
void bk_g17p_meta_wait_addr(struct bk_cmd_buffer *cmd, uint64_t address,
                            uint32_t value);

void agx_fill_velem_keys(const struct vk_vertex_input_state *vi,
                         uint64_t attribs_read, struct agx_velem_key *keys);

struct agx_robustness bk_prolog_robustness(struct bk_device *dev);
