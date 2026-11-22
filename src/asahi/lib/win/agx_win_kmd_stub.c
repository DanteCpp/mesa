/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Stub render node: an in-process stand-in for the g18win render node, with
 * no GPU behind it (AGX_WIN_KMD_STUB=fake, see agx_win_kmd.h).
 *
 * It takes the kernel driver's IOCTL codes and frames and applies the same
 * rules g18_uapi.h states for G4 (frame checks, offsets instead of pointers,
 * min-size copies, EOPNOTSUPP for the commands G4 does not serve, SUBMIT
 * admission, no syncs). It answers GET_PARAMS group 0 field by field the way
 * the kernel's K4 does (G4-PLAN 3.3), so Mesa takes exactly the path it will
 * take on the M6. SUBMIT is checked and recorded but not executed: output
 * buffers stay as the CPU left them.
 *
 * Every call is recorded as G18R v1 (agx_win_g18r.h) when
 * AGX_WIN_KMD_STUB_LOG names a file: FILE, PARAMS, VM_NEW, BO_NEW, BIND,
 * QUEUE_NEW, the PAGE/BLOB snapshot of every page bound in the VM before a
 * SUBMIT, SUBMIT with the command buffer, then the frees; everything else as
 * RAW_IOCTL. That is the L1 recorder's format, so the L1 tools (g18r.py
 * verify/dump/diff) read it and C3 can diff it against L1.
 *
 * Optional environment:
 *   AGX_WIN_KMD_STUB_FEATURES  hex: replaces the advertised feature mask
 *                              (default G18_RN_FEATURES_G4)
 *
 * Plain C11 with no OS headers, so the host test builds it anywhere.
 */

#include "agx_win_kmd.h"
#include "agx_win_g18r.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STUB_PAGE      16384u
#define STUB_MAX_BINDS 8192u
/* G2-DESIGN 5.2: the replay arena the kernel allocates BOs from (K2). */
#define STUB_ARENA_BYTES (160ull << 20)
/* G4-PLAN 3.3 / K:g18/drm.rs get_params, file.rs. */
#define STUB_VM_START   0x4000ull
#define STUB_VM_END     ((1ull << 42) - 0x8000ull)
#define STUB_VM_KERNEL_MIN 0x20000000ull
#define STUB_MAX_ATTACHMENTS 16u

struct stub_bo {
   uint8_t *raw, *ptr;
   uint64_t size;
   uint32_t vm_id;   /* VM_PRIVATE owner or 0 */
   uint32_t flags;
   bool live;        /* handle valid (GEM_CLOSE drops it, pages stay) */
   uint32_t *page_blob; /* last PAGE blob id recorded per page */
};

struct stub_bind {
   uint32_t flags, handle;
   uint64_t offset, range, addr;
};

struct stub_vm {
   bool live;
   uint64_t kernel_start, kernel_end;
   uint32_t nbinds;
   struct stub_bind *binds;
};

struct stub_queue {
   bool live;
   uint32_t vm_id;
};

struct stub_map {
   uint64_t va, size;
};

struct agx_win_stub {
   uint32_t file_id;
   uint32_t nbos;                       /* handles 1..nbos, never reused */
   struct stub_bo bo[G18_RN_MAX_BOS + 1];
   struct stub_vm vm[G18_RN_MAX_VMS + 1];
   struct stub_queue queue[G18_RN_MAX_QUEUES + 1];
   uint32_t nvms, nqueues, nmaps, submits;
   struct stub_map map[G18_RN_MAX_MAPS];
};

static atomic_flag stub_lock = ATOMIC_FLAG_INIT;
static uint32_t stub_files;     /* FILE ids handed out */
static uint32_t stub_open;      /* nodes open now */
static uint32_t stub_current;   /* FILE id of the last chunk */
static uint64_t stub_arena_used;

static void
stub_lock_acquire(void)
{
   while (atomic_flag_test_and_set_explicit(&stub_lock, memory_order_acquire))
      ;
}

static void
stub_lock_release(void)
{
   atomic_flag_clear_explicit(&stub_lock, memory_order_release);
}

/* ---- recording -------------------------------------------------------- */

static void
rec_file(struct agx_win_stub *s, uint32_t op)
{
   uint32_t p[2] = {s->file_id, op};
   g18r_chunk(G18R_FILE, p, sizeof(p), NULL, 0);
   stub_current = op == 2 ? 0 : s->file_id;
}

static void
rec_select(struct agx_win_stub *s)
{
   if (g18r_active() && stub_current != s->file_id)
      rec_file(s, 1);
}

