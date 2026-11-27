# First KMD recovery event

The `.153` diagnostic candidate adds provider
`{c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0}`, event **1**, version **1**,
level **2**, keyword **1**. It records the first transition to
`RecoveryPending`, after releasing engine/queue spinlocks and before
`DxgkCbSynchronizeExecution` requests GPU engine recovery. A notification
retry keeps the first status and does not emit a second record. A later reset
can start a new recovery episode.

The implementation accepts callers through DISPATCH_LEVEL and copies only
scalar fields. It performs no GPU access, allocation, disk write or command
pointer dereference. DriverEntry registers the provider and records its
`TraceStatus`; unload and failed initialization unregister it. The diagnostic
provider does not change submission admission, allocation limits or recovery
behavior.

The payload is a fixed 72-byte little-endian record:

| Byte offset | Type | Field |
| --- | --- | --- |
| 0, 4 | u32 each | Payload version, source |
| 8, 12 | u32 each | Adapter LUID low/high parts |
| 16 | u32 bits | Original NTSTATUS |
| 20 | u32 | Virtual-submit flags |
| 24 | u64 | Opaque context value |
| 32, 36, 40 | u32 each | DMA bytes, private bytes, UMD-private bytes |
| 44, 48, 52 | u32 each | Submission fence, last completed fence, graphics generation |
| 56, 60, 64, 68 | u32 each | Cached FIFO interrupt, MMU fault, PBDMA interrupt 0/1 |

Sources are `0=unspecified`, `1=submit_gate`, `2=submit`, `3=reject_drain`,
`4=preempt`, `5=worker`, `6=legacy_submit`. Submit metadata is zero for paths
without a virtual-submit argument. Cached register fields describe the last
cached channel state; they are not newly sampled hardware fault registers.
The original status and source identify the recovery trigger, not necessarily
its underlying cause.

Enable this GUID in the existing diagnostic session with keyword 1 and level
2 or higher. Retain the raw ETL as well as its `SWETW001` ProcessTrace export.
The decoder accepts that binary export (not ETL) and validates its event count,
lengths and schema before printing recovery records:

```sh
python3 tegra-x1-wddm/tools/decode_recovery.py dxg-start.etl.001.recovered.bin
```

ETW may lose buffers or leave the tail unflushed if execution stops. A circular
trace may also overwrite earlier records. No matching event does **not** prove
that no failure occurred. Check `TraceStatus`, provider enablement, export
identity, ETL finalization and loss counts with the existing evidence.

This event covers engine-recovery requests. A malformed virtual submission
returned as `STATUS_INVALID_PARAMETER` can instead put its calling device in
an error state without requesting engine recovery. The current provider does
not separately record that rejection in `.153`/`.154`. Check this path if device loss has no
corresponding recovery event; see Microsoft's
[SubmitCommandVirtual return contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual).

Windows ARM64 context/worker models verify first-status retention, duplicate
suppression, emission before OS notification, spinlock release, PASSIVE_LEVEL
submission failure and DISPATCH_LEVEL preemption failure. Decoder tests cover
fixed wire bytes and truncated/incompatible exports. These controls do not
prove live kernel ETW delivery. The later `sample-i153-native-20261111` trial
does retain five valid events on native LUID `b02db`, verifying live delivery.
They describe paging submissions returning `STATUS_DEVICE_NOT_READY` in
graphics generations 3 through 7. They follow the first observed device-error
mark and DWM's generic resource OOM, so none establishes the initial failure.
Zero cached register values do not establish healthy hardware. The native ETL
is unfinalized; its raw export and device hashes are retained.

OS-requested reset can also set `RecoveryPending` directly in
`isolate_backend`, without going through this notification event. Together
with the malformed-submit path above, that is an attribution gap in those
packages; event 2 below records these boundaries before state is cleared.

The implementation follows Microsoft's
[EtwRegister](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwregister),
[EtwWrite](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwwrite)
and [EtwUnregister](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-etwunregister)
contracts.

## Execution state in .155

The same provider adds event **2**, version **1**, informational level **4**,
keyword **1**. Enable this provider at level **4 or higher** to receive it;
the earlier level-2 boot-trace setup receives only event 1. It emits after
preemption admission, on submit/worker failure, and at OS reset entry/exit.
Successful preemption records are observations, not failure reports. Worker
failure is emitted before releasing the dispatch gate, so a later admission's
device-not-ready status cannot replace that record. Event 1 remains unchanged.

Each record copies scalar state under queue/engine locks and emits after
releasing both. It performs no additional GPU access, allocation or disk I/O.
The current work's polling path saves the fence and fault values it already
reads. The worker publishes its phase around mapping, activation, preparation,
submission, fence checks, retirement, software/paging work and notification.
Current-work clearing also takes the queue lock so the snapshot cannot race
that clear. The internal DMA-private size continues to be negotiated from the
updated structure size; the UMD/ICD interfaces and 512-MiB budget are unchanged.

The fixed little-endian payload is 152 bytes (`<8I3Q24I`):

