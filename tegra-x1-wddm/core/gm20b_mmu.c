/* SPDX-License-Identifier: MIT */
/* GM20B entry formats adapted from Linux nouveau v6.12:
 * vmmgf100.c, vmmgk20a.c, vmmgm200.c and vmmgm20b.c.
 * Copyright 2017 Red Hat Inc. See ../NOTICE for the upstream MIT notice.
 * Copyright 2026 ruje0504-2
 */
#include "gm20b_mmu.h"

static bool
page_address_valid(uint64_t address)
{
    return address < TG_GPU_VA_LIMIT && (address & 0xfffu) == 0;
}

bool
tg_gpu_va_indices(uint64_t va, struct tg_gpu_va_indices *indices)
{
    if (!indices || !page_address_valid(va))
        return false;
    indices->directory = (uint32_t)(va >> TG_GPU_DIRECTORY_SHIFT);
    indices->small_page = (uint32_t)((va >> TG_GPU_PAGE_SHIFT) & 0x3fffu);
    return true;
}

bool
tg_gpu_pte_kind_supported(uint64_t kind)
{
    /* Uncompressed kinds used by NVK for GM20B 4 KiB mappings. */
    switch (kind) {
    case TG_GPU_PTE_KIND_PITCH:
    case TG_GPU_PTE_KIND_Z16:
    case TG_GPU_PTE_KIND_S8Z24:
    case TG_GPU_PTE_KIND_S8:
    case TG_GPU_PTE_KIND_Z24S8:
    case TG_GPU_PTE_KIND_ZF32:
    case TG_GPU_PTE_KIND_ZF32_X24S8:
    case TG_GPU_PTE_KIND_GENERIC_16BX2:
        return true;
    default:
        return false;
    }
}

bool
tg_gpu_pte_4k_kind(uint64_t gpu_address, bool read_only, bool gpu_uncached,
                   uint8_t kind, uint64_t *pte)
{
    if (!pte || !page_address_valid(gpu_address) || !tg_gpu_pte_kind_supported(kind))
        return false;

    /* gk20a_vmm_aper(NCOH) returns 0 on GM20B. Using the discrete-GPU
     * SYS_NCOH aperture (3 << 33) here would address the wrong aperture.
     * Maxwell's kind occupies bits 43:36. VOL bypasses the GPU cache.
     */
    *pte = (gpu_address >> 8) | UINT64_C(1) |
           ((uint64_t)read_only << 2) | ((uint64_t)gpu_uncached << 32) |
           ((uint64_t)kind << 36);
    return true;
}

bool
tg_gpu_pte_4k(uint64_t gpu_address, bool read_only, bool gpu_uncached,
              uint64_t *pte)
{
    return tg_gpu_pte_4k_kind(gpu_address, read_only, gpu_uncached, TG_GPU_PTE_KIND_PITCH, pte);
}

bool
tg_gpu_pde_small(uint64_t table_gpu_address, uint64_t *pde)
{
    if (!pde || !page_address_valid(table_gpu_address))
        return false;

    /* Switchroot GM20B has honors_aperture=false: SoC DRAM uses the
     * VIDMEM selector (1 for a PDE), plus VOL (4). NCOH=3 is the discrete
     * GPU encoding, not the result of nvgpu_aperture_mask on this SoC.
     * The lower half remains absent so the SPT is selected.
     */
    *pde = (table_gpu_address << 24) | (UINT64_C(5) << 32);
    return true;
}

bool
tg_gpu_decode_pde_small(uint64_t pde, uint64_t *table_gpu_address)
{
    const uint64_t address = (pde >> 24) & ~UINT64_C(0xfff);
    uint64_t expected;
    if (!table_gpu_address || !tg_gpu_pde_small(address, &expected) || pde != expected)
        return false;
    *table_gpu_address = address;
    return true;
}

bool
tg_gpu_pte_allows_pitch_access(uint64_t pte, enum tg_gpu_pitch_access access)
{
    const uint64_t address = (pte & UINT64_C(0xfffffff0)) << 8;
    const bool read_only = (pte & UINT64_C(4)) != 0;
    const bool gpu_uncached = (pte & (UINT64_C(1) << 32)) != 0;
    const enum tg_gpu_pitch_access protection = read_only ? TG_GPU_PITCH_READ_ONLY : TG_GPU_PITCH_WRITE;
    uint64_t expected;
    return (access == TG_GPU_PITCH_READ || access == protection) &&
           tg_gpu_pte_4k(address, read_only, gpu_uncached, &expected) && pte == expected;
}

bool
tg_gpu_instance_pdb(uint64_t directory_gpu_address, uint64_t *pdb)
{
    if (!pdb || !page_address_valid(directory_gpu_address))
        return false;

    /* RAMIN +0x200: VIDMEM target 0, volatile PDB (4), 64 KiB big pages
     * (bit 11). Match Switchroot's resolved GM20B aperture policy.
     * RAMIN +0x208 must separately contain the inclusive VA limit.
     */
    *pdb = directory_gpu_address | UINT64_C(0x804);
    return true;
}
