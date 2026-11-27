/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_KMT_H
#define TG_GPU_KMT_H
#include <windows.h>
#include "kmt_abi.h"
#include "adapter_abi.h"

struct tg_kmt_api {
   LONG (WINAPI *enumerate)(const struct tg_kmt_enum *);
   LONG (WINAPI *query)(const struct tg_kmt_query *);
   LONG (WINAPI *close)(const struct tg_kmt_close *);
};
struct tg_kmt_scan {
   uint32_t enumerated, queried, matched, native_ready;
   LONG close_status;
   struct tg_gpu_adapter_info first_match, first_ready;
};
/* A successful scan owns no adapter handles. Unsupported adapters/ABIs are
 * skipped; OS enumeration/allocation/close failures are returned unchanged.
 */
LONG tg_gpu_kmt_scan(const struct tg_kmt_api *, struct tg_kmt_scan *);
LONG tg_gpu_kmt_scan_system(struct tg_kmt_scan *);
#endif
