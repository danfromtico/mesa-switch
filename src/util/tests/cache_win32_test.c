/* SPDX-License-Identifier: MIT */
/* Windows file/cache control only. No GPU execution is performed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "util/disk_cache.h"
#include "util/hex.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (Win32 %lu)\n", __LINE__, #x, GetLastError()); exit(1); } } while (0)

static const char payload[] = "compiled shader fixture\0binary payload\xff\x03";

static void
expect_blob(struct disk_cache *cache, const cache_key key, const void *data, size_t bytes)
{
   size_t size = 0;
   void *got = disk_cache_get(cache, key, &size);
   CHECK(got && size == bytes && !memcmp(got, data, bytes));
   free(got);
}

static PROCESS_INFORMATION
start_child(const wchar_t *argument)
{
   wchar_t exe[MAX_PATH], command[MAX_PATH + 64];
   CHECK(GetModuleFileNameW(NULL, exe, ARRAY_SIZE(exe)) > 0);
   CHECK(swprintf(command, ARRAY_SIZE(command), L"\"%ls\" %ls", exe, argument) > 0);
   STARTUPINFOW startup = {.cb = sizeof(startup)};
   PROCESS_INFORMATION process = {0};
   CHECK(CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
   return process;
}

static void
finish_child(PROCESS_INFORMATION process)
{
   CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
   DWORD result;
   CHECK(GetExitCodeProcess(process.hProcess, &result) && result == 0);
   CloseHandle(process.hThread);
   CloseHandle(process.hProcess);
}

static void
child(const wchar_t *argument)
{
   finish_child(start_child(argument));
}

static void
check_long_cache_root(const wchar_t *root)
{
   static const wchar_t component[] = L"\\long-cache-component-\u7f13\u5b58";
   wchar_t path[MAX_PATH + ARRAY_SIZE(component) + 1];
   size_t length = wcslen(root);
   CHECK(length < MAX_PATH);
   memcpy(path, root, (length + 1) * sizeof(*path));
   while (length < MAX_PATH) {
      memcpy(path + length, component, sizeof(component));
      length += ARRAY_SIZE(component) - 1;
   }
   CHECK(SetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", path));
   struct disk_cache *cache = disk_cache_create("gm20b-test", "driver-one", 0);
   CHECK(cache);
   cache_key key;
   disk_cache_compute_key(cache, "shader-one", 10, key);
   disk_cache_put(cache, key, payload, sizeof(payload), NULL);
   disk_cache_put_key(cache, key);
   disk_cache_destroy(cache);
   child(L"--read");
   CHECK(SetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", root));
}

int
main(int argc, char **argv)
{
   cache_key key;
   if (argc == 2 && !strcmp(argv[1], "--read")) {
      struct disk_cache *cache = disk_cache_create("gm20b-test", "driver-one", 0);
      CHECK(cache);
      disk_cache_compute_key(cache, "shader-one", 10, key);
      expect_blob(cache, key, payload, sizeof(payload));
      CHECK(disk_cache_has_key(cache, key));
      disk_cache_destroy(cache);
      puts("PASS: cache hit in a fresh process");
      return 0;
   }
   if (argc == 2 && !strcmp(argv[1], "--disabled")) {
      CHECK(disk_cache_create("gm20b-test", "driver-one", 0) == NULL);
      return 0;
   }
   if (argc == 2 && (!strcmp(argv[1], "--writer-a") || !strcmp(argv[1], "--writer-b"))) {
      struct disk_cache *cache = disk_cache_create("gm20b-test", "driver-one", 0);
      CHECK(cache);
      for (unsigned i = 0; i < 32; i++) {
         char name[32], bytes[1024];
         int n = snprintf(name, sizeof(name), "%s-%u", argv[1], i);
         memset(bytes, i, sizeof(bytes));
         disk_cache_compute_key(cache, name, n, key);
         disk_cache_put(cache, key, bytes, sizeof(bytes), NULL);
      }
      disk_cache_destroy(cache);
      return 0;
   }

   wchar_t temp[MAX_PATH], root[MAX_PATH];
   CHECK(GetTempPathW(ARRAY_SIZE(temp), temp) > 0);
   CHECK(swprintf(root, ARRAY_SIZE(root), L"%lsmesa-cache-%lu-%llu-\u7f13\u5b58",
                  temp, GetCurrentProcessId(), (unsigned long long)GetTickCount64()) > 0);
   CHECK(CreateDirectoryW(root, NULL));
   CHECK(SetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", root));
   CHECK(_putenv("MESA_SHADER_CACHE_MAX_SIZE=1M") == 0);
   CHECK(_putenv("MESA_SHADER_CACHE_SHOW_STATS=true") == 0);
   struct disk_cache *cache = disk_cache_create("gm20b-test", "driver-one", 0);
   CHECK(cache);
   disk_cache_compute_key(cache, "shader-one", 10, key);
   size_t size = 99;
   CHECK(!disk_cache_get(cache, key, &size) && size == 0);
   disk_cache_put(cache, key, payload, sizeof(payload), NULL);
   disk_cache_put_key(cache, key);
   CHECK(disk_cache_has_key(cache, key));
   disk_cache_wait_for_idle(cache);
   expect_blob(cache, key, payload, sizeof(payload));
   disk_cache_destroy(cache);
   child(L"--read");

   cache = disk_cache_create("gm20b-test", "driver-two", 0);
   CHECK(cache);
   cache_key other;
   disk_cache_compute_key(cache, "shader-one", 10, other);
   CHECK(memcmp(other, key, sizeof(key)) && !disk_cache_get(cache, other, NULL));
   disk_cache_destroy(cache);
   cache = disk_cache_create("different-gpu", "driver-one", 0);
   CHECK(cache);
   disk_cache_compute_key(cache, "shader-one", 10, other);
   CHECK(memcmp(other, key, sizeof(key)) && !disk_cache_get(cache, other, NULL));
   disk_cache_destroy(cache);

   cache = disk_cache_create("gm20b-test", "driver-one", 0);
   CHECK(cache);
   char hex[BLAKE3_HEX_LEN];
   mesa_bytes_to_hex(hex, key, sizeof(key));
   wchar_t filename[MAX_PATH];
   CHECK(swprintf(filename, ARRAY_SIZE(filename), L"%ls\\mesa_shader_cache_win32\\%hs.bin", root, hex) > 0);
   HANDLE file = CreateFileW(filename, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   CHECK(file != INVALID_HANDLE_VALUE);
   DWORD written;
   CHECK(WriteFile(file, "broken", 6, &written, NULL) && written == 6);
   CloseHandle(file);
   CHECK(!disk_cache_get(cache, key, NULL));
   disk_cache_put(cache, key, payload, sizeof(payload), NULL);
   disk_cache_wait_for_idle(cache);
   expect_blob(cache, key, payload, sizeof(payload));
   disk_cache_remove(cache, key);
   CHECK(!disk_cache_get(cache, key, NULL));
   disk_cache_destroy(cache);

   cache = disk_cache_create_custom("gm20b-test", "driver-one", 0, "bounded", 8192);
   CHECK(cache);
   char block[4096];
   memset(block, 0x39, sizeof(block));
   disk_cache_compute_key(cache, "first", 5, key);
   disk_cache_compute_key(cache, "second", 6, other);
   disk_cache_put(cache, key, block, sizeof(block), NULL);
   disk_cache_wait_for_idle(cache);
   expect_blob(cache, key, block, sizeof(block));
   disk_cache_put(cache, other, block, sizeof(block), NULL);
   disk_cache_wait_for_idle(cache);
   CHECK(!disk_cache_get(cache, key, NULL));
   expect_blob(cache, other, block, sizeof(block));
   disk_cache_destroy(cache);

   cache = disk_cache_create("gm20b-test", "driver-one", 0);
   CHECK(cache);
   PROCESS_INFORMATION a = start_child(L"--writer-a"), b = start_child(L"--writer-b");
   finish_child(a);
   finish_child(b);
   for (unsigned writer = 0; writer < 2; writer++) {
      for (unsigned i = 0; i < 32; i++) {
         char name[32], bytes[1024];
         int n = snprintf(name, sizeof(name), "--writer-%c-%u", 'a' + writer, i);
         memset(bytes, i, sizeof(bytes));
         disk_cache_compute_key(cache, name, n, key);
         expect_blob(cache, key, bytes, sizeof(bytes));
      }
   }
   disk_cache_destroy(cache);

   check_long_cache_root(root);

   CHECK(_putenv("MESA_SHADER_CACHE_DISABLE=true") == 0);
   child(L"--disabled");
   puts("PASS: Windows cache persistence, concurrent writers, namespaces, integrity, key index, eviction, and disable option; no GPU execution");
   return 0;
}
