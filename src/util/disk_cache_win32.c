/* SPDX-License-Identifier: MIT */
/* Win32 implementation of Mesa's blob cache. Cache I/O runs on a low-priority
 * worker. Content is published by rename, so readers never observe a writer's
 * partial file. A file lock and a shared size counter coordinate writers in
 * different processes; cache failures always degrade to a miss.
 */
#if defined(_WIN32) && defined(ENABLE_SHADER_CACHE)

#include <windows.h>
#include <shlobj.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "disk_cache.h"
#include "hex.h"
#include "u_debug.h"
#include "u_queue.h"
#include "mesa-blake3.h"
#include "log.h"

#define WIN_CACHE_MAGIC UINT64_C(0x314548434143534d) /* MSCACHE1 */
#define WIN_CACHE_INDEX_KEYS 65536u
#define WIN_CACHE_INDEX_SIZE (sizeof(uint64_t) + WIN_CACHE_INDEX_KEYS * sizeof(cache_key))

struct disk_cache {
   wchar_t *path;
   HANDLE index_file;
   HANDLE mapping;
   uint64_t *bytes;
   uint8_t *keys;
   uint64_t max_size;
   blake3_hasher key_seed;
   struct util_queue queue;
   disk_cache_put_cb put_cb;
   disk_cache_get_cb get_cb;
   volatile LONG hits, misses;
   bool show_stats;
};

struct win_cache_header {
   uint64_t magic;
   uint64_t size;
   cache_key key;
   blake3_hash digest;
};

struct win_cache_job {
   struct disk_cache *cache;
   cache_key key;
   void *data;
   size_t size;
};

struct win_cache_entry {
   wchar_t name[96];
   uint64_t bytes;
   uint64_t time;
};

static wchar_t *
join_path(const wchar_t *dir, const wchar_t *name)
{
   size_t length = wcslen(dir);
   bool separator = length && dir[length - 1] != L'\\' && dir[length - 1] != L'/';
   size_t n = length + separator + wcslen(name) + 1;
   wchar_t *path = malloc(n * sizeof(*path));
   if (path)
      swprintf(path, n, separator ? L"%ls\\%ls" : L"%ls%ls", dir, name);
   return path;
}

static wchar_t *
cache_filename(struct disk_cache *cache, const cache_key key)
{
   wchar_t name[2 * sizeof(cache_key) + 5];
   static const wchar_t hex[] = L"0123456789abcdef";
   for (unsigned i = 0; i < sizeof(cache_key); i++) {
      name[2 * i] = hex[key[i] >> 4];
      name[2 * i + 1] = hex[key[i] & 15];
   }
   wcscpy(name + 2 * sizeof(cache_key), L".bin");
   return join_path(cache->path, name);
}

static bool
cache_lock(struct disk_cache *cache)
{
   OVERLAPPED overlapped = {0};
   return cache->bytes && LockFileEx(cache->index_file, LOCKFILE_EXCLUSIVE_LOCK,
                                    0, 1, 0, &overlapped);
}

static void
cache_unlock(struct disk_cache *cache)
{
   OVERLAPPED overlapped = {0};
   UnlockFileEx(cache->index_file, 0, 1, 0, &overlapped);
}

static uint64_t
file_size(const wchar_t *path)
{
   WIN32_FILE_ATTRIBUTE_DATA info;
   if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info) ||
       (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      return 0;
   return ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
}

static int
oldest_first(const void *a, const void *b)
{
   const struct win_cache_entry *x = a, *y = b;
   return (x->time > y->time) - (x->time < y->time);
}

/* Called at cache creation to reconcile interrupted writes, and on eviction.
 * Only this backend's content filenames are eligible for removal.
 */