static void
rec_raw(uint32_t code, int32_t ret, uint32_t err, uint32_t flags,
        const void *in, uint32_t nin, const void *out, uint32_t nout)
{
   if (!g18r_active())
      return;
   uint8_t hdr[24];
   uint32_t v[6] = {code, (uint32_t)ret, err, flags, nin, nout};
   memcpy(hdr, v, sizeof(v));
   uint8_t buf[2 * G18_RN_ARG_MAX];
   if (nin > G18_RN_ARG_MAX)
      nin = G18_RN_ARG_MAX;
   if (nout > G18_RN_ARG_MAX)
      nout = G18_RN_ARG_MAX;
   memcpy(buf, in, nin);
   memcpy(buf + nin, out, nout);
   v[4] = nin;
   v[5] = nout;
   memcpy(hdr, v, sizeof(v));
   g18r_chunk(G18R_RAW_IOCTL, hdr, sizeof(hdr), buf, nin + nout);
}

static void
stub_log_begin(void)
{
   const char *path = getenv("AGX_WIN_KMD_STUB_LOG");
   if (!path || !*path || g18r_active())
      return;

   char capinfo[512];
   snprintf(capinfo, sizeof(capinfo),
            "recorder=agx-win-stub 1.0\n"
            "uname_r=windows\n"
            "machine=aarch64\n"
            "argv0=%s\n"
            "driver=%s %u.%u.%u (stub, no GPU)\n"
            "page_bytes=%u\n"
            "sync=0\n"
            "transport=g18_uapi rev %u rn abi %u\n",
            "vulkan_asahi", G18_RN_DRIVER_NAME, G18_RN_DRIVER_MAJOR,
            G18_RN_DRIVER_MINOR, G18_RN_DRIVER_PATCH, STUB_PAGE,
            G18_UAPI_HEADER_REV, G18_RN_ABI);
   g18r_begin(path, capinfo, sizeof(struct g18_asahi_params_global));
}

void
agx_win_stub_mark(uint32_t code, const char *text)
{
   stub_lock_acquire();
   uint32_t c = code;
   g18r_chunk(G18R_MARK, &c, 4, text, (uint32_t)strlen(text));
   stub_lock_release();
}

/* ---- open/close -------------------------------------------------------- */

struct agx_win_stub *
agx_win_stub_open(void)
{
   struct agx_win_stub *s = calloc(1, sizeof(*s));
   if (!s) {
      errno = ENOMEM;
      return NULL;
   }

   stub_lock_acquire();
   stub_log_begin();
   s->file_id = ++stub_files;
   stub_open++;
   rec_file(s, 0);
   stub_lock_release();
   return s;
}

void
agx_win_stub_close(struct agx_win_stub *s)
{
   if (!s)
      return;

   stub_lock_acquire();
   if (g18r_active()) {
      rec_select(s);
      rec_file(s, 2);
   }
   for (uint32_t h = 1; h <= s->nbos; h++) {
      free(s->bo[h].raw);
      free(s->bo[h].page_blob);
      stub_arena_used -= s->bo[h].size;
   }
   for (uint32_t v = 1; v <= G18_RN_MAX_VMS; v++)
      free(s->vm[v].binds);
   free(s);

   /* The record is complete when the last node closes (hk closes both of its
    * files on vkDestroyDevice / vkDestroyInstance). */
   if (--stub_open == 0)
      g18r_finish();
   stub_lock_release();
}

/* ---- helpers ------------------------------------------------------------ */

static struct stub_bo *
stub_bo(struct agx_win_stub *s, uint32_t handle)
{
   if (handle == 0 || handle > s->nbos || !s->bo[handle].live)
      return NULL;
   return &s->bo[handle];
}

static struct stub_vm *
stub_vm(struct agx_win_stub *s, uint32_t vm_id)
{
   if (vm_id == 0 || vm_id > G18_RN_MAX_VMS || !s->vm[vm_id].live)
      return NULL;
   return &s->vm[vm_id];
}

/* Region named by a pointer field: frame offset off, len bytes. */
static int
stub_region(uint32_t frame_size, uint32_t data_off, uint64_t off, uint64_t len,
            bool *none)
{
   *none = off == 0 && len == 0;
   if (*none)
      return 0;
   if ((off & 7) || off < data_off || off > frame_size ||
       len > frame_size - off)
      return G18_RN_EFAULT;
   return 0;
}

static void
stub_params(struct g18_asahi_params_global *p)
{
   /* G4-PLAN 3.3, field by field (K4). Identity: K:g18/drm.rs get_params;
    * the rest: K:file.rs get_params. */
   memset(p, 0, sizeof(*p));
   p->features = G18_RN_FEATURES_G4;
   const char *f = getenv("AGX_WIN_KMD_STUB_FEATURES");
   if (f && *f)
      p->features = strtoull(f, NULL, 16);
   p->gpu_generation = 18;
   p->gpu_variant = 'G';
   p->gpu_revision = 0x11; /* B1 */
   p->chip_id = 0x8152;
   p->num_dies = 1;
   p->num_clusters_total = 2;
   p->num_cores_per_cluster = 6;
   p->max_frequency_khz = 0;
   p->core_masks[0] = 0x3f;
   p->core_masks[1] = 0x3f;
   p->vm_start = STUB_VM_START;
   p->vm_end = STUB_VM_END;
   p->vm_kernel_min_size = STUB_VM_KERNEL_MIN;
   p->max_commands_per_submission = 1;
   p->max_attachments = STUB_MAX_ATTACHMENTS;
   p->command_timestamp_frequency_hz = 1000000000ull;
   p->usc_generation = 3;
   p->gpu_hal_generation = 300;
   p->memory_limit = STUB_ARENA_BYTES;
   p->client_memory_limit = STUB_ARENA_BYTES;
}