| Offsets | Values in order |
| --- | --- |
| 0–28, u32 | Version, source, LUID low/high, status, request fence, request flags, stage |
| 32, 40, 48, u64 | Request context, current context, current DMA address |
| 56–68, u32 | Current submission fence, last completed fence, expected marker, last observed marker |
| 72–84, u32 | Current DMA bytes, work flags, queue flags, queue NTSTATUS |
| 88–100, u32 | Graphics generation, channel stage, last channel register/value |
| 104–132, u32 | Cached FIFO/MMU/PBDMA0/PBDMA1, MC, GR, FECS error/host interrupt |
| 136–148, u32 | Poll count, request DMA bytes, request private bytes, request UMD-private bytes |

Sources are `1=preempt`, `2=worker_failure`, `3=submit_failure`, `4=reset_begin`
and `5=reset_end`. Request fence means the preemption ID for source 1 and the
submitted fence for source 3. Reset/worker records leave request fields zero.
The decoder names stages and expands work/queue bits into `work_state` and
`queue_state`; raw masks and addresses remain available. It counts execution
events separately from first-recovery events and still accepts old exports.

`observed_valid` marks an actual read; `observed_cpu` distinguishes the retired
CPU readback from the active BAR1 GPU fence. That observed value can precede a
later fault-only poll. Poll count zero means no completed observation. `mc_valid`
and `gr_valid` distinguish reads from zero-filled unsampled fields. Channel
stage/register/value are copied only with `channel_fields_valid`: a helper
may be writing them outside the engine lock while `hardware_busy` is set.
The published worker phase still identifies that helper. No raw command buffer
or referenced pointer is dereferenced for tracing.

The preemption event captures state after admission; concurrently completed
work may already be absent. Neither that absence nor a cached zero establishes
healthy hardware. ETW flush/loss and interrupted-tail limits still apply.
`stage` is the last published worker phase: for example, notification cleanup
can retain `notify` after current work has been cleared. Interpret a current
operation only when `active` is set. A DISPATCH-level preemption event can
interleave before a worker-failure event; timestamp order alone does not assign
the first cause, even though the worker's original failure is retained before
its dispatch gate is released.
Use reset-entry records before reset-exit/generation changes to find the
original work, rather than diagnosing it from later paging recovery requests.

Host wire-format/decoder checks and the full WDK build pass. Windows ARM64
context controls on `.59` verify DISPATCH-level preemption metadata, actual
uncompleted fence values, no additional GPU access, lock release before ETW,
channel-field suppression during a busy helper, rejected-submit metadata and
worker-failure emission while the dispatch gate is still held. The corrected
context fixture exits 0 at 01:32:25 UTC on November 12; reset controls exit 0
at 01:30:38 and verify paired entry/exit records for success and failure.
An initial new assertion incorrectly expected fence zero; the existing model
initializes the complement of the expected marker. That test expectation is
corrected without changing the driver's completion condition. Its failed
executable/result remain in the external evidence.

These are user-mode OS/GPU substitutes, not a physical GPU pass. Build/model
evidence is in `execution155-20261112`; the native trial follows below.

## Native .155 result

`execution155-native-20261112` uses signed KMD `add32c094862`, corrected UMD
`77dea046dde0` and unchanged ICD `6e2fad99d742`. The No3D stub initially lacks
an embedded signature; staging detects this before installation. The signed
stub and rebuilt catalog pass verification at 01:39:34 UTC on November 12.
The existing boot trace enables this provider at level 4.

The first ETL contains 238,953 exported events, including 94 execution-state
events (75 preemption, nine submit failures, five reset entries and five reset
exits), five first-recovery events and 23,285 UMD events. Native adapter LUID
`ba67c` reports the ACPI identity `SWOA0F01`. The retained first failure is:

| UTC | Observation |
| --- | --- |
| 01:45:45.908048 | Dxgkrnl starts client render submission 749. |
| 01:45:47.933467 | Preemption 71 is admitted; current work is still 749 at `gpu_fence`, with 131 polls. |
| 01:45:47.992958 | The DWM-associated device receives `VidSchMarkDeviceAsError`, numeric reason 7 (uninterpreted). |
| 01:45:48.099602 | Reset entry retains current 749, last completed 748, expected marker 80, observed 0, and 140 polls. |
| 01:45:48.120870 | Reset exit returns success; the graphics generation changes from 1 to 3. |
| 01:45:48.273855 | DWM reports generic `E_OUTOFMEMORY` from resource creation, 281 ms after the device-error mark. |

The active application packet is 64,864 bytes at the private command VA
`0x04004000`. It is prepared, attempted and submitted, with a valid GPU fence
observation; GPU/CPU-completed flags remain clear. Queue failure is still zero
at reset entry. Cached FIFO/MMU/PBDMA and GR/FECS error values are zero.
`cached_mc=0xd8213120` is **MC_ENABLE**, not an interrupt status. These samples
do not establish healthy hardware or identify the instruction that stalled.
The 286 retained resource-reservation exits between native adapter reporting
and the first device error all return zero; those exit records lack an adapter
field and are not a complete memory-budget measurement.

