#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "util/disk_cache.h"
#include "util/mesa-blake3.h"

struct path_case {
   const char *name;
   const wchar_t *component;
   size_t length;
   bool custom;
   bool prefixed;
   bool relative;
};

static const struct path_case cases[] = {
   {"short_default", L"short", 0, false, false, false},
   {"short_custom", L"custom", 0, true, false, false},
   {"long_filename", L"middle", 160, true, false, false},
   {"deep_default", L"deep", 295, false, false, false},
   {"deep_unicode_custom", L"\u7f13\u5b58", 295, true, false, false},
   {"extended_trailing", L"explicit", 284, true, true, false},
   {"relative_parent", L".\\discard\\..\\relative", 285, true, false, true},
   {"parent_is_file", L"blocker.dat\\child", 0, false, false, false},
};

static bool build_path(wchar_t *path, size_t cap, const wchar_t *root,
                       const struct path_case *test)
{
   const wchar_t *prefix = test->prefixed ? L"\\\\?\\" : L"";
   int n = test->relative ? swprintf(path, cap, L"%ls", test->component)
                          : swprintf(path, cap, L"%ls%ls\\%ls", prefix, root, test->component);
   if (n <= 0 || (size_t)n >= cap)
      return false;
   size_t used = n;
   while (used < test->length) {
      size_t remaining = test->length - used;
      if (remaining < 2 || used + remaining + 2 >= cap)
         return false;
      path[used++] = L'\\';
      size_t chars = remaining > 49 ? 47 : remaining - 1;
      for (size_t j = 0; j < chars; j++)
         path[used++] = L'\u6d4b';
   }
   if (test->prefixed)
      path[used++] = L'\\';
   path[used] = 0;
   return true;
}

int main(int argc, char **argv)
{
   if (argc != 2 || (strcmp(argv[1], "write") && strcmp(argv[1], "read")))
      return 2;
   const bool write = !strcmp(argv[1], "write");
   wchar_t root[512];
   DWORD root_length = GetEnvironmentVariableW(L"SWITCH_CACHE_CONTROL_ROOT", root, 512);
   if (!root_length || root_length >= 512 || !SetCurrentDirectoryW(root))
      return 3;
   for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
      const struct path_case *test = &cases[n];
      wchar_t path[1536];
      if (!build_path(path, 1536, root, test) ||
          !SetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", path))
         return 4;
      struct disk_cache *cache = test->custom
         ? disk_cache_create_custom("GM20B", "path-control", 0, "bounded", 8192)
         : disk_cache_create("GM20B", "path-control", 0);
      if (!cache)
         return 5;
      cache_key key;
      unsigned char bytes[1152], digest[32];
      for (size_t i = 0; i < sizeof(bytes); i++)
         bytes[i] = (unsigned char)(1 + (n * 17 + i * 19) % 251);
      disk_cache_compute_key(cache, test->name, strlen(test->name), key);
      if (write) {
         disk_cache_put(cache, key, bytes, sizeof(bytes), NULL);
         disk_cache_wait_for_idle(cache);
      }
      size_t actual = 0;
      void *got = disk_cache_get(cache, key, &actual);
      const bool match = got && actual == sizeof(bytes) && !memcmp(got, bytes, actual);
      if (got && !match) {
         free(got);
         disk_cache_destroy(cache);
         return 6;
      }
      _mesa_blake3_compute(bytes, sizeof(bytes), digest);
      printf("%s,%s,%zu,%u,%zu,", test->name, argv[1], wcslen(path), (unsigned)match, actual);
      for (size_t i = 0; i < sizeof(digest); i++)
         printf("%02x", digest[i]);
      putchar('\n');
      free(got);
      disk_cache_destroy(cache);
   }
   return 0;
}
