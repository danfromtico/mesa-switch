/* SPDX-License-Identifier: MIT */
/* Real WDK layouts/helper with explicit CPU substitutes for OS callbacks.
 * No real Dxgkrnl allocation, IOMMU route or GPU execution is proved here. */
#include "../windows/nvdec_memory.h"
#include <ntimage.h>

static ULONG_PTR cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64), .SecurityCookie = (ULONGLONG)&cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, const void *, ULONG, ULONG *, void *);
#define CHECK(c) do { if (!(c)) ExitProcess(__LINE__); } while (0)
void *memset(void *p, int c, size_t n)
{ UCHAR *d = p; while (n--) *d++ = (UCHAR)c; return p; }
void *memcpy(void *p, const void *q, size_t n)
{ UCHAR *d = p; const UCHAR *s = q; while (n--) *d++ = *s++; return p; }

static _Alignas(PAGE_SIZE) UCHAR pages[8][PAGE_SIZE * 2];
static _Alignas(16) UCHAR metadata[8][256];
static BOOLEAN pages_used[8], metadata_used[8], fail_allocate;
static UINT64 physical_override;
static HANDLE fail_free;
static UINT allocate_calls, free_calls, frees, metadata_count;

static PVOID NTAPI PoolAlloc(POOL_FLAGS flags, SIZE_T bytes, ULONG tag)
{
    (void)tag;
    CHECK(flags == POOL_FLAG_NON_PAGED && bytes <= sizeof(metadata[0]));
    for (UINT i = 0; i < 8; ++i) if (!metadata_used[i]) {
        metadata_used[i] = TRUE; ++metadata_count;
        return metadata[i];
    }
    return NULL;
}
static VOID NTAPI PoolFree(PVOID p, ULONG tag)
{
    (void)tag;
    for (UINT i = 0; i < 8; ++i) if (p == metadata[i]) {
        CHECK(metadata_used[i]); metadata_used[i] = FALSE; --metadata_count; return;
    }
    CHECK(FALSE);
}
PVOID (NTAPI *const __imp_ExAllocatePool2)(POOL_FLAGS, SIZE_T, ULONG) = PoolAlloc;
VOID (NTAPI *const __imp_ExFreePoolWithTag)(PVOID, ULONG) = PoolFree;
static PHYSICAL_ADDRESS NTAPI Physical(PVOID p)
{
    PHYSICAL_ADDRESS a = {0};
    for (UINT i = 0; i < 8; ++i) if (p == pages[i]) {
        CHECK(pages_used[i]);
        a.QuadPart = (LONGLONG)(physical_override ? physical_override : 0x90000000u + i * 0x10000u);
        return a;
    }
    CHECK(FALSE); return a;
}
PHYSICAL_ADDRESS (NTAPI *const __imp_MmGetPhysicalAddress)(PVOID) = Physical;
static NTSTATUS APIENTRY Allocate(HANDLE device, DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *a)
{
    CHECK(device == (HANDLE)0x1234);
    CHECK(a->NumberOfBytes && a->NumberOfBytes <= sizeof(pages[0]) && !(a->NumberOfBytes & (PAGE_SIZE - 1)));
    CHECK(a->LowestAcceptableAddress.QuadPart == 0 && a->HighestAcceptableAddress.QuadPart == UINT32_MAX);
    CHECK(!a->BoundaryAddressMultiple.QuadPart && a->CacheType == DXGK_MEMORY_CACHING_TYPE_NON_CACHED);
    ++allocate_calls;
    if (fail_allocate) return STATUS_NO_MEMORY;
    for (UINT i = 0; i < 8; ++i) if (!pages_used[i]) {
        pages_used[i] = TRUE;
        a->hMemoryHandle = (HANDLE)(ULONG_PTR)(i + 1); a->pMemory = pages[i];
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MEMORY;
}
static NTSTATUS APIENTRY Free(HANDLE device, const DXGKARGCB_FREECONTIGUOUSMEMORY *a)
{
    CHECK(device == (HANDLE)0x1234);
    ULONG_PTR i = (ULONG_PTR)a->hMemoryHandle;
    CHECK(i && i <= 8 && pages_used[i - 1]); ++free_calls;
    if (a->hMemoryHandle == fail_free) return STATUS_DEVICE_BUSY;
    pages_used[i - 1] = FALSE; ++frees;
    return STATUS_SUCCESS;
}
static DXGKRNL_INTERFACE dxgk = {
    .Version = DXGKDDI_INTERFACE_VERSION_WDDM2_6,
    .DeviceHandle = (HANDLE)0x1234,
    .DxgkCbAllocateContiguousMemory = Allocate,
    .DxgkCbFreeContiguousMemory = Free,
};

void start(void)
{
    TG_NVDEC_MEMORY pool = {0}, second = {0};
    struct tg_nvdec_dma first = {0}, other = {0};
    CHECK(TgNvdecMemoryInit(&pool, &dxgk) == STATUS_SUCCESS);
    CHECK(TgNvdecMemoryInit(&pool, &dxgk) == STATUS_INVALID_PARAMETER);
    CHECK(TgNvdecMemoryAllocate(&pool, 17, &first));
    CHECK(first.size == 17 && first.bus == 0x90000000 && first.cpu == pages[0]);
    CHECK(allocate_calls == 1 && metadata_count == 1);
    /* Finish never frees an active descriptor just to make cleanup pass. */
    CHECK(TgNvdecMemoryFinish(&pool) == STATUS_DEVICE_BUSY && !free_calls);
    CHECK(!TgNvdecMemoryAllocate(&pool, 17, &first) && allocate_calls == 1);
    TgNvdecMemoryRetire(&pool, &first);
    CHECK(!first.cpu && !first.bus && !first.size && frees == 1 && !metadata_count);
    CHECK(TgNvdecMemoryFinish(&pool) == STATUS_SUCCESS && !pool.Dxgk);

    CHECK(TgNvdecMemoryInit(&pool, &dxgk) == STATUS_SUCCESS);
    CHECK(TgNvdecMemoryAllocate(&pool, PAGE_SIZE + 1, &first));
    CHECK(TgNvdecMemoryAllocate(&pool, 256, &other));
    fail_free = (HANDLE)1;
    TgNvdecMemoryRetire(&pool, &first);
    CHECK(!first.cpu && pages_used[0] && metadata_count == 2);
    TgNvdecMemoryRetire(&pool, &other);
    CHECK(!pages_used[1] && metadata_count == 1);
    CHECK(TgNvdecMemoryFinish(&pool) == STATUS_DEVICE_BUSY && pool.Dxgk);
    CHECK(!TgNvdecMemoryAllocate(&pool, 256, &first));
    CHECK(pages_used[0] && metadata_count == 1 && !first.cpu);
    fail_free = NULL;
    CHECK(TgNvdecMemoryFinish(&pool) == STATUS_SUCCESS && !pages_used[0] && !metadata_count);

    CHECK(TgNvdecMemoryInit(&pool, &dxgk) == STATUS_SUCCESS);
    fail_allocate = TRUE;
    CHECK(!TgNvdecMemoryAllocate(&pool, 256, &first) && !first.cpu && !metadata_count);
    fail_allocate = FALSE;
    physical_override = UINT64_C(0xfffff000);
    CHECK(!TgNvdecMemoryAllocate(&pool, PAGE_SIZE + 1, &first));
    CHECK(!first.cpu && !pages_used[0] && !metadata_count);
    physical_override = UINT64_C(0x100000000); fail_free = (HANDLE)1;
    CHECK(!TgNvdecMemoryAllocate(&pool, PAGE_SIZE, &first));
    CHECK(!first.cpu && pages_used[0] && metadata_count == 1);
    fail_free = NULL; physical_override = 0;
    CHECK(TgNvdecMemoryFinish(&pool) == STATUS_SUCCESS && !metadata_count);

    CHECK(TgNvdecMemoryInit(&pool, &dxgk) == STATUS_SUCCESS);
    CHECK(TgNvdecMemoryInit(&second, &dxgk) == STATUS_SUCCESS);
    CHECK(TgNvdecMemoryAllocate(&second, 256, &first));
    UINT calls = free_calls;
    TgNvdecMemoryRetire(&pool, &first);
    CHECK(pool.Corrupt && first.cpu && free_calls == calls && pages_used[0]);
    /* Closing an empty owner is safe, and must not free the foreign buffer. */
    CHECK(TgNvdecMemoryFinish(&pool) == STATUS_SUCCESS && pages_used[0]);
    TgNvdecMemoryRetire(&second, &first);
    CHECK(TgNvdecMemoryFinish(&second) == STATUS_SUCCESS && !metadata_count);

    TG_NVDEC_MEMORY missing = {0}; DXGKRNL_INTERFACE unsupported = dxgk;
    unsupported.DxgkCbFreeContiguousMemory = NULL;
    CHECK(TgNvdecMemoryInit(&missing, &unsupported) == STATUS_NOT_SUPPORTED && !missing.Dxgk);
    unsupported = dxgk; unsupported.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_3;
    CHECK(TgNvdecMemoryInit(&missing, &unsupported) == STATUS_NOT_SUPPORTED);
    ULONG written; const char ok[] = "PASS: tracked NVDEC allocation, retained free failures, bounds and owner isolation (CPU only)\n";
    WriteFile(GetStdHandle((ULONG)-11), ok, sizeof(ok) - 1, &written, NULL);
    ExitProcess(0);
}