/* ---- commands ------------------------------------------------------------ */

static int
stub_get_params(struct agx_win_stub *s, uint8_t *frame, uint32_t fsize,
                uint32_t data_off, struct g18_asahi_get_params *a)
{
   bool none;
   int e = stub_region(fsize, data_off, a->pointer, a->size, &none);
   if (e)
      return e;
   if (a->pad)
      return G18_RN_EINVAL;

   switch (a->param_group) {
   case G18_ASAHI_PARAM_GROUP_GLOBAL: {
      struct g18_asahi_params_global p;
      stub_params(&p);
      uint64_t n = a->size < sizeof(p) ? a->size : sizeof(p);
      if (n)
         memcpy(frame + a->pointer, &p, n);
      rec_select(s);
      uint32_t h[2] = {0, (uint32_t)n};
      g18r_chunk(G18R_PARAMS, h, sizeof(h), &p, (uint32_t)n);
      return 0;
   }
   case G18_ASAHI_PARAM_GROUP_VM_STATUS: {
      struct g18_asahi_vm_status st = {0};
      uint64_t n = a->size < sizeof(st) ? a->size : sizeof(st);
      if (n < sizeof(st.vm_id))
         return G18_RN_EINVAL;
      memcpy(&st, frame + a->pointer, n);
      if (!stub_vm(s, st.vm_id))
         return G18_RN_ENOENT;
      st.flags = 0;
      st.error = 0;
      memcpy(frame + a->pointer, &st, n);
      return 0;
   }
   case G18_ASAHI_PARAM_GROUP_QUEUE_LIMITS:
      return G18_RN_EINVAL; /* INDEPENDENT_QUEUES not advertised */
   case G18_ASAHI_PARAM_GROUP_VM_STATUS_MIRROR:
      return G18_RN_EOPNOTSUPP; /* VM_STATUS_MIRROR not advertised */
   default:
      return G18_RN_EINVAL;
   }
}

static int
stub_vm_create(struct agx_win_stub *s, struct g18_asahi_vm_create *a)
{
   if (a->pad || a->kernel_end <= a->kernel_start ||
       a->kernel_end - a->kernel_start < STUB_VM_KERNEL_MIN ||
       a->kernel_start < STUB_VM_START || a->kernel_end > STUB_VM_END)
      return G18_RN_EINVAL;
   if (s->nvms == G18_RN_MAX_VMS)
      return G18_RN_ENOSPC;
   uint32_t id = ++s->nvms;
   s->vm[id] = (struct stub_vm){
      .live = true,
      .kernel_start = a->kernel_start,
      .kernel_end = a->kernel_end,
   };
   a->vm_id = id;
   rec_select(s);
   uint8_t p[24];
   uint32_t w[2] = {id, 0};
   memcpy(p, w, 8);
   memcpy(p + 8, &a->kernel_start, 8);
   memcpy(p + 16, &a->kernel_end, 8);
   g18r_chunk(G18R_VM_NEW, p, sizeof(p), NULL, 0);
   return 0;
}

static int
stub_gem_create(struct agx_win_stub *s, struct g18_asahi_gem_create *a)
{
   if (a->pad || a->size == 0 ||
       (a->flags & ~(G18_ASAHI_GEM_WRITEBACK | G18_ASAHI_GEM_VM_PRIVATE)))
      return G18_RN_EINVAL;
   if ((a->flags & G18_ASAHI_GEM_VM_PRIVATE) ? !stub_vm(s, a->vm_id)
                                             : a->vm_id != 0)
      return G18_RN_EINVAL;
   if (s->nbos == G18_RN_MAX_BOS)
      return G18_RN_ENOSPC;

   uint64_t size = (a->size + STUB_PAGE - 1) & ~(uint64_t)(STUB_PAGE - 1);
   if (size > STUB_ARENA_BYTES - stub_arena_used)
      return G18_RN_ENOMEM;

   uint8_t *raw = calloc(1, size + STUB_PAGE);
   uint32_t *pb = calloc(size / STUB_PAGE, sizeof(uint32_t));
   if (!raw || !pb) {
      free(raw);
      free(pb);
      return G18_RN_ENOMEM;
   }
   uint32_t h = ++s->nbos;
   s->bo[h] = (struct stub_bo){
      .raw = raw,
      .ptr = (uint8_t *)(((uintptr_t)raw + STUB_PAGE - 1) &
                         ~(uintptr_t)(STUB_PAGE - 1)),
      .size = size,
      .vm_id = (a->flags & G18_ASAHI_GEM_VM_PRIVATE) ? a->vm_id : 0,
      .flags = a->flags,
      .live = true,
      .page_blob = pb,
   };
   stub_arena_used += size;
   a->size = size;
   a->handle = h;

   rec_select(s);
   uint8_t p[24];
   uint32_t w[4] = {h, 0, a->flags, s->bo[h].vm_id};
   memcpy(p, w, 16);
   memcpy(p + 16, &size, 8);
   g18r_chunk(G18R_BO_NEW, p, sizeof(p), NULL, 0);
   return 0;
}

