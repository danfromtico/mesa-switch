# NVDEC resource-pair controller

`Pair.ps1` is a controlled KMD/ACPI resource trial, not a decoder installer.
The main chat prepares `pair-config.json`, verifies both signed packages with
Windows SignTool, and copies the exact installed activation files. The script
retains the original UMD/ICD registration, power policy and five-second logon
task. It must run from a protected trial directory as administrator.

When `pair-config.json` includes `Provider`, the same trial also carries the
clock-provider SYS/INF/CAT and its exact original package. All32 package files
must pass signature/catalog membership checks. After `Prepare`, run both
`StagePackage` and `StageProviderPackage`; neither action installs a driver or
restarts Windows. The latter can reconcile an interrupted observation only
when a durable zero-exit receipt and the complete registered package exist.

After `StageCandidate` has loaded the candidate DSDT with native GPU startup
closed, run `SelectProvider` before `SelectCandidate`. Provider replacement is
rejected while the GPU is active or the loaded table differs. If the public
installer explicitly reports that a restart is required, `ProviderRestart`
can request it once; rerun `SelectProvider` after that boot to verify the
result without installing again. `FinishCandidate` still only queries resource
state. The bounded decoder program is a separate, explicit subsequent action.

Recovery is symmetric: `Recover` loads the original DSDT with GPU startup
closed, then `SelectOriginalProvider` restores the original clock provider
before `SelectBaseline` can activate the original KMD. The same recorded-restart
rule applies when required. Failed selection, pending import or unknown binary
identity must be inspected; they never authorize a repeated install.

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
   the paired ACPI table and requests one restart.
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
