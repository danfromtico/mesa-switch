/* SPDX-License-Identifier: MIT */
#include "diagnostic_events.h"

static const GUID provider_id={0xc27c1f9b,0x62e8,0x4e21,{0xb2,0x9b,0x6b,0xb2,0xd5,0x85,0xc6,0xd0}};
static REGHANDLE provider;

NTSTATUS TgGpuTraceInitialize(VOID)
{
    PAGED_CODE();
    return EtwRegister(&provider_id,NULL,NULL,&provider);
}
VOID TgGpuTraceShutdown(VOID)
{
    PAGED_CODE();
    if(provider){(VOID)EtwUnregister(provider);provider=0;}
}
static VOID write_event_ver(USHORT id,UCHAR version,UCHAR level,const VOID *record,ULONG bytes)
{
    const EVENT_DESCRIPTOR event={.Id=id,.Version=version,.Level=level,.Keyword=1};
    EVENT_DATA_DESCRIPTOR data={0};
    if(!provider)return;
    data.Ptr=(ULONGLONG)(ULONG_PTR)record;
    data.Size=bytes;
    /* This reports the first internal failure before recovery notification.
     * ETW loss/flush limits still apply; no disk I/O or GPU work is added. */
    (VOID)EtwWrite(provider,&event,NULL,1,&data);
}
static VOID write_event(USHORT id,UCHAR level,const VOID *record,ULONG bytes)
{write_event_ver(id,1,level,record,bytes);}
VOID TgGpuTraceRecovery(const TG_GPU_RECOVERY_EVENT *record)
{write_event(1,2,record,sizeof(*record));}
VOID TgGpuTraceFaultIsr(const TG_GPU_FAULT_ISR_EVENT *record)
{write_event_ver(10,2,2,record,sizeof(*record));}
VOID TgGpuTracePagingRejection(const TG_GPU_PAGING_REJECTION_EVENT *record)
{write_event(11,2,record,sizeof(*record));}
VOID TgGpuTracePagingDirectory(const TG_GPU_PAGING_REJECTION_EVENT *record)
{write_event(12,2,record,sizeof(*record));}
VOID TgGpuTraceExecution(const TG_GPU_EXECUTION_EVENT *record)
{write_event(2,4,record,sizeof(*record));}
static const EVENT_DESCRIPTOR tdr_event={.Id=8,.Version=1,.Level=4,.Keyword=1};
BOOLEAN TgGpuTraceTdrEnabled(VOID)
{return provider && EtwEventEnabled(provider,&tdr_event);}
VOID TgGpuTraceTdr(const TG_GPU_TDR_EVENT *record)
{write_event(8,4,record,sizeof(*record));}
static const EVENT_DESCRIPTOR context_event={.Id=9,.Version=1,.Level=4,.Keyword=1};
BOOLEAN TgGpuTraceContextEnabled(VOID)
{return provider && EtwEventEnabled(provider,&context_event);}
VOID TgGpuTraceContext(const TG_GPU_CONTEXT_EVENT *record)
{write_event(9,4,record,sizeof(*record));}
VOID TgGpuTraceDisplay(const TG_GPU_DISPLAY_EVENT *record)
{write_event(4,4,record,sizeof(*record));}
VOID TgGpuTraceStartupDiagnostic(const TG_GPU_STARTUP_DIAGNOSTIC_EVENT *record)
{write_event(6,4,record,sizeof(*record));}
static const EVENT_DESCRIPTOR reset_packet_event={.Id=3,.Version=1,.Level=4,.Keyword=1};
BOOLEAN TgGpuTraceResetPacketEnabled(VOID)
{return provider && EtwEventEnabled(provider,&reset_packet_event);}
VOID TgGpuTraceResetPacket(const TG_GPU_RESET_PACKET_EVENT *record,const VOID *commands)
{
    EVENT_DATA_DESCRIPTOR data[2]={0};
    if(!provider || record->ChunkBytes>TG_GPU_TRACE_COMMAND_CHUNK_BYTES ||
       (record->ChunkBytes && !commands))return;
    data[0].Ptr=(ULONGLONG)(ULONG_PTR)record;data[0].Size=sizeof(*record);
    data[1].Ptr=(ULONGLONG)(ULONG_PTR)commands;data[1].Size=record->ChunkBytes;
    (VOID)EtwWrite(provider,&reset_packet_event,NULL,record->ChunkBytes?2:1,data);
}