/* Remove [addr, addr+range) from the VM's binds, splitting partly covered
 * ones (G18R memory model). */
static int
stub_unmap_range(struct stub_vm *vm, uint64_t addr, uint64_t range)
{
   uint64_t end = addr + range;
   for (uint32_t i = 0; i < vm->nbinds;) {
      struct stub_bind b = vm->binds[i];
      uint64_t bend = b.addr + b.range;
      if (bend <= addr || b.addr >= end) {
         i++;
         continue;
      }
      /* drop b, re-add its uncovered left and right parts */
      vm->binds[i] = vm->binds[--vm->nbinds];
      bool single = b.flags & G18_ASAHI_BIND_SINGLE_PAGE;
      if (b.addr < addr) {
         if (vm->nbinds == STUB_MAX_BINDS)
            return G18_RN_ENOSPC;
         struct stub_bind l = b;
         l.range = addr - b.addr;
         vm->binds[vm->nbinds++] = l;
      }
      if (bend > end) {
         if (vm->nbinds == STUB_MAX_BINDS)
            return G18_RN_ENOSPC;
         struct stub_bind r = b;
         r.addr = end;
         r.range = bend - end;
         if (!single)
            r.offset += end - b.addr;
         vm->binds[vm->nbinds++] = r;
      }
   }
   return 0;
}

static int
stub_vm_bind(struct agx_win_stub *s, uint8_t *frame, uint32_t fsize,
             uint32_t data_off, struct g18_asahi_vm_bind *a)
{
   struct stub_vm *vm = stub_vm(s, a->vm_id);
   if (!vm || a->pad)
      return G18_RN_EINVAL;
   if (a->num_binds && !a->stride)
      return G18_RN_EINVAL;
   bool none;
   int e = stub_region(fsize, data_off, a->userptr,
                       (uint64_t)a->num_binds * a->stride, &none);
   if (e)
      return e;

   const uint32_t known = G18_ASAHI_BIND_UNBIND | G18_ASAHI_BIND_READ |
                          G18_ASAHI_BIND_WRITE | G18_ASAHI_BIND_SINGLE_PAGE;
   uint32_t copy = a->stride < sizeof(struct g18_asahi_gem_bind_op)
                      ? a->stride
                      : sizeof(struct g18_asahi_gem_bind_op);

   /* Validate every op, then commit (K3). */
   for (uint32_t i = 0; i < a->num_binds; i++) {
      struct g18_asahi_gem_bind_op op = {0};
      memcpy(&op, frame + a->userptr + (uint64_t)i * a->stride, copy);
      if ((op.flags & ~known) || ((op.addr | op.range | op.offset) &
                                  (STUB_PAGE - 1)) || !op.range)
         return G18_RN_EINVAL;
      if (op.addr < STUB_VM_START || op.addr + op.range > STUB_VM_END ||
          op.addr + op.range < op.addr)
         return G18_RN_EINVAL;
      if (op.addr < vm->kernel_end && op.addr + op.range > vm->kernel_start)
         return G18_RN_EINVAL; /* the kernel range is the driver's */
      if (op.flags & G18_ASAHI_BIND_UNBIND)
         continue;
      if (!(op.flags & (G18_ASAHI_BIND_READ | G18_ASAHI_BIND_WRITE)))
         return G18_RN_EINVAL;
      struct stub_bo *bo = stub_bo(s, op.handle);
      if (!bo)
         return G18_RN_ENOENT;
      if (bo->vm_id && bo->vm_id != a->vm_id)
         return G18_RN_EINVAL;
      uint64_t need = (op.flags & G18_ASAHI_BIND_SINGLE_PAGE) ? STUB_PAGE
                                                              : op.range;
      if (op.offset > bo->size || need > bo->size - op.offset)
         return G18_RN_EINVAL;
   }

   rec_select(s);
   for (uint32_t i = 0; i < a->num_binds; i++) {
      struct g18_asahi_gem_bind_op op = {0};
      memcpy(&op, frame + a->userptr + (uint64_t)i * a->stride, copy);
      if (!vm->binds) {
         vm->binds = calloc(STUB_MAX_BINDS, sizeof(*vm->binds));
         if (!vm->binds)
            return G18_RN_ENOMEM;
      }
      int err = stub_unmap_range(vm, op.addr, op.range);
      if (err)
         return err;
      if (!(op.flags & G18_ASAHI_BIND_UNBIND)) {
         if (vm->nbinds == STUB_MAX_BINDS)
            return G18_RN_ENOSPC;
         vm->binds[vm->nbinds++] = (struct stub_bind){
            .flags = op.flags,
            .handle = op.handle,
            .offset = op.offset,
            .range = op.range,
            .addr = op.addr,
         };
      }
      uint8_t p[40];
      uint32_t w[4] = {a->vm_id, op.flags, op.handle, 0};
      memcpy(p, w, 16);
      memcpy(p + 16, &op.offset, 8);
      memcpy(p + 24, &op.range, 8);
      memcpy(p + 32, &op.addr, 8);
      g18r_chunk(G18R_BIND, p, sizeof(p), NULL, 0);
   }
   return 0;
}

