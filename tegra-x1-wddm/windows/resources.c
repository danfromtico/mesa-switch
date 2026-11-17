/* SPDX-License-Identifier: MIT */
/* Copyright 2026 ruje0504-2 */
#include "resources.h"
#include "../platform/t214_dc.h"
#include "../platform/t214_resources.h"

static const ULONG TgGpuBases[TgGpuApertureCount] = {
    T214_GPU_REG_BASE, T214_GPU_BAR1_BASE, T214_GPU_AUX_BASE,
#define GPU_BASE(base, size) base,
    T214_GPU_EXTRA_RESOURCES(GPU_BASE)
#undef GPU_BASE
};
static const ULONG TgGpuSizes[TgGpuApertureCount] = {
    T214_GPU_REG_SIZE, T214_GPU_BAR1_SIZE, T214_GPU_AUX_SIZE,
#define GPU_SIZE(base, size) size,
    T214_GPU_EXTRA_RESOURCES(GPU_SIZE)
#undef GPU_SIZE
};
_Static_assert(TgGpuApertureCount == T214_GPU_RESOURCE_COUNT, "GPU resource count");
_Static_assert(T214_DC_WIN_A_ADDR_HI * sizeof(ULONG) < T214_GPU_DC_SIZE, "Display A aperture");
_Static_assert(T214_DC_PANEL_PITCH * T214_DC_PANEL_HEIGHT == T214_GPU_POST_FB_SIZE, "POST size");

static const struct { ULONG Base, Bytes; BOOLEAN Writable; } NvdecRanges[] = {
#define NVDEC_RANGE(name, base, size, writable) {base, size, writable},
    T214_NVDEC_RESOURCES(NVDEC_RANGE)
#undef NVDEC_RANGE
};
_Static_assert(TgNvdecApertureCount == T214_NVDEC_RESOURCE_COUNT, "NVDEC resources");
#define NVDEC_ALL_RESOURCES ((1u << TgNvdecApertureCount) - 1u)

