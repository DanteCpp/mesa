/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifdef ENABLE_SHADER_CACHE

#include "util/detect_os.h"

#if DETECT_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "util/crc32.h"
#include "util/mesa-blake3.h"
#include "util/ralloc.h"
#include "util/u_debug.h"
#include "util/u_queue.h"
#include "disk_cache.h"

#define WIN_CACHE_MAGIC   0x31435341u /* "ASC1" */
#define WIN_CACHE_VERSION 1u
#define WIN_CACHE_MAX_ENTRY_BYTES (16u * 1024u * 1024u)

struct win_cache_header {
   uint32_t magic;
   uint32_t version;
   uint32_t driver_blob_size;
   uint32_t payload_size;
   uint32_t crc32;
   uint32_t reserved;
};

struct disk_cache {
   WCHAR root_dir[MAX_PATH];
   bool has_disk;
   SRWLOCK queue_lock;
   struct util_queue cache_queue;
   bool queue_init;
   uint8_t *driver_keys_blob;
   size_t driver_keys_blob_size;
   disk_cache_put_cb blob_put_cb;
   disk_cache_get_cb blob_get_cb;
};

struct win_put_job {
   struct disk_cache *cache;
   cache_key key;
   void *data;
   size_t size;
   bool free_data;
};

static bool
win_cache_disabled(void)
{
   const char *v = getenv("MESA_SHADER_CACHE_DISABLE");
   if (v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "TRUE") == 0))
      return true;
   v = getenv("MESA_GLSL_CACHE_DISABLE");
   if (v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "TRUE") == 0))
      return true;
   return false;
}

static bool
win_mkdir_recursive(const WCHAR *dir)
{
   WCHAR tmp[MAX_PATH];
   size_t len = wcslen(dir);
   if (len == 0 || len >= MAX_PATH)
      return false;
   memcpy(tmp, dir, (len + 1) * sizeof(WCHAR));
   for (size_t i = 0; i < len; ++i) {
      if (tmp[i] == L'/')
         tmp[i] = L'\\';
   }
   for (size_t i = 3; i < len; ++i) {
      if (tmp[i] == L'\\') {
         tmp[i] = L'\0';
         CreateDirectoryW(tmp, NULL);
         tmp[i] = L'\\';
      }
   }
   if (CreateDirectoryW(tmp, NULL))
      return true;
   DWORD err = GetLastError();
   if (err == ERROR_ALREADY_EXISTS) {
      DWORD attr = GetFileAttributesW(tmp);
      return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
   }
   return false;
}