static int
stub_queue_create(struct agx_win_stub *s, struct g18_asahi_queue_create *a,
                  uint32_t arg_size)
{
   if (a->flags || a->pad || !stub_vm(s, a->vm_id) ||
       a->priority > G18_ASAHI_PRIORITY_REALTIME)
      return G18_RN_EINVAL;
   /* USC window: 4 GiB aligned, inside the user range (AUR command.rs). */
   if ((a->usc_exec_base & 0xffffffffull) || a->usc_exec_base < STUB_VM_START ||
       a->usc_exec_base + 0x100000000ull > STUB_VM_END)
      return G18_RN_EINVAL;
   if (a->render_scs_layout[0] || a->render_scs_layout[1])
      return G18_RN_EINVAL; /* no render in G4 */
   if (s->nqueues == G18_RN_MAX_QUEUES)
      return G18_RN_ENOSPC;
   uint32_t id = ++s->nqueues;
   s->queue[id] = (struct stub_queue){.live = true, .vm_id = a->vm_id};
   a->queue_id = id;

   rec_select(s);
   uint32_t n = arg_size < sizeof(*a) ? arg_size : sizeof(*a);
   uint32_t h[2] = {id, n};
   g18r_chunk(G18R_QUEUE_NEW, h, sizeof(h), a, n);
   return 0;
}

static int
page_cmp(const void *x, const void *y)
{
   const uint64_t *a = x, *b = y;
   if (a[0] != b[0])
      return a[0] < b[0] ? -1 : 1;
   return a[1] < b[1] ? -1 : a[1] > b[1];
}

/* PAGE/BLOB records for every page bound in the VM (G18R "snapshot at
 * SUBMIT"): each page whose content differs from the last PAGE recorded. */
static void
stub_snapshot(struct agx_win_stub *s, struct stub_vm *vm)
{
   if (!g18r_active())
      return;
   uint64_t n = 0;
   for (uint32_t i = 0; i < vm->nbinds; i++) {
      const struct stub_bind *b = &vm->binds[i];
      n += (b->flags & G18_ASAHI_BIND_SINGLE_PAGE) ? 1 : b->range / STUB_PAGE;
   }
   uint64_t (*pages)[2] = malloc((n ? n : 1) * sizeof(*pages));
   if (!pages)
      return;
   uint64_t k = 0;
   for (uint32_t i = 0; i < vm->nbinds; i++) {
      const struct stub_bind *b = &vm->binds[i];
      uint64_t cnt =
         (b->flags & G18_ASAHI_BIND_SINGLE_PAGE) ? 1 : b->range / STUB_PAGE;
      for (uint64_t p = 0; p < cnt; p++) {
         pages[k][0] = b->handle;
         pages[k][1] = b->offset + p * STUB_PAGE;
         k++;
      }
   }
   qsort(pages, k, sizeof(*pages), page_cmp);
   for (uint64_t i = 0; i < k; i++) {
      if (i && pages[i][0] == pages[i - 1][0] && pages[i][1] == pages[i - 1][1])
         continue;
      struct stub_bo *bo = &s->bo[pages[i][0]];
      if (!bo->ptr || pages[i][1] >= bo->size)
         continue;
      uint32_t pg = (uint32_t)(pages[i][1] / STUB_PAGE);
      uint32_t blob = g18r_blob(bo->ptr + pages[i][1]);
      if (blob == bo->page_blob[pg])
         continue;
      bo->page_blob[pg] = blob;
      uint8_t p[24] = {0};
      uint32_t w[2] = {(uint32_t)pages[i][0], blob};
      memcpy(p, w, 8);
      memcpy(p + 8, &pages[i][1], 8);
      g18r_chunk(G18R_PAGE_REC, p, sizeof(p), NULL, 0);
   }
   free(pages);
}

/* Admission (K5, the Linux G18 rules of K:g18/drm.rs 476-540): compute only,
 * one command, no samplers, stream <= 64 KiB, no constant scratch, no SCS
 * layout. */
