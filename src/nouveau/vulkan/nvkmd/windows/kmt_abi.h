/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_KMT_ABI_H
#define TG_GPU_KMT_ABI_H
#include <stddef.h>
#include <stdint.h>

/* The public Windows KMT query ABI from d3dkmthk.h for 64-bit (ARM64,
 * ARM64EC, x64) and 32-bit WOW64 (x86) user-mode callers. On _WIN32,
 * d3dukmdt.h uses native pointer widths in D3DKMT_* structures and WOW64
 * thunks pointer-bearing KMT packets to the ARM64 kernel. Kept separate from
 * Windows SDK headers because mixing those with MinGW's windows.h changes
 * unrelated Win32 declarations; test-kmt-sdk.c checks every consumed field
 * against the real SDK across all supported architectures.
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
_Static_assert(sizeof(void *) == 8 || sizeof(void *) == 4, "Windows 32/64-bit KMT ABI");
_Static_assert(sizeof(struct tg_kmt_adapter) == 20, "KMT adapter ABI");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(struct tg_kmt_enum) == 24, "64-bit KMT enum ABI");
_Static_assert(sizeof(struct tg_kmt_query) == 24, "64-bit KMT query ABI");
#else
_Static_assert(sizeof(struct tg_kmt_enum) == 16, "32-bit WOW64 KMT enum ABI");
_Static_assert(sizeof(struct tg_kmt_query) == 16, "32-bit WOW64 KMT query ABI");
#endif
#endif
