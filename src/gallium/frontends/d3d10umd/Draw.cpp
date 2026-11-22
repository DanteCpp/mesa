/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Draw.h --
 *    Functions that render 3D primitives.
 */


#include "Draw.h"
#include "Wddm.h"
#include "State.h"
#include "Shader.h"

#include "Debug.h"

#include "util/u_draw.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "tgsi/tgsi_exec.h"

/*
 * Heavy-draw tracing: draws with a long pixel shader (DWM's blur) log
 * their target, viewport, the first constants of PS cb0 and the softpipe progress counters,
 * and a watchdog thread reports progress every 20 s while such a draw runs.
 */
static volatile LONG heavy_in_flight;
static ULONGLONG heavy_t0, heavy_runs0, heavy_iters0, heavy_fs0;
static HANDLE heavy_thread;

/* one address: module-relative when inside a loaded module */
static void
HeavyAddr(char *buf, size_t n, DWORD64 a)
{
   HMODULE m = NULL;
   char name[MAX_PATH] = "?";
   if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &m) && m) {
      GetModuleFileNameA(m, name, sizeof name);
      const char *b = strrchr(name, '\\');
      snprintf(buf, n, " %s+0x%llx", b ? b + 1: name, (unsigned long long)(a - (DWORD64)(uintptr_t)m));
   } else {
      snprintf(buf, n, " 0x%llx", (unsigned long long)a);
   }
}

/* stack sample of the drawing thread: pc, lr and the frame-pointer chain */
static void
HeavySample(void)
{
#if defined(_M_ARM64) || defined(__aarch64__)
   if (!heavy_thread || SuspendThread(heavy_thread) == (DWORD)-1)
      return;
   CONTEXT ctx;
   memset(&ctx, 0, sizeof ctx);
   ctx.ContextFlags = CONTEXT_FULL;
   char line[2048];
   size_t len = 0;
   if (GetThreadContext(heavy_thread, &ctx)) {
      DWORD64 pcs[20];
      unsigned n = 0;
      pcs[n++] = ctx.Pc;
      pcs[n++] = ctx.Lr;
      DWORD64 fp = ctx.Fp;
      while (n < 20 && fp && !(fp & 7)) {
         DWORD64 fr[2];
         SIZE_T got = 0;
         if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)fp, fr, sizeof fr, &got) ||
             got != sizeof fr || fr[0] <= fp)
            break;
         pcs[n++] = fr[1];
         fp = fr[0];
      }
      for (unsigned i = 0; i < n && len < sizeof line - 128; i++) {
         HeavyAddr(line + len, sizeof line - len, pcs[i]);
         len += strlen(line + len);
      }
   } else {
      snprintf(line, sizeof line, " GetThreadContext failed %lu", GetLastError());
   }
   ResumeThread(heavy_thread);
   DebugPrintf("watch: stack%s\n", line);
#endif
}

static DWORD WINAPI
HeavyWatch(void *arg)
{
   for (;;) {
      Sleep(15000);
      if (heavy_in_flight) {
         DebugPrintf("watch: heavy draw running %llu ms: tgsi runs +%llu fs +%llu loop iters +%llu\n",
                     GetTickCount64() - heavy_t0, tgsi_exec_runs - heavy_runs0,
                     tgsi_exec_fs_runs - heavy_fs0, tgsi_exec_loop_iters - heavy_iters0);
         HeavySample();
      }
   }
   return 0;
}

static bool
HeavyBegin(Device *pDevice, const char *what, unsigned count)
{
   if (!UmdTrace() || !pDevice->bound_ps || pDevice->bound_ps->ntokens < 1000)
      return false;
   static LONG started;
   if (!InterlockedExchange(&started, 1))
      CloseHandle(CreateThread(NULL, 0, HeavyWatch, NULL, 0, NULL));
   unsigned cb[12] = {0};
   unsigned cbsize = 0;
   if (pDevice->ps_cb0) {
      cbsize = pDevice->ps_cb0->width0;
      pipe_buffer_read(pDevice->pipe, pDevice->ps_cb0, 0, MIN2(cbsize, sizeof cb), cb);
   }
   unsigned rw = 0, rh = 0, tw = 0, th = 0;
   if (pDevice->fb.nr_cbufs && pDevice->fb.cbufs[0].texture) {
      rw = pDevice->fb.cbufs[0].texture->width0;
      rh = pDevice->fb.cbufs[0].texture->height0;
   }
   struct pipe_sampler_view *sv = pDevice->sampler_views[MESA_SHADER_FRAGMENT][0];
   if (sv && sv->texture) {
      tw = sv->texture->width0;
      th = sv->texture->height0;
   }
   DebugPrintf("heavy %s: ps %u dwords, count %u, rt0 %ux%u, t0 %ux%u, vp %.0f,%.0f %.0fx%.0f, "
               "cb0 %u bytes: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x\n",
               what, pDevice->bound_ps->ntokens, count, rw, rh, tw, th,
               pDevice->vp[0], pDevice->vp[1], pDevice->vp[2], pDevice->vp[3], cbsize,
               cb[0], cb[1], cb[2], cb[3], cb[4], cb[5], cb[6], cb[7], cb[8], cb[9], cb[10], cb[11]);
   heavy_t0 = GetTickCount64();
   heavy_runs0 = tgsi_exec_runs;
   heavy_iters0 = tgsi_exec_loop_iters;
   heavy_fs0 = tgsi_exec_fs_runs;
   if (heavy_thread)
      CloseHandle(heavy_thread);
   heavy_thread = NULL;
   DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &heavy_thread,
                   THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
   heavy_in_flight = 1;
   return true;
}

