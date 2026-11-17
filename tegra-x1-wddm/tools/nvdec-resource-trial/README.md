# NVDEC resource-pair controller

`Pair.ps1` is a controlled KMD/ACPI resource trial, not a decoder installer.
The main chat prepares `pair-config.json`, verifies both signed packages with
Windows SignTool, and copies the exact installed activation files. The script
retains the original UMD/ICD registration, power policy and five-second logon
task. It must run from a protected trial directory as administrator.

An incremental KMD candidate can use the currently verified activation as its
baseline. If that activation retains userspace at an earlier protected path,
`BaselineUserspaceDirectory` names it explicitly; the registered UMD must match
both `BaselineFiles` and the copied activation manifest. Registration is kept
at that path. Identical baseline/candidate provider SYS identities can be reused,
but still require the actual loaded PE identity. Equal PE fields for different
file hashes remain ambiguous and rejected. A reused provider cannot justify a
known-other-image reboot. An identical ACPI file is not rewritten; volume flush,
unbuffered identity and post-boot loaded-table checks still apply.
The new activation's embedded rollback KMD, ACPI and INF metadata are rebuilt
from that current baseline, so cloning an activation cannot silently keep an
older recovery target. Its existing display-preference artifact is retained
and must be checked against the current90-degree mode before use.

When `pair-config.json` includes `Provider`, the same trial also carries the
clock-provider SYS/INF/CAT and its exact original package. All32 package files
must pass signature/catalog membership checks. After `Prepare`, run both
`StagePackage` and `StageProviderPackage`; neither action installs a driver or
restarts Windows. The latter can reconcile an interrupted observation only
when a durable zero-exit receipt and the complete registered package exist.

After `StageCandidate` has loaded the candidate DSDT with native GPU startup
closed, run `SelectProvider` before `SelectCandidate`. Provider replacement is
rejected while the GPU is active or the loaded table differs. Changed providers
use the ARM64 `defer-provider.exe` built from `../defer_provider.c`; its length
and SHA256 must be in `InputFiles`. It selects the exact compatible signed INF
with public SetupAPI `DI_DONOTCALLCONFIGMG | DI_NEEDREBOOT` flags. Every changed
provider requires a boot; a successful copy and Code0 do not verify the loaded
image. `ProviderRestart` can request that boot once; rerun `SelectProvider`
afterward to verify the result without installing again. The native
2026-11-17 round demonstrated that this first boot can still run the previous
image even though the disk SYS and selected INF already match the target.
Both candidate selection and recovery now require `loaded-image-identity.exe`
from `../loaded_image_identity.c`, with its complete length/SHA256 in
`InputFiles`. It records a short, uniquely named system-logger session containing only
image events, stops it, and reads the public ETW/TDH image rundown. The loaded
provider's PE checksum, timestamp and image size must match the independently
hashed package; these fields are not a cryptographic hash of kernel memory.
The paired images must be distinguishable by these fields.