static VOID map_nvdec_resources(const DXGKRNL_INTERFACE *Dxgk,
                                TG_GPU_RESOURCES *r)
{
    struct tg_nvdec_resource_info *s = &r->NvdecInfo;
    struct tg_nvdec_client_info *c = &r->NvdecClients;
    ULONG i;
    if (s->status == TG_NVDEC_RESOURCES_INVALID)
        return;
    if (s->resource_mask != NVDEC_ALL_RESOURCES) {
        s->status = s->resource_mask ? TG_NVDEC_RESOURCES_PARTIAL : TG_NVDEC_RESOURCES_ABSENT;
        return;
    }
    s->flags = TG_NVDEC_QUERY_RESOURCES_VALID;
    for (i = 0; i < TgNvdecApertureCount; i++) {
        PVOID mapping = NULL;
        NTSTATUS status = Dxgk->DxgkCbMapMemory(Dxgk->DeviceHandle,
            r->NvdecMemory[i].u.Memory.Start, NvdecRanges[i].Bytes,
            FALSE, FALSE, MmNonCached, &mapping);
        if (NT_SUCCESS(status) && !mapping)
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        if (!NT_SUCCESS(status)) {
            s->status = TG_NVDEC_RESOURCES_MAP_FAILED;
            s->map_status = (UINT32)status;
            /* Optional decode mapping failure does not tear down the working
             * render device. Retain any failed-unmap pointer for StopDevice. */
            while (i) {
                --i;
                if (NT_SUCCESS(Dxgk->DxgkCbUnmapMemory(Dxgk->DeviceHandle,
                                                      r->NvdecMapping[i])))
                    r->NvdecMapping[i] = NULL;
            }
            return;
        }
        r->NvdecMapping[i] = mapping;
    }
    s->flags |= TG_NVDEC_QUERY_MAPPED;
    /* Read only always-on identification, PMC, CAR and MC views. Do not read
     * the unpowered NVDEC aperture or an unleased Host1x clock domain. */
    s->hidrev = READ_REGISTER_ULONG((PULONG)r->Mapping[TgGpuHidrev]);
    s->powergate_status = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecPmc] + 3);
    c->pmc_clamp_status = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecPmc]);
    c->pmc_toggle = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecPmc] + 1);
    s->clock_y = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecCarY]);
    s->reset_y = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecCarY] + 3);
    s->nvdec_clock_source = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecCarSource]);
    s->nvjpg_clock_source = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecCarSource] + 1);
    s->smmu_config = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecSmmu]);
    s->smmu_host1x_asid = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecHcAsid]);
    s->smmu_nvdec_asid = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecNvdecAsid]);
    s->smmu_nvjpg_asid = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecNvjpgAsid]);
    c->smmu_translation_enable0 = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecSmmuEn01]);
    c->smmu_translation_enable1 = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecSmmuEn01] + 1);
    c->smmu_translation_enable3 = READ_REGISTER_ULONG((PULONG)r->NvdecMapping[TgNvdecSmmuEn3]);
    c->mc_flush = READ_REGISTER_ULONG((PULONG)r->Mapping[TgGpuMcFlush]);
    c->mc_flush_status = READ_REGISTER_ULONG((PULONG)r->Mapping[TgGpuMcFlush] + 1);
    if (c->mc_flush != MAXULONG && c->mc_flush_status != MAXULONG &&
        s->smmu_host1x_asid != MAXULONG && s->smmu_nvdec_asid != MAXULONG &&
        s->smmu_nvjpg_asid != MAXULONG) {
        c->flags = TG_NVDEC_CLIENT_REGS_VALID;
        /* ASID_ENABLE gates each SWGROUP even when all per-client enables
         * are set (the T210 ATF boot policy). Preserve those raw words;
         * all ones is a legitimate value, not evidence of a failed read.
         * This snapshot covers all three groups; live H.264 DMA checks
         * only HC/NVDEC. Never write SMMU state to obtain bypass. */
        if (!((s->smmu_host1x_asid | s->smmu_nvdec_asid |
               s->smmu_nvjpg_asid) & (1u << 31)))
            c->flags |= TG_NVDEC_CLIENT_TRANSLATION_DISABLED;
    }
    if (s->hidrev == MAXULONG || s->powergate_status == MAXULONG ||
        s->clock_y == MAXULONG || s->reset_y == MAXULONG ||
        s->nvdec_clock_source == MAXULONG || s->nvjpg_clock_source == MAXULONG ||
        s->smmu_config == MAXULONG || s->smmu_host1x_asid == MAXULONG ||
        s->smmu_nvdec_asid == MAXULONG || s->smmu_nvjpg_asid == MAXULONG) {
        s->status = TG_NVDEC_RESOURCES_REGS_INVALID;
        return;
    }
    s->flags |= TG_NVDEC_QUERY_BOOT_REGS_VALID;
    s->status = TG_NVDEC_RESOURCES_CAPTURED;
}

static uint32_t read_dc_register(void *registers, uint32_t word)
{
    return READ_REGISTER_ULONG((PULONG)registers + word);
}

NTSTATUS
TgGpuReleaseResources(const DXGKRNL_INTERFACE *Dxgk,
                      TG_GPU_RESOURCES *Resources)
{
    NTSTATUS result = STATUS_SUCCESS;
    ULONG i;
    PAGED_CODE();
    Resources->NvdecInfo.flags = 0;
    Resources->NvdecClients.flags = 0;
    for (i = TgNvdecApertureCount; i != 0; --i) {
        if (Resources->NvdecMapping[i - 1]) {
            NTSTATUS status = Dxgk->DxgkCbUnmapMemory(Dxgk->DeviceHandle,
                                                     Resources->NvdecMapping[i - 1]);
            if (NT_SUCCESS(status))
                Resources->NvdecMapping[i - 1] = NULL;
            else if (NT_SUCCESS(result))
                result = status;
        }
    }
    if (Resources->DisplayRegisters) {
        NTSTATUS status = Dxgk->DxgkCbUnmapMemory(Dxgk->DeviceHandle, Resources->DisplayRegisters);
        if (NT_SUCCESS(status))
            Resources->DisplayRegisters = NULL;
        else if (NT_SUCCESS(result))
            result = status;
    }
    if(Resources->LicMapping) {
        NTSTATUS status=Dxgk->DxgkCbUnmapMemory(Dxgk->DeviceHandle,Resources->LicMapping);
        if(NT_SUCCESS(status)) Resources->LicMapping=NULL;
        else if (NT_SUCCESS(result)) result=status;
    }
    for (i = TgGpuApertureCount; i != 0; --i) {
        if (Resources->Mapping[i - 1]) {
            NTSTATUS status = Dxgk->DxgkCbUnmapMemory(
                Dxgk->DeviceHandle, Resources->Mapping[i - 1]);
            if (NT_SUCCESS(status))
                Resources->Mapping[i - 1] = NULL;
            else if (NT_SUCCESS(result))
                result = status;
        }
    }
    return result;
}

