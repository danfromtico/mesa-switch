/* SPDX-License-Identifier: MIT */
/* Run against the real WDK types in ARM64 Windows/WinPE. No GPU is touched. */
#include "../windows/paging.h"
#include "../windows/table_segment.h"
#include "../windows/root_table.h"
#include "../windows/paging_buffer.h"
#include "../windows/resources.h"
#include "../platform/t214_dc.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>

/* Same ARM64 loader metadata as the existing performance tool. */
static ULONG_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),
    .SecurityCookie = (ULONGLONG)&image_cookie
};

__declspec(dllimport) void __stdcall ExitProcess(ULONG Code);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG Handle);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE File, const void *Buffer,
                                            ULONG Bytes, ULONG *Written, void *Overlapped);
static UINT64 table[TG_GPU_TABLE_ENTRIES], scratch[TG_GPU_TABLE_ENTRIES];
static ULONG calls, failed_index;
static void FailCheck(UINT line)
{
    char message[64] = "FAIL paging/resource control line ";
    char digits[10];
    UINT n = 0, at = sizeof("FAIL paging/resource control line ") - 1;
    ULONG written;
    do { digits[n++] = '0' + line % 10; line /= 10; } while (line);
    while (n) message[at++] = digits[--n];
    message[at++] = '\n';
    WriteFile(GetStdHandle((ULONG)-12), message, at, &written, NULL);
    ExitProcess(failed_index);
}
#define CHECK(x) do { ++failed_index; if (!(x)) FailCheck(__LINE__); } while (0)

#include "resource_fixture.h"

/* Native Hekate timing and direct Window A aliases, independent of the
 * driver's definitions. The linear buffer has a 2880-byte stride.
 */
static ULONG boot_display[T214_GPU_DC_SIZE / sizeof(ULONG)] = {
    [0x032] = 0x00000020,
    [0x402] = 0x20000000,
    [0x407] = 0x00010048,
    [0x408] = 0x00090048,
    [0x409] = 0x050002d0,
    [0x40a] = 0x000a0088,
    [0xb80] = 0x40000000,
    [0xb83] = 0x0000000c,
    [0xb85] = 0x050002d0,
    [0xb86] = 0x05000b40,
    [0xb89] = 0x10001000,
    [0xb8a] = 0x05a00b40,
    [0xbc0] = 0xf5a00000
};

