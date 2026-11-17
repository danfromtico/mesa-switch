# NVDEC resource-pair controller

`Pair.ps1` is a controlled KMD/ACPI resource trial, not a decoder installer.
The main chat prepares `pair-config.json`, verifies both signed packages with
Windows SignTool, and copies the exact installed activation files. The script
retains the original UMD/ICD registration, power policy and five-second logon
task. It must run from a protected trial directory as administrator.

The main chat drives these separate actions and reads each saved result before
continuing:

1. `Prepare` checks all baseline files and the loaded DSDT, captures the
   task/registration/power backup, and builds a separate activation directory
   from the installed files.
2. `StageCandidate` stages the signed package, closes native startup, writes the
   paired ACPI table and requests one restart.
3. After the restart, `SelectCandidate` saves the loaded DSDT, GPU presence and
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
4. Once activation completes, `FinishCandidate` checks the loaded table again
   and requires full dynamic EMC/QoS readiness with all error fields zero
   before running the bounded resource query. Failure records
   `CandidatePlatformRejectedAfterActivation` and requires recovery. A
   successful query does not prove DMA or decode. Inspect its register validity
   and resource status before proceeding.
5. `Recover` closes native startup, restores the original ACPI table and requests
   one restart. After that boot, `SelectBaseline` restores the original KMD,
   registration and task. `FinishBaseline` verifies the original file set and
   loaded DSDT.

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