static int
stub_admit(const uint8_t *cb, uint32_t size, uint32_t *ncompute)
{
   uint32_t off = 0;
   *ncompute = 0;
   while (off < size) {
      struct g18_asahi_cmd_header h;
      if (size - off < sizeof(h))
         return G18_RN_EINVAL;
      memcpy(&h, cb + off, sizeof(h));
      off += sizeof(h);
      if (h.size > size - off)
         return G18_RN_EINVAL;
      const uint8_t *pl = cb + off;
      off += h.size;

      switch (h.cmd_type) {
      case G18_ASAHI_SET_COMPUTE_ATTACHMENTS:
         if (h.size % sizeof(struct g18_asahi_attachment))
            return G18_RN_EINVAL;
         break;
      case G18_ASAHI_CMD_COMPUTE: {
         struct g18_asahi_cmd_compute_g18 c = {0};
         if (h.size < sizeof(c.base.legacy))
            return G18_RN_EINVAL;
         if (h.size > sizeof(c)) {
            for (uint32_t i = sizeof(c); i < h.size; i++)
               if (pl[i])
                  return G18_RN_EINVAL;
         }
         memcpy(&c, pl, h.size < sizeof(c) ? h.size : sizeof(c));
         const uint32_t ok = G18_ASAHI_COMPUTE_USES_TEXTURE_STATE |
                             G18_ASAHI_COMPUTE_G18_RESOURCES;
         const struct g18_asahi_cmd_compute *l = &c.base.legacy;
         if ((l->flags & ~ok) || l->sampler_count || l->sampler_heap ||
             l->cdm_ctrl_stream_end <= l->cdm_ctrl_stream_base ||
             l->cdm_ctrl_stream_end - l->cdm_ctrl_stream_base > 65536 ||
             c.base.scs_layout ||
             c.resources.constant_scratch_bytes)
            return G18_RN_EINVAL;
         if ((l->flags & G18_ASAHI_COMPUTE_G18_RESOURCES) &&
             h.size < sizeof(c))
            return G18_RN_EINVAL;
         (*ncompute)++;
         break;
      }
      default:
         return G18_RN_EINVAL; /* no render, no render resources in G4 */
      }
   }
   return *ncompute > 1 ? G18_RN_EINVAL : 0;
}

static int
stub_submit(struct agx_win_stub *s, uint8_t *frame, uint32_t fsize,
            uint32_t data_off, struct g18_asahi_submit *a)
{
   /* G18_SUBMIT_ASYNC as the kernel takes it; the stub never runs a job,
    * so every SUBMIT is complete when it returns. */
   if (a->syncs || a->in_sync_count || a->out_sync_count || a->pad ||
       (a->flags & ~G18_SUBMIT_ASYNC))
      return G18_RN_EINVAL;
   if (a->queue_id == 0 || a->queue_id > G18_RN_MAX_QUEUES ||
       !s->queue[a->queue_id].live)
      return G18_RN_ENOENT;
   bool none;
   int e = stub_region(fsize, data_off, a->cmdbuf, a->cmdbuf_size, &none);
   if (e)
      return e;
   if (none)
      return 0; /* an empty command buffer is a no-op (K5) */

   uint32_t ncompute;
   e = stub_admit(frame + a->cmdbuf, a->cmdbuf_size, &ncompute);
   if (e)
      return e;

   struct stub_vm *vm = stub_vm(s, s->queue[a->queue_id].vm_id);
   if (!vm)
      return G18_RN_ENODEV;

   rec_select(s);
   stub_snapshot(s, vm);
   /* The record keeps the kernel's flags (the asynchronous return is not
    * part of what C3 compares). */
   uint32_t h[8] = {s->submits++, a->queue_id, a->flags & ~G18_SUBMIT_ASYNC, 0, 0, 0,
                    a->cmdbuf_size, 0};
   g18r_chunk(G18R_SUBMIT, h, sizeof(h), frame + a->cmdbuf, a->cmdbuf_size);
   return 0;
}

static int
stub_gem_map(struct agx_win_stub *s, struct g18_rn_gem_map *a)
{
   struct stub_bo *bo = stub_bo(s, a->handle);
   if (!bo)
      return G18_RN_ENOENT;
   if (a->flags || a->fixed_va)
      return G18_RN_EINVAL;
   uint64_t size = a->size ? a->size : bo->size;
   if ((size & (STUB_PAGE - 1)) || size > bo->size)
      return G18_RN_EINVAL;
   if (s->nmaps == G18_RN_MAX_MAPS)
      return G18_RN_ENOSPC;
   a->va = (uint64_t)(uintptr_t)bo->ptr;
   a->size = size;
   s->map[s->nmaps++] = (struct stub_map){.va = a->va, .size = size};

   rec_select(s);
   uint8_t p[24] = {0};
   uint32_t w[2] = {a->handle, 0x3 | (1u << 8)}; /* RW, shared */
   memcpy(p, w, 8);
   memcpy(p + 8, &size, 8);
   g18r_chunk(G18R_MMAP, p, sizeof(p), NULL, 0);
   return 0;
}

