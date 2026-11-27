/* SPDX-License-Identifier: MIT */
/* Fail-fast native fixture imports: the resource-only adapter must never
 * reach live file I/O, scheduling or delays. The other fixtures cover those
 * paths with their own explicit substitutes. */
#include "../windows/gr_firmware_io.h"
NTSTATUS TgGpuFirmwareLoad(TG_GPU_FIRMWARE *f)
{
    CHECK(restart_fixture.active && f && !f->netlist);
    if(!NT_SUCCESS(restart_fixture.fw_status))return restart_fixture.fw_status;
    f->netlist=(PVOID)1;
    ++restart_fixture.fw_loads;
    return STATUS_SUCCESS;
}
VOID TgGpuFirmwareFree(TG_GPU_FIRMWARE *f)
{
    CHECK(restart_fixture.active && f);
    if(f->netlist){CHECK(restart_fixture.fw_frees<restart_fixture.fw_loads);++restart_fixture.fw_frees;}
    memset(f,0,sizeof(*f));
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
static NTSTATUS UnexpectedThread(PHANDLE h,ULONG access,POBJECT_ATTRIBUTES a,HANDLE p,
    PCLIENT_ID id,PKSTART_ROUTINE routine,PVOID context)
{
    CHECK(restart_fixture.active && h && access==SYNCHRONIZE && a &&
        a->Attributes==OBJ_KERNEL_HANDLE && !p && !id && routine && context);
    if(!NT_SUCCESS(restart_fixture.thread_status))return restart_fixture.thread_status;
    restart_fixture.active_worker=context;
    *h=(HANDLE)0x777;
    ++restart_fixture.threads_started;
    return STATUS_SUCCESS;
}
static NTSTATUS UnexpectedTerminate(NTSTATUS status){CHECK(FALSE);return STATUS_UNSUCCESSFUL;}
static PEX_TIMER UnexpectedTimerAlloc(PEXT_CALLBACK callback,PVOID context,ULONG flags)
{CHECK(FALSE);return NULL;}
static BOOLEAN UnexpectedTimerSet(PEX_TIMER timer,LONGLONG due,LONGLONG period,PEXT_SET_PARAMETERS params)
{CHECK(FALSE);return FALSE;}
static BOOLEAN UnexpectedTimerCancel(PEX_TIMER timer,PEXT_CANCEL_PARAMETERS params)
{CHECK(FALSE);return FALSE;}
static BOOLEAN UnexpectedTimerDelete(PEX_TIMER timer,BOOLEAN cancel,BOOLEAN wait,PEXT_DELETE_PARAMETERS params)
{CHECK(FALSE);return FALSE;}
static NTSTATUS NTAPI UnexpectedWait(ULONG count,PVOID objects[],WAIT_TYPE type,KWAIT_REASON reason,
    KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout,PKWAIT_BLOCK blocks)
{CHECK(FALSE);return STATUS_UNSUCCESSFUL;}
static NTSTATUS NTAPI UnexpectedOpen(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES a,PIO_STATUS_BLOCK iosb,
    PLARGE_INTEGER size,ULONG attr,ULONG share,ULONG disposition,ULONG options,PVOID ea,ULONG ea_size)
{
    CHECK(restart_fixture.active && level==PASSIVE_LEVEL && h && iosb);
    ++restart_fixture.opens;
    iosb->Status=STATUS_SUCCESS;
    *h=(HANDLE)0x551;
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI UnexpectedIo(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID c,PIO_STATUS_BLOCK iosb,
    ULONG code,PVOID in,ULONG in_bytes,PVOID out,ULONG out_bytes)
{CHECK(FALSE);return STATUS_UNSUCCESSFUL;}
static VOID NTAPI UnexpectedStall(ULONG us){CHECK(restart_fixture.active && us>0);}
static NTSTATUS NTAPI UnexpectedDelay(KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER interval)
{CHECK(restart_fixture.active && mode==KernelMode && !alertable && interval && interval->QuadPart<0);return STATUS_SUCCESS;}
static NTSTATUS NTAPI UnexpectedOpenKey(PHANDLE k,ACCESS_MASK a,POBJECT_ATTRIBUTES o)
{CHECK(restart_fixture.active && k && o);return STATUS_OBJECT_NAME_NOT_FOUND;}
static NTSTATUS NTAPI UnexpectedQueryValueKey(HANDLE k,PUNICODE_STRING n,KEY_VALUE_INFORMATION_CLASS c,
    PVOID i,ULONG l,PULONG r)
{CHECK(FALSE);return STATUS_UNSUCCESSFUL;}
static LARGE_INTEGER NTAPI UnexpectedQpc(PLARGE_INTEGER f)
{
    LARGE_INTEGER z={0};
    CHECK(restart_fixture.active);
    if(f)f->QuadPart=1000000LL;
    z.QuadPart=1000000LL;
    return z;
}
#pragma clang diagnostic pop
NTSTATUS (* const __imp_PsCreateSystemThread)(PHANDLE,ULONG,POBJECT_ATTRIBUTES,HANDLE,PCLIENT_ID,PKSTART_ROUTINE,PVOID)=UnexpectedThread;
NTSTATUS (* const __imp_PsTerminateSystemThread)(NTSTATUS)=UnexpectedTerminate;
__typeof__(&ExAllocateTimer) const __imp_ExAllocateTimer=UnexpectedTimerAlloc;
__typeof__(&ExSetTimer) const __imp_ExSetTimer=UnexpectedTimerSet;
__typeof__(&ExCancelTimer) const __imp_ExCancelTimer=UnexpectedTimerCancel;
__typeof__(&ExDeleteTimer) const __imp_ExDeleteTimer=UnexpectedTimerDelete;
NTSTATUS (NTAPI * const __imp_KeWaitForMultipleObjects)(ULONG,PVOID[],WAIT_TYPE,KWAIT_REASON,
    KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER,PKWAIT_BLOCK)=UnexpectedWait;
NTSTATUS (NTAPI * const __imp_ZwCreateFile)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,
    PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG)=UnexpectedOpen;
NTSTATUS (NTAPI * const __imp_ZwDeviceIoControlFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,
    ULONG,PVOID,ULONG,PVOID,ULONG)=UnexpectedIo;
VOID (NTAPI * const __imp_KeStallExecutionProcessor)(ULONG)=UnexpectedStall;
NTSTATUS (NTAPI * const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=UnexpectedDelay;
NTSTATUS (NTAPI * const __imp_ZwOpenKey)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES)=UnexpectedOpenKey;
NTSTATUS (NTAPI * const __imp_ZwQueryValueKey)(HANDLE,PUNICODE_STRING,KEY_VALUE_INFORMATION_CLASS,
    PVOID,ULONG,PULONG)=UnexpectedQueryValueKey;
LARGE_INTEGER (NTAPI * const __imp_KeQueryPerformanceCounter)(PLARGE_INTEGER)=UnexpectedQpc;
