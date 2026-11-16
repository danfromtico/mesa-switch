/* SPDX-License-Identifier: MIT */
/* Query the retained adapter-start snapshot. No devices, contexts, channels,
 * firmware, DMA buffers or live MMIO accesses are created by this program. */
#include "kmt.h"
#include "nvdec_abi.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int
matches(const struct tg_gpu_adapter_info *i, const struct tg_kmt_luid *luid)
{
   return i->version == TG_GPU_ADAPTER_ABI && i->size == sizeof(*i) &&
          !i->reserved && (i->flags & TG_GPU_ADAPTER_LUID_VALID) &&
          !(i->flags & ~(TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT)) &&
          i->vendor == 0x10de && i->chipset == 0x12e && i->sm == 53 &&
          i->gr_class == 0xb197 && i->memory_budget_bytes == TG_GPU_MEMORY_BUDGET_BYTES &&
          (luid->low || luid->high) && i->luid_low == luid->low && i->luid_high == luid->high;
}

int
main(void)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!gdi) return 2;
   const struct tg_kmt_api api = {
      .enumerate = (void *)GetProcAddress(gdi, "D3DKMTEnumAdapters3"),
      .query = (void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"),
      .close = (void *)GetProcAddress(gdi, "D3DKMTCloseAdapter"),
   };
   if (!api.enumerate || !api.query || !api.close) {
      FreeLibrary(gdi);
      return 2;
   }
   struct tg_kmt_enum e = {.filter = TG_KMT_INCLUDE_COMPUTE_ONLY};
   LONG status = api.enumerate(&e), close_status = 0;
   if (status < 0 || !e.count || e.count > 4096) {
      fprintf(stderr, "KMT count query: status=%08" PRIx32 ", count=%u\n", (uint32_t)status, e.count);
      FreeLibrary(gdi);
      return 3;
   }
   const uint32_t capacity = e.count;
   struct tg_kmt_adapter *adapters = calloc(capacity, sizeof(*adapters));
   if (!adapters) { FreeLibrary(gdi); return 4; }
   e.adapters = adapters;
   status = api.enumerate(&e);
   unsigned matched = 0, supported = 0;
   printf("{\"scope\":\"adapter-start resource snapshot; not live DMA readiness\","
          "\"decode_tested\":false,\"adapters\":[");
   if (status >= 0) {
      uint32_t count = e.count;
      if (count > capacity) { count = capacity; status = (LONG)0xc000000d; }
      for (uint32_t n = 0; n < count; ++n) {
         if (!adapters[n].handle) { status = (LONG)0xc000000d; continue; }
         struct tg_gpu_adapter_info base = {.version = TG_GPU_ADAPTER_ABI, .size = sizeof(base)};
         struct tg_kmt_query q = {.handle = adapters[n].handle, .type = TG_KMT_UMDRIVERPRIVATE,
                                 .data = &base, .bytes = sizeof(base)};
         if (api.query(&q) >= 0 && matches(&base, &adapters[n].luid)) {
            struct tg_gpu_adapter_nvdec_clients_info info = {
               .resource.base = {.version = TG_GPU_ADAPTER_ABI, .size = sizeof(base)},
            };
            q.data = &info; q.bytes = sizeof(info);
            LONG queried = api.query(&q);
            const struct tg_nvdec_resource_info *s = &info.resource.nvdec;
            const struct tg_nvdec_client_info *c = &info.clients;
            const int available = queried >= 0 && matches(&info.resource.base, &adapters[n].luid) &&
               s->version == TG_NVDEC_QUERY_ABI && s->size == sizeof(*s);
            const int clients_available = available &&
               c->version == TG_NVDEC_CLIENT_QUERY_ABI && c->size == sizeof(*c);
            supported += available;
            printf("%s{\"luid\":\"%08" PRIx32 "%08" PRIx32 "\","
                   "\"query_status\":\"%08" PRIx32 "\",\"snapshot_available\":%s,"
                   "\"version\":%u,\"size\":%u,\"flags\":%u,\"resource_mask\":%u,"
                   "\"resource_status\":%u,\"map_status\":\"%08" PRIx32 "\","
                   "\"hidrev\":\"%08" PRIx32 "\",\"powergate_status\":\"%08" PRIx32 "\","
                   "\"clock_y\":\"%08" PRIx32 "\",\"reset_y\":\"%08" PRIx32 "\","
                   "\"nvdec_clock_source\":\"%08" PRIx32 "\",\"nvjpg_clock_source\":\"%08" PRIx32 "\","
                   "\"smmu_config\":\"%08" PRIx32 "\",\"smmu_host1x_asid\":\"%08" PRIx32 "\","
                   "\"smmu_nvdec_asid\":\"%08" PRIx32 "\",\"smmu_nvjpg_asid\":\"%08" PRIx32 "\","
                   "\"clients_available\":%s,\"client_flags\":%u,"
                   "\"smmu_translation_enable0\":\"%08" PRIx32 "\","
                   "\"smmu_translation_enable1\":\"%08" PRIx32 "\","
                   "\"smmu_translation_enable3\":\"%08" PRIx32 "\","
                   "\"mc_flush\":\"%08" PRIx32 "\",\"mc_flush_status\":\"%08" PRIx32 "\","
                   "\"pmc_clamp_status\":\"%08" PRIx32 "\",\"pmc_toggle\":\"%08" PRIx32 "\"}",
                   matched++ ? "," : "", (uint32_t)adapters[n].luid.high, adapters[n].luid.low,
                   (uint32_t)queried, available ? "true" : "false", s->version, s->size, s->flags,
                   s->resource_mask, s->status, s->map_status, s->hidrev, s->powergate_status,
                   s->clock_y, s->reset_y, s->nvdec_clock_source, s->nvjpg_clock_source,
                   s->smmu_config, s->smmu_host1x_asid, s->smmu_nvdec_asid, s->smmu_nvjpg_asid,
                   clients_available ? "true" : "false", c->flags,
                   c->smmu_translation_enable0, c->smmu_translation_enable1,
                   c->smmu_translation_enable3, c->mc_flush, c->mc_flush_status,
                   c->pmc_clamp_status, c->pmc_toggle);
         }
         const struct tg_kmt_close c = {.handle = adapters[n].handle};
         LONG closed = api.close(&c);
         if (closed < 0 && close_status >= 0) close_status = closed;
      }
   }
   printf("],\"enumerate_status\":\"%08" PRIx32 "\",\"close_status\":\"%08" PRIx32 "\","
          "\"matching_adapters\":%u,\"supported_queries\":%u}\n",
          (uint32_t)status, (uint32_t)close_status, matched, supported);
   free(adapters);
   FreeLibrary(gdi);
   return status < 0 || close_status < 0 ? 5 : (supported == 1 && matched == 1 ? 0 : 6);
}