static void
HeavyEnd(Device *pDevice)
{
   /* rasterize now so the timing covers the pixel work */
   pDevice->pipe->flush(pDevice->pipe, NULL, 0);
   heavy_in_flight = 0;
   DebugPrintf("heavy done: %llu ms, tgsi runs %llu fs %llu, loop iters %llu\n",
               GetTickCount64() - heavy_t0, tgsi_exec_runs - heavy_runs0,
               tgsi_exec_fs_runs - heavy_fs0, tgsi_exec_loop_iters - heavy_iters0);
}

static unsigned
ClampedUAdd(unsigned a,
            unsigned b)
{
   unsigned c = a + b;
   if (c < a) {
      return 0xffffffff;
   }
   return c;
}


/* stride is required in order to set the element data */
static void
update_velems(Device *pDevice)
{
   if (!pDevice->velems_changed)
      return;

   if(pDevice->element_layout) {
      struct cso_velems_state *state = &pDevice->element_layout->state;
      for (unsigned i = 0; i < state->count; i++)
         state->velems[i].src_stride = pDevice->vertex_strides[state->velems[i].vertex_buffer_index];
      cso_set_vertex_elements(pDevice->cso, state);
   }

   pDevice->velems_changed = false;
}

/*
 * We have to resolve the stream output state for empty geometry shaders.
 * In particular we've remapped the output indices when translating the
 * shaders so now the register_index variables in the stream output
 * state are incorrect and we need to remap them back to the correct
 * state.
 */
static void
ResolveState(Device *pDevice)
{
   RotDiagDraw(pDevice, pDevice->fb.nr_cbufs ? pDevice->fb.cbufs[0].texture: NULL);
   if (pDevice->cfTarget)
      CfDamageDraw(pDevice);
   if (pDevice->proxiedHead)
      WddmSyncShared(pDevice);
   if (UmdTraceAll())
      DebugPrintf("ResolveState: shared synced, drawing\n");
   if (pDevice->bound_empty_gs && pDevice->bound_vs &&
       pDevice->bound_vs->state.tokens) {
      Shader *gs = pDevice->bound_empty_gs;
      Shader *vs = pDevice->bound_vs;
      bool remapped = false;
      struct pipe_context *pipe = pDevice->pipe;
      if (!gs->output_resolved) {
         for (unsigned i = 0; i < gs->state.stream_output.num_outputs; ++i) {
            unsigned mapping =
               ShaderFindOutputMapping(vs, gs->state.stream_output.output[i].register_index);
            if (mapping != gs->state.stream_output.output[i].register_index) {
               gs->state.stream_output.output[i].register_index = mapping;
               remapped = true;
            }
         }
         if (remapped) {
            pipe->delete_gs_state(pipe, gs->handle);
            gs->handle = pipe->create_gs_state(pipe, &gs->state);
         }
         gs->output_resolved = true;
      }
      pipe->bind_gs_state(pipe, gs->handle);
   }
   update_velems(pDevice);

   if (pDevice->vbuffers_changed) {
      cso_set_vertex_buffers(pDevice->cso, PIPE_MAX_ATTRIBS, pDevice->vertex_buffers);
      pDevice->vbuffers_changed = false;
   }
}


