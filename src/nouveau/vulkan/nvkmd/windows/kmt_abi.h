/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_KMT_ABI_H
#define TG_GPU_KMT_ABI_H
#include <stddef.h>
#include <stdint.h>

/* The public ARM64 KMT query ABI from d3dkmthk.h. Kept separate from the
 * Windows SDK headers because mixing those with MinGW's windows.h changes
 * unrelated Win32 declarations. test-kmt-sdk.c checks every consumed field
 * against the real SDK; these are not private kernel syscall declarations.
 */
struct tg_kmt_luid { uint32_t low; int32_t high; };
struct tg_kmt_adapter {
   uint32_t handle;
   struct tg_kmt_luid luid;
   uint32_t sources;
   int32_t precise_present_regions;
};
struct tg_kmt_enum { uint64_t filter; uint32_t count; struct tg_kmt_adapter *adapters; };
struct tg_kmt_query { uint32_t handle, type; void *data; uint32_t bytes; };
struct tg_kmt_close { uint32_t handle; };
#define TG_KMT_INCLUDE_COMPUTE_ONLY UINT64_C(1)
#define TG_KMT_UMDRIVERPRIVATE 0u
_Static_assert(sizeof(void *) == 8, "ARM64 KMT only");
_Static_assert(sizeof(struct tg_kmt_adapter) == 20, "KMT adapter ABI");
_Static_assert(sizeof(struct tg_kmt_enum) == 24, "KMT enum ABI");
_Static_assert(sizeof(struct tg_kmt_query) == 24, "KMT query ABI");
#endif
