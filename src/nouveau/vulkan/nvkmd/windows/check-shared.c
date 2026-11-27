/* SPDX-License-Identifier: MIT */
/* Live KMT allocation sharing. This test does not submit GPU drawing work. */
#include "kmt_native.h"
#include "allocation_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LONG
record(const char *operation, LONG status)
{
   printf("%s status=0x%08lx\n", operation, (unsigned long)status);
   return status;
}

static bool
readback(const void *cpu, uint32_t words, uint32_t pattern, const char *phase)
{
   MemoryBarrier();
   for (uint32_t i = 0; i < words; ++i) {
      const uint32_t actual = ((const volatile uint32_t *)cpu)[i];
      if (actual != (pattern ^ i)) {
         fprintf(stderr, "%s: word %lu expected=%08lx actual=%08lx\n", phase,
                 (unsigned long)i, (unsigned long)(pattern ^ i), (unsigned long)actual);
         return false;
      }
   }
   printf("READBACK matched %lu words %s\n", (unsigned long)words, phase);
   return true;
}

int main(int argc, char **argv)
{
   if (argc != 2 || strlen(argv[1]) != 16 ||
       strspn(argv[1], "0123456789abcdefABCDEF") != 16) {
      fprintf(stderr, "usage: nvk-windows-shared-check.exe <16-hex-digit-GM20B-LUID>\n");
      return 1;
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   const uint64_t wanted = strtoull(argv[1], NULL, 16);
   struct tg_kmt_scan scan;
   LONG status = record("scan", tg_gpu_kmt_scan_system(&scan));
   if (status < 0 || scan.matched != 1 ||
       ((uint64_t)(uint32_t)scan.first_match.luid_high << 32 | scan.first_match.luid_low) != wanted) {
      fprintf(stderr, "The requested live GM20B LUID was not found.\n");
      return 2;
   }
   printf("GM20B luid=%08lx%08lx budget=%llu\n",
          (unsigned long)scan.first_match.luid_high, (unsigned long)scan.first_match.luid_low,
          (unsigned long long)scan.first_match.memory_budget_bytes);

   struct tg_kmt_device producer = {0}, consumer = {0};
   struct tg_kmt_allocation original = {0}, imported = {0};
   struct tg_kmt_va source_va = {0}, imported_va = {0};
   HANDLE shared = NULL;
   uint32_t source_context = 0, imported_context = 0;
   void *source_cpu = NULL, *imported_cpu = NULL;
   bool passed = false;
   const uint32_t words = 4096, pattern = 0x7d38a165;
#define REQUIRE(call) do { status = record(#call, (call)); if (status < 0) goto cleanup; } while (0)
   REQUIRE(tg_kmt_open_device(&producer, &scan.first_match));
   REQUIRE(tg_kmt_open_device(&consumer, &scan.first_match));
   REQUIRE(tg_kmt_create_context(&producer, &source_context));
   REQUIRE(tg_kmt_create_context(&consumer, &imported_context));
   REQUIRE(tg_kmt_allocate_shared(&producer, words * sizeof(uint32_t), true, &original));
   REQUIRE(tg_kmt_reserve(&producer, original.size, TG_KMT_PAGE_SIZE, 0, &source_va));
   REQUIRE(tg_kmt_map(&producer, &source_va, 0, &original, 0, original.size, 0, false));
   REQUIRE(tg_kmt_lock(&producer, &original, &source_cpu));
   /* Aligned scalar accesses are suitable for Tegra write-combined maps. */
   for (uint32_t i = 0; i < words; ++i)
      ((volatile uint32_t *)source_cpu)[i] = pattern ^ i;
   MemoryBarrier();
   REQUIRE(tg_kmt_unlock(&producer, &original));
   source_cpu = NULL;
   REQUIRE(tg_kmt_export_memory(&original, &shared));
   REQUIRE(tg_kmt_import_memory(&consumer, shared, &imported));
   printf("handles producer device=%08lx resource=%08lx allocation=%08lx; "
          "consumer device=%08lx resource=%08lx allocation=%08lx\n",
          (unsigned long)producer.handle, (unsigned long)original.resource,
          (unsigned long)original.handle, (unsigned long)consumer.handle,
          (unsigned long)imported.resource, (unsigned long)imported.handle);
   if (imported.size != original.size || imported.flags != TG_GPU_ALLOCATION_CPU_VISIBLE)
      goto cleanup;
   REQUIRE(tg_kmt_reserve(&consumer, imported.size, TG_KMT_PAGE_SIZE, 0, &imported_va));
   REQUIRE(tg_kmt_map(&consumer, &imported_va, 0, &imported, 0, imported.size, 0, false));

   REQUIRE(tg_kmt_lock(&consumer, &imported, &imported_cpu));
   if (!readback(imported_cpu, words, pattern, "before producer release"))
      goto cleanup;
   REQUIRE(tg_kmt_unlock(&consumer, &imported));
   imported_cpu = NULL;

   REQUIRE(tg_kmt_free_va(&producer, &source_va));
   REQUIRE(tg_kmt_free(&producer, &original));
   REQUIRE(tg_kmt_lock(&consumer, &imported, &imported_cpu));
   if (!readback(imported_cpu, words, pattern, "after producer release"))
      goto cleanup;
   REQUIRE(tg_kmt_unlock(&consumer, &imported));
   imported_cpu = NULL;

   /* Only the importer's local resource remains for the final readback. */
   if (!CloseHandle(shared))
      goto cleanup;
   shared = NULL;
   REQUIRE(tg_kmt_lock(&consumer, &imported, &imported_cpu));
   if (!readback(imported_cpu, words, pattern, "after NT-handle close"))
      goto cleanup;
   passed = true;

cleanup:
#undef REQUIRE
#define RELEASE(call) do { if (record(#call, (call)) < 0) passed = false; } while (0)
   if (source_cpu)
      RELEASE(tg_kmt_unlock(&producer, &original));
   if (imported_cpu)
      RELEASE(tg_kmt_unlock(&consumer, &imported));
   if (source_va.reserved_size)
      RELEASE(tg_kmt_free_va(&producer, &source_va));
   if (imported_va.reserved_size)
      RELEASE(tg_kmt_free_va(&consumer, &imported_va));
   if (original.handle || original.resource)
      RELEASE(tg_kmt_free(&producer, &original));
   if (imported.handle || imported.resource)
      RELEASE(tg_kmt_free(&consumer, &imported));
   if (shared && !CloseHandle(shared))
      passed = false;
   if (source_context)
      RELEASE(tg_kmt_destroy_context(source_context));
   if (imported_context)
      RELEASE(tg_kmt_destroy_context(imported_context));
   printf("remaining budgets producer=%llu consumer=%llu\n",
          (unsigned long long)producer.allocated, (unsigned long long)consumer.allocated);
   if (producer.allocated || consumer.allocated)
      passed = false;
   if (producer.adapter)
      RELEASE(tg_kmt_close_device(&producer));
   if (consumer.adapter)
      RELEASE(tg_kmt_close_device(&consumer));
#undef RELEASE
   puts(passed ? "PASS: live KMT shared storage and CPU readback" : "FAIL: live KMT shared storage");
   return passed ? 0 : 3;
}
