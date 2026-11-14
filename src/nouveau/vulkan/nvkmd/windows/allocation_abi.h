/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_ALLOCATION_ABI_H
#define TEGRA_GPU_ALLOCATION_ABI_H
#include <stdint.h>

#define TG_GPU_ALLOCATION_ABI 1u
#define TG_GPU_ALLOCATION_CPU_VISIBLE 1u
#define TG_GPU_ALLOCATION_DISPLAY_PRIMARY 2u
#define TG_GPU_ALLOCATION_LINEAR_SURFACE 4u
/* Shared KMD/UMD description of GPU storage. DISPLAY_PRIMARY selects the
 * fixed native scanout format and contiguous VidMm segment. No request
 * supplies an address. GPU VA alignment is chosen when mapping the allocation.
 * Host-visible backing is initially write-combined; cached/coherent memory
 * types are not advertised by this request.
 */
struct tg_gpu_allocation_request {
    uint32_t version, flags;
    uint64_t bytes;
};
_Static_assert(sizeof(struct tg_gpu_allocation_request)==16,"allocation ABI");

/* Explicit pitch-linear primary/shadow layout. Format and Rotation use the
 * numeric D3DDDIFORMAT and D3DDDI_ROTATION values; neither is a raw display
 * register value. Width/Height/Pitch and Rotation describe allocation storage.
 * ModeRotation is the primary's display-mode rotation for DescribeAllocation,
 * independent of storage: NONPREROTATED pixels still scan out in portrait.
 * Zero retains the old descriptor's Rotation value. Shadows leave it zero.
 * This uses the last formerly reserved DWORD without changing the size;
 * older KMDs reject nonzero values instead of misinterpreting the layout.
 * The old 32-byte portrait descriptor ended with a zero reserved word and
 * is still accepted by the KMD. New producers supply all 48 bytes. */
#define TG_GPU_PRIMARY_LEGACY_BYTES 32u
struct tg_gpu_primary_allocation_request {
    struct tg_gpu_allocation_request Buffer;
    uint32_t Format;
    struct { uint32_t Numerator, Denominator; } RefreshRate;
    uint32_t Rotation;
    uint32_t Width, Height, Pitch, ModeRotation;
};
_Static_assert(sizeof(struct tg_gpu_primary_allocation_request)==48,"primary layout ABI");

/* Existing linear-surface ABI: 128-byte rows, four-row storage height,
 * and page-rounded backing. Format uses D3DDDIFORMAT values. */
struct tg_gpu_linear_allocation_request {
    struct tg_gpu_allocation_request Buffer;
    uint32_t Format, Width, Height, Pitch;
};
_Static_assert(sizeof(struct tg_gpu_linear_allocation_request)==32,"linear surface ABI");

/* Resource-level data precedes the UMD's image/layout descriptor. VidMm
 * retains that descriptor for OpenResource; the KMD validates only the
 * storage envelope. This is one atomic allocation set, as required when
 * creating a shared D3D resource. Appending allocations would invalidate
 * both its storage size and the retained layout descriptor.
 *
 * allocation_bytes is the sum of page-rounded allocation sizes. All fields
 * are values: the UMD payload must contain no process-local pointers or
 * handles. A resource without private data keeps the original buffer ABI.
 */
#define TG_GPU_RESOURCE_ABI 1u
struct tg_gpu_resource_request {
    uint32_t version, header_bytes;
    uint32_t allocation_count, driver_data_bytes;
    uint64_t allocation_bytes;
    uint32_t reserved[2];
};
_Static_assert(sizeof(struct tg_gpu_resource_request)==32,"resource ABI");
#endif
