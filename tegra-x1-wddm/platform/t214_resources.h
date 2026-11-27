/* SPDX-License-Identifier: MIT */
/* Shared by the HDH-001 ACPI table and the Windows render backend. */
#ifndef TEGRA_GPU_T214_RESOURCES_H
#define TEGRA_GPU_T214_RESOURCES_H

#define T214_GPU_ACPI_ID          "SWOA0F01"
#define T214_GPU_REG_BASE         0x57000000
#define T214_GPU_REG_SIZE         0x01000000
#define T214_GPU_BAR1_BASE        0x58000000
#define T214_GPU_BAR1_SIZE        0x01000000
#define T214_GPU_AUX_BASE         0x538F0000
#define T214_GPU_AUX_SIZE         0x00001000
/* Tegra GIC SPI 157/158, plus the 32 private interrupts for ACPI GSIs. */
#define T214_GPU_STALL_GSI        189
#define T214_GPU_NONSTALL_GSI     190
/* Optional CPU-side LIC forwarding for SPI 157 (stall) / SPI 158 (nonstall).
 * Only GPU bits 29 and 30 are owned in LIC bank 4 (FIR, FIR_SET/CLR, IER,
 * IER_SET/CLR and IRQ/FIQ class).
 */
#define T214_GPU_LIC_BASE         0x60004414
#define T214_GPU_LIC_SIZE         0x0000001C
#define T214_GPU_LIC_STALL        0x20000000u
#define T214_GPU_LIC_NONSTALL     0x40000000u

/* Hekate IPL scanout on HDH-001: 720 x 1280 x 4 bytes, pitch 2880. */
#define T214_GPU_POST_FB_BASE     0xF5A00000
#define T214_GPU_POST_FB_SIZE     0x00384000
/* Display A, including its direct Window A registers. */
#define T214_GPU_DC_BASE          0x54200000
#define T214_GPU_DC_SIZE          0x00004000
#define T214_GPU_DC_GSI           105

/* Narrow GPU power/clock registers, excluding the fan owner's CPU CAR
 * block. PLLP is read through that owner; PMIC access uses its SPB target.
 */
#define T214_GPU_POWER_RESOURCES(X) \
    X(0x60006280, 0x18) \
    X(0x6000667C, 0x04) \
    X(0x7000E6D4, 0x04) \
    X(0x70019970, 0x08) \
    X(0x70019AA8, 0x08)
#define T214_GPU_ID_RESOURCES(X) \
    X(0x70000804, 0x04) \
    X(0x7000F930, 0x04) \
    X(0x7000FB04, 0x04)
#define T214_GPU_EXTRA_RESOURCES(X) T214_GPU_POWER_RESOURCES(X) T214_GPU_ID_RESOURCES(X)
#define T214_GPU_RESOURCE_COUNT 11

/* Optional decode bundle, assigned to the same GPU0 PnP device. CAR L belongs
 * to FAN0 and is deliberately excluded. MC flush is already owned above.
 * SMMU views are read-only until an actual translation contract is selected.
 * Fields: name, physical base, length, writable. */
#define T214_NVDEC_RESOURCES(X) \
    X(Registers, 0x54480000, 0x00040000, 1) \
    X(Host1x,    0x50000000, 0x00040000, 1) \
    X(Pmc,       0x7000E42C, 0x00000010, 1) \
    X(CarY,      0x60006298, 0x00000018, 1) \
    X(CarSource, 0x60006698, 0x00000008, 1) \
    X(Smmu,      0x70019010, 0x00000004, 0) \
    X(HcAsid,    0x70019250, 0x00000004, 0) \
    X(NvdecAsid, 0x70019AB4, 0x00000004, 0) \
    X(NvjpgAsid, 0x70019AC0, 0x00000004, 0) \
    X(SmmuEn01,  0x70019228, 0x00000008, 0) \
    X(SmmuEn3,   0x70019234, 0x00000004, 0) \
    X(HostSource,0x60006180, 0x00000004, 0)
#define T214_NVDEC_RESOURCE_COUNT 12

#endif