static void
cache_trim(struct disk_cache *cache, uint64_t target, const wchar_t *keep)
{
   wchar_t *pattern = join_path(cache->path, L"*");
   if (!pattern)
      return;
   WIN32_FIND_DATAW data;
   HANDLE find = FindFirstFileW(pattern, &data);
   free(pattern);
   if (find == INVALID_HANDLE_VALUE)
      return;
   struct win_cache_entry *entries = NULL;
   size_t count = 0, capacity = 0;
   uint64_t bytes = 0;
   bool complete = true;
   do {
      size_t n = wcslen(data.cFileName);
      if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
          n != 2 * sizeof(cache_key) + 4 ||
          wcscmp(data.cFileName + n - 4, L".bin"))
         continue;
      if (wcsspn(data.cFileName, L"0123456789abcdef") != 2 * sizeof(cache_key))
         continue;
      if (count == capacity) {
         size_t next = capacity ? capacity * 2 : 64;
         void *grown = realloc(entries, next * sizeof(*entries));
         if (!grown) {
            complete = false;
            break;
         }
         entries = grown;
         capacity = next;
      }
      struct win_cache_entry *entry = &entries[count++];
      wcscpy(entry->name, data.cFileName);
      entry->bytes = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
      entry->time = ((uint64_t)data.ftLastAccessTime.dwHighDateTime << 32) |
                    data.ftLastAccessTime.dwLowDateTime;
      bytes += entry->bytes;
   } while (FindNextFileW(find, &data));
   complete = complete && GetLastError() == ERROR_NO_MORE_FILES;
   FindClose(find);
   if (complete) {
      if (count > 1)
         qsort(entries, count, sizeof(*entries), oldest_first);
      for (size_t i = 0; i < count && bytes > target; i++) {
         wchar_t *path = join_path(cache->path, entries[i].name);
         if (path && (!keep || wcscmp(path, keep)) && DeleteFileW(path))
            bytes -= entries[i].bytes;
         free(path);
      }
      *cache->bytes = bytes;
   }
   free(entries);
}

static void
cache_write_job(void *job_data, void *gdata, int thread_index)
{
   struct win_cache_job *job = job_data;
   struct disk_cache *cache = job->cache;
   if (cache->put_cb) {
      if (job->size <= LONG_MAX)
         cache->put_cb(job->key, sizeof(cache_key), job->data, job->size);
      return;
   }
   if (!cache->bytes || job->size > UINT32_MAX ||
       job->size + sizeof(struct win_cache_header) > cache->max_size)
      return;
   wchar_t *path = cache_filename(cache, job->key);
   if (!path)
      return;
   static LONG sequence;
   wchar_t temp_name[96];
   swprintf(temp_name, ARRAY_SIZE(temp_name), L"%lu-%lu-%lu.tmp",
            GetCurrentProcessId(), GetCurrentThreadId(),
            (unsigned long)InterlockedIncrement(&sequence));
   wchar_t *temp = join_path(cache->path, temp_name);
   if (!temp) {
      free(path);
      return;
   }
   HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      goto done;
   struct win_cache_header header = {.magic = WIN_CACHE_MAGIC, .size = job->size};
   memcpy(header.key, job->key, sizeof(header.key));
   _mesa_blake3_compute(job->data ? job->data : "", job->size, header.digest);
   DWORD written;
   bool valid = WriteFile(file, &header, sizeof(header), &written, NULL) && written == sizeof(header);
   if (valid && job->size)
      valid = WriteFile(file, job->data, job->size, &written, NULL) && written == job->size;
   CloseHandle(file);
   if (valid && cache_lock(cache)) {
      uint64_t old_size = file_size(path);
      uint64_t new_size = sizeof(header) + job->size;
      uint64_t target = cache->max_size - new_size + old_size;
      if (*cache->bytes > target)
         cache_trim(cache, target, path);
      if (*cache->bytes <= target && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING))
         *cache->bytes = (*cache->bytes >= old_size ? *cache->bytes - old_size : 0) + new_size;
      cache_unlock(cache);
   }
   DeleteFileW(temp);
done:
   free(temp);
   free(path);
}

static void
cache_free_job(void *job_data, void *gdata, int thread_index)
{
   struct win_cache_job *job = job_data;
   free(job->data);
   free(job);
}

static void
queue_put(struct disk_cache *cache, const cache_key key, void *data,
          size_t size)
{
   if (!cache || (!cache->bytes && !cache->put_cb)) {
      free(data);
      return;
   }
   struct win_cache_job *job = calloc(1, sizeof(*job));
   if (!job) {
      free(data);
      return;
   }
   job->cache = cache;
   job->data = data;
   job->size = size;
   memcpy(job->key, key, sizeof(cache_key));
   util_queue_add_job(&cache->queue, job, NULL, cache_write_job, cache_free_job, size);
}