NTSTATUS
TgGpuAcquireResources(const DXGKRNL_INTERFACE *Dxgk,
                      TG_GPU_RESOURCES *Resources)
{
    DXGK_DEVICE_INFO info = {0};
    PCM_PARTIAL_RESOURCE_LIST list;
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *display = NULL;
    ULONG i, memoryMask = 0, interruptCount = 0;
    BOOLEAN hasPostFramebuffer = FALSE;
    NTSTATUS status;
    PAGED_CODE();

    if(Resources->LicMapping || Resources->DisplayRegisters) return STATUS_INVALID_DEVICE_STATE;
    for (i = 0; i < TgGpuApertureCount; ++i)
        if (Resources->Mapping[i])
            return STATUS_INVALID_DEVICE_STATE;
    for (i = 0; i < TgNvdecApertureCount; ++i)
        if (Resources->NvdecMapping[i])
            return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(Resources, sizeof(*Resources));
    Resources->NvdecInfo.version = TG_NVDEC_QUERY_ABI;
    Resources->NvdecInfo.size = sizeof(Resources->NvdecInfo);
    Resources->NvdecClients.version = TG_NVDEC_CLIENT_QUERY_ABI;
    Resources->NvdecClients.size = sizeof(Resources->NvdecClients);

    status = Dxgk->DxgkCbGetDeviceInformation(Dxgk->DeviceHandle, &info);
    if (!NT_SUCCESS(status))
        return status;
    /* GPU0 supplies render and narrow power resources. The optional POST
     * aperture is validated here. Mapping it requires display ownership. */
    if (!info.TranslatedResourceList || info.TranslatedResourceList->Count != 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    list = &info.TranslatedResourceList->List[0].PartialResourceList;
    for (i = 0; i < list->Count; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *resource = &list->PartialDescriptors[i];
        if (resource->Type == CmResourceTypeMemory) {
            ULONG n;
            for (n = 0; n < TgNvdecApertureCount; n++)
                if (resource->u.Memory.Start.QuadPart == NvdecRanges[n].Base)
                    break;
            if (n < TgNvdecApertureCount) {
                const ULONG bit = 1u << n;
                const USHORT access = resource->Flags &
                    (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY);
                if (resource->u.Memory.Length != NvdecRanges[n].Bytes ||
                    resource->ShareDisposition != CmResourceShareDeviceExclusive ||
                    access != (NvdecRanges[n].Writable ? 0 : CM_RESOURCE_MEMORY_READ_ONLY) ||
                    (Resources->NvdecInfo.resource_mask & bit)) {
                    Resources->NvdecInfo.status = TG_NVDEC_RESOURCES_INVALID;
                } else {
                    Resources->NvdecMemory[n] = *resource;
                    Resources->NvdecInfo.resource_mask |= bit;
                }
                continue;
            }
            if (resource->u.Memory.Start.QuadPart == T214_GPU_DC_BASE) {
                if (display || resource->u.Memory.Length != T214_GPU_DC_SIZE ||
                    resource->ShareDisposition != CmResourceShareDeviceExclusive ||
                    (resource->Flags & CM_RESOURCE_MEMORY_WRITE_ONLY))
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                display = resource;
                continue;
            }
            if (resource->u.Memory.Start.QuadPart == T214_GPU_POST_FB_BASE) {
                if (hasPostFramebuffer ||
                    resource->u.Memory.Length != T214_GPU_POST_FB_SIZE ||
                    resource->ShareDisposition != CmResourceShareDeviceExclusive ||
                    (resource->Flags & (CM_RESOURCE_MEMORY_READ_ONLY |
                                        CM_RESOURCE_MEMORY_WRITE_ONLY)) != 0)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                hasPostFramebuffer = TRUE;
                continue;
            }
            if(resource->u.Memory.Start.QuadPart==T214_GPU_LIC_BASE) {
                if(resource->u.Memory.Length!=T214_GPU_LIC_SIZE ||
                   resource->ShareDisposition!=CmResourceShareDeviceExclusive || Resources->Lic.Type)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                Resources->Lic=*resource;
                continue;
            }
            ULONG aperture;
            for (aperture = 0; aperture < TgGpuApertureCount; ++aperture)
                if (resource->u.Memory.Start.QuadPart == TgGpuBases[aperture])
                    break;
            if (aperture == TgGpuApertureCount ||
                resource->u.Memory.Length != TgGpuSizes[aperture] ||
                (memoryMask & (1u << aperture)) != 0 ||
                resource->ShareDisposition != CmResourceShareDeviceExclusive)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            Resources->Memory[aperture] = *resource;
            memoryMask |= 1u << aperture;
        } else if (resource->Type == CmResourceTypeInterrupt) {
            if (interruptCount == 3 ||
                (resource->Flags & (CM_RESOURCE_INTERRUPT_LATCHED |
                                    CM_RESOURCE_INTERRUPT_MESSAGE)) != 0)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            /* _CRS orders the GPU stall and nonstall lines before Display A.
             * Keep the translated GPU vectors, levels and affinity.
             */
            if (interruptCount < 2)
                Resources->Interrupt[interruptCount] = *resource;
            else if (resource->ShareDisposition != CmResourceShareDeviceExclusive)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interruptCount;
        }
    }
    if (memoryMask != (1u << TgGpuApertureCount) - 1u || interruptCount < 2 ||
        (interruptCount == 3 && (!hasPostFramebuffer || !display ||
                               (display->Flags & CM_RESOURCE_MEMORY_READ_ONLY))))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Resources->PanelAvailable = interruptCount == 3;

    for (i = 0; i < TgGpuApertureCount; ++i) {
        status = Dxgk->DxgkCbMapMemory(Dxgk->DeviceHandle,
            Resources->Memory[i].u.Memory.Start, TgGpuSizes[i],
            FALSE, FALSE, MmNonCached, &Resources->Mapping[i]);
        if (!NT_SUCCESS(status)) {
            NTSTATUS cleanup = TgGpuReleaseResources(Dxgk, Resources);
            return NT_SUCCESS(cleanup) ? status : cleanup;
        }
    }
    if(Resources->Lic.Type) {
        status=Dxgk->DxgkCbMapMemory(Dxgk->DeviceHandle,Resources->Lic.u.Memory.Start,
            T214_GPU_LIC_SIZE,FALSE,FALSE,MmNonCached,&Resources->LicMapping);
        if(!NT_SUCCESS(status)) {
            NTSTATUS cleanup=TgGpuReleaseResources(Dxgk,Resources);
            return NT_SUCCESS(cleanup)?status:cleanup;
        }
    }
    if (display) {
        status = Dxgk->DxgkCbMapMemory(Dxgk->DeviceHandle, display->u.Memory.Start,
            T214_GPU_DC_SIZE, FALSE, FALSE, MmNonCached, &Resources->DisplayRegisters);
        if (NT_SUCCESS(status)) {
            const struct tg_t214_dc_io io = {Resources->DisplayRegisters, read_dc_register, NULL};
            if (!tg_t214_dc_has_boot_mode(&io))
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (!NT_SUCCESS(status)) {
            NTSTATUS cleanup = TgGpuReleaseResources(Dxgk, Resources);
            return NT_SUCCESS(cleanup) ? status : cleanup;
        }
    }
    map_nvdec_resources(Dxgk, Resources);
    return STATUS_SUCCESS;
}