static NTSTATUS APIENTRY MapDisplayResource(HANDLE device, PHYSICAL_ADDRESS address,
    ULONG bytes, BOOLEAN io, BOOLEAN user, MEMORY_CACHING_TYPE cache, PVOID *out)
{
    if (address.QuadPart != T214_GPU_DC_BASE)
        return ResourceMap(device, address, bytes, io, user, cache, out);
    CHECK(device == (HANDLE)1 && !io && !user && cache == MmNonCached);
    CHECK(bytes == sizeof(boot_display) && resource_maps == T214_GPU_RESOURCE_COUNT + 1);
    ++resource_maps;
    resource_live |= 1u << (T214_GPU_RESOURCE_COUNT + 1);
    *out = boot_display;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY UnmapDisplayResource(HANDLE device, PVOID mapping)
{
    if (mapping != boot_display)
        return ResourceUnmap(device, mapping);
    CHECK(device == (HANDLE)1 && (resource_live & (1u << (T214_GPU_RESOURCE_COUNT + 1))));
    resource_live &= ~(1u << (T214_GPU_RESOURCE_COUNT + 1));
    ++resource_unmaps;
    return STATUS_SUCCESS;
}

static void TestDisplayResources(void)
{
    DXGKRNL_INTERFACE dxgk = {0};
    TG_GPU_RESOURCES resources = {0};
    dxgk.DeviceHandle = (HANDLE)1;
    dxgk.DxgkCbGetDeviceInformation = ResourceInfo;
    dxgk.DxgkCbMapMemory = MapDisplayResource;
    dxgk.DxgkCbUnmapMemory = UnmapDisplayResource;

    CM_PARTIAL_RESOURCE_DESCRIPTOR display = {0};
    display.Type = CmResourceTypeMemory;
    display.ShareDisposition = CmResourceShareDeviceExclusive;
    display.Flags = CM_RESOURCE_MEMORY_READ_ONLY;
    display.u.Memory.Start.QuadPart = T214_GPU_DC_BASE;
    display.u.Memory.Length = T214_GPU_DC_SIZE;
    CM_PARTIAL_RESOURCE_LIST *list = ResourceList(TRUE);
    list->PartialDescriptors[list->Count++] = display;

    CHECK(TgGpuAcquireResources(&dxgk, &resources) == STATUS_SUCCESS);
    CHECK(resources.DisplayRegisters == boot_display && !resources.PanelAvailable);
    CHECK(TgGpuReleaseResources(&dxgk, &resources) == STATUS_SUCCESS && !resource_live);
    CHECK(!resources.DisplayRegisters && resource_unmaps == T214_GPU_RESOURCE_COUNT + 2);

    list = ResourceList(TRUE);
    list->PartialDescriptors[list->Count++] = display;
    boot_display[0xb8a] -= 4;
    CHECK(TgGpuAcquireResources(&dxgk, &resources) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!resources.DisplayRegisters && !resource_live && resource_unmaps == resource_maps);
    boot_display[0xb8a] += 4;

    CM_PARTIAL_RESOURCE_DESCRIPTOR irq = {0};
    irq.Type = CmResourceTypeInterrupt;
    irq.ShareDisposition = CmResourceShareDeviceExclusive;
    irq.u.Interrupt.Vector = 0x904; /* Translated, as seen on Lite. */
    list = ResourceList(TRUE);
    list->PartialDescriptors[list->Count++] = display;
    list->PartialDescriptors[list->Count++] = irq;
    CHECK(TgGpuAcquireResources(&dxgk, &resources) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!resource_maps);

    CM_PARTIAL_RESOURCE_DESCRIPTOR post = {0};
    post.Type = CmResourceTypeMemory;
    post.ShareDisposition = CmResourceShareDeviceExclusive;
    post.u.Memory.Start.QuadPart = T214_GPU_POST_FB_BASE;
    post.u.Memory.Length = T214_GPU_POST_FB_SIZE;
    display.Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    list = ResourceList(TRUE);
    list->PartialDescriptors[list->Count++] = post;
    list->PartialDescriptors[list->Count++] = display;
    list->PartialDescriptors[list->Count++] = irq;
    CHECK(TgGpuAcquireResources(&dxgk, &resources) == STATUS_SUCCESS);
    CHECK(resources.PanelAvailable);
    CHECK(resources.Interrupt[0].u.Interrupt.Vector == 0x200 &&
          resources.Interrupt[1].u.Interrupt.Vector == 0x201);
    CHECK(TgGpuReleaseResources(&dxgk, &resources) == STATUS_SUCCESS && !resource_live);
}

#include "nvdec_resource_fixture.h"

struct display_model {
    ULONG regs[T214_GPU_DC_SIZE / sizeof(ULONG)];
    ULONG update, armed_address, active_address;
    ULONG armed_options, active_options;
};

static uint32_t DisplayRead(void *context, uint32_t word)
{
    struct display_model *model = context;
    if (model->regs[0x040] & 1) {
        if (word == 0xbc0) return model->active_address;
        if (word == 0xb80) return model->active_options;
    }
    return model->regs[word];
}

static void DisplayWrite(void *context, uint32_t word, uint32_t value)
{
    struct display_model *model = context;
    if (word == 0x037) {
        CHECK(value == 4); /* Acknowledge only the VBlank W1C bit. */
        model->regs[word] &= ~value;
    } else if (word == 0x041) {
        if (value == 0x100 || value == 0x200 || value == 0x300) {
            model->update = value >> 8;
            model->armed_address = model->regs[0xbc0];
            model->armed_options = model->regs[0xb80];
        } else {
            CHECK(value == model->update && (value == 1 || value == 2 || value == 3));
            model->regs[word] |= value;
            model->update = 0;
        }
    } else {
        CHECK(word == 0x038 || word == 0x039 || word == 0x040 || word == 0x043 ||
              word == 0xb80 || word == 0xbc0);
        model->regs[word] = value;
    }
}

static void DisplayFrame(struct display_model *model)
{
    if (model->regs[0x041] & 2) {
        model->active_address = model->armed_address;
        model->active_options = model->armed_options;
    }
    model->regs[0x041] = 0;
    model->regs[0x037] |= model->regs[0x039] & 4;
}

static void DisplayLine(struct display_model *model)
{
    if ((model->regs[0x043]&4) && (model->regs[0x041]&2)) {
        model->active_address=model->armed_address;
        model->active_options=model->armed_options;
        model->regs[0x041]&=~2u;
    }
}

static void TestDisplayFlip(void)
{
    static struct display_model model;
    RtlCopyMemory(model.regs, boot_display, sizeof(model.regs));
    model.regs[0x043] = 0x454;
    model.regs[0x039] = 1; /* Masked context-switch source stays intact. */
    model.regs[0x037] = 1;
    model.active_address = T214_GPU_POST_FB_BASE;
    model.active_options = model.regs[0xb80];
    const struct tg_t214_dc_io io = {&model, DisplayRead, DisplayWrite};
    struct tg_t214_dc dc = {0};

    CHECK(tg_t214_dc_acquire(&dc, &io) == TG_T214_DC_OK);
    CHECK(model.regs[0x043] == 0x450 && model.regs[0x040] == 0);
    CHECK(tg_t214_dc_queue_flip(&dc, 0xc1000000) == TG_T214_DC_BUSY);
    tg_t214_dc_set_vblank(&dc, true);
    CHECK(model.regs[0x038] == 4 && model.regs[0x039] == 5);
    CHECK(!tg_t214_dc_ack_vblank(&dc));
    DisplayFrame(&model);
    CHECK(tg_t214_dc_ack_vblank(&dc) && model.regs[0x037] == 1);
    CHECK(tg_t214_dc_queue_flip(&dc, 0xc1000004) == TG_T214_DC_INVALID);
    CHECK(tg_t214_dc_queue_flip(&dc, 0xfff00000) == TG_T214_DC_INVALID);

    CHECK(tg_t214_dc_queue_flip(&dc, 0xc1000000) == TG_T214_DC_OK);
    CHECK(tg_t214_dc_active_address(&dc) == T214_GPU_POST_FB_BASE);
    CHECK(tg_t214_dc_queue_flip(&dc, T214_GPU_POST_FB_BASE) == TG_T214_DC_BUSY);
    DisplayFrame(&model);
    CHECK(tg_t214_dc_ack_vblank(&dc));
    CHECK(tg_t214_dc_active_address(&dc) == 0xc1000000);
    CHECK(tg_t214_dc_release(&dc) == TG_T214_DC_BUSY);

    UINT64 immediate_address;
    bool immediate_visible;
    CHECK(tg_t214_dc_queue_immediate(&dc,0xc2000000,true)==TG_T214_DC_OK);
    CHECK(model.regs[0x043]==0x454 && !tg_t214_dc_latched_scanout(&dc,&immediate_address,&immediate_visible));
    DisplayLine(&model);
    CHECK(model.active_address==0xc2000000);
    CHECK(tg_t214_dc_latched_scanout(&dc,&immediate_address,&immediate_visible) &&
          immediate_visible && model.regs[0x041]==1);
    DisplayFrame(&model); /* Complete the counter-mode transition too. */
    CHECK(tg_t214_dc_latched_scanout(&dc,&immediate_address,&immediate_visible) && immediate_visible);
    CHECK(tg_t214_dc_queue_immediate(&dc,0xc3000000,true)==TG_T214_DC_OK);
    CHECK(!tg_t214_dc_latched_scanout(&dc,&immediate_address,&immediate_visible));
    DisplayLine(&model);
    CHECK(tg_t214_dc_latched_scanout(&dc,&immediate_address,&immediate_visible) && immediate_address==0xc3000000);
    CHECK(tg_t214_dc_queue_immediate(&dc,0xc3000000,false)==TG_T214_DC_OK);
    CHECK(model.regs[0x043]==0x450);
    DisplayLine(&model); /* Visibility changes must wait for VBlank. */
    CHECK(!tg_t214_dc_latched_scanout(&dc,&immediate_address,&immediate_visible));
    DisplayFrame(&model);
    CHECK(tg_t214_dc_latched_scanout(&dc,&immediate_address,&immediate_visible) && !immediate_visible);

    CHECK(tg_t214_dc_queue_scanout(&dc, T214_GPU_POST_FB_BASE, false) == TG_T214_DC_OK);
    UINT64 address;
    bool visible;
    CHECK(!tg_t214_dc_latched_scanout(&dc, &address, &visible));
    DisplayFrame(&model);
    CHECK(tg_t214_dc_ack_vblank(&dc));
    CHECK(tg_t214_dc_latched_scanout(&dc, &address, &visible) &&
          address == T214_GPU_POST_FB_BASE && !visible);
    CHECK(tg_t214_dc_release(&dc) == TG_T214_DC_BUSY);
    CHECK(tg_t214_dc_queue_scanout(&dc, T214_GPU_POST_FB_BASE, true) == TG_T214_DC_OK);
    DisplayFrame(&model);
    CHECK(tg_t214_dc_ack_vblank(&dc));
    CHECK(tg_t214_dc_latched_scanout(&dc, &address, &visible) &&
          address == T214_GPU_POST_FB_BASE && visible);
    CHECK(tg_t214_dc_release(&dc) == TG_T214_DC_BUSY);
    DisplayFrame(&model);
    CHECK(tg_t214_dc_release(&dc) == TG_T214_DC_OK);
    CHECK(!dc.owned && !dc.vblank && model.active_address == T214_GPU_POST_FB_BASE);
    CHECK(model.regs[0x043] == 0x454 && model.regs[0x040] == 0);
    CHECK(model.regs[0x038] == 0 && model.regs[0x039] == 1 && model.regs[0x037] == 1);
}

static void TestResources(void)
{
    DXGKRNL_INTERFACE dxgk={0};TG_GPU_RESOURCES resources={0};
    dxgk.DeviceHandle=(HANDLE)1;dxgk.DxgkCbGetDeviceInformation=ResourceInfo;
    dxgk.DxgkCbMapMemory=ResourceMap;dxgk.DxgkCbUnmapMemory=ResourceUnmap;
    for(UINT lic=0;lic<2;++lic) {
        ResourceList((BOOLEAN)lic);
        CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_SUCCESS);
        CHECK(resource_maps==T214_GPU_RESOURCE_COUNT+lic);
        CHECK((resources.LicMapping!=NULL)==lic);
        CHECK(resources.Interrupt[0].u.Interrupt.Vector==0x200&&resources.Interrupt[1].u.Interrupt.Vector==0x201);
        CHECK(TgGpuReleaseResources(&dxgk,&resources)==STATUS_SUCCESS&&!resource_live);
    }
    CM_PARTIAL_RESOURCE_LIST *list=ResourceList(TRUE);
    CM_PARTIAL_RESOURCE_DESCRIPTOR post = {0};
    post.Type = CmResourceTypeMemory;
    post.ShareDisposition = CmResourceShareDeviceExclusive;
    post.u.Memory.Start.QuadPart = T214_GPU_POST_FB_BASE;
    post.u.Memory.Length = T214_GPU_POST_FB_SIZE;
    list->PartialDescriptors[list->Count++] = post;
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_SUCCESS);
    CHECK(resource_maps==T214_GPU_RESOURCE_COUNT+1);
    CHECK(TgGpuReleaseResources(&dxgk,&resources)==STATUS_SUCCESS&&!resource_live);

    list=ResourceList(TRUE);
    list->PartialDescriptors[list->Count++] = post;
    list->PartialDescriptors[list->Count++] = post;
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_DEVICE_CONFIGURATION_ERROR&&!resource_maps);
    --list->Count;
    list->PartialDescriptors[list->Count-1].u.Memory.Length -= PAGE_SIZE;
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_DEVICE_CONFIGURATION_ERROR&&!resource_maps);
    list->PartialDescriptors[list->Count-1] = post;
    list->PartialDescriptors[list->Count-1].Flags = CM_RESOURCE_MEMORY_READ_ONLY;
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_DEVICE_CONFIGURATION_ERROR&&!resource_maps);

    list=ResourceList(TRUE);
    list->PartialDescriptors[T214_GPU_RESOURCE_COUNT].u.Memory.Length=4;
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_DEVICE_CONFIGURATION_ERROR&&!resource_maps);
    ResourceList(TRUE);resource_fail_map=T214_GPU_RESOURCE_COUNT+1;
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_INSUFFICIENT_RESOURCES&&!resource_live);
    CHECK(resource_unmaps==T214_GPU_RESOURCE_COUNT);
    ResourceList(TRUE);resource_fail_map=T214_GPU_RESOURCE_COUNT+1;resource_fail_unmap=3;
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_UNSUCCESSFUL);
    CHECK(resources.Mapping[2]&&resource_live==4);
    CHECK(TgGpuAcquireResources(&dxgk,&resources)==STATUS_INVALID_DEVICE_STATE);
    resource_fail_unmap=0;
    CHECK(TgGpuReleaseResources(&dxgk,&resources)==STATUS_SUCCESS&&!resource_live);
}

