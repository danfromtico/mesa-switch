/* SPDX-License-Identifier: MIT */
#include "kmt.h"
#include <assert.h>
#include <stdio.h>

static unsigned mode, calls, closes, queries;
static LONG WINAPI enumerate(const struct tg_kmt_enum *in)
{
   struct tg_kmt_enum *e = (void *)in;
   calls++;
   assert(e->filter == TG_KMT_INCLUDE_COMPUTE_ONLY);
   if (mode == 20) { e->count = UINT32_MAX; return (LONG)0xc0000001; }
   if (!e->adapters) { e->count = mode == 21 ? 0 : 3; return 0; }
   assert(e->count == 3);
   if (mode == 22) { e->count = UINT32_MAX; return (LONG)0xc0000023; }
   e->adapters[0] = (struct tg_kmt_adapter){ .handle = 1, .luid = {1,2} };
   e->adapters[1] = (struct tg_kmt_adapter){ .handle = 2, .luid = {3,4} };
   e->adapters[2] = (struct tg_kmt_adapter){ .handle = 3, .luid = {5,6} };
   return 0;
}
static LONG WINAPI query(const struct tg_kmt_query *q)
{
   queries++;
   assert(q->type == TG_KMT_UMDRIVERPRIVATE && q->bytes == sizeof(struct tg_gpu_adapter_info));
   struct tg_gpu_adapter_info *i = q->data;
   assert(i->version == TG_GPU_ADAPTER_ABI && i->size == sizeof(*i));
   if (q->handle != 2) return (LONG)0xc00000bb;
   *i = (struct tg_gpu_adapter_info){
      .version = TG_GPU_ADAPTER_ABI, .size = sizeof(*i), .flags = TG_GPU_ADAPTER_LUID_VALID,
      .memory_budget_bytes = TG_GPU_MEMORY_BUDGET_BYTES, .vendor = 0x10de,
      .chipset = 0x12e, .sm = 53, .gr_class = 0xb197, .luid_low = 3, .luid_high = 4,
   };
   switch (mode) {
   case 1: i->flags |= TG_GPU_ADAPTER_NATIVE_SUBMIT; break;
   case 2: i->version++; break;
   case 3: i->size--; break;
   case 4: i->reserved = 1; break;
   case 5: i->flags = TG_GPU_ADAPTER_NATIVE_SUBMIT; break;
   case 6: i->flags |= 4; break;
   case 7: i->vendor++; break;
   case 8: i->chipset++; break;
   case 9: i->sm++; break;
   case 10: i->gr_class++; break;
   case 11: i->memory_budget_bytes *= 2; break;
   case 12: i->luid_low++; break;
   case 13: i->luid_high++; break;
   }
   return 0;
}
static LONG WINAPI close_adapter(const struct tg_kmt_close *c)
{
   assert(c->handle == ++closes);
   return mode == 23 && c->handle == 2 ? (LONG)0xc0000001 : 0;
}
int main(void)
{
   const struct tg_kmt_api api = {enumerate, query, close_adapter};
   struct tg_kmt_scan scan;
   for (mode = 0; mode < 14; mode++) {
      calls = closes = queries = 0;
      assert(tg_gpu_kmt_scan(&api, &scan) == 0);
      assert(calls == 2 && closes == 3 && queries == 3 && scan.enumerated == 3 && scan.queried == 1);
      assert(scan.matched == (mode < 2) && scan.native_ready == (mode == 1) && !scan.close_status);
   }
   for (mode = 20; mode <= 23; mode++) {
      calls = closes = queries = 0;
      LONG r = tg_gpu_kmt_scan(&api, &scan);
      assert((mode == 21) == (r == 0));
      if (mode < 23) assert(!closes && !queries && !scan.enumerated);
      else assert(closes == 3 && queries == 3 && scan.close_status == r);
   }
   assert(tg_gpu_kmt_scan(NULL, &scan) < 0 && tg_gpu_kmt_scan(&api, NULL) < 0);
   puts("KMT controlled transport: 20 cases passed; no real adapter/GPU model");
   return 0;
}
