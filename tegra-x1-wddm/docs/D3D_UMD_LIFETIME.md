# Native D3D runtime exit-path investigation

2026-11-10, Switch Lite HDH-001, Windows 11 ARM64 23H2.

**The barrier lifetime fix passes controlled and native validation.**
With the patched UMD, .143 completes the first native runtime run after
demand start and two subsequent process runs on GM20B LUID `a59ef`.
Each checks 16,384 RGBA/BGRA pixels and completes the original implicit
release path with exit 0 and no timeout: 49,152 matching pixels in total.
The ICD, KMD core, probe and compiler frame-pointer settings are unchanged.
The temporary C11 frame-pointer diagnostic DLL was not used for these runs.

| .143 native runtime run | UTC | Result |
| --- | --- | --- |
| First after demand start | 21:03:58–21:04:10 | 16,384 matching pixels, implicit release, exit 0 |
| New process | 21:05:49–21:06:02 | 16,384 matching pixels, implicit release, exit 0 |
| New process | 21:06:57–21:07:10 | 16,384 matching pixels, implicit release, exit 0 |

The .144 regression with the rebuilt ICD/UMD also matches all 16,384 pixels
and exits normally. [Later sharing and runtime result](VULKAN_WIN32_MEMORY.md)

This fixes the independently reproduced Mesa barrier defect and validates
the native exit path under these tests. D3D runtime shared resources, DWM composition
and Win32 Vulkan presentation remain separate unverified work.
[Barrier fix and regressions](https://github.com/ruje0504-2/mesa-switch-woa/commit/3e757c76)

.143 artifacts are under `/private/tmp/t214-gpu-round/runtime-ddi143.rvqimgbg/`.
All 38 test records and five recovery records match device SHA256/lengths;
the ETL export has 133,424
records with matching count and boundaries. .59 Code 0, original driver/ACPI
hashes, removed test tasks/permit and temporary UMD/ICD registrations, and
unchanged display/sleep settings were verified at 21:14:45 UTC. The 512 MiB
GPU ceiling and firmware/Hekate settings remain unchanged.
The final committed fixture also checks that all four queue workers were
created. Both cases pass again at 21:18:39–21:18:40 UTC with executable
SHA256 `709a4cced4c1ecaaac59bd939661957d8b58ef06d7ef97609f7ba119236a6f76`.
Its five records match device hashes in `physical/verified-controls.json`.

## Barrier lifetime defect and controlled fix

.142 captures one original exception after 16,384 matching pixels and
S_OK device status. The frame chain reaches RtlpWaitOnCriticalSection,
RtlEnterCriticalSection, RtlSleepConditionVariableCS and the UMD's
`cnd_wait+0x14`. The read is at address `0x24`; the critical-section pointer
is `2c771f7e0`. Its allocation class is not established by that address.
Omitted frame pointers in the UMD prevent this chain from naming the
immediate caller above cnd_wait. A separate diagnostic UMD retaining C11
frame pointers was built but has not been installed.

Source inspection identifies two defects in the portable barrier path used
on Windows. `util_barrier_wait` returns true to every participant, while
`util_queue_finish_execute` uses true to authorize barrier destruction.
Moreover, waking the waiters does not mean they have finished reacquiring
the barrier's critical section. Destroying it from a worker can invalidate
an object another worker is still using. Windows explicitly reacquires the
critical section before returning from its condition wait.
[SleepConditionVariableCS contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablecs)

The fix returns one serial result per generation and moves destruction to
the queue-finish caller after all worker fences have completed. This protects
the stack-allocated finish barrier until every participant has left it.
No KMD capabilities, GPU commands or memory budget are changed.

The new `barrier_lifetime_test` runs on .59 without using the GPU:

| ARM64 control | Original | Patched |
| --- | --- | --- |
| Four participants, eight barrier generations | Four serial returns in generation 0; exit 1 | One serial return in each generation; exit 0 |
| Four-worker queue finish/lifetime | Access violation `c0000005` in about 0.2 seconds | 512 finishes and 2,048 jobs complete; exit 0 |

The original controls run at 20:46:21–20:46:22 UTC; patched controls run at
20:51:06 UTC. Neither times out. Their executable hashes are respectively
`a836cd13d6ab89a04b5537d8db439172d0cd362b0a179c017925cb7f08ab2e97`
and `dfc42d06bc84f041189cb11714c049827b9078a35bb2ecbf7f20eeb4104bd372`.
.143 provides the native verification described above.

.142 artifacts: `/private/tmp/t214-gpu-round/runtime-ddi142.7ljajbwg/`.
All 28 test records and 15 recovery/control records match device SHA256 and
lengths. The exported ETL has 132,241 records with matching boundaries/count.
.59 Code 0 and the original driver/ACPI hashes were verified at 20:41:31 UTC;
temporary registrations, tasks and permit are gone, and power reports match.
The original Windows-critical-section address should not be described as
necessarily heap allocated; the queue finish barrier itself is on its
caller's stack.

## Earlier .141 evidence

**At .141 the access violation was unresolved.** .141 keeps .140's UMD, ICD
and KMD core and tests the original ComPtr scope-exit release order. All
three runs create a native D3D11 device at feature level 10_0 on GM20B LUID
`97642`, match 16,384 RGBA/BGRA pixels and report S_OK from
GetDeviceRemovedReason. Their process exit results differ:

| UTC | Probe | Process result |
| --- | --- | --- |
| 20:01:32–20:01:43 | Implicit release; old module-query exception reporter | `c0000005`, no timeout |
| 20:04:35–20:05:17 | Implicit release; raw-address reporter and module bases recorded before drawing | 0, no timeout |
| 20:06:36–20:06:50 | Same raw-address probe | 0, no timeout |

The normal exits do not establish a driver fix. The earlier .140 results
and rendering scope are in [D3D UMD results](D3D_UMD_RESULT.md).
Resource sharing and DWM presentation remain pending.

## What the exception record establishes

The first .141 run prints two access-violation headers, both for a read at
address `0x24`, before printing `ntdll+0x5d144` and LR `ntdll+0x5d0bc`.
The old reporter calls GetModuleHandleEx before printing an address, so
this record does not establish the original fault PC or identify a caller.
It may include a secondary fault during reporting.

The retrieved device `ntdll.dll` is version 10.0.22621.2860, SHA256
`dc52ae94b0a10af0dba3846ba536c90bfa5ff5afafa873e0de58d1f56116b5a3`.
Microsoft's symbol file has the image's GUID
`C41C3765-5AF0-0300-8A00-6793E97CB265`. Its section-relative native symbols
corroborate the PE exports RtlWaitOnAddress (`5d080`) and
RtlEnterCriticalSection (`30390`); the ARM64EC nested exports are different.
RtlpWaitOnCriticalSection starts at `5d098`, placing the recorded PC at
offset `ac`. The instruction there reads `[x10 + 0x24]`, consistent with
the reported null-base read. The LR is inside the same routine and does
not identify its caller. The downloaded PDB header age is 3 while the
image's RSDS age is 1; retain this distinction alongside the GUID and RVA
checks, rather than claiming an unqualified debugger symbol match.
[Microsoft symbol artifact](https://msdl.microsoft.com/download/symbols/ntdll.pdb/C41C37655AF003008A006793E97CB2651/ntdll.pdb)

The probe now records module bases while execution is normal and avoids
loader queries in its exception handler. It records raw PC/LR, access type,
fault address, thread ID, selected registers and a bounded ARM64 frame-pointer
chain. The chain is not a complete stack unwind. This keeps fault reporting
independent of module-list lookup during teardown; it does not change the
driver's locking or object lifetime.
[Microsoft DLL and loader-lock guidance](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)

A controlled ARM64 child process on the .59 baseline reads a deliberately
inaccessible page at 20:14:04 UTC. It exits with the expected `c0000005`,
records exactly one exception, reports the injected address and retains
three frame-chain entries. That validates the revised diagnostic on this
device; it does not reproduce or fix the native runtime fault. The frame-chain
version has not yet captured that fault.

## Recovery and evidence

.59 Code 0, original driver/ACPI hashes, removed test tasks/permit and
temporary UMD/ICD registrations, and unchanged AC/DC display/sleep settings
were verified at 20:12:58 UTC. Firmware/Hekate and the 512 MiB GPU budget
were unchanged.

Artifacts: `/private/tmp/t214-gpu-round/runtime-ddi141.6f5gyzh6/`.
All 39 test records match device SHA256/lengths in `physical/verified.json`.
Eight recovery/control records match in `physical/verified-recovery.json`.
The ETL export has 140,854 records with matching count and boundaries.
The retained first-event journal is identical across the three runs; it
does not locate the later user-mode failure.

| Artifact | SHA256 |
| --- | --- |
| First native run, access violation | `88e8623282fc4f709933fd15419517d76b162645578fa17d619fe0d51d5fb3a8` |
| First raw-address run, exit 0 | `a7152846705ba4e0cebcfb4cf312ae0511dc5ea70b2fba08cb02e4a036fe776c` |
| Second raw-address run, exit 0 | `f3f7a12c0ad6c45ac36fc3bdead2c20e6ee9563192bd97a8a229df00ca3dbf64` |
| Controlled exception-capture executable | `afbf4d0a76308d2ad1712d6abf9f42b5ca536fe326a2c99633eee63a1485e727` |

This earlier capture plan led to the .142 original trace and the barrier
fix validated in .143 above. Further work is runtime resource sharing,
synchronization and DWM presentation.