/* File APIs keep the extended-length spelling for every cache child,
 * including the 64-digit blob names and find patterns. */
static wchar_t *
extended_cache_path(const wchar_t *path)
{
   if (!wcsncmp(path, L"\\\\?\\", 4) || !wcsncmp(path, L"\\\\.\\", 4))
      return _wcsdup(path);

   DWORD capacity = GetFullPathNameW(path, 0, NULL, NULL);
   if (!capacity)
      return NULL;
   wchar_t *full = calloc(capacity, sizeof(*full));
   if (!full)
      return NULL;
   DWORD length = GetFullPathNameW(path, capacity, full, NULL);
   wchar_t *extended = NULL;
   if (length && length < capacity) {
      extended = !wcsncmp(full, L"\\\\", 2)
         ? join_path(L"\\\\?\\UNC", full + 2)
         : join_path(L"\\\\?", full);
   }
   free(full);
   return extended;
}

static DWORD
create_directory(const wchar_t *path)
{
   if (CreateDirectoryW(path, NULL))
      return ERROR_SUCCESS;
   DWORD error = GetLastError();
   if (error == ERROR_ALREADY_EXISTS) {
      DWORD attributes = GetFileAttributesW(path);
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_DIRECTORY))
         return ERROR_SUCCESS;
   }
   return error;
}

static bool
create_cache_directories(wchar_t *path)
{
   DWORD error = create_directory(path);
   if (error != ERROR_PATH_NOT_FOUND)
      return error == ERROR_SUCCESS;

   /* Skip the drive or volume root, or both the UNC server and share. */
   wchar_t *root = path + 4;
   if (!_wcsnicmp(root, L"UNC\\", 4)) {
      wchar_t *server_end = wcschr(root + 4, L'\\');
      if (!server_end)
         return false;
      root = server_end + 1;
   }
   wchar_t *separator = wcschr(root, L'\\');
   while (separator) {
      separator = wcschr(separator + 1, L'\\');
      if (separator)
         *separator = L'\0';
      error = create_directory(path);
      if (separator)
         *separator = L'\\';
      if (error != ERROR_SUCCESS)
         return false;
   }
   return error == ERROR_SUCCESS;
}

static wchar_t *
cache_directory(const char *custom)
{
   DWORD n = GetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", NULL, 0);
   wchar_t *base = calloc(n ? n : MAX_PATH, sizeof(*base));
   if (!base)
      return NULL;
   if (n) {
      DWORD length = GetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", base, n);
      if (!length || length >= n) {
         free(base);
         return NULL;
      }
   } else if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                                    NULL, SHGFP_TYPE_CURRENT, base))) {
      free(base);
      return NULL;
   }
   wchar_t suffix[96] = L"mesa_shader_cache_win32";
   if (custom) {
      blake3_hash hash;
      _mesa_blake3_compute(custom, strlen(custom), hash);
      char hex[BLAKE3_HEX_LEN];
      mesa_bytes_to_hex(hex, hash, sizeof(hash));
      swprintf(suffix, ARRAY_SIZE(suffix), L"mesa_shader_cache_win32-%hs", hex);
   }
   wchar_t *path = join_path(base, suffix);
   free(base);
   if (!path)
      return NULL;
   wchar_t *extended = extended_cache_path(path);
   free(path);
   if (extended && !create_cache_directories(extended)) {
      free(extended);
      return NULL;
   }
   return extended;
}

static uint64_t
cache_size_limit(void)
{
   const char *value = debug_get_option("MESA_SHADER_CACHE_MAX_SIZE", NULL);
#ifdef MESA_SHADER_CACHE_MAX_SIZE
   if (!value)
      value = MESA_SHADER_CACHE_MAX_SIZE;
#endif
   if (value) {
      char *end;
      uint64_t size = strtoull(value, &end, 10);
      uint64_t multiplier = (*end == 'k' || *end == 'K') ? 1024 :
                            (*end == 'm' || *end == 'M') ? 1024 * 1024 :
                            UINT64_C(1024) * 1024 * 1024;
      if (end != value && size && size <= UINT64_MAX / multiplier)
         return size * multiplier;
   }
   return UINT64_C(1024) * 1024 * 1024;
}