Dxgkrnl context/device records associate the work with DWM PID 484 and the
native adapter. KMD and scheduler context handles differ; they are not equated
by pointer value. Dxgkrnl emits completion/stop records for 749/750 after the
device-error mark but just before reset entry, while the KMD still reports
749 uncompleted. Those cleanup records are not evidence of physical GPU
completion. Packet type 3 is system preemption, per Microsoft's
[DMA packet enum](https://learn.microsoft.com/en-us/previous-versions/hh706296(v=vs.85)).
The five later first-recovery events describe paging failures after reset,
not this initial fence wait. The command words and hardware fetch position
are not in event 2; capture those next before choosing a functional fix.

The native trace is **unfinalized** (`EndTime=0`, `BuffersWritten=0`), with no
reported loss. Its export hash is
`babaa9de1f73fdfec28c0536ff7823180983c6f17c446c027f2c53f05db35e7a`.
The second trace is finalized (493 buffers), contains 178,093 exported events
and no UMD/KMD diagnostic events; its export hash is
`7346298a7d99a922145cfbafe22693c2c8c5e3aa8ffb40cf5378f0504446410d`.
The device-template decoder retains a known Dwm-Core event 203 payload-length
mismatch (1,291 instances); those events are excluded from the interpretation.
The first trace has 15 successful DWM flip callbacks, but no retained native
PNG or Load result. Neither those callbacks nor the later controller Code 0
record at 01:46:05 proves a complete desktop.

The user reports black output/reboot; an extra boot occurs at 01:46:50.
All 159 returned files (136,536,269 bytes) match device lengths/hashes.
Recovery `.59` Code 0, cleanup and unchanged power are verified at 01:52:36;
portrait is unchanged at 01:52:55. The separate capture
`desktop-after-execution155-20261112/geometry-control.png` at 01:58:42 is
visually inspected: full taskbar/Start and right-side tray/clock, 720x1280,
DPI 96, with `.59` before/after. SHA256:
`f5c7360c560cabfa8b879ff73504e28577c9459cf29d4d43676fbf434e0d308b`.
No shell repair is needed this time. This is recovery software-display evidence,
not native GPU acceleration, physical landscape or sustained stability.

## Reset packet capture in .156

Event **3**, version **1**, level **4**, keyword **1** retains an active
application packet before reset frees its immutable KMD command snapshot.
The call runs at PASSIVE_LEVEL only after admission is paused, the worker is
joined and `DispatchGate` is held. It holds no spinlock. When this event is
disabled, the path performs no extra GPU reads. It does not read an application
VA, borrowed private-buffer pointer, shader buffer or GPU-writable command
mirror. Paging and software work are excluded. Reset return values, fence
completion and memory policy are unchanged.

Each event contains a 144-byte little-endian header (`<4I2Q28I`) followed by
at most 1,024 command bytes. At most one 64-KiB packet is captured per reset
attempt, without memory allocation or disk I/O. The fields are:

| Offsets | Values |
| --- | --- |
| 0–12, u32 | Version, chunk bytes, adapter LUID low/high |
| 16, 24, u64 | KMD context identity, private command VA |
| 32–52, u32 | Submission fence, graphics generation, expected marker, DMA bytes, captured command bytes, chunk offset |
| 56–140, u32 | Hardware flags, MC_ENABLE, PBDMA_ENABLE, PBDMA status before/after, USERD GET/PUT/reference, GP GET/PUT/FETCH, PB GET low/high, PB FETCH low/high, PB header, method0, data0, engine0/1 status, GR interrupt/current context |

The read-only register sample precedes chunk emission and is repeated in each
header. Hardware flag bits are `1=enabled FIFO`, `2=matching PBDMA slot before
and after sampling`, `4=validated USERD BAR1 alias`, `8=enabled GR`.
Unflagged fields are not valid observations. Even a flagged snapshot is
sequential, not atomic: high/low pointer words and method/data can advance
while the GPU runs. Do not infer a stable instruction from one register alone.
PB GET and PB FETCH use the distinct registers in the pinned NVIDIA/Switchroot
[GM20B header](https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/include/nvgpu/hw/gm20b/hw_pbdma_gm20b.h).

Use `decode_recovery.py EXPORT --packet-dir DIR` to retain raw chunks and
reconstruct complete command captures. It emits `.bin` only for a contiguous,
consistent capture; missing/overlapping chunks or differing headers remain
explicitly incomplete in metadata. A metadata-only record with captured size
zero is not an empty successful GPU packet. Existing ETW loss/tail limits apply.

Host tests cover matching/changing/foreign PBDMA ownership, read-only access
and exact register identity; decoder checks cover missing and duplicate chunks.
The WDK build passes. Real-WDK ARM64 models on `.59` pass at 02:13:09 and
02:13:18 UTC on November 12, checking owned bytes, 1,024-byte boundaries, no
extra I/O when disabled, retained completion/lifetime state and capture only
after successful worker join. These are controlled substitutes, not native
delivery. Artifacts/results are in `reset-packet156-20261112`; the candidate
is not installed and its native capture remains unverified.