static int
stub_gem_unmap(struct agx_win_stub *s, struct g18_rn_gem_unmap *a)
{
   for (uint32_t i = 0; i < s->nmaps; i++) {
      if (s->map[i].va != a->va)
         continue;
      if (a->size && a->size != s->map[i].size)
         return G18_RN_EINVAL;
      s->map[i] = s->map[--s->nmaps];
      return 0;
   }
   return G18_RN_EINVAL;
}

/* Kernel-side size of each command's argument. */
static uint32_t
stub_arg_size(uint32_t cmd)
{
   switch (cmd) {
   case G18_RN_GET_PARAMS: return sizeof(struct g18_asahi_get_params);
   case G18_RN_GET_TIME: return sizeof(struct g18_asahi_get_time);
   case G18_RN_VM_CREATE: return sizeof(struct g18_asahi_vm_create);
   case G18_RN_VM_DESTROY: return sizeof(struct g18_asahi_vm_destroy);
   case G18_RN_VM_BIND: return sizeof(struct g18_asahi_vm_bind);
   case G18_RN_GEM_CREATE: return sizeof(struct g18_asahi_gem_create);
   case G18_RN_GEM_MMAP_OFFSET: return sizeof(struct g18_asahi_gem_mmap_offset);
   case G18_RN_GEM_BIND_OBJECT: return sizeof(struct g18_asahi_gem_bind_object);
   case G18_RN_QUEUE_CREATE: return sizeof(struct g18_asahi_queue_create);
   case G18_RN_QUEUE_DESTROY: return sizeof(struct g18_asahi_queue_destroy);
   case G18_RN_SUBMIT: return sizeof(struct g18_asahi_submit);
   case G18_RN_GEM_MADVISE: return sizeof(struct g18_asahi_gem_madvise);
   case G18_RN_GEM_USERPTR: return sizeof(struct g18_asahi_gem_userptr);
   case G18_RN_GEM_RESIDENCY: return sizeof(struct g18_asahi_gem_residency);
   case G18_RN_VERSION: return sizeof(struct g18_rn_version);
   case G18_RN_GEM_CLOSE: return sizeof(struct g18_rn_gem_close);
   case G18_RN_GEM_MAP: return sizeof(struct g18_rn_gem_map);
   case G18_RN_GEM_UNMAP: return sizeof(struct g18_rn_gem_unmap);
   default: return 0;
   }
}

static int
stub_dispatch(struct agx_win_stub *s, uint32_t cmd, uint8_t *frame,
              uint32_t fsize, uint32_t data_off, void *arg, uint32_t arg_size)
{
   switch (cmd) {
   case G18_RN_GET_PARAMS:
      return stub_get_params(s, frame, fsize, data_off, arg);

   case G18_RN_GET_TIME: {
      struct g18_asahi_get_time *a = arg;
      if (a->flags)
         return G18_RN_EINVAL;
      struct timespec ts;
      timespec_get(&ts, TIME_UTC);
      a->gpu_timestamp = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
      return 0;
   }

   case G18_RN_VM_CREATE:
      return stub_vm_create(s, arg);

   case G18_RN_VM_DESTROY: {
      struct g18_asahi_vm_destroy *a = arg;
      struct stub_vm *vm = stub_vm(s, a->vm_id);
      if (!vm || a->pad)
         return G18_RN_EINVAL;
      vm->live = false; /* tables stay allocated (G4-PLAN 3.2) */
      rec_select(s);
      uint32_t p[2] = {a->vm_id, 0};
      g18r_chunk(G18R_VM_FREE, p, sizeof(p), NULL, 0);
      return 0;
   }

   case G18_RN_VM_BIND:
      return stub_vm_bind(s, frame, fsize, data_off, arg);

   case G18_RN_GEM_CREATE:
      return stub_gem_create(s, arg);

   case G18_RN_QUEUE_CREATE:
      return stub_queue_create(s, arg, arg_size);

   case G18_RN_QUEUE_DESTROY: {
      struct g18_asahi_queue_destroy *a = arg;
      if (a->queue_id == 0 || a->queue_id > G18_RN_MAX_QUEUES ||
          !s->queue[a->queue_id].live || a->pad)
         return G18_RN_ENOENT;
      s->queue[a->queue_id].live = false;
      rec_select(s);
      uint32_t p[2] = {a->queue_id, 0};
      g18r_chunk(G18R_QUEUE_FREE, p, sizeof(p), NULL, 0);
      return 0;
   }

   case G18_RN_SUBMIT:
      return stub_submit(s, frame, fsize, data_off, arg);

   case G18_RN_GEM_MADVISE: {
      struct g18_asahi_gem_madvise *a = arg;
      if (!stub_bo(s, a->handle))
         return G18_RN_ENOENT;
      if (a->madv > G18_ASAHI_GEM_MADV_DONTNEED || a->pad)
         return G18_RN_EINVAL;
      a->retained = 1;
      return 0;
   }

   case G18_RN_GEM_MMAP_OFFSET:
   case G18_RN_GEM_BIND_OBJECT:
   case G18_RN_GEM_USERPTR:
   case G18_RN_GEM_RESIDENCY:
      return G18_RN_EOPNOTSUPP;

   case G18_RN_VERSION: {
      struct g18_rn_version *a = arg;
      memset(a, 0, sizeof(*a));
      a->abi = G18_RN_ABI;
      a->major = G18_RN_DRIVER_MAJOR;
      a->minor = G18_RN_DRIVER_MINOR;
      a->patchlevel = G18_RN_DRIVER_PATCH;
      a->state = G18_STATE_LIVE;
      a->flags = G18_INFO_LIVE;
      strcpy(a->name, G18_RN_DRIVER_NAME);
      strcpy(a->desc, "agx-win-stub: in-process, no GPU");
      return 0;
   }

   case G18_RN_GEM_CLOSE: {
      struct g18_rn_gem_close *a = arg;
      struct stub_bo *bo = stub_bo(s, a->handle);
      if (!bo || a->pad)
         return bo ? G18_RN_EINVAL : G18_RN_ENOENT;
      bo->live = false; /* pages, binds and maps stay (G4-PLAN 3.2) */
      rec_select(s);
      uint32_t p[2] = {a->handle, 0};
      g18r_chunk(G18R_BO_FREE, p, sizeof(p), NULL, 0);
      return 0;
   }

   case G18_RN_GEM_MAP:
      return stub_gem_map(s, arg);

   case G18_RN_GEM_UNMAP:
      return stub_gem_unmap(s, arg);

   default:
      return G18_RN_ENOTTY;
   }
}

