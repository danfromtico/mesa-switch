/* SPDX-License-Identifier: MIT */
/* Host multi-threaded regression suite for windows/platform_io.c + SwFanGpuPmic:
 * covers all 7 GC-V9F-01..05 adversarial concurrency & lifecycle cases
 * (including 2-pthread ObfReferenceObject vs PnP QUERY_REMOVE and 2-pthread
 * CLAIM_RAIL vs RELEASE_RAIL split-lock overtake).
 */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void VOID, *PVOID, *HANDLE;
typedef uint8_t UCHAR, UINT8, *PUCHAR, BOOLEAN;
typedef uint16_t USHORT, UINT16, WCHAR;
typedef const WCHAR *PCWSTR;
typedef uint32_t ULONG, UINT, UINT32, *PULONG, POOL_FLAGS, ACCESS_MASK;
typedef int32_t LONG, NTSTATUS;
typedef uint64_t ULONGLONG, UINT64;
typedef int64_t LONGLONG;
typedef uintptr_t ULONG_PTR, SIZE_T;
typedef intptr_t LONG_PTR;
typedef char CCHAR;
typedef unsigned KIRQL;
typedef int KPROCESSOR_MODE, EVENT_TYPE, KWAIT_REASON, WORK_QUEUE_TYPE, KPRIORITY;
typedef union _LARGE_INTEGER {
    LONGLONG QuadPart;
    struct { uint32_t LowPart; int32_t HighPart; } u;
} LARGE_INTEGER, *PLARGE_INTEGER, PHYSICAL_ADDRESS;
typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; const WCHAR *Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct _OBJECT_ATTRIBUTES { ULONG Length; HANDLE RootDirectory; PUNICODE_STRING ObjectName; ULONG Attributes; PVOID SecurityDescriptor, SecurityQualityOfService; } OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef struct _IO_STATUS_BLOCK { NTSTATUS Status; ULONG_PTR Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
typedef struct _GUID { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef struct { pthread_mutex_t *ptr; } KSPIN_LOCK, *PKSPIN_LOCK;
typedef struct _KEVENT { pthread_mutex_t mtx; pthread_cond_t cv; bool signaled; } KEVENT, *PRKEVENT;

#define NTAPI
#define FASTCALL
#define PAGED_CODE() ((void)0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define __forceinline inline
#define TRUE 1
#define FALSE 0
#define MAXULONG 0xffffffffu
#define KernelMode 0
#define Executive 0
#define NotificationEvent 0
#define DelayedWorkQueue 1
#define POOL_FLAG_NON_PAGED 0x40ULL
#define OBJ_CASE_INSENSITIVE 0x40u
#define OBJ_KERNEL_HANDLE 0x200u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_READ_DATA 1u
#define FILE_WRITE_DATA 2u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_OPEN 1u
#define FILE_NON_DIRECTORY_FILE 0x40u
#define IRP_MJ_DEVICE_CONTROL 14u
#define CmResourceTypeMemory 3u
#define EventCategoryTargetDeviceChange 2
#define IO_NO_INCREMENT 0

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS                    ((NTSTATUS)0x00000000)
#define STATUS_PENDING                    ((NTSTATUS)0x00000103)
#define STATUS_TIMEOUT                    ((NTSTATUS)0x00000102)
#define STATUS_UNSUCCESSFUL               ((NTSTATUS)0xC0000001)
#define STATUS_NOT_SUPPORTED              ((NTSTATUS)0xC00000BB)
#define STATUS_INVALID_PARAMETER          ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_REQUEST     ((NTSTATUS)0xC0000010)
#define STATUS_MORE_PROCESSING_REQUIRED   ((NTSTATUS)0xC0000016)
#define STATUS_ACCESS_DENIED              ((NTSTATUS)0xC0000022)
#define STATUS_INSUFFICIENT_RESOURCES     ((NTSTATUS)0xC000009A)
#define STATUS_DEVICE_DATA_ERROR          ((NTSTATUS)0xC000009C)
#define STATUS_IO_TIMEOUT                 ((NTSTATUS)0xC00000B5)
#define STATUS_CANCELLED                  ((NTSTATUS)0xC0000120)
#define STATUS_INVALID_DEVICE_STATE       ((NTSTATUS)0xC0000184)
#define STATUS_IO_DEVICE_ERROR            ((NTSTATUS)0xC0000185)
#define STATUS_DEVICE_PROTOCOL_ERROR      ((NTSTATUS)0xC0000186)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182)
#define STATUS_DEVICE_NOT_READY           ((NTSTATUS)0xC00000A3)
#define STATUS_DEVICE_BUSY                ((NTSTATUS)0x80000011)

#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define FILE_DEVICE_UNKNOWN 0x00000022u
#define METHOD_BUFFERED 0u
#define FILE_ANY_ACCESS 0u

#define DECLARE_CONST_UNICODE_STRING(_var, _str) \
    static const WCHAR _var##_buf[] = _str; \
    const UNICODE_STRING _var = { sizeof(_var##_buf) - sizeof(WCHAR), sizeof(_var##_buf), _var##_buf }
#define InitializeObjectAttributes(p, n, a, r, s) do { \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES); (p)->RootDirectory = (r); \
    (p)->Attributes = (a); (p)->ObjectName = (n); \
    (p)->SecurityDescriptor = (s); (p)->SecurityQualityOfService = NULL; \
} while (0)
#define IsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)

static const GUID GUID_TARGET_DEVICE_QUERY_REMOVE     = {1,0,0,{0}};
static const GUID GUID_TARGET_DEVICE_REMOVE_COMPLETE  = {2,0,0,{0}};
static const GUID GUID_TARGET_DEVICE_REMOVE_CANCELLED = {3,0,0,{0}};

struct _DRIVER_OBJECT;
typedef struct _DEVICE_OBJECT { struct _DRIVER_OBJECT *DriverObject; CCHAR StackSize; ULONG DeviceType; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT { PDEVICE_OBJECT DeviceObject; UNICODE_STRING DriverName; } DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _FILE_OBJECT { PDEVICE_OBJECT DeviceObject; } FILE_OBJECT, *PFILE_OBJECT;
typedef void *POBJECT_TYPE, *POBJECT_HANDLE_INFORMATION;
extern POBJECT_TYPE *IoFileObjectType;

typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    PFILE_OBJECT FileObject;
    struct { struct { ULONG OutputBufferLength, InputBufferLength, IoControlCode; } DeviceIoControl; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

struct _IRP;
typedef NTSTATUS (*PIO_COMPLETION_ROUTINE)(PDEVICE_OBJECT, struct _IRP *, PVOID);
typedef struct _IRP {
    KPROCESSOR_MODE RequestorMode;
    BOOLEAN Cancel;
    struct { PVOID SystemBuffer; } AssociatedIrp;
    IO_STATUS_BLOCK IoStatus;
    IO_STACK_LOCATION Stack;
    PIO_COMPLETION_ROUTINE CompCb;
    PVOID CompCtx;
} IRP, *PIRP;
#define IoGetNextIrpStackLocation(irp) (&(irp)->Stack)

typedef VOID (*PIO_WORKITEM_ROUTINE)(PDEVICE_OBJECT, PVOID);
typedef struct _IO_WORKITEM { PDEVICE_OBJECT Dev; bool queued; bool freed; } *PIO_WORKITEM;
typedef int IO_NOTIFICATION_EVENT_CATEGORY;
typedef NTSTATUS (*PDRIVER_NOTIFICATION_CALLBACK_ROUTINE)(PVOID, PVOID);
typedef struct { GUID Event; PFILE_OBJECT FileObject; } TARGET_DEVICE_REMOVAL_NOTIFICATION;

typedef struct {
    UCHAR Type;
    union { struct { LARGE_INTEGER Start; ULONG Length; } Memory; } u;
} CM_PARTIAL_RESOURCE_DESCRIPTOR;

static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG ex, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, ex, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
static inline LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG ex, LONGLONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, ex, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
#define KeMemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _DataSynchronizationBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
static inline ULONG READ_REGISTER_ULONG(PULONG p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline VOID WRITE_REGISTER_ULONG(PULONG p, ULONG v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

#include "platform/t214_resources.h"
enum tg_gpu_aperture {
    TgGpuRegisters, TgGpuBar1, TgGpuAuxRegisters, TgGpuCarX, TgGpuOut5,
    TgGpuClamp, TgGpuMcFlush, TgGpuMcAsid, TgGpuHidrev, TgGpuSpeedo,
    TgGpuCalibration, TgGpuApertureCount
};
typedef struct _TG_GPU_RESOURCES {
    CM_PARTIAL_RESOURCE_DESCRIPTOR Memory[TgGpuApertureCount];
    PVOID Mapping[TgGpuApertureCount];
    PVOID LicMapping;
} TG_GPU_RESOURCES;

#define TEGRA_GPU_WINDOWS_RESOURCES_H
#define _NTIFS_
#define INITGUID
#define _WDMGUID_H_

static POBJECT_TYPE g_fake_ft = (POBJECT_TYPE)(uintptr_t)0x46494c45u;
POBJECT_TYPE *IoFileObjectType = &g_fake_ft;
static _Atomic ULONGLONG g_now_100ns = 10000000ULL;
static ULONGLONG KeQueryInterruptTime(VOID) { return atomic_load(&g_now_100ns); }

typedef void *WDFDEVICE, *WDFIOTARGET, *WDFINTERRUPT, *WDFREQUEST, *WDFWAITLOCK;
typedef struct { void *Buffer; size_t Size; } WDF_MEMORY_DESCRIPTOR;
typedef struct { int64_t Timeout; } WDF_REQUEST_SEND_OPTIONS;
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define WDF_REQUEST_SEND_OPTION_TIMEOUT 1
#define WDF_REL_TIMEOUT_IN_MS(n) (n)
#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(m, b, s) (*(m) = (WDF_MEMORY_DESCRIPTOR){ (b), (s) })
#define WDF_REQUEST_SEND_OPTIONS_INIT(o, f) ((void)(f), memset((o), 0, sizeof(*(o))))
#define RtlZeroMemory(p, n) memset((p), 0, (n))
#define CPU_M4_VOUT 0x26u
#include "integration/switch-lite/windows/SwitchFan/src/t214/emc_gpu_vote.h"
#define SWFAN_EMC_RATE_VALID 1u

typedef struct {
    UCHAR *Car;
    struct { UCHAR PmicAddress; } Config;
    void *Pmic;
    BOOLEAN IsLite, Stopping, CpuConfigured, GpuBridgeSeen, TempValid;
    ULONG LastTemp;
    volatile LONG GpuDvfsPolicy;
    ULONG GpuReportedKhz, GpuReportedUv, GpuStatusWord;
    struct { ULONGLONG SampleTime; struct { ULONG ActualKhz, Flags; } Published; } Emc;
    struct tg_emc_gpu_vote EmcGpuVote;
    ULONGLONG EmcGpuAbiTime, GpuSampleTime, GpuAbiTime, TempSampleTime;
    pthread_mutex_t pmic_mtx, cpu_mtx;
} SWFAN_DEVICE, *PSWFAN_DEVICE;

static SWFAN_DEVICE g_swfan;
static UCHAR g_pmic_regs[256];
static _Atomic ULONG g_provider_claims = 0, g_provider_releases = 0;

static VOID SwFanCpuLock(PSWFAN_DEVICE d) { pthread_mutex_lock(&d->cpu_mtx); }
static VOID SwFanCpuUnlock(PSWFAN_DEVICE d) { pthread_mutex_unlock(&d->cpu_mtx); }
static VOID SwFanPmicLock(PSWFAN_DEVICE d) { pthread_mutex_lock(&d->pmic_mtx); }
static VOID SwFanPmicUnlock(PSWFAN_DEVICE d) { pthread_mutex_unlock(&d->pmic_mtx); }
static ULONG SwCpuRead(PSWFAN_DEVICE d, ULONG off) { (void)d; (void)off; return 0x12345678u; }
static VOID KeDelayExecutionThread_SwFan(int m, BOOLEAN a, LARGE_INTEGER *d) { (void)m;(void)a;(void)d; }
#define KeDelayExecutionThread KeDelayExecutionThread_SwFan
static NTSTATUS SwFanReadReg(void *t, UCHAR reg, UCHAR *v) { (void)t; *v = g_pmic_regs[reg]; return STATUS_SUCCESS; }
static NTSTATUS WdfIoTargetSendWriteSynchronously(void *t, void *r, WDF_MEMORY_DESCRIPTOR *m,
    void *o, WDF_REQUEST_SEND_OPTIONS *s, ULONG_PTR *n) {
    UCHAR *b = m->Buffer; (void)t;(void)r;(void)o;(void)s;
    g_pmic_regs[b[0]] = b[1]; *n = 2; return STATUS_SUCCESS;
}
struct sw_req { void *buf; size_t len; };
static int WdfRequestGetRequestorMode(WDFREQUEST r) { (void)r; return KernelMode; }
static NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST r, size_t min_l, PVOID *b, size_t *l) {
    struct sw_req *sr = r; if (sr->len < min_l) return STATUS_INVALID_PARAMETER;
    *b = sr->buf; if (l) *l = sr->len; return STATUS_SUCCESS;
}
static NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r, size_t min_l, PVOID *b, size_t *l) {
    return WdfRequestRetrieveInputBuffer(r, min_l, b, l);
}
#include "integration/switch-lite/windows/SwitchFan/src/t214/gpu_pmic.h"
#undef KeDelayExecutionThread

static DRIVER_OBJECT g_drv;
static DEVICE_OBJECT g_owner_dev = { .DriverObject = &g_drv, .StackSize = 1 };
static DEVICE_OBJECT g_target_dev = { .StackSize = 1 };
static FILE_OBJECT   g_file_obj = { .DeviceObject = &g_target_dev };

static _Atomic LONG g_file_refs = 0, g_owner_refs = 0, g_live_pool = 0, g_live_irps = 0, g_live_wis = 0;
static _Atomic LONG g_handles = 0, g_ref_from_zero_count = 0, g_double_queue_wi_count = 0, g_state_uaf_count = 0;
static PDRIVER_NOTIFICATION_CALLBACK_ROUTINE g_pnp_cb = NULL;
static PVOID g_pnp_ctx = NULL;

static bool g_async_claim = false;
static bool g_async_release = false;
static PIRP g_pending_claim_irp = NULL;
static PIRP g_pending_release_irp = NULL;
static bool g_query_remove_only_in_reg = false;
static bool g_defer_compensate_wi = false;
static PIO_WORKITEM g_deferred_comp_wi = NULL;
static PIO_WORKITEM_ROUTINE g_deferred_comp_cb = NULL;
static PVOID g_deferred_comp_ctx = NULL;

static int g_hook_mode = 0;
static PVOID g_hook_plat = NULL;
static PVOID g_hook_state = NULL;
static TG_GPU_RESOURCES *g_hook_res = NULL;
static unsigned g_hook_fired = 0;

static pthread_mutex_t g_sync_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_sync_cv  = PTHREAD_COND_INITIALIZER;
static bool g_pause_in_ref_file = false;
static bool g_paused_in_ref_file = false;
static bool g_resume_ref_file = false;

static bool g_pause_after_reclaim_check = false;
static int  g_at_reclaim_check_count = 0;
static bool g_release_reclaim_check_t1 = false;
static bool g_release_reclaim_check_t2 = false;
static pthread_t g_t1_id;

static VOID after_spin_unlock(PKSPIN_LOCK l);
static VOID NTAPI platform_compensate_work(PDEVICE_OBJECT, PVOID);

static VOID KeInitializeSpinLock(PKSPIN_LOCK l) {
    pthread_mutex_t *m = calloc(1, sizeof(*m));
    pthread_mutex_init(m, NULL);
    l->ptr = m;
}
static VOID KeAcquireSpinLock(PKSPIN_LOCK l, KIRQL *irql) { *irql = 0; pthread_mutex_lock(l->ptr); }
static VOID KeReleaseSpinLock(PKSPIN_LOCK l, KIRQL irql) { (void)irql; pthread_mutex_unlock(l->ptr); after_spin_unlock(l); }

static PVOID ExAllocatePool2(POOL_FLAGS f, SIZE_T sz, ULONG tag) {
    (void)f;(void)tag; atomic_fetch_add(&g_live_pool, 1); return calloc(1, sz);
}
static VOID ExFreePoolWithTag(PVOID p, ULONG tag) {
    (void)tag;
    if (atomic_fetch_sub(&g_live_pool, 1) <= 0) atomic_fetch_add(&g_state_uaf_count, 1);
    free(p);
}
static PIRP IoAllocateIrp(CCHAR s, BOOLEAN q) {
    (void)s;(void)q; atomic_fetch_add(&g_live_irps, 1); return calloc(1, sizeof(IRP));
}
static VOID IoFreeIrp(PIRP irp) { atomic_fetch_sub(&g_live_irps, 1); free(irp); }

static LONG_PTR ObfReferenceObject(PVOID o) {
    if (o == &g_file_obj) {
        pthread_mutex_lock(&g_sync_mtx);
        if (g_pause_in_ref_file) {
            g_pause_in_ref_file = false;
            g_paused_in_ref_file = true;
            pthread_cond_broadcast(&g_sync_cv);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 25000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            while (!g_resume_ref_file) {
                if (pthread_cond_timedwait(&g_sync_cv, &g_sync_mtx, &ts) != 0)
                    break;
            }
        }
        pthread_mutex_unlock(&g_sync_mtx);
        LONG prev = atomic_fetch_add(&g_file_refs, 1);
        if (prev <= 0) atomic_fetch_add(&g_ref_from_zero_count, 1);
        return prev + 1;
    }
    if (o == &g_owner_dev) return atomic_fetch_add(&g_owner_refs, 1) + 1;
    abort();
}
static LONG_PTR ObfDereferenceObject(PVOID o) {
    if (o == &g_file_obj) return atomic_fetch_sub(&g_file_refs, 1) - 1;
    if (o == &g_owner_dev) return atomic_fetch_sub(&g_owner_refs, 1) - 1;
    abort();
}
static PDEVICE_OBJECT IoGetRelatedDeviceObject(PFILE_OBJECT f) { return f->DeviceObject; }
static NTSTATUS ZwCreateFile(HANDLE *h, ACCESS_MASK a, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
    PLARGE_INTEGER sz, ULONG at, ULONG sh, ULONG disp, ULONG opt, PVOID ea, ULONG eal) {
    (void)a;(void)oa;(void)sz;(void)at;(void)sh;(void)disp;(void)opt;(void)ea;(void)eal;
    atomic_fetch_add(&g_handles, 1); io->Status = STATUS_SUCCESS; *h = (HANDLE)(uintptr_t)0x123; return STATUS_SUCCESS;
}
static NTSTATUS ZwClose(HANDLE h) { assert(h == (HANDLE)(uintptr_t)0x123); atomic_fetch_sub(&g_handles, 1); return STATUS_SUCCESS; }
static NTSTATUS ObReferenceObjectByHandle(HANDLE h, ACCESS_MASK a, POBJECT_TYPE t, KPROCESSOR_MODE m, PVOID *o, POBJECT_HANDLE_INFORMATION i) {
    (void)h;(void)a;(void)t;(void)m;(void)i;
    atomic_fetch_add(&g_file_refs, 1); *o = &g_file_obj; return STATUS_SUCCESS;
}
static NTSTATUS IoRegisterPlugPlayNotification(IO_NOTIFICATION_EVENT_CATEGORY c, ULONG f, PVOID d,
    PDRIVER_OBJECT drv, PDRIVER_NOTIFICATION_CALLBACK_ROUTINE cb, PVOID ctx, PVOID *e) {
    (void)c;(void)f;(void)d;(void)drv;
    g_pnp_cb = cb; g_pnp_ctx = ctx; *e = (PVOID)(uintptr_t)0x777;
    if (g_query_remove_only_in_reg) {
        g_query_remove_only_in_reg = false;
        TARGET_DEVICE_REMOVAL_NOTIFICATION rn = { .Event = GUID_TARGET_DEVICE_QUERY_REMOVE, .FileObject = &g_file_obj };
        assert(cb(&rn, ctx) == STATUS_SUCCESS);
    }
    return STATUS_SUCCESS;
}
static NTSTATUS IoUnregisterPlugPlayNotificationEx(PVOID e) {
    assert(e == (PVOID)(uintptr_t)0x777);
    g_pnp_cb = NULL;
    return STATUS_SUCCESS;
}
static VOID KeInitializeEvent(PRKEVENT ev, EVENT_TYPE t, BOOLEAN st) {
    (void)t; pthread_mutex_init(&ev->mtx, NULL); pthread_cond_init(&ev->cv, NULL); ev->signaled = (st != 0);
}
static LONG KeSetEvent(PRKEVENT ev, KPRIORITY p, BOOLEAN w) {
    (void)p;(void)w;
    pthread_mutex_lock(&ev->mtx); ev->signaled = true; pthread_cond_broadcast(&ev->cv); pthread_mutex_unlock(&ev->mtx);
    return 0;
}
static PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT dev) {
    PIO_WORKITEM wi = calloc(1, sizeof(*wi));
    wi->Dev = dev;
    atomic_fetch_add(&g_live_wis, 1);
    atomic_fetch_add(&g_owner_refs, 1);
    return wi;
}
static VOID IoFreeWorkItem(PIO_WORKITEM wi) {
    if (wi->freed) { atomic_fetch_add(&g_double_queue_wi_count, 100); return; }
    wi->freed = true;
    wi->queued = false;
    atomic_fetch_sub(&g_live_wis, 1);
    atomic_fetch_sub(&g_owner_refs, 1);
    free(wi);
}
static VOID IoQueueWorkItem(PIO_WORKITEM wi, PIO_WORKITEM_ROUTINE r, WORK_QUEUE_TYPE qt, PVOID ctx) {
    (void)qt;
    if (wi->queued || wi->freed) {
        atomic_fetch_add(&g_double_queue_wi_count, 1);
        return;
    }
    if (g_defer_compensate_wi && r == platform_compensate_work) {
        wi->queued = true;
        g_deferred_comp_wi = wi;
        g_deferred_comp_cb = r;
        g_deferred_comp_ctx = ctx;
        return;
    }
    wi->queued = true;
    wi->queued = false;
    r(&g_owner_dev, ctx);
}
static NTSTATUS IoSetCompletionRoutineEx(PDEVICE_OBJECT dev, PIRP irp, PIO_COMPLETION_ROUTINE cb,
    PVOID ctx, BOOLEAN s, BOOLEAN e, BOOLEAN c) {
    (void)dev;(void)s;(void)e;(void)c;
    atomic_fetch_add(&g_owner_refs, 1);
    irp->CompCb = cb; irp->CompCtx = ctx;
    return STATUS_SUCCESS;
}
static BOOLEAN IoCancelIrp(PIRP irp) { irp->Cancel = TRUE; return FALSE; }
static NTSTATUS IoCallDriver(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    struct tg_gpu_pmic_request *op = irp->AssociatedIrp.SystemBuffer;
    if (g_async_claim && op->reg == TG_GPU_PMIC_CLAIM_RAIL) {
        g_async_claim = false;
        g_pending_claim_irp = irp;
        pthread_mutex_lock(&g_sync_mtx);
        pthread_cond_broadcast(&g_sync_cv);
        pthread_mutex_unlock(&g_sync_mtx);
        return STATUS_PENDING;
    }
    if (g_async_release && op->reg == TG_GPU_PMIC_RELEASE_RAIL) {
        g_async_release = false;
        g_pending_release_irp = irp;
        return STATUS_PENDING;
    }
    if (op->reg == TG_GPU_PMIC_CLAIM_RAIL) atomic_fetch_add(&g_provider_claims, 1);
    if (op->reg == TG_GPU_PMIC_RELEASE_RAIL) atomic_fetch_add(&g_provider_releases, 1);
    struct sw_req sr = { op, sizeof(*op) };
    ULONG_PTR info = 0;
    NTSTATUS st = SwFanGpuPmic(&g_swfan, &sr, &info);
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = NT_SUCCESS(st) ? info : 0;
    assert(irp->CompCb(&g_target_dev, irp, irp->CompCtx) == STATUS_MORE_PROCESSING_REQUIRED);
    atomic_fetch_sub(&g_owner_refs, 1);
    return st;
}
static NTSTATUS KeWaitForSingleObject(PVOID obj, KWAIT_REASON r, KPROCESSOR_MODE m, BOOLEAN a, PLARGE_INTEGER timeout) {
    (void)r;(void)m;(void)a;
    PRKEVENT ev = obj;
    pthread_mutex_lock(&ev->mtx);
    bool sig = ev->signaled;
    pthread_mutex_unlock(&ev->mtx);
    if (sig) return STATUS_SUCCESS;
    if ((g_pending_claim_irp || g_pending_release_irp) && timeout && timeout->QuadPart < 0) {
        atomic_fetch_add(&g_now_100ns, (ULONGLONG)(-timeout->QuadPart));
        return STATUS_TIMEOUT;
    }
    for (;;) {
        pthread_mutex_lock(&ev->mtx);
        sig = ev->signaled;
        pthread_mutex_unlock(&ev->mtx);
        if (sig) return STATUS_SUCCESS;
        sched_yield();
    }
}
static VOID KeStallExecutionProcessor(ULONG us) { (void)us; }
static NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE m, BOOLEAN a, PLARGE_INTEGER iv) {
    (void)m;(void)a;
    atomic_fetch_add(&g_now_100ns, (ULONGLONG)(-iv->QuadPart));
    return STATUS_SUCCESS;
}

#define TG_PLATFORM_PTHREAD_MODEL
#include "platform_unicode_fixture.h"
#include "windows/platform_io.c"

static VOID set_iso(TG_GPU_RESOURCES *r, BOOLEAN isolated) {
    ((PULONG)r->Mapping[TgGpuCarX])[3] = isolated ? (1u << 24) : 0u;
    ((PULONG)r->Mapping[TgGpuClamp])[0] = isolated ? 1u : 0u;
}

static VOID complete_pending_irp(PIRP *slot, BOOLEAN execute_provider) {
    PIRP irp = *slot;
    assert(irp != NULL);
    *slot = NULL;
    NTSTATUS st = STATUS_CANCELLED;
    ULONG_PTR info = 0;
    if (execute_provider) {
        struct tg_gpu_pmic_request *op = irp->AssociatedIrp.SystemBuffer;
        if (op->reg == TG_GPU_PMIC_CLAIM_RAIL) atomic_fetch_add(&g_provider_claims, 1);
        if (op->reg == TG_GPU_PMIC_RELEASE_RAIL) atomic_fetch_add(&g_provider_releases, 1);
        struct sw_req sr = { op, sizeof(*op) };
        st = SwFanGpuPmic(&g_swfan, &sr, &info);
        assert(st == STATUS_SUCCESS);
    }
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    assert(irp->CompCb(&g_target_dev, irp, irp->CompCtx) == STATUS_MORE_PROCESSING_REQUIRED);
    atomic_fetch_sub(&g_owner_refs, 1);
}

static VOID after_spin_unlock(PKSPIN_LOCK l) {
    TG_GPU_PLATFORM *p = g_hook_plat;
    TG_PLATFORM_RAIL_STATE *s = g_hook_state;
    if (g_pause_after_reclaim_check && s && l == &s->Lock &&
        s->LastRailOpState != TgRailOpInFlight && s->InFlightClaimSeq == 0) {
        pthread_mutex_lock(&g_sync_mtx);
        if (g_pause_after_reclaim_check) {
            g_at_reclaim_check_count++;
            pthread_cond_broadcast(&g_sync_cv);
            if (pthread_equal(pthread_self(), g_t1_id)) {
                while (!g_release_reclaim_check_t1)
                    pthread_cond_wait(&g_sync_cv, &g_sync_mtx);
            } else {
                while (!g_release_reclaim_check_t2)
                    pthread_cond_wait(&g_sync_cv, &g_sync_mtx);
            }
        }
        pthread_mutex_unlock(&g_sync_mtx);
        return;
    }
    if (!g_hook_mode || !p || !s || l != &s->Lock) return;
    if (g_hook_mode == 1 && s->DeferReleasePending) {
        g_hook_mode = 0;
        g_hook_fired++;
        complete_pending_irp(&g_pending_claim_irp, TRUE);
    } else if (g_hook_mode == 2 && s->DeferReleasePending) {
        g_hook_mode = 0;
        g_hook_fired++;
        set_iso(g_hook_res, FALSE);
        TgGpuPlatformRefreshIsolation(p, g_hook_res);
    }
}

struct worker_req_arg { TG_GPU_PLATFORM *p; struct tg_gpu_pmic_request op; NTSTATUS st; };
static void *req_thread_fn(void *arg) {
    struct worker_req_arg *a = arg;
    a->st = TgGpuPlatformTryRequest(a->p, &a->op);
    return NULL;
}

int main(void) {
    static ULONG regs[TgGpuApertureCount][4], lic[T214_GPU_LIC_SIZE / 4];
    static const ULONG bases[] = {
        T214_GPU_REG_BASE, T214_GPU_BAR1_BASE, T214_GPU_AUX_BASE,
#define B(b, s) b,
        T214_GPU_EXTRA_RESOURCES(B)
#undef B
    };
    TG_GPU_RESOURCES res = {0};
    for (UINT i = 0; i < TgGpuApertureCount; ++i) {
        res.Memory[i].Type = CmResourceTypeMemory;
        res.Memory[i].u.Memory.Start.QuadPart = bases[i];
        res.Memory[i].u.Memory.Length = sizeof(regs[i]);
        res.Mapping[i] = regs[i];
    }
    res.LicMapping = lic;

    pthread_mutex_init(&g_swfan.pmic_mtx, NULL);
    pthread_mutex_init(&g_swfan.cpu_mtx, NULL);
    g_swfan.IsLite = TRUE;
    g_swfan.CpuConfigured = TRUE;
    g_swfan.Pmic = (void *)1;
    g_swfan.Config.PmicAddress = 0x33;
    g_swfan.TempValid = TRUE;
    g_swfan.LastTemp = 42;
    g_pmic_regs[0x06] = 0x54;
    g_pmic_regs[0x23] = 0x46;
    g_drv.DeviceObject = &g_owner_dev;
    TgGpuSetPlatformDriverObject(&g_drv);

    /* 1. GC-V9F-01a: 2-thread ObfReferenceObject(FileObject) vs PnP QUERY_REMOVE */
    {
        TG_GPU_PLATFORM plat = {0};
        set_iso(&res, TRUE);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuOpenPlatform(&plat, &res) == STATUS_SUCCESS);
        pthread_mutex_lock(&g_sync_mtx);
        g_pause_in_ref_file = true;
        g_paused_in_ref_file = false;
        g_resume_ref_file = false;
        pthread_mutex_unlock(&g_sync_mtx);

        struct worker_req_arg wa = { .p = &plat, .op = {0, TG_GPU_PMIC_QUERY_ADDRESS, 0, 0} };
        pthread_t tid;
        assert(pthread_create(&tid, NULL, req_thread_fn, &wa) == 0);
        pthread_mutex_lock(&g_sync_mtx);
        while (!g_paused_in_ref_file)
            pthread_cond_wait(&g_sync_cv, &g_sync_mtx);
        pthread_mutex_unlock(&g_sync_mtx);

        TARGET_DEVICE_REMOVAL_NOTIFICATION n = { .Event = GUID_TARGET_DEVICE_QUERY_REMOVE, .FileObject = &g_file_obj };
        assert(g_pnp_cb(&n, g_pnp_ctx) == STATUS_SUCCESS);
        pthread_mutex_lock(&g_sync_mtx);
        g_resume_ref_file = true;
        pthread_cond_broadcast(&g_sync_cv);
        pthread_mutex_unlock(&g_sync_mtx);
        pthread_join(tid, NULL);

        assert(atomic_load(&g_ref_from_zero_count) == 0);
        assert(TgGpuClosePlatform(&plat) == STATUS_SUCCESS);
    }

    /* 2. GC-V9F-01b: PnP QUERY_REMOVE during reconnect registration rollback */
    {
        TG_GPU_PLATFORM plat = {0};
        UCHAR addr = 0;
        set_iso(&res, TRUE);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuOpenPlatform(&plat, &res) == STATUS_SUCCESS);
        TARGET_DEVICE_REMOVAL_NOTIFICATION n = { .Event = GUID_TARGET_DEVICE_QUERY_REMOVE, .FileObject = &g_file_obj };
        assert(g_pnp_cb(&n, g_pnp_ctx) == STATUS_SUCCESS);
        n.Event = GUID_TARGET_DEVICE_REMOVE_COMPLETE;
        assert(g_pnp_cb(&n, g_pnp_ctx) == STATUS_SUCCESS);

        g_query_remove_only_in_reg = true;
        (void)TgGpuPlatformPreflight(&plat, &addr);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuPlatformPreflight(&plat, &addr) == STATUS_SUCCESS);
        assert(TgGpuPlatformAbandonRail(&plat, addr) == STATUS_SUCCESS);
        assert(TgGpuClosePlatform(&plat) == STATUS_SUCCESS);
    }

    /* 3. GC-V9F-02: Zero double-queue of CompensateWorkItem across paired completions */
    {
        TG_GPU_PLATFORM plat = {0};
        UCHAR addr = 0;
        set_iso(&res, TRUE);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuOpenPlatform(&plat, &res) == STATUS_SUCCESS);
        assert(TgGpuPlatformPreflight(&plat, &addr) == STATUS_SUCCESS);
        g_swfan.TempSampleTime = KeQueryInterruptTime();

        g_async_release = true;
        struct tg_gpu_pmic_request rel = { 0x33, TG_GPU_PMIC_RELEASE_RAIL, 1, 0x5446u };
        assert(TgGpuPlatformTryRequest(&plat, &rel) == STATUS_IO_TIMEOUT && g_pending_release_irp != NULL);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        g_async_claim = true;
        assert(TgGpuPlatformPreflight(&plat, &addr) == STATUS_IO_TIMEOUT && g_pending_claim_irp != NULL);

        ULONG rel0 = atomic_load(&g_provider_releases);
        assert(TgGpuPlatformAbandonRail(&plat, 0) == STATUS_IO_TIMEOUT);

        g_defer_compensate_wi = true;
        complete_pending_irp(&g_pending_claim_irp, TRUE);
        assert(g_deferred_comp_cb != NULL);
        complete_pending_irp(&g_pending_release_irp, FALSE);

        g_deferred_comp_wi->queued = false;
        g_deferred_comp_cb(&g_owner_dev, g_deferred_comp_ctx);
        g_defer_compensate_wi = false;

        assert(atomic_load(&g_double_queue_wi_count) == 0);
        assert(atomic_load(&g_provider_releases) == rel0 + 1);
        assert(!g_swfan.EmcGpuVote.claimed);
        assert(TgGpuClosePlatform(&plat) == STATUS_SUCCESS);
    }

    /* 4. GC-V9F-03a: Pending CLAIM completion immediately after DeferReleasePending arm */
    {
        TG_GPU_PLATFORM plat = {0};
        UCHAR addr = 0;
        set_iso(&res, TRUE);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuOpenPlatform(&plat, &res) == STATUS_SUCCESS);
        g_async_claim = true;
        assert(TgGpuPlatformPreflight(&plat, &addr) == STATUS_IO_TIMEOUT && g_pending_claim_irp != NULL);
        ULONG rel0 = atomic_load(&g_provider_releases);
        g_hook_plat = &plat;
        g_hook_state = plat.RailState;
        g_hook_res = &res;
        g_hook_mode = 1;
        (void)TgGpuPlatformAbandonRail(&plat, 0);
        assert(g_hook_fired == 1);
        assert(atomic_load(&g_provider_releases) == rel0 + 1);
        assert(!g_swfan.EmcGpuVote.claimed && !TgGpuQueryUnresolvedRail(NULL, NULL, NULL, NULL));
        assert(TgGpuClosePlatform(&plat) == STATUS_SUCCESS);
    }

    /* 5. GC-V9F-03b: Isolation revocation between DeferReleasePending arm and ledger record */
    {
        TG_GPU_PLATFORM plat = {0};
        UCHAR addr = 0;
        set_iso(&res, TRUE);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuOpenPlatform(&plat, &res) == STATUS_SUCCESS);
        g_async_claim = true;
        assert(TgGpuPlatformPreflight(&plat, &addr) == STATUS_IO_TIMEOUT && g_pending_claim_irp != NULL);
        ULONG rel0 = atomic_load(&g_provider_releases);
        g_hook_plat = &plat;
        g_hook_state = plat.RailState;
        g_hook_res = &res;
        g_hook_mode = 2;
        g_hook_fired = 0;
        (void)TgGpuPlatformAbandonRail(&plat, 0);
        complete_pending_irp(&g_pending_claim_irp, TRUE);
        BOOLEAN iso = TRUE;
        assert(g_hook_fired == 1);
        assert(atomic_load(&g_provider_releases) == rel0);
        assert(g_swfan.EmcGpuVote.claimed && TgGpuQueryUnresolvedRail(&iso, NULL, NULL, NULL) && !iso);
        set_iso(&res, TRUE);
        assert(TgGpuPlatformAbandonRail(&plat, 0) == STATUS_SUCCESS);
        assert(!g_swfan.EmcGpuVote.claimed && !TgGpuQueryUnresolvedRail(NULL, NULL, NULL, NULL));
        assert(TgGpuClosePlatform(&plat) == STATUS_SUCCESS);
    }

    /* 6. GC-V9F-04: Closed automatic reclaim completing after RemoveDevice + 0xCC poison */
    {
        TG_GPU_PLATFORM plat = {0};
        UCHAR addr = 0;
        set_iso(&res, TRUE);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuOpenPlatform(&plat, &res) == STATUS_SUCCESS);
        assert(TgGpuPlatformPreflight(&plat, &addr) == STATUS_SUCCESS && g_swfan.EmcGpuVote.claimed);
        g_swfan.TempSampleTime = KeQueryInterruptTime();

        TARGET_DEVICE_REMOVAL_NOTIFICATION n = { .Event = GUID_TARGET_DEVICE_QUERY_REMOVE, .FileObject = &g_file_obj };
        assert(g_pnp_cb(&n, g_pnp_ctx) == STATUS_SUCCESS);
        n.Event = GUID_TARGET_DEVICE_REMOVE_COMPLETE;
        assert(g_pnp_cb(&n, g_pnp_ctx) == STATUS_SUCCESS && plat.NeedReclaim);
        g_swfan.EmcGpuVote.claimed = false;
        g_swfan.EmcGpuVote.claim_token = 0;

        g_async_claim = true;
        struct tg_gpu_pmic_request rep = { 0, TG_GPU_PMIC_REPORT_ABI, 1, TG_GPU_LIVE_COUNTER_ABI };
        assert(TgGpuPlatformTryRequest(&plat, &rep) == STATUS_IO_TIMEOUT && g_pending_claim_irp != NULL);

        ULONG rel0 = atomic_load(&g_provider_releases);
        (void)TgGpuPlatformAbandonRail(&plat, 0);
        assert(TgGpuClosePlatform(&plat) == STATUS_SUCCESS);
        TgGpuPlatformRefreshIsolation(&plat, &res);
        TgGpuHandOffUnresolvedRail(&plat, plat.PreservedGpuIsolated);
        (void)TgGpuRetryUnresolvedRailRelease();
        memset(&plat, 0xcc, sizeof(plat));

        complete_pending_irp(&g_pending_claim_irp, TRUE);
        for (size_t b = 0; b < sizeof(plat); ++b) assert(((UCHAR *)&plat)[b] == 0xcc);
        assert(atomic_load(&g_provider_releases) == rel0 + 1);
        assert(!g_swfan.EmcGpuVote.claimed && !TgGpuQueryUnresolvedRail(NULL, NULL, NULL, NULL));
    }

    /* 7. GC-V9F-05: 2-thread CLAIM_RAIL vs RELEASE_RAIL split-lock overtake */
    {
        TG_GPU_PLATFORM plat = {0};
        UCHAR addr = 0;
        set_iso(&res, TRUE);
        g_swfan.TempSampleTime = KeQueryInterruptTime();
        assert(TgGpuOpenPlatform(&plat, &res) == STATUS_SUCCESS);
        assert(TgGpuPlatformPreflight(&plat, &addr) == STATUS_SUCCESS && g_swfan.EmcGpuVote.claimed);
        g_swfan.TempSampleTime = KeQueryInterruptTime();

        g_hook_plat = &plat;
        g_hook_state = plat.RailState;
        g_at_reclaim_check_count = 0;
        g_release_reclaim_check_t1 = false;
        g_release_reclaim_check_t2 = false;
        g_pause_after_reclaim_check = true;

        struct worker_req_arg c_arg = { .p = &plat, .op = {0, TG_GPU_PMIC_CLAIM_RAIL, 1, TG_GPU_EMC_VOTE_ABI} };
        struct worker_req_arg r_arg = { .p = &plat, .op = {0x33, TG_GPU_PMIC_RELEASE_RAIL, 1, 0x5446u} };
        pthread_t t2;
        assert(pthread_create(&g_t1_id, NULL, req_thread_fn, &c_arg) == 0);
        pthread_mutex_lock(&g_sync_mtx);
        while (g_at_reclaim_check_count < 1)
            pthread_cond_wait(&g_sync_cv, &g_sync_mtx);
        pthread_mutex_unlock(&g_sync_mtx);

        assert(pthread_create(&t2, NULL, req_thread_fn, &r_arg) == 0);
        pthread_mutex_lock(&g_sync_mtx);
        while (g_at_reclaim_check_count < 2)
            pthread_cond_wait(&g_sync_cv, &g_sync_mtx);
        g_async_claim = true;
        g_pause_after_reclaim_check = false;
        g_release_reclaim_check_t1 = true;
        pthread_cond_broadcast(&g_sync_cv);
        while (!g_pending_claim_irp)
            pthread_cond_wait(&g_sync_cv, &g_sync_mtx);
        g_release_reclaim_check_t2 = true;
        pthread_cond_broadcast(&g_sync_cv);
        pthread_mutex_unlock(&g_sync_mtx);

        pthread_join(t2, NULL);
        pthread_join(g_t1_id, NULL);

        assert(r_arg.st == STATUS_DEVICE_BUSY);
        complete_pending_irp(&g_pending_claim_irp, TRUE);
        BOOLEAN ledger = TgGpuQueryUnresolvedRail(NULL, NULL, NULL, NULL);
        assert(g_swfan.EmcGpuVote.claimed == (plat.RailClaimed || plat.RailOwnershipUncertain || ledger));
        assert(TgGpuPlatformAbandonRail(&plat, addr) == STATUS_SUCCESS);
        assert(!g_swfan.EmcGpuVote.claimed && !TgGpuQueryUnresolvedRail(NULL, NULL, NULL, NULL));
        assert(TgGpuClosePlatform(&plat) == STATUS_SUCCESS);
    }

    assert(atomic_load(&g_live_pool) == 0 && atomic_load(&g_live_irps) == 0 &&
           atomic_load(&g_live_wis) == 0 && atomic_load(&g_file_refs) == 0 &&
           atomic_load(&g_owner_refs) == 0 && atomic_load(&g_handles) == 0 &&
           atomic_load(&g_state_uaf_count) == 0);
    puts("PASS: platform_io multi-threaded PnP/close/claim/release/compensation regressions");
    return 0;
}