/* The kernel normally supplies these CRT primitives. Keep the test executable
 * freestanding so it can run in the stock ARM64 PE image without a UCRT.
 */
void *memset(void *destination, int value, size_t bytes)
{
    unsigned char *p = destination;
    while (bytes--) *p++ = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t bytes)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    while (bytes--) *out++ = *in++;
    return destination;
}

/* Exercise real backend code with controlled OS allocation callbacks. These
 * are test-owned bytes, not GPU-accessible memory or a physical allocation.
 */
static _Alignas(PAGE_SIZE) UCHAR segment_bytes[TG_GPU_TABLE_BYTES * 2];
static _Alignas(PAGE_SIZE) UCHAR alias_bytes[TG_GPU_TABLE_BYTES * 2];
static BOOLEAN bad_alias_page;
static BOOLEAN fail_allocate, fail_free;
static UINT allocate_calls, free_calls;
static PHYSICAL_ADDRESS NTAPI TestPhysical(PVOID Address)
{
    PHYSICAL_ADDRESS result;
    ULONG_PTR p = (ULONG_PTR)Address, base;
    BOOLEAN alias = p >= (ULONG_PTR)alias_bytes && p < (ULONG_PTR)alias_bytes + sizeof(alias_bytes);
    base = alias ? (ULONG_PTR)alias_bytes : (ULONG_PTR)segment_bytes;
    CHECK(p >= base && p < base + sizeof(segment_bytes));
    result.QuadPart = 0x90000000 + p - base;
    if (alias && bad_alias_page && p - base == PAGE_SIZE) result.QuadPart += PAGE_SIZE;
    return result;
}
PHYSICAL_ADDRESS (NTAPI * const __imp_MmGetPhysicalAddress)(PVOID) = TestPhysical;