int
agx_win_stub_ioctl(struct agx_win_stub *s, uint32_t code, void *frame_,
                   uint32_t size)
{
   uint8_t *frame = frame_;
   struct g18_rn_hdr hdr;

   if (!G18_IOCTL_IS_RN(code))
      return -ENOTTY;
   const uint32_t cmd = ((code >> 2) & 0xfffu) - G18_RN_FN_BASE;
   const uint32_t ksize = stub_arg_size(cmd);
   if (!ksize)
      return -ENOTTY;

   if (!frame || size < sizeof(hdr))
      return -EINVAL;
   memcpy(&hdr, frame, sizeof(hdr));
   if (hdr.abi != G18_RN_ABI || hdr.cmd != cmd || hdr.flags || hdr.reserved ||
       hdr.frame_size > size || hdr.frame_size > G18_RN_FRAME_MAX ||
       hdr.arg_size > G18_RN_ARG_MAX ||
       hdr.frame_size < G18_RN_ARG_OFFSET + hdr.arg_size)
      return -EINVAL;

   const uint32_t data_off = G18_RN_DATA_OFFSET(hdr.arg_size);
   const uint32_t ncopy = hdr.arg_size < ksize ? hdr.arg_size : ksize;
   uint64_t argbuf[G18_RN_ARG_MAX / 8] = {0};
   memcpy(argbuf, frame + G18_RN_ARG_OFFSET, ncopy);
   uint8_t in[G18_RN_ARG_MAX];
   memcpy(in, argbuf, ncopy);

   stub_lock_acquire();
   int32_t result = stub_dispatch(s, cmd, frame, hdr.frame_size, data_off,
                                  argbuf, hdr.arg_size);

   /* Calls without a typed record (and failed typed ones) as RAW_IOCTL. */
   bool typed = cmd == G18_RN_VM_CREATE || cmd == G18_RN_VM_DESTROY ||
                cmd == G18_RN_VM_BIND || cmd == G18_RN_GEM_CREATE ||
                cmd == G18_RN_QUEUE_CREATE || cmd == G18_RN_QUEUE_DESTROY ||
                cmd == G18_RN_SUBMIT || cmd == G18_RN_GEM_CLOSE ||
                cmd == G18_RN_GEM_MAP ||
                (cmd == G18_RN_GET_PARAMS &&
                 ((struct g18_asahi_get_params *)in)->param_group == 0);
   if (!typed || result) {
      rec_select(s);
      rec_raw(code, result ? -1 : 0, (uint32_t)result, result ? 2 : 0, in,
              ncopy, argbuf, result ? 0 : ncopy);
   }
   stub_lock_release();

   memcpy(frame + G18_RN_ARG_OFFSET, argbuf, ncopy);
   hdr.result = result;
   hdr.out_size = ncopy;
   memcpy(frame, &hdr, sizeof(hdr));
   return 0;
}