static bool
win_dir_writable(const WCHAR *dir)
{
   if (!win_mkdir_recursive(dir))
      return false;
   WCHAR probe[MAX_PATH];
   int n = _snwprintf(probe, MAX_PATH, L"%ls\\.probe_%lu_%lu",
                      dir, (unsigned long)GetCurrentProcessId(),
                      (unsigned long)GetCurrentThreadId());
   if (n <= 0 || n >= MAX_PATH)
      return false;
   HANDLE h = CreateFileW(probe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return false;
   CloseHandle(h);
   return true;
}

static bool
win_pick_cache_dir(WCHAR out[MAX_PATH], const char *custom_name)
{
   const WCHAR *leaf = L"AuroraShaderCache";
   WCHAR custom_w[96];
   if (custom_name && custom_name[0]) {
      int cn = MultiByteToWideChar(CP_UTF8, 0, custom_name, -1, custom_w, 96);
      if (cn > 1)
         leaf = custom_w;
   }

   WCHAR env_dir[MAX_PATH];
   DWORD n = GetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", env_dir, MAX_PATH);
   if (n > 0 && n < MAX_PATH - 40) {
      if (win_dir_writable(env_dir)) {
         wcscpy(out, env_dir);
         return true;
      }
   }

   /* Prefer %LOCALAPPDATA% for normal user apps; fall back to
    * C:\ProgramData\AuroraSilicon\ShaderCache for system/DWM-1 accounts. */
   n = GetEnvironmentVariableW(L"LOCALAPPDATA", env_dir, MAX_PATH);
   if (n > 0 && n < MAX_PATH - 64) {
      WCHAR cand[MAX_PATH];
      int wn = _snwprintf(cand, MAX_PATH, L"%ls\\%ls", env_dir, leaf);
      if (wn > 0 && wn < MAX_PATH - 80 && win_dir_writable(cand)) {
         wcscpy(out, cand);
         return true;
      }
   }

   WCHAR progdata[MAX_PATH];
   n = GetEnvironmentVariableW(L"ProgramData", progdata, MAX_PATH);
   if (n > 0 && n < MAX_PATH - 80) {
      WCHAR cand[MAX_PATH];
      int wn = _snwprintf(cand, MAX_PATH, L"%ls\\AuroraSilicon\\%ls", progdata, leaf);
      if (wn > 0 && wn < MAX_PATH - 80 && win_dir_writable(cand)) {
         wcscpy(out, cand);
         return true;
      }
   }

   if (win_dir_writable(L"C:\\ProgramData\\AuroraSilicon\\ShaderCache")) {
      wcscpy(out, L"C:\\ProgramData\\AuroraSilicon\\ShaderCache");
      return true;
   }
   return false;
}

static bool
win_key_path(const struct disk_cache *cache, const cache_key key,
             WCHAR bucket_out[MAX_PATH], WCHAR file_out[MAX_PATH])
{
   static const WCHAR hex[16] = L"0123456789abcdef";
   WCHAR khex[CACHE_KEY_SIZE * 2 + 1];
   for (size_t i = 0; i < CACHE_KEY_SIZE; ++i) {
      khex[i * 2]     = hex[(key[i] >> 4) & 0xf];
      khex[i * 2 + 1] = hex[key[i] & 0xf];
   }
   khex[CACHE_KEY_SIZE * 2] = L'\0';

   if (bucket_out) {
      int bn = _snwprintf(bucket_out, MAX_PATH, L"%ls\\%lc%lc",
                          cache->root_dir, khex[0], khex[1]);
      if (bn <= 0 || bn >= MAX_PATH)
         return false;
   }
   int fn = _snwprintf(file_out, MAX_PATH, L"%ls\\%lc%lc\\%ls.bin",
                       cache->root_dir, khex[0], khex[1], khex + 2);
   return fn > 0 && fn < MAX_PATH - 32;
}

static bool
win_write_all(HANDLE h, const void *buf, size_t size)
{
   const uint8_t *p = (const uint8_t *)buf;
   while (size > 0) {
      DWORD chunk = (size > 0x40000000u) ? 0x40000000u : (DWORD)size;
      DWORD written = 0;
      if (!WriteFile(h, p, chunk, &written, NULL) || written == 0)
         return false;
      p += written;
      size -= written;
   }
   return true;
}

static bool
win_read_all(HANDLE h, void *buf, size_t size)
{
   uint8_t *p = (uint8_t *)buf;
   while (size > 0) {
      DWORD chunk = (size > 0x40000000u) ? 0x40000000u : (DWORD)size;
      DWORD got = 0;
      if (!ReadFile(h, p, chunk, &got, NULL) || got == 0)
         return false;
      p += got;
      size -= got;
   }
   return true;
}

static void
win_cache_put_exec(void *job, void *gdata, int thread_index)
{
   (void)gdata;
   (void)thread_index;
   struct win_put_job *j = (struct win_put_job *)job;
   struct disk_cache *cache = j->cache;

   if (cache->blob_put_cb) {
      cache->blob_put_cb(j->key, CACHE_KEY_SIZE, j->data, (signed long)j->size);
      return;
   }
   if (!cache->has_disk || j->size == 0 || j->size > WIN_CACHE_MAX_ENTRY_BYTES)
      return;

   WCHAR bucket[MAX_PATH], dst_path[MAX_PATH], tmp_path[MAX_PATH];
   if (!win_key_path(cache, j->key, bucket, dst_path))
      return;
   CreateDirectoryW(bucket, NULL);

   int tn = _snwprintf(tmp_path, MAX_PATH, L"%ls.%lu.%lu.tmp",
                       dst_path, (unsigned long)GetCurrentProcessId(),
                       (unsigned long)GetCurrentThreadId());
   if (tn <= 0 || tn >= MAX_PATH)
      return;

   struct win_cache_header hdr;
   hdr.magic = WIN_CACHE_MAGIC;
   hdr.version = WIN_CACHE_VERSION;
   hdr.driver_blob_size = (uint32_t)cache->driver_keys_blob_size;
   hdr.payload_size = (uint32_t)j->size;
   hdr.crc32 = util_hash_crc32(j->data, j->size);
   hdr.reserved = 0u;

   HANDLE h = CreateFileW(tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return;

   bool ok = win_write_all(h, &hdr, sizeof(hdr)) &&
             (cache->driver_keys_blob_size == 0 ||
              win_write_all(h, cache->driver_keys_blob, cache->driver_keys_blob_size)) &&
             win_write_all(h, j->data, j->size);
   CloseHandle(h);

   if (ok) {
      if (!MoveFileExW(tmp_path, dst_path, MOVEFILE_REPLACE_EXISTING))
         DeleteFileW(tmp_path);
   } else {
      DeleteFileW(tmp_path);
   }
}

static void
win_cache_put_cleanup(void *job, void *gdata, int thread_index)
{
   (void)gdata;
   (void)thread_index;
   struct win_put_job *j = (struct win_put_job *)job;
   if (j->free_data)
      free(j->data);
   free(j);
}

static bool
win_cache_ensure_queue(struct disk_cache *cache)
{
   if (cache->queue_init)
      return true;
   AcquireSRWLockExclusive(&cache->queue_lock);
   if (!cache->queue_init) {
      cache->queue_init = util_queue_init(&cache->cache_queue, "disk$", 64, 2,
                                          UTIL_QUEUE_INIT_RESIZE_IF_FULL |
                                          UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY,
                                          NULL);
   }
   bool ok = cache->queue_init;
   ReleaseSRWLockExclusive(&cache->queue_lock);
   return ok;
}

struct disk_cache *
disk_cache_create_custom(const char *gpu_name, const char *driver_id,
                         uint64_t driver_flags, const char *cache_dir_name,
                         uint32_t max_size)
{
   (void)max_size;
   if (win_cache_disabled())
      return NULL;

   struct disk_cache *cache = rzalloc(NULL, struct disk_cache);
   if (!cache)
      return NULL;
   InitializeSRWLock(&cache->queue_lock);

   const char *gpu = gpu_name ? gpu_name : "";
   const char *drv = driver_id ? driver_id : "";
   size_t gpu_sz = strlen(gpu) + 1;
   size_t drv_sz = strlen(drv) + 1;
   uint8_t ver = 1;
   uint8_t ptr_sz = (uint8_t)sizeof(void *);
   size_t blob_sz = sizeof(ver) + sizeof(ptr_sz) + gpu_sz + drv_sz + sizeof(driver_flags);
   cache->driver_keys_blob = ralloc_size(cache, blob_sz);
   if (!cache->driver_keys_blob) {
      ralloc_free(cache);
      return NULL;
   }
   uint8_t *p = cache->driver_keys_blob;
   *p++ = ver;
   *p++ = ptr_sz;
   memcpy(p, gpu, gpu_sz); p += gpu_sz;
   memcpy(p, drv, drv_sz); p += drv_sz;
   memcpy(p, &driver_flags, sizeof(driver_flags));
   cache->driver_keys_blob_size = blob_sz;

   cache->has_disk = win_pick_cache_dir(cache->root_dir, cache_dir_name);
   /* Defer cache_queue thread creation until the first disk_cache_put() so
    * read-only tools (e.g. vulkaninfo / furmark --vkinfo) spawn zero extra
    * threads that could race DLL_PROCESS_DETACH on abrupt process exit. */
   return cache;
}

struct disk_cache *
disk_cache_create(const char *gpu_name, const char *driver_id,
                  uint64_t driver_flags)
{
   return disk_cache_create_custom(gpu_name, driver_id, driver_flags, NULL, 0);
}

void
disk_cache_destroy(struct disk_cache *cache)
{
   if (!cache)
      return;
   if (cache->queue_init) {
      util_queue_finish(&cache->cache_queue);
      util_queue_destroy(&cache->cache_queue);
      cache->queue_init = false;
   }
   ralloc_free(cache);
}

void
disk_cache_wait_for_idle(struct disk_cache *cache)
{
   if (cache && cache->queue_init)
      util_queue_finish(&cache->cache_queue);
}

void
disk_cache_remove(struct disk_cache *cache, const cache_key key)
{
   if (!cache || !cache->has_disk)
      return;
   WCHAR dst_path[MAX_PATH];
   if (win_key_path(cache, key, NULL, dst_path))
      DeleteFileW(dst_path);
}

static void
win_cache_put_common(struct disk_cache *cache, const cache_key key,
                     void *data, size_t size, bool take_ownership)
{
   if (!cache || !data || size == 0 || (!cache->has_disk && !cache->blob_put_cb)) {
      if (take_ownership)
         free(data);
      return;
   }
   if (!win_cache_ensure_queue(cache)) {
      if (take_ownership)
         free(data);
      return;
   }
   struct win_put_job *j = (struct win_put_job *)malloc(
      sizeof(*j) + (take_ownership ? 0 : size));
   if (!j) {
      if (take_ownership)
         free(data);
      return;
   }
   j->cache = cache;
   memcpy(j->key, key, CACHE_KEY_SIZE);
   j->size = size;
   if (take_ownership) {
      j->data = data;
      j->free_data = true;
   } else {
      j->data = (void *)(j + 1);
      memcpy(j->data, data, size);
      j->free_data = false;
   }
   util_queue_add_job(&cache->cache_queue, j, NULL,
                      win_cache_put_exec, win_cache_put_cleanup, size);
}

void
disk_cache_put(struct disk_cache *cache, const cache_key key,
               const void *data, size_t size,
               struct cache_item_metadata *cache_item_metadata)
{
   (void)cache_item_metadata;
   win_cache_put_common(cache, key, (void *)data, size, false);
}

void
disk_cache_put_nocopy(struct disk_cache *cache, const cache_key key,
                      void *data, size_t size,
                      struct cache_item_metadata *cache_item_metadata)
{
   (void)cache_item_metadata;
   win_cache_put_common(cache, key, data, size, true);
}

void *
disk_cache_get(struct disk_cache *cache, const cache_key key, size_t *size)
{
   if (size)
      *size = 0;
   if (!cache)
      return NULL;

   if (cache->blob_get_cb) {
      signed long sz = cache->blob_get_cb(key, CACHE_KEY_SIZE, NULL, 0);
      if (sz <= 0 || (size_t)sz > WIN_CACHE_MAX_ENTRY_BYTES)
         return NULL;
      void *buf = malloc((size_t)sz);
      if (!buf)
         return NULL;
      if (cache->blob_get_cb(key, CACHE_KEY_SIZE, buf, sz) != sz) {
         free(buf);
         return NULL;
      }
      if (size)
         *size = (size_t)sz;
      return buf;
   }

   if (!cache->has_disk)
      return NULL;

   WCHAR dst_path[MAX_PATH];
   if (!win_key_path(cache, key, NULL, dst_path))
      return NULL;

   HANDLE h = CreateFileW(dst_path, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return NULL;

   struct win_cache_header hdr;
   if (!win_read_all(h, &hdr, sizeof(hdr)) ||
       hdr.magic != WIN_CACHE_MAGIC ||
       hdr.version != WIN_CACHE_VERSION ||
       hdr.driver_blob_size != cache->driver_keys_blob_size ||
       hdr.payload_size == 0 ||
       hdr.payload_size > WIN_CACHE_MAX_ENTRY_BYTES) {
      CloseHandle(h);
      return NULL;
   }

   if (hdr.driver_blob_size > 0) {
      uint8_t stack_blob[256];
      uint8_t *blob_buf = (hdr.driver_blob_size <= sizeof(stack_blob))
                             ? stack_blob
                             : (uint8_t *)malloc(hdr.driver_blob_size);
      if (!blob_buf) {
         CloseHandle(h);
         return NULL;
      }
      bool ok = win_read_all(h, blob_buf, hdr.driver_blob_size) &&
                memcmp(blob_buf, cache->driver_keys_blob, hdr.driver_blob_size) == 0;
      if (blob_buf != stack_blob)
         free(blob_buf);
      if (!ok) {
         CloseHandle(h);
         return NULL;
      }
   }

   void *payload = malloc(hdr.payload_size);
   if (!payload) {
      CloseHandle(h);
      return NULL;
   }
   bool ok = win_read_all(h, payload, hdr.payload_size);
   CloseHandle(h);
   if (!ok || util_hash_crc32(payload, hdr.payload_size) != hdr.crc32) {
      free(payload);
      return NULL;
   }

   if (size)
      *size = hdr.payload_size;
   return payload;
}

void
disk_cache_put_key(struct disk_cache *cache, const cache_key key)
{
   static const uint8_t marker = 1u;
   disk_cache_put(cache, key, &marker, sizeof(marker), NULL);
}

bool
disk_cache_has_key(struct disk_cache *cache, const cache_key key)
{
   if (!cache || !cache->has_disk)
      return false;
   WCHAR dst_path[MAX_PATH];
   if (!win_key_path(cache, key, NULL, dst_path))
      return false;
   return GetFileAttributesW(dst_path) != INVALID_FILE_ATTRIBUTES;
}

void
disk_cache_compute_key(struct disk_cache *cache, const void *data, size_t size,
                       cache_key key)
{
   blake3_hasher ctx;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, cache->driver_keys_blob,
                       cache->driver_keys_blob_size);
   _mesa_blake3_update(&ctx, data, size);
   _mesa_blake3_final(&ctx, key);
}

void
disk_cache_set_callbacks(struct disk_cache *cache, disk_cache_put_cb put,
                         disk_cache_get_cb get)
{
   if (!cache)
      return;
   cache->blob_put_cb = put;
   cache->blob_get_cb = get;
   win_cache_ensure_queue(cache);
}

#else /* !DETECT_OS_WINDOWS */

#include <ctype.h>
#include <ftw.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>

#include "util/compress.h"
#include "util/crc32.h"
#include "util/u_debug.h"
#include "util/rand_xor.h"
#include "util/u_atomic.h"
#include "util/mesa-blake3.h"
#include "util/perf/cpu_trace.h"
#include "util/ralloc.h"
#include "util/compiler.h"
#include "util/log.h"

#include "disk_cache.h"
#include "disk_cache_os.h"

/* The cache version should be bumped whenever a change is made to the
 * structure of cache entries or the index. This will give any 3rd party
 * applications reading the cache entries a chance to adjust to the changes.
 *
 * - The cache version is checked internally when reading a cache entry. If we
 *   ever have a mismatch we are in big trouble as this means we had a cache
 *   collision. In case of such an event please check the skys for giant
 *   asteroids and that the entire Mesa team hasn't been eaten by wolves.
 *
 * - There is no strict requirement that cache versions be backwards
 *   compatible but effort should be taken to limit disruption where possible.
 */
#define CACHE_VERSION 1

#define DRV_KEY_CPY(_dst, _src, _src_size) \
do {                                       \
   memcpy(_dst, _src, _src_size);          \
   _dst += _src_size;                      \
} while (0);

static bool
disk_cache_init_queue(struct disk_cache *cache)
{
   if (util_queue_is_initialized(&cache->cache_queue))
      return true;

   /* 4 threads were chosen below because just about all modern CPUs currently
    * available that run Mesa have *at least* 4 cores. For these CPUs allowing
    * more threads can result in the queue being processed faster, thus
    * avoiding excessive memory use due to a backlog of cache entrys building
    * up in the queue. Since we set the UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY
    * flag this should have little negative impact on low core systems.
    *
    * The queue will resize automatically when it's full, so adding new jobs
    * doesn't stall.
    */
   return util_queue_init(&cache->cache_queue, "disk$", 32, 4,
                          UTIL_QUEUE_INIT_RESIZE_IF_FULL |
                          UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY |
                          UTIL_QUEUE_INIT_SET_FULL_THREAD_AFFINITY, NULL);
}

static struct disk_cache *
disk_cache_type_create(const char *gpu_name,
                       const char *driver_id,
                       const char *cache_dir_name,
                       uint64_t driver_flags,
                       enum disk_cache_type cache_type,
                       uint64_t max_size)
{
   void *local;
   struct disk_cache *cache = NULL;

   uint8_t cache_version = CACHE_VERSION;
   size_t cv_size = sizeof(cache_version);

   /* A ralloc context for transient data during this invocation. */
   local = ralloc_context(NULL);
   if (local == NULL)
      goto fail;

   cache = rzalloc(NULL, struct disk_cache);
   if (cache == NULL)
      goto fail;

   /* Assume failure. */
   cache->path_init_failed = true;
   cache->type = DISK_CACHE_NONE;

   if (!disk_cache_enabled())
      goto path_fail;

   const char *path =
      disk_cache_generate_cache_dir(local, gpu_name, driver_id, cache_dir_name, cache_type, true);
   if (!path)
      goto path_fail;

   cache->path = ralloc_strdup(cache, path);
   if (cache->path == NULL)
      goto path_fail;

   /* Cache tests that want to have a disabled cache compression are using
    * the "make_check_uncompressed" for the driver_id name.  Hence here we
    * disable disk cache compression when mesa's build tests require it.
    */
   if (strcmp(driver_id, "make_check_uncompressed") == 0)
      cache->compression_disabled = true;

   if (cache_type == DISK_CACHE_SINGLE_FILE) {
      if (!disk_cache_load_cache_index_foz(local, cache))
         goto path_fail;
   } else if (cache_type == DISK_CACHE_DATABASE) {
      if (!disk_cache_db_load_cache_index(local, cache))
         goto path_fail;
   }

   if (!os_get_option("MESA_SHADER_CACHE_DIR") && !os_get_option("MESA_GLSL_CACHE_DIR"))
      disk_cache_touch_cache_user_marker(cache->path);

   cache->type = cache_type;

   cache->stats.enabled = debug_get_bool_option("MESA_SHADER_CACHE_SHOW_STATS",
                                                false);

   if (!disk_cache_mmap_cache_index(local, cache))
      goto path_fail;

   cache->max_size = max_size;

   if (cache->type == DISK_CACHE_DATABASE)
      mesa_cache_db_multipart_set_size_limit(&cache->cache_db, cache->max_size);

   if (!disk_cache_init_queue(cache))
      goto fail;

   cache->path_init_failed = false;

 path_fail:

   cache->driver_keys_blob_size = cv_size;

   /* Create driver id keys */
   size_t id_size = strlen(driver_id) + 1;
   size_t gpu_name_size = strlen(gpu_name) + 1;
   cache->driver_keys_blob_size += id_size;
   cache->driver_keys_blob_size += gpu_name_size;

   /* We sometimes store entire structs that contains a pointers in the cache,
    * use pointer size as a key to avoid hard to debug issues.
    */
   uint8_t ptr_size = sizeof(void *);
   size_t ptr_size_size = sizeof(ptr_size);
   cache->driver_keys_blob_size += ptr_size_size;

   size_t driver_flags_size = sizeof(driver_flags);
   cache->driver_keys_blob_size += driver_flags_size;

   cache->driver_keys_blob =
      ralloc_size(cache, cache->driver_keys_blob_size);
   if (!cache->driver_keys_blob)
      goto fail;

   uint8_t *drv_key_blob = cache->driver_keys_blob;
   DRV_KEY_CPY(drv_key_blob, &cache_version, cv_size)
   DRV_KEY_CPY(drv_key_blob, driver_id, id_size)
   DRV_KEY_CPY(drv_key_blob, gpu_name, gpu_name_size)
   DRV_KEY_CPY(drv_key_blob, &ptr_size, ptr_size_size)
   DRV_KEY_CPY(drv_key_blob, &driver_flags, driver_flags_size)

   /* Seed our rand function */
   s_rand_xorshift128plus(cache->seed_xorshift128plus, true);

   ralloc_free(local);

   return cache;

 fail:
   ralloc_free(cache);
   ralloc_free(local);

   return NULL;
}

struct disk_cache *
disk_cache_create(const char *gpu_name, const char *driver_id,
                  uint64_t driver_flags)
{
   enum disk_cache_type cache_type;
   struct disk_cache *cache;
   uint64_t max_size = 0;
   const char *max_size_str;

   if (debug_get_bool_option("MESA_DISK_CACHE_SINGLE_FILE", false)) {
      cache_type = DISK_CACHE_SINGLE_FILE;
   } else if (debug_get_bool_option("MESA_DISK_CACHE_DATABASE", false)) {
      cache_type = DISK_CACHE_DATABASE;
      /* Since switching the default cache to <mesa_shader_cache_db>, remove the
       * old cache folder if it hasn't been modified for more than 7 days.
       */
      if (!os_get_option("MESA_SHADER_CACHE_DIR") && !os_get_option("MESA_GLSL_CACHE_DIR") &&
          disk_cache_enabled())
         disk_cache_delete_old_cache();
   } else if (debug_get_bool_option("MESA_DISK_CACHE_MULTI_FILE", true)) {
      cache_type = DISK_CACHE_MULTI_FILE;
   } else {
      return NULL;
   }

   max_size_str = os_get_option("MESA_SHADER_CACHE_MAX_SIZE");

   if (!max_size_str) {
      max_size_str = os_get_option("MESA_GLSL_CACHE_MAX_SIZE");
      if (max_size_str)
         fprintf(stderr,
                 "*** MESA_GLSL_CACHE_MAX_SIZE is deprecated; "
                 "use MESA_SHADER_CACHE_MAX_SIZE instead ***\n");
   }

#ifdef MESA_SHADER_CACHE_MAX_SIZE
   if (!max_size_str) {
      max_size_str = MESA_SHADER_CACHE_MAX_SIZE;
   }
#endif

   if (max_size_str) {
      char *end;
      max_size = strtoul(max_size_str, &end, 10);
      if (end == max_size_str) {
         max_size = 0;
      } else {
         switch (*end) {
         case 'K':
         case 'k':
            max_size *= 1024;
            break;
         case 'M':
         case 'm':
            max_size *= 1024*1024;
            break;
         case '\0':
         case 'G':
         case 'g':
         default:
            max_size *= 1024*1024*1024;
            break;
         }
      }
   }

   /* Default to 1GB for maximum cache size. */
   if (max_size == 0) {
      max_size = 1024*1024*1024;
   }

   /* Create main writable cache. */
   cache = disk_cache_type_create(gpu_name, driver_id, NULL, driver_flags,
                                  cache_type, max_size);
   if (!cache)
      return NULL;

   /* If MESA_DISK_CACHE_SINGLE_FILE is unset and MESA_DISK_CACHE_COMBINE_RW_WITH_RO_FOZ
    * is set, then enable additional Fossilize RO caches together with the RW
    * cache.  At first we will check cache entry presence in the RO caches and
    * if entry isn't found there, then we'll fall back to the RW cache.
    */
   if (cache_type != DISK_CACHE_SINGLE_FILE && !cache->path_init_failed &&
       debug_get_bool_option("MESA_DISK_CACHE_COMBINE_RW_WITH_RO_FOZ", false)) {

      /* Create read-only cache used for sharing prebuilt shaders.
       * If cache entry will be found in this cache, then the main cache
       * will be bypassed.
       */
      cache->foz_ro_cache = disk_cache_type_create(gpu_name, driver_id, NULL,
                                                   driver_flags,
                                                   DISK_CACHE_SINGLE_FILE,
                                                   max_size);
   }

   return cache;
}

struct disk_cache *
disk_cache_create_custom(const char *gpu_name, const char *driver_id,
                         uint64_t driver_flags, const char *cache_dir_name,
                         uint32_t max_size)
{
   return disk_cache_type_create(gpu_name, driver_id, cache_dir_name, 0,
                                 DISK_CACHE_DATABASE, max_size);
}

void
disk_cache_destroy(struct disk_cache *cache)
{
   if (unlikely(cache && cache->stats.enabled)) {
      mesa_logi("disk shader cache:  hits = %u, misses = %u\n",
                cache->stats.hits,
                cache->stats.misses);
   }

   if (cache && util_queue_is_initialized(&cache->cache_queue)) {
      util_queue_finish(&cache->cache_queue);
      util_queue_destroy(&cache->cache_queue);

      if (cache->foz_ro_cache)
         disk_cache_destroy(cache->foz_ro_cache);

      if (cache->type == DISK_CACHE_SINGLE_FILE)
         foz_destroy(&cache->foz_db);

      if (cache->type == DISK_CACHE_DATABASE)
         mesa_cache_db_multipart_close(&cache->cache_db);

      disk_cache_destroy_mmap(cache);
   }

   ralloc_free(cache);
}

void
disk_cache_wait_for_idle(struct disk_cache *cache)
{
   util_queue_finish(&cache->cache_queue);
}

void
disk_cache_remove(struct disk_cache *cache, const cache_key key)
{
   if (cache->type == DISK_CACHE_DATABASE) {
      mesa_cache_db_multipart_entry_remove(&cache->cache_db, key);
      return;
   }

   char *filename = disk_cache_get_cache_filename(cache, key);
   if (filename == NULL) {
      return;
   }

   disk_cache_evict_item(cache, filename);
}

static struct disk_cache_put_job *
create_put_job(struct disk_cache *cache, const cache_key key,
               void *data, size_t size,
               struct cache_item_metadata *cache_item_metadata,
               bool take_ownership)
{
   struct disk_cache_put_job *dc_job = (struct disk_cache_put_job *)
      malloc(sizeof(struct disk_cache_put_job) + (take_ownership ? 0 : size));

   if (dc_job) {
      dc_job->cache = cache;
      memcpy(dc_job->key, key, sizeof(cache_key));
      if (take_ownership) {
         dc_job->data = data;
      } else {
         dc_job->data = dc_job + 1;
         memcpy(dc_job->data, data, size);
      }
      dc_job->size = size;

      /* Copy the cache item metadata */
      if (cache_item_metadata) {
         dc_job->cache_item_metadata.type = cache_item_metadata->type;
         if (cache_item_metadata->type == CACHE_ITEM_TYPE_GLSL) {
            dc_job->cache_item_metadata.num_keys =
               cache_item_metadata->num_keys;
            dc_job->cache_item_metadata.keys = (cache_key *)
               malloc(cache_item_metadata->num_keys * sizeof(cache_key));

            if (!dc_job->cache_item_metadata.keys)
               goto fail;

            memcpy(dc_job->cache_item_metadata.keys,
                   cache_item_metadata->keys,
                   sizeof(cache_key) * cache_item_metadata->num_keys);
         }
      } else {
         dc_job->cache_item_metadata.type = CACHE_ITEM_TYPE_UNKNOWN;
         dc_job->cache_item_metadata.keys = NULL;
      }
   }

   return dc_job;

fail:
   free(dc_job);

   return NULL;
}

static void
destroy_put_job(void *job, void *gdata, int thread_index)
{
   if (job) {
      struct disk_cache_put_job *dc_job = (struct disk_cache_put_job *) job;
      free(dc_job->cache_item_metadata.keys);
      free(job);
   }
}

static void
destroy_put_job_nocopy(void *job, void *gdata, int thread_index)
{
   struct disk_cache_put_job *dc_job = (struct disk_cache_put_job *) job;
   free(dc_job->data);
   destroy_put_job(job, gdata, thread_index);
}

static void
blob_put_compressed(struct disk_cache *cache, const cache_key key,
         const void *data, size_t size);

static void
cache_put(void *job, void *gdata, int thread_index)
{
   assert(job);

   unsigned i = 0;
   char *filename = NULL;
   struct disk_cache_put_job *dc_job = (struct disk_cache_put_job *) job;

   if (dc_job->cache->blob_put_cb) {
      blob_put_compressed(dc_job->cache, dc_job->key, dc_job->data, dc_job->size);
   } else if (dc_job->cache->type == DISK_CACHE_SINGLE_FILE) {
      disk_cache_write_item_to_disk_foz(dc_job);
   } else if (dc_job->cache->type == DISK_CACHE_DATABASE) {
      disk_cache_db_write_item_to_disk(dc_job);
   } else if (dc_job->cache->type == DISK_CACHE_MULTI_FILE) {
      filename = disk_cache_get_cache_filename(dc_job->cache, dc_job->key);
      if (filename == NULL)
         goto done;

      /* If the cache is too large, evict something else first. */
      while (p_atomic_read_relaxed(&dc_job->cache->size->value) + dc_job->size > dc_job->cache->max_size &&
             i < 8) {
         disk_cache_evict_lru_item(dc_job->cache);
         i++;
      }

      disk_cache_write_item_to_disk(dc_job, filename);

done:
      free(filename);
   }
}

struct blob_cache_entry {
   uint32_t uncompressed_size;
   uint8_t compressed_data[];
};

static void
blob_put_compressed(struct disk_cache *cache, const cache_key key,
         const void *data, size_t size)
{
   MESA_TRACE_FUNC();

   size_t max_buf = util_compress_max_compressed_len(size);
   struct blob_cache_entry *entry = malloc(max_buf + sizeof(*entry));
   if (!entry)
      goto out;

   entry->uncompressed_size = size;

   size_t compressed_size =
         util_compress_deflate(data, size, entry->compressed_data, max_buf);
   if (!compressed_size)
      goto out;

   unsigned entry_size = compressed_size + sizeof(*entry);
   // The curly brackets are here to only trace the blob_put_cb call
   {
      MESA_TRACE_SCOPE("blob_put");
      cache->blob_put_cb(key, CACHE_KEY_SIZE, entry, entry_size);
   }

out:
   free(entry);
}

static void *
blob_get_compressed(struct disk_cache *cache, const cache_key key,
                    size_t *size)
{
   MESA_TRACE_FUNC();

   /* This is what Android EGL defines as the maxValueSize in egl_cache_t
    * class implementation.
    */
   const signed long max_blob_size = 64 * 1024;
   struct blob_cache_entry *entry = malloc(max_blob_size);
   if (!entry)
      return NULL;

   signed long entry_size;
   // The curly brackets are here to only trace the blob_get_cb call
   {
      MESA_TRACE_SCOPE("blob_get");
      entry_size = cache->blob_get_cb(key, CACHE_KEY_SIZE, entry, max_blob_size);
   }

   if (!entry_size) {
      free(entry);
      return NULL;
   }

   void *data = malloc(entry->uncompressed_size);
   if (!data) {
      free(entry);
      return NULL;
   }

   unsigned compressed_size = entry_size - sizeof(*entry);
   bool ret = util_compress_inflate(entry->compressed_data, compressed_size,
                                    data, entry->uncompressed_size);
   if (!ret) {
      free(data);
      free(entry);
      return NULL;
   }

   if (size)
      *size = entry->uncompressed_size;

   free(entry);

   return data;
}

void
disk_cache_put(struct disk_cache *cache, const cache_key key,
               const void *data, size_t size,
               struct cache_item_metadata *cache_item_metadata)
{
   if (!util_queue_is_initialized(&cache->cache_queue))
      return;

   struct disk_cache_put_job *dc_job =
      create_put_job(cache, key, (void*)data, size, cache_item_metadata, false);

   if (dc_job) {
      util_queue_fence_init(&dc_job->fence);
      util_queue_add_job(&cache->cache_queue, dc_job, &dc_job->fence,
                         cache_put, destroy_put_job, dc_job->size);
   }
}

void
disk_cache_put_nocopy(struct disk_cache *cache, const cache_key key,
                      void *data, size_t size,
                      struct cache_item_metadata *cache_item_metadata)
{
   if (!util_queue_is_initialized(&cache->cache_queue)) {
      free(data);
      return;
   }

   struct disk_cache_put_job *dc_job =
      create_put_job(cache, key, data, size, cache_item_metadata, true);

   if (dc_job) {
      util_queue_fence_init(&dc_job->fence);
      util_queue_add_job(&cache->cache_queue, dc_job, &dc_job->fence,
                         cache_put, destroy_put_job_nocopy, dc_job->size);
   }
}

void *
disk_cache_get(struct disk_cache *cache, const cache_key key, size_t *size)
{
   void *buf = NULL;

   if (size)
      *size = 0;

   if (cache->foz_ro_cache)
      buf = disk_cache_load_item_foz(cache->foz_ro_cache, key, size);

   if (!buf) {
      if (cache->blob_get_cb) {
         buf = blob_get_compressed(cache, key, size);
      } else if (cache->type == DISK_CACHE_SINGLE_FILE) {
         buf = disk_cache_load_item_foz(cache, key, size);
      } else if (cache->type == DISK_CACHE_DATABASE) {
         buf = disk_cache_db_load_item(cache, key, size);
      } else if (cache->type == DISK_CACHE_MULTI_FILE) {
         char *filename = disk_cache_get_cache_filename(cache, key);
         if (filename)
            buf = disk_cache_load_item(cache, filename, size);
      }
   }

   if (unlikely(cache->stats.enabled)) {
      if (buf)
         p_atomic_inc(&cache->stats.hits);
      else
         p_atomic_inc(&cache->stats.misses);
   }

   return buf;
}

void
disk_cache_put_key(struct disk_cache *cache, const cache_key key)
{
   const uint32_t *key_chunk = (const uint32_t *) key;
   int i = CPU_TO_LE32(*key_chunk) & CACHE_INDEX_KEY_MASK;
   unsigned char *entry;

   if (cache->blob_put_cb) {
      cache->blob_put_cb(key, CACHE_KEY_SIZE, key_chunk, sizeof(uint32_t));
      return;
   }

   if (cache->path_init_failed)
      return;

   entry = &cache->stored_keys[i * CACHE_KEY_SIZE];

   memcpy(entry, key, CACHE_KEY_SIZE);
}

/* This function lets us test whether a given key was previously
 * stored in the cache with disk_cache_put_key(). The implement is
 * efficient by not using syscalls or hitting the disk. It's not
 * race-free, but the races are benign. If we race with someone else
 * calling disk_cache_put_key, then that's just an extra cache miss and an
 * extra recompile.
 */
bool
disk_cache_has_key(struct disk_cache *cache, const cache_key key)
{
   const uint32_t *key_chunk = (const uint32_t *) key;
   int i = CPU_TO_LE32(*key_chunk) & CACHE_INDEX_KEY_MASK;
   unsigned char *entry;

   if (cache->blob_get_cb) {
      uint32_t blob;
      return cache->blob_get_cb(key, CACHE_KEY_SIZE, &blob, sizeof(uint32_t));
   }

   if (cache->path_init_failed)
      return false;

   entry = &cache->stored_keys[i * CACHE_KEY_SIZE];

   return memcmp(entry, key, CACHE_KEY_SIZE) == 0;
}

void
disk_cache_compute_key(struct disk_cache *cache, const void *data, size_t size,
                       cache_key key)
{
   blake3_hasher ctx;

   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, cache->driver_keys_blob,
                     cache->driver_keys_blob_size);
   _mesa_blake3_update(&ctx, data, size);
   _mesa_blake3_final(&ctx, key);
}

void
disk_cache_set_callbacks(struct disk_cache *cache, disk_cache_put_cb put,
                         disk_cache_get_cb get)
{
   cache->blob_put_cb = put;
   cache->blob_get_cb = get;
   disk_cache_init_queue(cache);
}

#endif /* !DETECT_OS_WINDOWS */

#endif /* ENABLE_SHADER_CACHE */