static struct pipe_resource *
create_null_index_buffer(struct pipe_context *ctx, uint num_indices,
                         unsigned *restart_index, unsigned *index_size,
                         unsigned *ib_offset)
{
   unsigned buf_size = num_indices * sizeof(unsigned);
   unsigned *buf = (unsigned*)MALLOC(buf_size);
   struct pipe_resource *ibuf;

   memset(buf, 0, buf_size);

   ibuf = pipe_buffer_create_with_data(ctx,
                                       PIPE_BIND_INDEX_BUFFER,
                                       PIPE_USAGE_IMMUTABLE,
                                       buf_size, buf);
   *index_size = 4;
   *restart_index = 0xffffffff;
   *ib_offset = 0;

   FREE(buf);

   return ibuf;
}

/*
 * ----------------------------------------------------------------------
 *
 * Draw --
 *
 *    The Draw function draws nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Draw(D3D10DDI_HDEVICE hDevice,   // IN
     UINT VertexCount,           // IN
     UINT StartVertexLocation)   // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   ResolveState(pDevice);

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   bool heavy = HeavyBegin(pDevice, "Draw", VertexCount);
   /* through the cso context: on GPU screens (zink) u_vbuf binds the driver's vertex buffers and
    * elements only inside its own draw_vbo, so a draw straight to the pipe has none bound */
   cso_draw_arrays(pDevice->cso,
                   pDevice->primitive,
                   StartVertexLocation,
                   VertexCount);
   if (heavy)
      HeavyEnd(pDevice);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexed --
 *
 *    The DrawIndexed function draws indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexed(D3D10DDI_HDEVICE hDevice,  // IN
            UINT IndexCount,           // IN
            UINT StartIndexLocation,   // IN
            INT BaseVertexLocation)    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCount,
                                  &restart_index, &index_size, &ib_offset);
   }

   ResolveState(pDevice);

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCount;
   info.index.resource = null_ib ? null_ib: pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.primitive_restart = true;
   info.restart_index = restart_index;

   bool heavy = HeavyBegin(pDevice, "DrawIndexed", IndexCount);
   cso_draw_vbo(pDevice->cso, &info, 0, NULL, &draw, 1);
   if (heavy)
      HeavyEnd(pDevice);

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawInstanced --
 *
 *    The DrawInstanced function draws particular instances
 *    of nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawInstanced(D3D10DDI_HDEVICE hDevice,      // IN
              UINT VertexCountPerInstance,   // IN
              UINT InstanceCount,            // IN
              UINT StartVertexLocation,      // IN
              UINT StartInstanceLocation)    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   if (!InstanceCount) {
      return;
   }

   ResolveState(pDevice);

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   cso_draw_arrays_instanced(pDevice->cso,
                             pDevice->primitive,
                             StartVertexLocation,
                             VertexCountPerInstance,
                             StartInstanceLocation,
                             InstanceCount);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexedInstanced --
 *
 *    The DrawIndexedInstanced function draws particular
 *    instances of indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexedInstanced(D3D10DDI_HDEVICE hDevice,   // IN
                     UINT IndexCountPerInstance, // IN
                     UINT InstanceCount,         // IN
                     UINT StartIndexLocation,    // IN
                     INT BaseVertexLocation,     // IN
                     UINT StartInstanceLocation) // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   if (!InstanceCount) {
      return;
   }

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCountPerInstance,
                                  &restart_index, &index_size, &ib_offset);
   }

   ResolveState(pDevice);

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCountPerInstance;
   info.index.resource = null_ib ? null_ib: pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.start_instance = StartInstanceLocation;
   info.instance_count = InstanceCount;
   info.primitive_restart = true;
   info.restart_index = restart_index;

   bool heavy = HeavyBegin(pDevice, "DrawIndexedInstanced", IndexCountPerInstance);
   cso_draw_vbo(pDevice->cso, &info, 0, NULL, &draw, 1);
   if (heavy)
      HeavyEnd(pDevice);

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawAuto --
 *
 *    The DrawAuto function works similarly to the Draw function,
 *    except DrawAuto is used for the special case where vertex
 *    data is written through the stream-output unit and then
 *    recycled as a vertex buffer. The driver determines the number
 *    of primitives, in part, by how much data was written to the
 *    buffer through stream output.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawAuto(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_indirect_info indirect;


   if (!pDevice->draw_so_target) {
      LOG_UNSUPPORTED("DrawAuto without a set source buffer!");
      return;
   }

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   ResolveState(pDevice);

   util_draw_init_info(&info);
   info.mode = pDevice->primitive;
   memset(&indirect, 0, sizeof indirect);
   indirect.count_from_stream_output = pDevice->draw_so_target;

   cso_draw_vbo(pDevice->cso, &info, 0, &indirect, NULL, 1);
}
