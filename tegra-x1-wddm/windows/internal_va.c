/* SPDX-License-Identifier: MIT */
#include "internal_va.h"

NTSTATUS TgGpuCreateInternalVa(const DXGKRNL_INTERFACE *dxgk, HANDLE process,
    const TG_GPU_PRIVATE_MEMORY *commands, TG_GPU_INTERNAL_VA *v)
{
    DXGKARGCB_RESERVEGPUVIRTUALADDRESSRANGE reserve={0};
    UINT64 entries[TG_GPU_COMMAND_BYTES/PAGE_SIZE]={0}, physical; NTSTATUS status; UINT i;
    if (!dxgk || !commands || !commands->Handle || !commands->CpuAddress ||
        commands->Bytes!=TG_GPU_COMMAND_BYTES || !v || v->Leaf.Handle ||
        !dxgk->DxgkCbReserveGpuVirtualAddressRange) return STATUS_INVALID_PARAMETER;
    physical=(UINT64)commands->SystemPhysicalAddress.QuadPart;
    if (physical<0x80000000 || physical>UINT64_C(0x100000000)-TG_GPU_COMMAND_BYTES)
        return STATUS_INVALID_ADDRESS;
    for (i=0;i<TG_GPU_COMMAND_BYTES/PAGE_SIZE;++i) {
        if(i==TG_GPU_COMMAND_RUNLIST/PAGE_SIZE)continue;
        const BOOLEAN qmd=i>=TG_GPU_COMMAND_QMD/PAGE_SIZE;
        if (!tg_gpu_pte_4k(physical+(UINT64)i*PAGE_SIZE,i!=2 && !qmd,true,&entries[i]))
            return STATUS_INVALID_ADDRESS;
        /* GM20B's PB fetch reads the KERNEL entry's instructions as USER:
         * the physical .51 fault was READ/PRIV at Base+0x1000. Keep that
         * packet read-only but fetchable; ring and writable fence stay PRIV.
         * GP_ENTRY1.PRIV describes the entry's execution, not its PB mapping.
         */
        if(i==0 || i==2)entries[i]|=2;
    }
    reserve.hDxgkProcess=process;
    reserve.SizeInBytes=TG_GPU_INTERNAL_COVERAGE;
    reserve.Alignment=(UINT)TG_GPU_INTERNAL_COVERAGE;
    /* BaseAddress=0 lets VidMm choose; AllowUserModeMapping remains false. */
    status=dxgk->DxgkCbReserveGpuVirtualAddressRange(dxgk->DeviceHandle,&reserve);
    if (!NT_SUCCESS(status)) return status;
    if (!reserve.StartVirtualAddress || (reserve.StartVirtualAddress&(TG_GPU_INTERNAL_COVERAGE-1)) ||
        reserve.StartVirtualAddress>TG_GPU_VA_LIMIT-TG_GPU_INTERNAL_COVERAGE)
        return STATUS_INVALID_ADDRESS;
    status=TgGpuAllocatePrivateMemory(dxgk,TG_GPU_TABLE_BYTES,0x80000000,0xffffffff,&v->Leaf);
    if (!NT_SUCCESS(status)) return status; /* failed process creation drops its OS reservation */
    v->Base=reserve.StartVirtualAddress;
    for (i=0;i<TG_GPU_COMMAND_BYTES/PAGE_SIZE;++i) ((volatile UINT64 *)v->Leaf.CpuAddress)[i]=entries[i];
    /* Remaining entries and the physical runlist page stay unmapped. RAMIN
     * is a separate allocation and never appears in this GPU VA mapping.
     */
    TgGpuDataSyncBarrier(); return STATUS_SUCCESS;
}
NTSTATUS TgGpuInstallInternalRoot(const TG_GPU_TABLE_SEGMENT *tables,
    const TG_GPU_ROOT_TABLE *root, const TG_GPU_INTERNAL_VA *v)
{
    UINT64 pde,physical,index;PVOID cpu;NTSTATUS status;
    if (!root || !root->Valid || !v || !v->Leaf.Handle ||
        v->Leaf.Bytes!=TG_GPU_TABLE_BYTES || !v->Base ||
        (v->Base&(TG_GPU_INTERNAL_COVERAGE-1)) || v->Base>=root->VaLimit ||
        TG_GPU_INTERNAL_COVERAGE>root->VaLimit-v->Base ||
        !tg_gpu_pde_small((UINT64)v->Leaf.SystemPhysicalAddress.QuadPart,&pde))
        return STATUS_INVALID_ADDRESS;
    index=v->Base>>TG_GPU_DIRECTORY_SHIFT;
    if (index>=root->Entries) return STATUS_INVALID_ADDRESS;
    status=TgGpuMapTableSegment(tables,root->Address.SegmentOffset,root->Bytes,&cpu,&physical);
    if (!NT_SUCCESS(status)) return status;
    if (physical!=root->DeviceAddress) return STATUS_INVALID_ADDRESS;
    ((volatile UINT64 *)cpu)[index]=pde;
    TgGpuDataSyncBarrier();return STATUS_SUCCESS;
}