static struct disk_cache *
create_cache(const char *gpu_name, const char *driver_id, uint64_t flags,
             const char *custom, uint64_t max_size)
{
#ifdef SHADER_CACHE_DISABLE_BY_DEFAULT
   const bool disabled_default = true;
#else
   const bool disabled_default = false;
#endif
   if (debug_get_bool_option("MESA_SHADER_CACHE_DISABLE", disabled_default))
      return NULL;
   struct disk_cache *cache = calloc(1, sizeof(*cache));
   if (!cache)
      return NULL;
   cache->index_file = INVALID_HANDLE_VALUE;
   cache->max_size = max_size;
   cache->show_stats = debug_get_bool_option("MESA_SHADER_CACHE_SHOW_STATS", false);
   _mesa_blake3_init(&cache->key_seed);
   const uint64_t version = WIN_CACHE_MAGIC;
   const uint8_t pointer_size = sizeof(void *);
   _mesa_blake3_update(&cache->key_seed, &version, sizeof(version));
   _mesa_blake3_update(&cache->key_seed, gpu_name, strlen(gpu_name) + 1);
   _mesa_blake3_update(&cache->key_seed, driver_id, strlen(driver_id) + 1);
   _mesa_blake3_update(&cache->key_seed, &flags, sizeof(flags));
   _mesa_blake3_update(&cache->key_seed, &pointer_size, sizeof(pointer_size));
   if (!util_queue_init(&cache->queue, "disk$", 32, 1,
                       UTIL_QUEUE_INIT_RESIZE_IF_FULL | UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY,
                       NULL)) {
      free(cache);
      return NULL;
   }
   cache->path = cache_directory(custom);
   if (!cache->path)
      return cache; /* Application blob callbacks can still be installed. */
   wchar_t *index = join_path(cache->path, L"index");
   if (!index)
      return cache;
   cache->index_file = CreateFileW(index, GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   free(index);
   if (cache->index_file == INVALID_HANDLE_VALUE)
      return cache; /* Existing entries may still be readable. */
   cache->mapping = CreateFileMappingW(cache->index_file, NULL, PAGE_READWRITE,
                                       0, WIN_CACHE_INDEX_SIZE, NULL);
   if (cache->mapping)
      cache->bytes = MapViewOfFile(cache->mapping, FILE_MAP_ALL_ACCESS, 0, 0, WIN_CACHE_INDEX_SIZE);
   if (cache->bytes)
      cache->keys = (uint8_t *)(cache->bytes + 1);
   if (cache_lock(cache)) {
      cache_trim(cache, cache->max_size, NULL);
      cache_unlock(cache);
   }
   return cache;
}

struct disk_cache *
disk_cache_create(const char *gpu_name, const char *driver_id, uint64_t flags)
{
   return create_cache(gpu_name, driver_id, flags, NULL, cache_size_limit());
}

struct disk_cache *
disk_cache_create_custom(const char *gpu_name, const char *driver_id,
                         uint64_t flags, const char *name, uint32_t max_size)
{
   return create_cache(gpu_name, driver_id, flags, name, max_size ? max_size : cache_size_limit());
}

void
disk_cache_destroy(struct disk_cache *cache)
{
   if (!cache)
      return;
   util_queue_finish(&cache->queue);
   util_queue_destroy(&cache->queue);
   if (cache->show_stats)
      mesa_logi("disk shader cache: hits = %ld, misses = %ld", cache->hits, cache->misses);
   if (cache->bytes)
      UnmapViewOfFile(cache->bytes);
   if (cache->mapping)
      CloseHandle(cache->mapping);
   if (cache->index_file != INVALID_HANDLE_VALUE)
      CloseHandle(cache->index_file);
   free(cache->path);
   free(cache);
}

void
disk_cache_wait_for_idle(struct disk_cache *cache)
{
   if (cache)
      util_queue_finish(&cache->queue);
}

void
disk_cache_compute_key(struct disk_cache *cache, const void *data, size_t size, cache_key key)
{
   blake3_hasher hash = cache->key_seed;
   _mesa_blake3_update(&hash, data, size);
   _mesa_blake3_final(&hash, key);
}

void
disk_cache_put(struct disk_cache *cache, const cache_key key, const void *data,
               size_t size, struct cache_item_metadata *metadata)
{
   if (!cache)
      return;
   void *copy = malloc(size ? size : 1);
   if (!copy)
      return;
   if (size)
      memcpy(copy, data, size);
   queue_put(cache, key, copy, size);
}

void
disk_cache_put_nocopy(struct disk_cache *cache, const cache_key key, void *data,
                      size_t size, struct cache_item_metadata *metadata)
{
   queue_put(cache, key, data, size);
}

static void *
cache_read(struct disk_cache *cache, const cache_key key, size_t *size)
{
   if (!cache || !cache->path)
      return NULL;
   wchar_t *path = cache_filename(cache, key);
   if (!path)
      return NULL;
   const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
   HANDLE file = CreateFileW(path, GENERIC_READ | FILE_WRITE_ATTRIBUTES, sharing,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      file = CreateFileW(path, GENERIC_READ, sharing, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   free(path);
   if (file == INVALID_HANDLE_VALUE)
      return NULL;
   void *data = NULL;
   struct win_cache_header header;
   LARGE_INTEGER length;
   DWORD read;
   if (!GetFileSizeEx(file, &length) ||
       !ReadFile(file, &header, sizeof(header), &read, NULL) || read != sizeof(header) ||
       header.magic != WIN_CACHE_MAGIC || memcmp(header.key, key, sizeof(cache_key)) ||
       header.size > UINT32_MAX || header.size > cache->max_size ||
       (uint64_t)length.QuadPart != sizeof(header) + header.size)
      goto done;
   data = malloc(header.size ? header.size : 1);
   if (!data)
      goto done;
   bool valid = !header.size ||
                (ReadFile(file, data, header.size, &read, NULL) && read == header.size);
   blake3_hash digest;
   if (valid)
      _mesa_blake3_compute(data, header.size, digest);
   if (!valid || memcmp(digest, header.digest, sizeof(digest))) {
      free(data);
      data = NULL;
   } else {
      if (size)
         *size = header.size;
      FILETIME now;
      GetSystemTimeAsFileTime(&now);
      SetFileTime(file, NULL, &now, NULL);
   }
done:
   CloseHandle(file);
   return data;
}

void *
disk_cache_get(struct disk_cache *cache, const cache_key key, size_t *size)
{
   if (size)
      *size = 0;
   if (!cache)
      return NULL;
   void *data = NULL;
   if (cache->get_cb) {
      long n = cache->get_cb(key, sizeof(cache_key), NULL, 0);
      if (n > 0 && (uint64_t)n <= cache->max_size) {
         data = malloc(n);
         if (data && cache->get_cb(key, sizeof(cache_key), data, n) != n) {
            free(data);
            data = NULL;
         }
         if (data && size)
            *size = n;
      }
   } else {
      data = cache_read(cache, key, size);
   }
   InterlockedIncrement(data ? &cache->hits : &cache->misses);
   return data;
}

void
disk_cache_remove(struct disk_cache *cache, const cache_key key)
{
   if (!cache || !cache->path)
      return;
   wchar_t *path = cache_filename(cache, key);
   if (path && cache_lock(cache)) {
      uint64_t old_size = file_size(path);
      if (DeleteFileW(path))
         *cache->bytes = *cache->bytes >= old_size ? *cache->bytes - old_size : 0;
      cache_unlock(cache);
   }
   free(path);
}

void
disk_cache_put_key(struct disk_cache *cache, const cache_key key)
{
   if (!cache || !cache->keys)
      return;
   /* Like Mesa's other cache backends, this is a best-effort key index.
    * Colliding slots may replace one another; blob storage is independent. */
   unsigned slot = key[0] | ((unsigned)key[1] << 8);
   memcpy(cache->keys + slot * sizeof(cache_key), key, sizeof(cache_key));
}

bool
disk_cache_has_key(struct disk_cache *cache, const cache_key key)
{
   if (!cache || !cache->keys)
      return false;
   unsigned slot = key[0] | ((unsigned)key[1] << 8);
   return !memcmp(cache->keys + slot * sizeof(cache_key), key, sizeof(cache_key));
}

void
disk_cache_set_callbacks(struct disk_cache *cache, disk_cache_put_cb put,
                         disk_cache_get_cb get)
{
   if (!cache)
      return;
   util_queue_finish(&cache->queue);
   cache->put_cb = put;
   cache->get_cb = get;
}

#endif
