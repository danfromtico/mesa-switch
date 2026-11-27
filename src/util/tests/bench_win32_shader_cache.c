/* SPDX-License-Identifier: MIT */
/* Compare the same Win32 cache directory across builds. Pass an isolated,
 * existing directory as MESA_SHADER_CACHE_DIR and run --seed once first. */
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/disk_cache.h"

#define ENTRIES 768
#define OPEN_SAMPLES 8
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed at line %d\n", __LINE__); return 1; } } while (0)

static uint64_t
ticks(void)
{
   LARGE_INTEGER time;
   QueryPerformanceCounter(&time);
   return (uint64_t)time.QuadPart;
}

static struct disk_cache *
open_cache(void)
{
   return disk_cache_create_custom("GM20B-cache-bench", "1", 0,
                                   "bench_win32_cache", 32 * 1024 * 1024);
}

int
main(int argc, char **argv)
{
   const bool seed = argc == 2 && !strcmp(argv[1], "--seed");
   if (argc != 2 || (!seed && strcmp(argv[1], "--measure")) ||
       !GetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", NULL, 0)) {
      fprintf(stderr, "Set MESA_SHADER_CACHE_DIR to a test directory and use --seed or --measure\n");
      return 2;
   }
   LARGE_INTEGER freq;
   CHECK(QueryPerformanceFrequency(&freq));
   struct disk_cache *cache = open_cache();
   CHECK(cache);
   cache_key present[ENTRIES], missing[ENTRIES];
   for (uint32_t i = 0; i < ENTRIES; i++) {
      uint32_t absent = i + ENTRIES;
      disk_cache_compute_key(cache, &i, sizeof(i), present[i]);
      disk_cache_compute_key(cache, &absent, sizeof(absent), missing[i]);
      if (seed)
         disk_cache_put(cache, present[i], &i, sizeof(i), NULL);
   }
   if (seed) {
      disk_cache_wait_for_idle(cache);
      disk_cache_destroy(cache);
      printf("seeded %d\n", ENTRIES);
      return 0;
   }
   const uint64_t start = ticks();
   for (uint32_t i = 0; i < ENTRIES; i++) {
      size_t size = 1;
      CHECK(!disk_cache_get(cache, missing[i], &size) && size == 0);
   }
   const uint64_t misses_end = ticks();
   for (uint32_t i = 0; i < ENTRIES; i++) {
      size_t size = 0;
      void *blob = disk_cache_get(cache, present[i], &size);
      CHECK(blob && size == sizeof(i) && !memcmp(blob, &i, size));
      free(blob);
   }
   const uint64_t hits_end = ticks();
   disk_cache_destroy(cache);
   printf("CACHE qpc=%" PRIu64 " entries=%u misses=%" PRIu64 " hits=%" PRIu64 "\n",
          (uint64_t)freq.QuadPart, (unsigned)ENTRIES, misses_end - start, hits_end - misses_end);
   for (unsigned i = 0; i < OPEN_SAMPLES; i++) {
      uint64_t before = ticks();
      cache = open_cache();
      uint64_t elapsed = ticks() - before;
      CHECK(cache);
      disk_cache_destroy(cache);
      printf("OPEN %u %" PRIu64 "\n", i, elapsed);
   }
   return 0;
}