`provider-loaded-{candidate,baseline}.json` retains observations per boot.
Missing events, lost events, helper failure or an unknown image stop selection.
An interrupted observation must be inspected rather than rerun in the same
boot. A completed capture with event loss may be replaced once in that boot
only by a different, hashed observer executable and an unchanged driver target;
the failed receipt remains in history. The initial1 MiB buffer cap lost17 events
on a native first-boot rundown. The helper now reserves2 MiB and caps expansion
at4 MiB, while still rejecting any event loss. No trace failure authorizes a boot.
Only a recorded mismatch with the known other package permits the
explicit `ProviderLoadRestart` action, once, with GPU startup closed. It does
not reinstall a driver. Rerun the appropriate provider selection after that
boot; a persistent mismatch cannot trigger another reconciliation restart.
The image checker uses Microsoft's [image rundown schema](https://learn.microsoft.com/en-us/windows/win32/etw/image-load)
and [system logger flags](https://learn.microsoft.com/en-us/windows/win32/etw/event-trace-properties).
Build with LLVM-MinGW:
`aarch64-w64-mingw32-clang -std=c11 -O2 -Wall -Wextra -Werror -static -municode ../loaded_image_identity.c -ladvapi32 -ltdh -lole32 -o loaded-image-identity.exe`.
Archive the small identity receipts and remove the temporary ETL files with
the trial directory after recovery.

`FinishCandidate` still only queries resource
state. The bounded decoder program is a separate, explicit subsequent action.

Recovery is symmetric: `Recover` loads the original DSDT with GPU startup
closed, then `SelectOriginalProvider` restores the original clock provider
before `SelectBaseline` can activate the original KMD. The same recorded-restart
rule applies when required. Failed selection, pending import or unknown binary
identity must be inspected; they never authorize a repeated install.

The earlier live `UpdateDriverForPlugAndPlayDevices` provider replacement
remained incomplete across an unexpected reboot. The last complete SetupAPI
entry was `Restarting Devices`.
The candidate KMD was not selected and no decoder submission occurred. The
live replacement route is removed; neither the helper's success nor a later
recovery establishes the cause of that reboot. The deferred-install contract
is documented by Microsoft under
[SP_DEVINSTALL_PARAMS](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/ns-setupapi-sp_devinstall_params_a)
and [SetupDiInstallDevice](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdiinstalldevice).
The application calls `SetupDiCallClassInstaller`, not the lower default
handler directly. Inspect mode only constructs an in-memory selection.
For LLVM-MinGW, build the helper with
`aarch64-w64-mingw32-clang -std=c11 -O2 -Wall -Wextra -Werror -municode ../defer_provider.c -lsetupapi -o defer-provider.exe`.

The main chat drives these separate actions and reads each saved result before
continuing:

1. `Prepare` checks all baseline files and the loaded DSDT, captures the
   task/registration/power backup, and builds a separate activation directory
   from the installed files.
2. `StagePackage` locates an already registered candidate by exact SYS identity,
   checks all thirteen signed package members and verifies the published INF
   and driver-store INF using public SetupAPI calls. It reuses a matching
   package. If none exists, it records import intent, invokes `pnputil` without
   `/install`, and verifies the registered contents afterward. It never changes
   the boot table or activation task and never restarts Windows. If observation
   expires during import, inspect the state/store before taking another action.
3. `StageCandidate` requires the completed staging receipt and rechecks the
   registered package. It performs no import: it closes native startup, writes
   the paired ACPI table and requests one restart. Before requesting that boot,
   `Commit-BootTable` flushes the file, resolves and flushes its actual volume,
   then requires an exact SHA256/length match from a bounded unbuffered read.
   `boot-table-candidate.json` records copy, volume flush, readback and failure
   boundaries. An existing receipt cannot be silently retried. `Recover` uses
   the same contract with `boot-table-baseline.json`. Any I/O failure or mismatch
   prevents the restart request. This strengthens the observable handoff; it
   does not prove that winload subsequently reads or accepts the table, nor
   establish the cause of the previously observed old-table boot. Loaded-DSDT
   and device checks after the boot remain mandatory.
4. After the restart, `SelectCandidate` saves the loaded DSDT, GPU presence and
   inherited power/EMC status before checking the closed gate. The loaded table
   must match the candidate, and the GPU must be present. Closed-gate EMC needs
   fresh rate/enumeration and valid ACTMON, with no enumeration/ACTMON/QoS or
   latched mailbox/transition faults. Dynamic/QoS flags depend on the GPU's
   handshake and are not required yet. `SwEmcNotReady` is permitted only at the
   unchanged high boot rate, with zero transitions and neither dynamic nor QoS
   enabled; it remains a pending floor-provider state, not active readiness.
   Only then may the controller select the KMD and start the retained task.
   Failed/missing platform status stops at
   `CandidatePlatformRejected`; recover the original pair rather than retrying
   selection. `Inspect` observes progress; it never retries activation.
5. Once activation completes, `FinishCandidate` checks the loaded table again
   and requires full dynamic EMC/QoS readiness with all error fields zero
   before running the bounded resource query. Failure records
   `CandidatePlatformRejectedAfterActivation` and requires recovery. A
   successful query does not prove DMA or decode. Inspect its register validity
   and resource status before proceeding.
6. `Recover` closes native startup, restores the original ACPI table and requests
   one restart. After that boot, `SelectBaseline` verifies the loaded original
   DSDT before restoring the KMD, registration and task. `FinishBaseline`
   verifies the original file set and loaded DSDT.

SSH observation timeouts do not authorize repeating an action. Interrupted or
unexpected states require reading the durable state, selection receipts and
task/process status. The main chat remains responsible for recovery, evidence
collection and cleanup; no background trial or reboot loop is installed.

The first native run on 2026-11-16 used the archived controller hash in
[the native round record](../../docs/NVDEC_RESOURCE_PREFLIGHT_20261116.json). The current script additionally saves
the pre-selection EMC state and a timestamp/action on every state update. That
instrumentation was added after an unresolved ACTMON readback difference; it
does not retrospectively fill the missing observation in the first run.
The current controller also rejects that known failure before candidate
selection and checks active EMC during baseline preparation. Fan33's matching
source distinguishes the closed-gate handoff from active GPU operation.
The loaded table comes from the public `GetSystemFirmwareTable` API (the first
DSDT); a disk-file hash alone does not establish which table Windows loaded.

The volume operation uses the documented administrative
[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
contract. Unbuffered reads use an allocation-aligned buffer and64KiB request
multiples, bounded to1MiB, and require the actual file byte count. This has been
checked on the target storage; an unsupported alignment fails the handoff.
`tests/Test-BootTable.ps1` replaces native effects and checks that candidate and
recovery reboots cannot follow a failed or interrupted commit. Those controls
are not hardware durability or boot acceptance evidence.
Its pure PowerShell control exercises the actual selection and finish branches
with native operations replaced; it does not prove hardware behavior.

A later combined import/table-change boot loaded a4568-byte DSDT without GPU0,
despite the expected candidate file on disk. The same candidate table had
loaded in the ACPI-only control. Import and boot-table handoff are now separate
actions so a verified existing package can be reused without repeating import.
That sequencing change does not establish why the earlier override was absent;
the loaded-table guard remains mandatory. Staging receipts use the existing
durable JSON writer instead of relying on a redirected text log, which was
all NUL bytes after the failed boot.
