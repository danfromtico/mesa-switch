#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the real resident report/cache helpers with interrupted publication.

Only WDK scalar types and interlocked primitives are replaced for the host.
The registered DDI and actual trace publishers also have ARM64 WDK controls.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
wdk = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef uint8_t UCHAR,BOOLEAN; typedef uint16_t USHORT;
typedef uint32_t ULONG; typedef int32_t LONG,NTSTATUS;
typedef uint64_t ULONGLONG; typedef size_t SIZE_T; typedef void VOID;
typedef struct {ULONG LowPart;LONG HighPart;} LUID;
#define C_ASSERT(x) _Static_assert(x,#x)
#define FIELD_OFFSET(t,m) offsetof(t,m)
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset(p,0,n)
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
#define _IRQL_requires_(x)
#define _IRQL_requires_max_(x)
static void (*interrupt_writer)(volatile LONG *);
static unsigned atomic_reads;
static LONG InterlockedCompareExchange(volatile LONG *p,LONG desired,LONG compare){
 atomic_reads++;assert(atomic_reads<512);
 LONG old=__atomic_load_n(p,__ATOMIC_SEQ_CST);
 if(old==compare)__atomic_compare_exchange_n(p,&compare,desired,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
 if(interrupt_writer)interrupt_writer(p);
 return old;
}
static LONG InterlockedExchange(volatile LONG *p,LONG value){return __atomic_exchange_n(p,value,__ATOMIC_SEQ_CST);}
'''
test = r'''
#include <stdio.h>
#include "debug_report.h"
static TG_GPU_DEBUG_CACHE cache;
static TG_GPU_RECOVERY_EVENT next;
static void publish_during_read(volatile LONG *address){
 if(address==&cache.RecoveryWords[3]){
  interrupt_writer=NULL;
  TgGpuCacheRecovery(&cache,&next);
 }
}
static void reset_publish_during_read(volatile LONG *address){
 if(address==&cache.RecoveryWords[3]){
  interrupt_writer=NULL;
  TgGpuResetDebugCache(&cache);
  TgGpuCacheRecovery(&cache,&next);
 }
}
int main(int argc,char **argv){
 assert(argc==3);
 TG_GPU_DEBUG_REPORT out;
 TG_GPU_RECOVERY_EVENT first={.Version=1,.Source=5,.AdapterLuid={0x1234,2},
  .Status=(NTSTATUS)0xc00000b5,.SubmissionFence=77,.LastCompletedFence=76};
 TG_GPU_EXECUTION_EVENT execution={.Version=1,.Source=1,.Stage=7,.SubmissionFence=77,.Expected=8,.Observed=7};
 assert(TgGpuCopyDebugReport(&cache,76,0x141,&out,sizeof(out))==0&&!out.Flags);
 TgGpuCacheRecovery(&cache,&first);TgGpuCacheExecution(&cache,&execution);
 assert(TgGpuCopyDebugReport(&cache,76,0x141,&out,sizeof(out))==0&&out.Flags==3);
 assert(!memcmp(&out.Recovery,&first,sizeof(first))&&!memcmp(&out.Execution,&execution,sizeof(execution)));
 FILE *f=fopen(argv[1],"wb");assert(f&&fwrite(&out,1,sizeof(out),f)==sizeof(out));fclose(f);
 TG_GPU_TDR_EVENT contract={.Version=1,.Operation=TgGpuTdrQueryEngine,.Phase=1,
  .Flags=TgGpuTdrSubmittedValid|TgGpuTdrResultValid|TgGpuTdrEngineSnapshot|
         TgGpuTdrQuerySampleValid|TgGpuTdrHealthy|TgGpuTdrQueuePending|TgGpuTdrIdentityValid,
  .Engine=0x12345678,.AdapterLuid={0x1234,2},.LastSubmittedFence=79,
  .LastCompletedFence=78,.Result=1,.QuerySampleFence=77};
 f=fopen(argv[2],"wb");assert(f&&fwrite(&contract,1,sizeof(contract),f)==sizeof(contract));fclose(f);
 /* If the interrupted writer cannot finish, return immediately; no spin. */
 cache.RecoverySequence=3;atomic_reads=0;
 assert(TgGpuCopyDebugReport(&cache,76,0x141,&out,sizeof(out))==0);
 assert(out.Flags==(TgGpuDebugRecoveryBusy|TgGpuDebugExecutionValid)&&!out.Recovery.Version);
 assert(atomic_reads==41); /* one odd sequence + complete 38-word execution */
 cache.RecoverySequence=2;next=first;next.SubmissionFence=88;next.LastCompletedFence=87;
 interrupt_writer=publish_during_read;atomic_reads=0;
 assert(TgGpuCopyDebugReport(&cache,87,0x117,&out,sizeof(out))==0);
 assert(out.Flags==(TgGpuDebugRecoveryBusy|TgGpuDebugExecutionValid));
 const UCHAR *discarded=(const UCHAR *)&out.Recovery;
 for(unsigned i=0;i<sizeof(first);i++)assert(discarded[i]==0);
 atomic_reads=0;assert(TgGpuCopyDebugReport(&cache,87,0x117,&out,sizeof(out))==0);
 assert(out.Flags==3&&out.Recovery.SubmissionFence==88&&out.Reason==0x117);
 /* Reset and a fresh publication must not reuse a reader's old sequence. */
 cache.RecoverySequence=0;TgGpuCacheRecovery(&cache,&first);
 next=first;next.Source=2;next.AdapterLuid.LowPart=0x2222;next.SubmissionFence=88;
 interrupt_writer=reset_publish_during_read;atomic_reads=0;
 assert(TgGpuCopyDebugReport(&cache,87,0x117,&out,sizeof(out))==0);
 assert(out.Flags==(TgGpuDebugRecoveryBusy|TgGpuDebugExecutionBusy));
 for(unsigned i=0;i<sizeof(out.Recovery);i++)assert(!((UCHAR *)&out.Recovery)[i]);
 atomic_reads=0;assert(TgGpuCopyDebugReport(&cache,87,0x117,&out,sizeof(out))==0);
 assert(out.Flags==(TgGpuDebugRecoveryValid|TgGpuDebugExecutionBusy));
 assert(!memcmp(&out.Recovery,&next,sizeof(next)));
 TgGpuCacheExecution(&cache,&execution);
 cache.RecoverySequence=(LONG)0xfffffffeu;TgGpuCacheRecovery(&cache,&first);
 assert(cache.RecoverySequence==2);
 /* Initialization invalidates used records without restarting their sequence. */
 TgGpuResetDebugCache(&cache);atomic_reads=0;
 assert(TgGpuCopyDebugReport(&cache,76,0x117,&out,sizeof(out))==0&&
        out.Flags==(TgGpuDebugRecoveryBusy|TgGpuDebugExecutionBusy));
 assert(atomic_reads==2);
 TgGpuCacheRecovery(&cache,&first);TgGpuCacheExecution(&cache,&execution);
 cache.ExecutionSequence|=1;atomic_reads=0;
 assert(TgGpuCopyDebugReport(&cache,76,0x141,&out,sizeof(out))==0);
 assert(out.Flags==(TgGpuDebugRecoveryValid|TgGpuDebugExecutionBusy));
 const UCHAR *unavailable=(const UCHAR *)&out.Execution;
 for(unsigned i=0;i<sizeof(out.Execution);i++)assert(!unavailable[i]);
 cache.ExecutionSequence=2;
 for(unsigned n=0;n<=sizeof(out)+2;n++){
  UCHAR guarded[sizeof(out)+8];memset(guarded,0xa5,sizeof(guarded));atomic_reads=0;
  NTSTATUS status=TgGpuCopyDebugReport(&cache,76,0x116,guarded+3,n);
  unsigned bytes=n<32?0:n<sizeof(out)?n:sizeof(out);
  assert(status==(n<32?STATUS_UNSUCCESSFUL:0));
  for(unsigned i=0;i<3;i++)assert(guarded[i]==0xa5);
  for(unsigned i=3+bytes;i<sizeof(guarded);i++)assert(guarded[i]==0xa5);
  if(bytes){
   memset(&out,0,sizeof(out));memcpy(&out,guarded+3,bytes);
   assert(out.WrittenBytes==bytes&&out.ReportBytes==256);
   assert(!!(out.Flags&TgGpuDebugRecoveryValid)==(bytes>=104));
   assert(!!(out.Flags&TgGpuDebugExecutionValid)==(bytes==256));
  }
 }
 puts("PASS: actual dump-cache publication, interrupted/changed snapshots, wrap, truncation, unaligned output and bounded reads");
}
'''
with tempfile.TemporaryDirectory(prefix='tg-debug-report-') as directory:
    path = Path(directory)
    (path / 'ntddk.h').write_text(wdk)
    (path / 'test.c').write_text(test)
    subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(path),
                    '-I' + str(root / 'windows'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    wire = path / 'report.bin'
    tdr_wire = path / 'tdr.bin'
    subprocess.run([str(path / 'test'), str(wire), str(tdr_wire)], check=True)
    sys.path.insert(0, str(root / 'tools'))
    from decode_debug_report import decode, HEADER
    data = wire.read_bytes()
    report = decode(data)
    assert report['reason'] == '0x00000141' and report['last_completed_fence'] == 76
    assert report['recovery']['submission_fence'] == 77
    assert report['execution']['stage_name'] == 'gpu_fence'
    assert report['execution']['expected'] == 8 and report['execution']['observed'] == 7
    # Parse the actual C wire layout, then check incomplete/contradictory headers.
    for written, flags, accepted in ((32, 4, True), (104, 5, True), (256, 10, True),
                                    (32, 5, False), (104, 7, False), (256, 9, False),
                                    (256, 3, True)):
        header = list(HEADER.unpack_from(data));header[3] = written;header[5] = flags
        candidate = HEADER.pack(*header) + data[32:written]
        try:
            parsed = decode(candidate)
        except ValueError:
            assert not accepted
        else:
            assert accepted
    for candidate in (data[:31], data[:-1], b'BAD!' + data[4:]):
        try:
            decode(candidate)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted a truncated/incompatible report')
    print('PASS: decoder accepts the actual C layout and rejects missing/conflicting records')
    from decode_recovery import TDR_PAYLOAD, _decode_event
    tdr_data = tdr_wire.read_bytes()
    observed = _decode_event(8, 1, tdr_data, {})
    assert TDR_PAYLOAD.size == len(tdr_data) == 64
    assert observed['operation'] == 'query_engine' and observed['result'] == 1
    assert observed['last_submitted_fence'] == 79 and observed['last_completed_fence'] == 78
    assert observed['previous_query_fence'] == 77 and observed['queue']['pending']
    assert observed['adapter_luid'] == '0000000200001234'
    original = list(TDR_PAYLOAD.unpack(tdr_data))
    for index, value in ((0, 2), (1, 6), (2, 0), (3, 4), (3, 8192), (7, -1), (12, 2)):
        row = list(original); row[index] = value
        try:
            _decode_event(8, 1, TDR_PAYLOAD.pack(*row), {})
        except ValueError:
            pass
        else:
            raise AssertionError('accepted inconsistent TDR validity or output')
    row = list(original);row[1] = 1;row[2] = 0;row[3] = 1 | 4 | 4096;row[12] = row[13] = 0
    entry = _decode_event(8, 1, TDR_PAYLOAD.pack(*row), {})
    assert entry['status'] is None and entry['result'] is None and entry['queue'] is None
    row = list(original);row[1] = 5;row[3] = 2;row[5] = row[6] = row[10] = row[11] = row[13] = 0
    query = _decode_event(8, 1, TDR_PAYLOAD.pack(*row), {})
    assert query['result'] == 1 and query['last_submitted_fence'] is None
    assert query['last_completed_fence'] is None and query['adapter_luid'] is None
    row = list(original);row[1] = 4;row[3] = 1 | 4 | 32 | 512 | 4096
    row[7] = -1073741823;row[12] = row[13] = 0;row[14] = -1073741643
    failed = _decode_event(8, 1, TDR_PAYLOAD.pack(*row), {})
    assert failed['result'] is None and failed['status'] == '0xc0000001'
    assert failed['queue']['failure_status'] == '0xc00000b5'
    print('PASS: TDR C wire layout, actual-output validity and unavailable-field decoding')
