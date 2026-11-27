/* SPDX-License-Identifier: MIT */
/* Include after the paging test's core/display callbacks. All register views
 * below are CPU arrays; this controls ownership and cleanup, not real DMA. */
static const ULONG nvdec_resource_profile[][3] = {
#define NVDEC_PAIR(name, base, size, writable) {base, size, writable},
    T214_NVDEC_RESOURCES(NVDEC_PAIR)
#undef NVDEC_PAIR
};
static ULONG nvdec_words[TgNvdecApertureCount][6], nvdec_hidrev;
static ULONG nvdec_mc_flush[2];
static UINT nvdec_maps, nvdec_unmaps, nvdec_live;
static UINT nvdec_fail_map, nvdec_null_map, nvdec_fail_unmap;

static NTSTATUS APIENTRY NvdecResourceMap(HANDLE device, PHYSICAL_ADDRESS address,
    ULONG bytes, BOOLEAN io, BOOLEAN user, MEMORY_CACHING_TYPE cache, PVOID *out)
{
    for (UINT i = 0; i < TgNvdecApertureCount; ++i) {
        if (address.QuadPart != nvdec_resource_profile[i][0]) continue;
        CHECK(device == (HANDLE)1 && !io && !user && cache == MmNonCached);
        CHECK(bytes == nvdec_resource_profile[i][1] && i == nvdec_maps);
        ++nvdec_maps;
        if (nvdec_maps == nvdec_fail_map) {
            *out = (PVOID)(ULONG_PTR)0xbad; /* A failed callback owns no mapping. */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (nvdec_maps == nvdec_null_map) {
            *out = NULL;
            return STATUS_SUCCESS;
        }
        nvdec_live |= 1u << i;
        *out = nvdec_words[i];
        return STATUS_SUCCESS;
    }
    NTSTATUS status = MapDisplayResource(device, address, bytes, io, user, cache, out);
    if (NT_SUCCESS(status) && address.QuadPart == resource_profile[TgGpuHidrev][0])
        *out = &nvdec_hidrev;
    if (NT_SUCCESS(status) && address.QuadPart == resource_profile[TgGpuMcFlush][0])
        *out = nvdec_mc_flush;
    return status;
}

static NTSTATUS APIENTRY NvdecResourceUnmap(HANDLE device, PVOID mapping)
{
    for (UINT i = 0; i < TgNvdecApertureCount; ++i) {
        if (mapping != nvdec_words[i]) continue;
        CHECK(device == (HANDLE)1 && (nvdec_live & (1u << i)));
        ++nvdec_unmaps;
        if (i + 1 == nvdec_fail_unmap) return STATUS_UNSUCCESSFUL;
        nvdec_live &= ~(1u << i);
        return STATUS_SUCCESS;
    }
    if (mapping == &nvdec_hidrev)
        mapping = (PVOID)(ULONG_PTR)(0x10000 + (TgGpuHidrev + 1) * PAGE_SIZE);
    if (mapping == nvdec_mc_flush)
        mapping = (PVOID)(ULONG_PTR)(0x10000 + (TgGpuMcFlush + 1) * PAGE_SIZE);
    return UnmapDisplayResource(device, mapping);
}

static CM_PARTIAL_RESOURCE_LIST *NvdecResourceList(UINT count, BOOLEAN display)
{
    CM_PARTIAL_RESOURCE_LIST *list = ResourceList(TRUE);
    nvdec_maps = nvdec_unmaps = nvdec_live = 0;
    nvdec_fail_map = nvdec_null_map = nvdec_fail_unmap = 0;
    nvdec_hidrev = 0x22110021;
    RtlZeroMemory(nvdec_words, sizeof(nvdec_words));
    RtlZeroMemory(nvdec_mc_flush, sizeof(nvdec_mc_flush));
    nvdec_words[TgNvdecPmc][0] = 0x8f9a;
    nvdec_words[TgNvdecPmc][1] = 0x11;
    nvdec_words[TgNvdecPmc][3] = 0x02000000;
    nvdec_words[TgNvdecCarY][0] = 0x80;
    nvdec_words[TgNvdecCarY][3] = 0xc0;
    nvdec_words[TgNvdecCarSource][0] = 0x80000002;
    nvdec_words[TgNvdecCarSource][1] = 0x80000004;
    nvdec_words[TgNvdecSmmu][0] = 1;
    nvdec_words[TgNvdecHcAsid][0] = 0x80000001;
    nvdec_words[TgNvdecNvdecAsid][0] = 0x80000002;
    nvdec_words[TgNvdecNvjpgAsid][0] = 0x80000003;
    for (UINT i = 0; i < count; ++i) {
        CM_PARTIAL_RESOURCE_DESCRIPTOR *r = &list->PartialDescriptors[list->Count++];
        r->Type = CmResourceTypeMemory;
        r->ShareDisposition = CmResourceShareDeviceExclusive;
        r->Flags = nvdec_resource_profile[i][2] ? 0 : CM_RESOURCE_MEMORY_READ_ONLY;
        r->u.Memory.Start.QuadPart = nvdec_resource_profile[i][0];
        r->u.Memory.Length = nvdec_resource_profile[i][1];
    }
    if (display) {
        CM_PARTIAL_RESOURCE_DESCRIPTOR *r = &list->PartialDescriptors[list->Count++];
        r->Type = CmResourceTypeMemory;
        r->ShareDisposition = CmResourceShareDeviceExclusive;
        r->Flags = CM_RESOURCE_MEMORY_READ_ONLY;
        r->u.Memory.Start.QuadPart = T214_GPU_DC_BASE;
        r->u.Memory.Length = T214_GPU_DC_SIZE;
    }
    return list;
}

static void TestNvdecResources(void)
{
    DXGKRNL_INTERFACE dxgk = {0};
    TG_GPU_RESOURCES r = {0};
    dxgk.DeviceHandle = (HANDLE)1;
    dxgk.DxgkCbGetDeviceInformation = ResourceInfo;
    dxgk.DxgkCbMapMemory = NvdecResourceMap;
    dxgk.DxgkCbUnmapMemory = NvdecResourceUnmap;
    for (UINT count = 0; count < TgNvdecApertureCount; ++count) {
        NvdecResourceList(count, FALSE);
        CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS);
        CHECK(r.NvdecInfo.version == TG_NVDEC_QUERY_ABI && r.NvdecInfo.size == 64);
        CHECK(r.NvdecInfo.resource_mask == (1u << count) - 1);
        CHECK(r.NvdecInfo.status == (count ? TG_NVDEC_RESOURCES_PARTIAL : TG_NVDEC_RESOURCES_ABSENT));
        CHECK(!r.NvdecInfo.flags && !nvdec_maps && resource_maps == T214_GPU_RESOURCE_COUNT + 1);
        CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !resource_live);
    }
    for (UINT malformed = 0; malformed < 4; ++malformed) {
        CM_PARTIAL_RESOURCE_LIST *list = NvdecResourceList(TgNvdecApertureCount, FALSE);
        CM_PARTIAL_RESOURCE_DESCRIPTOR *last = &list->PartialDescriptors[list->Count - 1];
        if (!malformed) last->u.Memory.Length += 4;
        if (malformed == 1) last->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
        if (malformed == 2) last->ShareDisposition = CmResourceShareShared;
        if (malformed == 3) list->PartialDescriptors[list->Count++] = *last;
        CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS);
        CHECK(r.NvdecInfo.status == TG_NVDEC_RESOURCES_INVALID && !r.NvdecInfo.flags && !nvdec_maps);
        CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !resource_live);
    }
    NvdecResourceList(TgNvdecApertureCount, TRUE);
    CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS);
    CHECK(r.NvdecInfo.status == TG_NVDEC_RESOURCES_CAPTURED && r.NvdecInfo.flags == 7);
    CHECK(r.NvdecInfo.hidrev == nvdec_hidrev && r.NvdecInfo.powergate_status == 0x02000000);
    CHECK(r.NvdecInfo.clock_y == 0x80 && r.NvdecInfo.reset_y == 0xc0);
    CHECK(r.NvdecInfo.nvdec_clock_source == 0x80000002 && r.NvdecInfo.nvjpg_clock_source == 0x80000004);
    CHECK(r.NvdecInfo.smmu_config == 1 && r.NvdecInfo.smmu_host1x_asid == 0x80000001);
    CHECK(r.NvdecInfo.smmu_nvdec_asid == 0x80000002 && r.NvdecInfo.smmu_nvjpg_asid == 0x80000003);
    CHECK(r.NvdecClients.version == TG_NVDEC_CLIENT_QUERY_ABI && r.NvdecClients.size == 40);
    CHECK(r.NvdecClients.pmc_clamp_status == 0x8f9a && r.NvdecClients.pmc_toggle == 0x11);
    CHECK(r.NvdecClients.flags == TG_NVDEC_CLIENT_REGS_VALID);
    CHECK(r.DisplayRegisters == boot_display && !r.PanelAvailable);
    CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !nvdec_live && !resource_live);
    CHECK(!r.NvdecInfo.flags && nvdec_maps == nvdec_unmaps);
    CHECK(!r.NvdecClients.flags);

    for (UINT fail = 1; fail <= TgNvdecApertureCount; ++fail) {
        NvdecResourceList(TgNvdecApertureCount, FALSE);
        nvdec_fail_map = fail;
        CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS && resource_live);
        CHECK(r.NvdecInfo.status == TG_NVDEC_RESOURCES_MAP_FAILED && !nvdec_live);
        CHECK(r.NvdecInfo.flags == TG_NVDEC_QUERY_RESOURCES_VALID);
        CHECK(r.NvdecInfo.map_status == (UINT32)STATUS_INSUFFICIENT_RESOURCES);
        CHECK(nvdec_maps == fail && nvdec_unmaps == fail - 1);
        CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !resource_live);
    }
    NvdecResourceList(TgNvdecApertureCount, FALSE);
    nvdec_null_map = 2;
    CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS && !nvdec_live);
    CHECK(r.NvdecInfo.map_status == (UINT32)STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !resource_live);

    NvdecResourceList(TgNvdecApertureCount, TRUE);
    nvdec_fail_map = 3;
    nvdec_fail_unmap = 2;
    CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS && nvdec_live == 2);
    CHECK(r.NvdecMapping[1] == nvdec_words[1] && r.DisplayRegisters == boot_display);
    CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_INVALID_DEVICE_STATE);
    CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_UNSUCCESSFUL);
    CHECK(nvdec_live == 2 && !resource_live && !r.DisplayRegisters);
    CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_INVALID_DEVICE_STATE);
    nvdec_fail_unmap = 0;
    CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !nvdec_live);

    NvdecResourceList(TgNvdecApertureCount, FALSE);
    nvdec_words[TgNvdecNvdecAsid][0] = MAXULONG;
    CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS);
    CHECK(r.NvdecInfo.status == TG_NVDEC_RESOURCES_REGS_INVALID);
    CHECK(r.NvdecInfo.flags == (TG_NVDEC_QUERY_RESOURCES_VALID | TG_NVDEC_QUERY_MAPPED));
    CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !nvdec_live && !resource_live);

    /* Global CONFIG readback remains unresolved. A clear group ASID_ENABLE
     * still bypasses when ATF leaves all per-client enables set. Neither the
     * raw global value nor an unavailable group readback may be invented. */
    for (UINT scenario = 0; scenario < 17; ++scenario) {
        NvdecResourceList(TgNvdecApertureCount, FALSE);
        nvdec_words[TgNvdecSmmu][0] = MAXULONG;
        nvdec_words[TgNvdecHcAsid][0] = 0;
        nvdec_words[TgNvdecNvdecAsid][0] = 0;
        nvdec_words[TgNvdecNvjpgAsid][0] = 0;
        if (scenario == 1) nvdec_words[TgNvdecSmmuEn01][0] = 1u << 22;
        if (scenario == 2) nvdec_words[TgNvdecSmmuEn01][0] = 1u << 23;
        if (scenario == 3) nvdec_words[TgNvdecSmmuEn01][1] = 1u << 22;
        if (scenario == 4) nvdec_words[TgNvdecSmmuEn3][0] = 1u << 24;
        if (scenario == 5) nvdec_words[TgNvdecSmmuEn3][0] = 1u << 25;
        if (scenario == 6) nvdec_words[TgNvdecSmmuEn3][0] = 1u << 30;
        if (scenario == 7) nvdec_words[TgNvdecSmmuEn3][0] = 1u << 31;
        if (scenario >= 8 && scenario <= 10)
            nvdec_words[TgNvdecHcAsid + scenario - 8][0] = 1u << 31;
        if (scenario == 11) {
            nvdec_words[TgNvdecSmmuEn01][0] = MAXULONG;
            nvdec_words[TgNvdecSmmuEn01][1] = MAXULONG;
            nvdec_words[TgNvdecSmmuEn3][0] = MAXULONG;
        }
        if (scenario >= 12 && scenario <= 14)
            nvdec_words[TgNvdecHcAsid + scenario - 12][0] = MAXULONG;
        if (scenario == 15) nvdec_mc_flush[0] = MAXULONG;
        if (scenario == 16) nvdec_mc_flush[1] = MAXULONG;
        CHECK(TgGpuAcquireResources(&dxgk, &r) == STATUS_SUCCESS);
        CHECK(r.NvdecInfo.status == TG_NVDEC_RESOURCES_REGS_INVALID);
        CHECK(r.NvdecClients.flags == (scenario >= 12 ? 0 :
            TG_NVDEC_CLIENT_REGS_VALID | ((scenario >= 8 && scenario <= 10) ?
                0 : TG_NVDEC_CLIENT_TRANSLATION_DISABLED)));
        CHECK(r.NvdecInfo.smmu_config == MAXULONG);
        CHECK(!(r.NvdecInfo.flags & TG_NVDEC_QUERY_BOOT_REGS_VALID));
        if (scenario == 11)
            CHECK(r.NvdecClients.smmu_translation_enable0 == MAXULONG &&
                  r.NvdecClients.smmu_translation_enable1 == MAXULONG &&
                  r.NvdecClients.smmu_translation_enable3 == MAXULONG);
        CHECK(TgGpuReleaseResources(&dxgk, &r) == STATUS_SUCCESS && !nvdec_live && !resource_live);
    }
}