static NTSTATUS NTAPI Allocate(HANDLE Device, DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *Args)
{
    ++allocate_calls;
    CHECK(Device == (HANDLE)1 && Args->NumberOfBytes == sizeof(segment_bytes));
    CHECK(Args->LowestAcceptableAddress.QuadPart == 0x80000000 &&
          Args->HighestAcceptableAddress.QuadPart == 0xffffffff);
    CHECK(Args->CacheType == DXGK_MEMORY_CACHING_TYPE_NON_CACHED &&
          Args->BoundaryAddressMultiple.QuadPart == 0);
    if (fail_allocate) return STATUS_INSUFFICIENT_RESOURCES;
    Args->hMemoryHandle = (HANDLE)2;
    Args->pMemory = segment_bytes;
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI Free(HANDLE Device, const DXGKARGCB_FREECONTIGUOUSMEMORY *Args)
{
    ++free_calls;
    CHECK(Device == (HANDLE)1 && Args->hMemoryHandle == (HANDLE)2);
    return fail_free ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static void TestSegment(void)
{
    DXGKRNL_INTERFACE dxgk = {0};
    TG_GPU_TABLE_SEGMENT segment = {0};
    DXGK_SEGMENTDESCRIPTOR3 desc;
    DXGK_PTE ptes[2] = {0};
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update = {0};
    TG_GPU_PAGE_UPDATE_TARGET target = {0};
    PVOID cpu = NULL;
    UINT64 device = UINT64_MAX;
    ULONG before;
    dxgk.DeviceHandle = (HANDLE)1;
    dxgk.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_4;
    dxgk.DxgkCbAllocateContiguousMemory = Allocate;
    dxgk.DxgkCbFreeContiguousMemory = Free;
    CHECK(TgGpuCreateTableSegment(&dxgk,0,sizeof(segment_bytes),&segment) == STATUS_INVALID_PARAMETER);
    CHECK(TgGpuCreateTableSegment(&dxgk,1,PAGE_SIZE,&segment) == STATUS_INVALID_PARAMETER);
    CHECK(allocate_calls == 0);
    fail_allocate = TRUE;
    CHECK(TgGpuCreateTableSegment(&dxgk,1,sizeof(segment_bytes),&segment) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!segment.Memory.Handle && !segment.SegmentId);
    fail_allocate = FALSE;
    memset(segment_bytes,0xa5,sizeof(segment_bytes));
    CHECK(NT_SUCCESS(TgGpuCreateTableSegment(&dxgk,1,sizeof(segment_bytes),&segment)));
    CHECK(segment_bytes[0] == 0 && segment_bytes[sizeof(segment_bytes)-1] == 0);
    before = allocate_calls;
    CHECK(TgGpuCreateTableSegment(&dxgk,1,sizeof(segment_bytes),&segment) == STATUS_INVALID_DEVICE_STATE);
    CHECK(allocate_calls == before);
    CHECK(NT_SUCCESS(TgGpuDescribeTableSegment(&segment,&desc)));
    CHECK(desc.BaseAddress.QuadPart == 0x90000000 && desc.CpuTranslatedAddress.QuadPart == 0x90000000);
    CHECK(desc.Size == sizeof(segment_bytes) && desc.CommitLimit == sizeof(segment_bytes));
    CHECK(desc.Flags.CpuVisible && desc.Flags.LocalBudgetGroup && desc.Flags.PopulatedFromSystemMemory && !desc.Flags.Aperture &&
          !desc.Flags.CacheCoherent && !desc.Flags.PreservedDuringStandby && !desc.Flags.PreservedDuringHibernate);
    CHECK(NT_SUCCESS(TgGpuResolveTableSegment(&segment,1,TG_GPU_TABLE_BYTES,TG_GPU_TABLE_BYTES,&device)));
    CHECK(device == 0x90020000);
    CHECK(TgGpuResolveTableSegment(&segment,0,0,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,sizeof(segment_bytes)-PAGE_SIZE,TG_GPU_TABLE_BYTES,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,UINT64_MAX,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,1,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveTableSegment(&segment,1,0,0,&device) == STATUS_INVALID_ADDRESS);
    CHECK(device == 0x90020000); /* failed resolves publish no address */
    CHECK(NT_SUCCESS(TgGpuResolveVidMmPage(&segment,0,UINT64_C(0x170001000),PAGE_SIZE,&device)));
    CHECK(device == UINT64_C(0x170001000)); /* byte address above 4 GiB, not a PFN */
    CHECK(TgGpuResolveVidMmPage(&segment,2,0,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveVidMmPage(&segment,0,0x170001,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveVidMmPage(&segment,0,UINT64_C(1)<<34,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveVidMmPage(&segment,0,UINT64_MAX & ~UINT64_C(0xfff),PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    CHECK(TgGpuResolveVidMmPage(&segment,0,0x80000000,TG_GPU_TABLE_BYTES,&device) == STATUS_INVALID_ADDRESS);
    CHECK(device == UINT64_C(0x170001000)); /* rejection cannot publish a new address */
    CHECK(NT_SUCCESS(TgGpuMapTableSegment(&segment,0,TG_GPU_TABLE_BYTES,&cpu,&device)));
    CHECK(cpu == segment_bytes && device == 0x90000000);
    target.Entries = cpu; target.Size = TG_GPU_TABLE_BYTES;
    target.Scratch = scratch; target.ScratchEntries = TG_GPU_TABLE_ENTRIES;
    target.CpuMappingUncached = target.GpuPagesUncached = TRUE;
    target.Resolve = TgGpuResolveTableSegment; target.ResolveContext = &segment;
    update.PageTableLevel = 1; update.PageTableAddress.CpuVirtual = cpu;
    update.UpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    update.NumPageTableEntries = 1; update.pPageTableEntries = ptes;
    ptes[0].Valid = 1; ptes[0].Segment = 1; ptes[0].PageAddress = TG_GPU_TABLE_BYTES >> PAGE_SHIFT;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&update,&target)));
    CHECK(target.Entries[0] == UINT64_C(0x0090020500000000));
    /* Reject the whole batch when a later child table crosses the pool end. */
    update.StartIndex = 1; update.NumPageTableEntries = 2; ptes[1] = ptes[0];
    ptes[1].PageAddress = (sizeof(segment_bytes)-PAGE_SIZE) >> PAGE_SHIFT;
    CHECK(TgGpuUpdatePageTable(&update,&target) == STATUS_INVALID_ADDRESS);
    CHECK(target.Entries[1] == 0 && target.Entries[2] == 0);
    segment.Memory.SystemPhysicalAddress.QuadPart = 0x100000000;
    CHECK(TgGpuDescribeTableSegment(&segment,&desc) == STATUS_INVALID_PARAMETER);
    segment.Memory.SystemPhysicalAddress.QuadPart = 0xfffff000;
    CHECK(TgGpuResolveTableSegment(&segment,1,0,PAGE_SIZE,&device) == STATUS_INVALID_ADDRESS);
    segment.Memory.SystemPhysicalAddress.QuadPart = 0x90000000;
    fail_free = TRUE;
    CHECK(TgGpuDestroyTableSegment(&dxgk,&segment) == STATUS_UNSUCCESSFUL);
    CHECK(segment.SegmentId == 1 && segment.Memory.Handle == (HANDLE)2);
    fail_free = FALSE;
    CHECK(NT_SUCCESS(TgGpuDestroyTableSegment(&dxgk,&segment)));
    CHECK(!segment.SegmentId && !segment.Memory.Handle && !segment.Memory.CpuAddress && !segment.Memory.Bytes);
    CHECK(NT_SUCCESS(TgGpuDestroyTableSegment(&dxgk,&segment)) && free_calls == 2);
}

struct tlb_model {
    UINT32 Control, Root, Trigger; UINT Writes;
    volatile UINT64 *ExpectedEntry; UINT64 Expected;
};
static uint32_t TlbRead(void *Context, uint32_t Address)
{
    struct tlb_model *m = Context;
    CHECK(Address == 0x57100c80);
    return m->Control;
}
static void TlbWrite(void *Context, uint32_t Address, uint32_t Value)
{
    struct tlb_model *m = Context;
    if (m->ExpectedEntry) CHECK(*m->ExpectedEntry == m->Expected);
    ++m->Writes;
    if (Address == 0x57100cb8) m->Root = Value;
    else { CHECK(Address == 0x57100cbc); m->Trigger = Value; }
}
static void TlbDelay(void *Context, uint32_t Us)
{ UNREFERENCED_PARAMETER(Context); CHECK(Us == 2); }

static void TestRoot(void)
{
    static _Alignas(PAGE_SIZE) UINT32 instance[PAGE_SIZE/sizeof(UINT32)];
    TG_GPU_TABLE_SEGMENT segment = {
        .Memory = {(HANDLE)2, segment_bytes, {.QuadPart=0x90000000}, sizeof(segment_bytes)},
        .SegmentId = 1
    };
    TG_GPU_PRIVATE_MEMORY inst = {(HANDLE)3,instance,{.QuadPart=0x91000000},sizeof(instance)};
    DXGKARG_GETROOTPAGETABLESIZE size = {0};
    DXGKARG_SETROOTPAGETABLE set = {.hContext=(HANDLE)4, .Address={1,0,TG_GPU_TABLE_BYTES}, .NumEntries=3};
    TG_GPU_ROOT_TABLE root = {0};
    DXGK_BUILDPAGINGBUFFER_FLUSHTLB flush = {.RootPageTableAddress={1,0,TG_GPU_TABLE_BYTES}};
    struct tlb_model model = {.Control=0x18000};
    struct tg_gpu_power_io io = {&model,TlbRead,TlbWrite,NULL,NULL,TlbDelay, NULL};
    struct tg_gpu_power power = {.owned=true,.ready=true};
    CHECK(TgGpuRootTableSize(&size)==PAGE_SIZE && size.NumberOfPte==512);
    size.NumberOfPte=513;
    CHECK(TgGpuRootTableSize(&size)==2*PAGE_SIZE && size.NumberOfPte==1024);
    size.NumberOfPte=TG_GPU_TABLE_ENTRIES;
    CHECK(TgGpuRootTableSize(&size)==TG_GPU_TABLE_BYTES);
    size.NumberOfPte=TG_GPU_TABLE_ENTRIES+1;
    CHECK(TgGpuRootTableSize(&size)==0 && size.NumberOfPte==TG_GPU_TABLE_ENTRIES+1);
    size.NumberOfPte=1;size.PhysicalAdapterIndex=1;
    CHECK(TgGpuRootTableSize(&size)==0 && size.NumberOfPte==1);
    CHECK(NT_SUCCESS(TgGpuResolveRootTable(&segment,&set,&root)));
    CHECK(root.Valid && root.DeviceAddress==0x90020000 && root.Bytes==PAGE_SIZE && root.VaLimit==0xc000000);
    memset(instance,0xa5,sizeof(instance));
    CHECK(TgGpuWriteRootInstance(&root,&inst,FALSE)==STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(instance[128]==0xa5a5a5a5);
    CHECK(NT_SUCCESS(TgGpuWriteRootInstance(&root,&inst,TRUE)));
    CHECK(instance[128]==0x90020804 && instance[129]==0 && instance[130]==0x0bfff000 && instance[131]==0);
    CHECK(instance[127]==0xa5a5a5a5 && instance[132]==0xa5a5a5a5);
    CHECK(NT_SUCCESS(TgGpuFlushRootTable(&root,&flush,&io,&power)));
    CHECK(model.Root==0x900200 && model.Trigger==0x80000001 && model.Writes==2);
    flush.RootPageTableAddress.SegmentOffset=0;
    CHECK(TgGpuFlushRootTable(&root,&flush,&io,&power)==STATUS_INVALID_PARAMETER && model.Writes==2);
    flush.RootPageTableAddress=set.Address;
    flush.StartVirtualAddress=root.VaLimit;
    CHECK(TgGpuFlushRootTable(&root,&flush,&io,&power)==STATUS_INVALID_PARAMETER && model.Writes==2);
    flush.StartVirtualAddress=0; power.ready=false;
    CHECK(TgGpuFlushRootTable(&root,&flush,&io,&power)==STATUS_DEVICE_NOT_READY && model.Writes==2);
    power.ready=true;model.Control=0x10000;
    CHECK(TgGpuFlushRootTable(&root,&flush,&io,&power)==STATUS_IO_TIMEOUT);
    model.Control=UINT32_MAX;
    CHECK(TgGpuFlushRootTable(&root,&flush,&io,&power)==STATUS_DEVICE_DATA_ERROR);
    set.NumEntries=TG_GPU_TABLE_ENTRIES;
    CHECK(NT_SUCCESS(TgGpuResolveRootTable(&segment,&set,&root)));
    CHECK(root.Bytes==TG_GPU_TABLE_BYTES && root.VaLimit==TG_GPU_VA_LIMIT);
    CHECK(NT_SUCCESS(TgGpuWriteRootInstance(&root,&inst,TRUE)));
    CHECK(instance[130]==0xfffff000 && instance[131]==0xff);
    set.Address.SegmentOffset=sizeof(segment_bytes)-PAGE_SIZE;
    CHECK(TgGpuResolveRootTable(&segment,&set,&root)==STATUS_INVALID_ADDRESS && !root.Valid);
    CHECK(TgGpuWriteRootInstance(&root,&inst,TRUE)==STATUS_INVALID_PARAMETER);
    CHECK(instance[130]==0xfffff000 && instance[131]==0xff); /* no partial RAMIN write */
}

static void TestPitchMappings(void)
{
    static _Alignas(PAGE_SIZE) UINT64 entries[3 * TG_GPU_TABLE_ENTRIES];
    const UINT64 physical = 0x90000000;
    const UINT64 boundary = UINT64_C(1) << TG_GPU_DIRECTORY_SHIFT;
    const TG_GPU_TABLE_SEGMENT segment = {
        .Memory = {(HANDLE)2, entries, {.QuadPart=physical}, sizeof(entries)},
        .SegmentId = 1
    };
    const DXGKARG_SETROOTPAGETABLE set = {.Address={1,0,0}, .NumEntries=2};
    TG_GPU_ROOT_TABLE root;
    UINT64 *first = entries + TG_GPU_TABLE_ENTRIES;
    UINT64 *second = first + TG_GPU_TABLE_ENTRIES;
    CHECK(NT_SUCCESS(TgGpuResolveRootTable(&segment, &set, &root)));
    CHECK(tg_gpu_pde_small(physical + TG_GPU_TABLE_BYTES, &entries[0]));
    CHECK(tg_gpu_pde_small(physical + 2 * TG_GPU_TABLE_BYTES, &entries[1]));
    CHECK(tg_gpu_pte_4k(0x80000000, true, false, &first[TG_GPU_TABLE_ENTRIES - 1]));
    CHECK(TgGpuCheckPitchRange(&segment, &root, boundary - 32, boundary, TG_GPU_PITCH_READ_ONLY));
    CHECK(!TgGpuCheckPitchRange(&segment, &root, boundary - 32, boundary + 32, TG_GPU_PITCH_READ));
    CHECK(tg_gpu_pte_4k(0x80001000, true, true, &second[0]));
    CHECK(TgGpuCheckPitchRange(&segment, &root, boundary - 32, boundary + 32, TG_GPU_PITCH_READ_ONLY));
    CHECK(tg_gpu_pte_4k(0x80001000, false, true, &second[0]));
    CHECK(TgGpuCheckPitchRange(&segment, &root, boundary - 32, boundary + 32, TG_GPU_PITCH_READ));
    CHECK(!TgGpuCheckPitchRange(&segment, &root, boundary - 32, boundary + 32, TG_GPU_PITCH_WRITE));
    CHECK(TgGpuCheckPitchRange(&segment, &root, boundary, boundary + 32, TG_GPU_PITCH_WRITE));
    CHECK(tg_gpu_pte_4k_kind(0x80001000, true, true, TG_GPU_PTE_KIND_GENERIC_16BX2, &second[0]));
    CHECK(!TgGpuCheckPitchRange(&segment, &root, boundary - 32, boundary + 32, TG_GPU_PITCH_READ));
    CHECK(tg_gpu_pde_small(physical + sizeof(entries), &entries[1]));
    CHECK(!TgGpuCheckPitchRange(&segment, &root, boundary - 32, boundary + 32, TG_GPU_PITCH_READ));
}

static NTSTATUS Resolve(PVOID Context, UINT Segment, UINT64 Offset, SIZE_T Bytes, UINT64 *Address)
{
    UNREFERENCED_PARAMETER(Context);
    ++calls;
    if (Segment > 1 || Offset > 0x200000 || Bytes > 0x200000 - Offset)
        return STATUS_INVALID_ADDRESS;
    *Address = (Segment ? UINT64_C(0x170000000) : UINT64_C(0x160000000)) + Offset;
    return STATUS_SUCCESS;
}

static void TestPagingBuffer(void)
{
    static _Alignas(8) UCHAR commands[512];
    TG_GPU_TABLE_SEGMENT segment = {
        .Memory = {(HANDLE)2,segment_bytes,{.QuadPart=0x90000000},sizeof(segment_bytes)},
        .SegmentId = 1
    };
    TG_GPU_ROOT_TABLE root = {0};
    DXGKARG_SETROOTPAGETABLE set = {.Address={1,0,TG_GPU_TABLE_BYTES},.NumEntries=1};
    TG_GPU_PAGING_BUILD build = {&segment,TG_GPU_TABLE_BYTES,&root,
        scratch,TG_GPU_TABLE_ENTRIES,TRUE,TRUE};
    DXGK_PTE ptes[3] = {0};
    DXGKARG_BUILDPAGINGBUFFER args = {0}, flush = {0};
    UINT64 *entries = (UINT64 *)segment_bytes;
    struct tlb_model model = {.Control=0x18000,.ExpectedEntry=&entries[7],.Expected=UINT64_C(0x101700011)};
    struct tg_gpu_power_io io = {&model,TlbRead,TlbWrite,NULL,NULL,TlbDelay, NULL};
    struct tg_gpu_power power = {.owned=true,.ready=true};
    SIZE_T prefix, bytes, completed = 99;
    UINT saved, i;
    CHECK(NT_SUCCESS(TgGpuResolveRootTable(&segment,&set,&root)));
    memset(segment_bytes,0xa5,sizeof(segment_bytes));
    memset(commands,0xcc,sizeof(commands));
    for (i=0;i<3;++i) { ptes[i].Valid=1;ptes[i].Segment=1;ptes[i].PageAddress=i+1; }
    ptes[0].Segment=0;ptes[0].PageAddress=UINT64_C(0x170001000)>>PAGE_SHIFT;
    ptes[2].Segment=0;ptes[2].PageAddress=0x80003000u>>PAGE_SHIFT;ptes[2].ReadOnly=1;
    args.Operation=DXGK_OPERATION_UPDATE_PAGE_TABLE;
    args.UpdatePageTable.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    args.UpdatePageTable.PageTableAddress.GpuPhysical=(D3DGPU_PHYSICAL_ADDRESS){1,0,0};
    args.UpdatePageTable.StartIndex=7;args.UpdatePageTable.NumPageTableEntries=3;
    args.UpdatePageTable.pPageTableEntries=ptes;
    args.pDmaBuffer=commands;args.DmaSize=64;
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(args.pDmaBuffer==commands && args.MultipassOffset==0 && commands[0]==0xcc);
    /* A bad last entry cannot publish a valid first partial record. */
    args.DmaSize=72;ptes[2].NoExecute=1;
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_NOT_SUPPORTED);
    CHECK(args.pDmaBuffer==commands && args.MultipassOffset==0 && commands[0]==0xcc);
    ptes[2].NoExecute=0;
    ptes[2].PageAddress=UINT64_C(1)<<(34-PAGE_SHIFT);
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_INVALID_ADDRESS);
    CHECK(args.pDmaBuffer==commands && args.MultipassOffset==0 && commands[0]==0xcc);
    ptes[2].PageAddress=0x80003000u>>PAGE_SHIFT;
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(args.MultipassOffset==1 && args.pDmaBuffer==commands+72);
    args.DmaSize=sizeof(commands)-72;
    CHECK(NT_SUCCESS(TgGpuBuildPagingBuffer(&args,&build)) && args.MultipassOffset==0);
    prefix=(UCHAR *)args.pDmaBuffer-commands;
    CHECK(entries[7]==UINT64_C(0xa5a5a5a5a5a5a5a5) && model.Writes==0);
    flush.Operation=DXGK_OPERATION_FLUSH_TLB;flush.pDmaBuffer=args.pDmaBuffer;
    flush.DmaSize=(UINT)(sizeof(commands)-prefix);
    flush.FlushTlb.RootPageTableAddress=set.Address;
    CHECK(NT_SUCCESS(TgGpuBuildPagingBuffer(&flush,&build)) && model.Writes==0);
    bytes=(UCHAR *)flush.pDmaBuffer-commands;
    /* No borrowed PTE pointer survives BuildPagingBuffer. */
    memset(ptes,0,sizeof(ptes));
    CHECK(TgGpuExecutePagingBuffer(&segment,commands,bytes,FALSE,&io,&power,&completed,NULL)==STATUS_GRAPHICS_ALLOCATION_BUSY);
    CHECK(completed==0 && entries[7]==UINT64_C(0xa5a5a5a5a5a5a5a5) && !model.Writes);
    power.ready=false;
    CHECK(TgGpuExecutePagingBuffer(&segment,commands,bytes,TRUE,&io,&power,&completed,NULL)==STATUS_DEVICE_NOT_READY);
    CHECK(completed==0 && !model.Writes);power.ready=true;
    /* Corrupt the trailing operation, retaining the valid update prefix. */
    saved=*(UINT *)(commands+prefix);*(UINT *)(commands+prefix)=UINT32_MAX;
    CHECK(TgGpuExecutePagingBuffer(&segment,commands,bytes,TRUE,&io,&power,&completed,NULL)==STATUS_INVALID_PARAMETER);
    CHECK(completed==0 && entries[7]==UINT64_C(0xa5a5a5a5a5a5a5a5) && !model.Writes);
    *(UINT *)(commands+prefix)=saved;
    CHECK(NT_SUCCESS(TgGpuExecutePagingBuffer(&segment,commands,bytes,TRUE,&io,&power,&completed,NULL)));
    CHECK(completed==bytes && model.Writes==2 && model.Root==0x900200);
    CHECK(entries[7]==UINT64_C(0x101700011) && entries[8]==UINT64_C(0x100900021) && entries[9]==UINT64_C(0x100800035));
    model.Control=0x10000;
    CHECK(TgGpuExecutePagingBuffer(&segment,commands,bytes,TRUE,&io,&power,&completed,NULL)==STATUS_IO_TIMEOUT);
    CHECK(completed==prefix && completed!=bytes); /* do not complete the paging fence */

    /* The executor stops at a CE record and reports only preceding bytes.
     * The source VA is the OS-supplied address, without AllocationOffset. */
    model.Control=0x18000;
    DXGKARG_BUILDPAGINGBUFFER dma = {.Operation=DXGK_OPERATION_VIRTUAL_FILL,
        .pDmaBuffer=commands+bytes,.DmaSize=sizeof(commands)-(UINT)bytes};
    dma.FillVirtual=(DXGK_BUILDPAGINGBUFFER_FILLVIRTUAL){.DestinationVirtualAddress=0x1030,
        .FillSizeInBytes=4000,.FillPattern=0x78563412};
    CHECK(TgGpuBuildPagingBuffer(&dma,&build)==STATUS_SUCCESS);
    SIZE_T copy_offset=(UCHAR *)dma.pDmaBuffer-commands;
    dma.Operation=DXGK_OPERATION_VIRTUAL_TRANSFER;
    dma.DmaSize=(UINT)(sizeof(commands)-copy_offset);
    dma.TransferVirtual=(DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL){
        .AllocationOffsetInBytes=PAGE_SIZE,.SourceVirtualAddress=0x1030,
        .DestinationVirtualAddress=0x2030,.TransferSizeInBytes=4000};
    dma.TransferVirtual.Flags.Src64KBPages=1;
    CHECK(TgGpuBuildPagingBuffer(&dma,&build)==STATUS_NOT_SUPPORTED);
    dma.TransferVirtual.Flags.Flags=0;
    CHECK(TgGpuBuildPagingBuffer(&dma,&build)==STATUS_SUCCESS);
    SIZE_T end=(UCHAR *)dma.pDmaBuffer-commands;
    BOOLEAN requires=FALSE;
    CHECK(TgGpuValidatePagingBuffer(&segment,commands,end,&requires)==STATUS_SUCCESS && requires);
    TG_GPU_PAGING_DMA_STEP step={0};
    CHECK(TgGpuExecutePagingBuffer(&segment,commands,end,TRUE,&io,&power,&completed,&step)==STATUS_SUCCESS);
    CHECK(completed==bytes && step.RecordBytes==copy_offset-bytes && step.Operation.fill &&
        step.Operation.bytes==4000 && step.Operation.destination==0x1030 && step.Operation.pattern==0x78563412);
    CHECK(TgGpuExecutePagingBuffer(&segment,commands+copy_offset,end-copy_offset,TRUE,&io,&power,&completed,&step)==STATUS_SUCCESS);
    CHECK(!completed && step.RecordBytes==end-copy_offset && !step.Operation.fill &&
        step.Operation.source==0x1030 && step.Operation.destination==0x2030 && step.Operation.bytes==4000);

    /* VidMm carries an NVK color or depth/stencil kind in DriverProtection. */
    args.pDmaBuffer=commands;args.DmaSize=sizeof(commands);
    args.UpdatePageTable.StartIndex=10;args.UpdatePageTable.NumPageTableEntries=1;
    ptes[0]=(DXGK_PTE){.Valid=1,.ReadOnly=1,.Segment=1,.PageAddress=1};
    args.UpdatePageTable.DriverProtection=0xff;
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_NOT_SUPPORTED && args.pDmaBuffer==commands);
    args.UpdatePageTable.DriverProtection=0x02; /* Compressed Z16. */
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_NOT_SUPPORTED && args.pDmaBuffer==commands);
    args.UpdatePageTable.DriverProtection=0x101; /* Bits above the kind field. */
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_NOT_SUPPORTED && args.pDmaBuffer==commands);
    const UINT8 kinds[] = {0xfe, 0x01, 0x11, 0x2a, 0x46, 0x7b, 0xc3};
    for (UINT i = 0; i < sizeof(kinds); i++) {
        args.pDmaBuffer=commands;args.DmaSize=sizeof(commands);
        args.UpdatePageTable.DriverProtection=kinds[i];
        CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_SUCCESS);
        bytes=(UCHAR *)args.pDmaBuffer-commands;
        CHECK(TgGpuExecutePagingBuffer(&segment,commands,bytes,TRUE,&io,&power,&completed,NULL)==STATUS_SUCCESS);
        CHECK(completed==bytes && entries[10]==(((UINT64)kinds[i]<<36)|UINT64_C(0x100900015)));
    }

    /* Mandatory immediate initialization accepts a different CPU alias only
     * when every page resolves to the owned contiguous table span.
     */
    args.pDmaBuffer=NULL;args.MultipassOffset=0;
    args.UpdatePageTable.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    args.UpdatePageTable.PageTableAddress.CpuVirtual=alias_bytes;
    args.UpdatePageTable.StartIndex=12;args.UpdatePageTable.NumPageTableEntries=1;
    ptes[0].Valid=1;ptes[0].Segment=1;ptes[0].PageAddress=1;
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_NOT_SUPPORTED);
    args.UpdatePageTable.DriverProtection=0;ptes[0].ReadOnly=0;
    CHECK(NT_SUCCESS(TgGpuBuildPagingBuffer(&args,&build)));
    CHECK(entries[12]==UINT64_C(0x100900011));
    entries[12]=0;bad_alias_page=TRUE;
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_INVALID_ADDRESS && entries[12]==0);
    bad_alias_page=FALSE;build.InitialMappingUncached=FALSE;
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_NOT_SUPPORTED && entries[12]==0);
    /* The Lite's first VidMm update cleared a leaf-table tail with a DMA
     * pointer present. Only entries in that range belong to this update. */
    build.InitialMappingUncached=TRUE;
    memset(entries,0xa5,TG_GPU_TABLE_BYTES);
    memset(commands,0xcc,sizeof(commands));
    args.pDmaBuffer=commands;args.DmaSize=sizeof(commands);
    args.UpdatePageTable.StartIndex=128;
    args.UpdatePageTable.NumPageTableEntries=TG_GPU_TABLE_ENTRIES-128;
    args.UpdatePageTable.Flags.InitialUpdate=1;
    args.UpdatePageTable.Flags.Repeat=1;
    ptes[0]=(DXGK_PTE){0};
    CHECK(TgGpuBuildPagingBuffer(&args,&build)==STATUS_SUCCESS);
    CHECK(args.pDmaBuffer==commands && args.DmaSize==sizeof(commands) && !args.MultipassOffset);
    for (i=0;i<sizeof(commands);++i) CHECK(commands[i]==0xcc);
    for (i=0;i<128;++i) CHECK(entries[i]==UINT64_C(0xa5a5a5a5a5a5a5a5));
    for (;i<TG_GPU_TABLE_ENTRIES;++i) CHECK(entries[i]==0);
}

