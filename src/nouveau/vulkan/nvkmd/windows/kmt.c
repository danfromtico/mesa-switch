/* SPDX-License-Identifier: MIT */
#include "kmt.h"
#include <stdlib.h>
#include <string.h>

#define TG_STATUS_INVALID_PARAMETER ((LONG)0xc000000d)
#define TG_STATUS_NO_MEMORY ((LONG)0xc0000017)
#define TG_STATUS_NOT_SUPPORTED ((LONG)0xc00000bb)

static int
matches(const struct tg_gpu_adapter_info *i, const struct tg_kmt_luid *luid)
{
   return i->version == TG_GPU_ADAPTER_ABI && i->size == sizeof(*i) &&
          !i->reserved && (i->flags & TG_GPU_ADAPTER_LUID_VALID) &&
          !(i->flags & ~(TG_GPU_ADAPTER_LUID_VALID | TG_GPU_ADAPTER_NATIVE_SUBMIT)) &&
          i->vendor == 0x10de && i->chipset == 0x12e &&
          i->sm == 53 && i->gr_class == 0xb197 &&
          i->memory_budget_bytes == TG_GPU_MEMORY_BUDGET_BYTES &&
          (luid->low || luid->high) &&
          i->luid_low == luid->low && i->luid_high == luid->high;
}

LONG
tg_gpu_kmt_scan(const struct tg_kmt_api *api, struct tg_kmt_scan *out)
{
   if (!out || !api || !api->enumerate || !api->query || !api->close)
      return TG_STATUS_INVALID_PARAMETER;
   memset(out, 0, sizeof(*out));
   struct tg_kmt_enum e = { .filter = TG_KMT_INCLUDE_COMPUTE_ONLY };
   LONG status = api->enumerate(&e);
   if (status < 0 || !e.count)
      return status;
   const uint32_t capacity = e.count;
   struct tg_kmt_adapter *adapters = calloc(capacity, sizeof(*adapters));
   if (!adapters)
      return TG_STATUS_NO_MEMORY;
   e.adapters = adapters;
   status = api->enumerate(&e);
   if (status >= 0) {
      /* NumAdapters is undefined on failure. Only successful enumeration
       * transfers ownership of returned handles to this caller.
       */
      uint32_t count = e.count;
      if (count > capacity) {
         count = capacity;
         status = TG_STATUS_INVALID_PARAMETER;
      }
      out->enumerated = count;
      for (uint32_t n = 0; n < count; n++) {
         if (!adapters[n].handle) {
            status = TG_STATUS_INVALID_PARAMETER;
            continue;
         }
         struct tg_gpu_adapter_info info = {
            .version = TG_GPU_ADAPTER_ABI, .size = sizeof(info),
         };
         const struct tg_kmt_query q = {
            .handle = adapters[n].handle, .type = TG_KMT_UMDRIVERPRIVATE,
            .data = &info, .bytes = sizeof(info),
         };
         if (api->query(&q) >= 0) {
            out->queried++;
            if (matches(&info, &adapters[n].luid)) {
               if (out->matched++ == 0)
                  out->first_match = info;
               if ((info.flags & TG_GPU_ADAPTER_NATIVE_SUBMIT) &&
                   out->native_ready++ == 0)
                  out->first_ready = info;
            }
         }
         const struct tg_kmt_close c = { .handle = adapters[n].handle };
         LONG closed = api->close(&c);
         if (closed < 0 && !out->close_status)
            out->close_status = closed;
      }
   }
   free(adapters);
   return status < 0 ? status : out->close_status;
}

LONG
tg_gpu_kmt_scan_system(struct tg_kmt_scan *out)
{
   if (!out)
      return TG_STATUS_INVALID_PARAMETER;
   memset(out, 0, sizeof(*out));
   HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!gdi)
      return TG_STATUS_NOT_SUPPORTED;
   struct tg_kmt_api api = {
      .enumerate = (void *)GetProcAddress(gdi, "D3DKMTEnumAdapters3"),
      .query = (void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"),
      .close = (void *)GetProcAddress(gdi, "D3DKMTCloseAdapter"),
   };
   LONG status = api.enumerate && api.query && api.close ?
      tg_gpu_kmt_scan(&api, out) : TG_STATUS_NOT_SUPPORTED;
   FreeLibrary(gdi);
   return status;
}