void start(void)
{
    DXGK_PTE ptes[3] = {0};
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u = {0};
    TG_GPU_PAGE_UPDATE_TARGET target = {table, sizeof(table), scratch,
        TG_GPU_TABLE_ENTRIES, TRUE, TRUE, Resolve, NULL};
    DXGK_PAGE_TABLE_LEVEL_DESC desc;
    UINT64 before;
    ULONG i, written;
    static const char message[] = "PASS: WDK physical system/table PTEs, multipass ordering, alias bounds and TLB failures\r\n";
    TestResources();
    TestDisplayResources();
    TestNvdecResources();
    TestDisplayFlip();
    TestSegment();
    TestRoot();
    TestPitchMappings();
    TestPagingBuffer();
    for (i = 0; i < TG_GPU_TABLE_ENTRIES; ++i) table[i] = UINT64_C(0xa5a5a5a5a5a5a5a5);
    u.PageTableAddress.CpuVirtual = table;
    u.UpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    u.pPageTableEntries = ptes;
    u.StartIndex = 7;
    u.NumPageTableEntries = 2;
    ptes[0].Valid = 1; ptes[0].PageAddress = 1; ptes[0].ReadOnly = 1;
    ptes[1].Valid = 1; ptes[1].PageAddress = 2;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == UINT64_C(0x101600015));
    CHECK(table[8] == UINT64_C(0x101600021));
    CHECK(table[6] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    CHECK(table[9] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    CHECK(calls == 2);
    u.Flags.Repeat = 1; u.NumPageTableEntries = 3; calls = 0;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == table[8] && table[8] == table[9]);
    ptes[0].Valid = 0; ptes[0].PageAddress = UINT64_MAX;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == 0 && table[8] == 0 && table[9] == 0);
    CHECK(calls == 3); /* invalid entries never resolve stale addresses */
    u.Flags.Repeat = 0; u.NumPageTableEntries = 2;
    ptes[0].Valid = 1; ptes[0].PageAddress = 3;
    ptes[1].NoExecute = 1;
    before = table[7];
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED);
    CHECK(table[7] == before && table[8] == 0);
    ptes[1].NoExecute = 0; ptes[1].CacheCoherent = 1;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED);
    ptes[1].CacheCoherent = 0; ptes[1].PageAddress = 0x201000u >> PAGE_SHIFT;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_ADDRESS);
    CHECK(table[7] == before && table[8] == 0);
    ptes[1].PageAddress = (UINT64_MAX >> PAGE_SHIFT) + 1;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_PARAMETER);
    CHECK(table[7] == before && table[8] == 0);
    u.StartIndex = TG_GPU_TABLE_ENTRIES - 1;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_PARAMETER);
    u.StartIndex = 7; u.NumPageTableEntries = 1; u.PageTableLevel = 1;
    ptes[0].ReadOnly = 0;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED); /* system-segment table too big */
    ptes[0].Segment = 1; ptes[0].PageAddress = 0x20000u >> PAGE_SHIFT;
    CHECK(NT_SUCCESS(TgGpuUpdatePageTable(&u, &target)));
    CHECK(table[7] == UINT64_C(0x0170020500000000));
    ptes[0].PageAddress = 0x1f0000u >> PAGE_SHIFT;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_INVALID_ADDRESS); /* whole child must fit */
    CHECK(table[7] == UINT64_C(0x0170020500000000));
    ptes[0].PageAddress = 0x20000u >> PAGE_SHIFT; ptes[0].PageTablePageSize = DXGK_PTE_PAGE_TABLE_PAGE_64KB;
    CHECK(TgGpuUpdatePageTable(&u, &target) == STATUS_NOT_SUPPORTED);
    CHECK(TgGpuDescribePageTable(0, 0, &desc) == STATUS_INVALID_PARAMETER);
    CHECK(NT_SUCCESS(TgGpuDescribePageTable(0, 1, &desc)));
    CHECK(desc.PageTableSegmentId == 1 && desc.PagingProcessPageTableSegmentId == 1);
    CHECK(desc.PageTableIndexBitCount == 14 && desc.PageTableSizeInBytes == 131072);
    WriteFile(GetStdHandle((ULONG)-11), message, sizeof(message)-1, &written, NULL);
    ExitProcess(0);
}
