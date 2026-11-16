# NVDEC resource-pair controller

`Pair.ps1` is a controlled KMD/ACPI resource trial, not a decoder installer.
The main chat prepares `pair-config.json`, verifies both signed packages with
Windows SignTool, and copies the exact installed activation files. The script
retains the original UMD/ICD registration, power policy and five-second logon
task. It must run from a protected trial directory as administrator.

The main chat drives these separate actions and reads each saved result before
continuing:

1. `Prepare` checks all baseline files, captures the task/registration/power
   backup, and builds a separate activation directory from the installed files.
2. `StageCandidate` stages the signed package, closes native startup, writes the
   paired ACPI table and requests one restart.
3. After the restart, `SelectCandidate` verifies the closed gate, saves the
   inherited power/EMC status, selects the KMD and starts the retained activation
   task. `Inspect` observes progress; it never retries activation.
4. Once activation completes, `FinishCandidate` runs the bounded resource
   query. A successful query is not a successful DMA or decode test. Inspect its
   register validity and resource status before proceeding.
5. `Recover` closes native startup, restores the original ACPI table and requests
   one restart. After that boot, `SelectBaseline` restores the original KMD,
   registration and task. `FinishBaseline` verifies the original file set.

SSH observation timeouts do not authorize repeating an action. Interrupted or
unexpected states require reading the durable state, selection receipts and
task/process status. The main chat remains responsible for recovery, evidence
collection and cleanup; no background trial or reboot loop is installed.

The first native run on 2026-11-16 used the archived controller hash in
[the native round record](../../docs/NVDEC_RESOURCE_PREFLIGHT_20261116.json). The current script additionally saves
the pre-selection EMC state and a timestamp/action on every state update. That
instrumentation was added after an unresolved ACTMON readback difference; it
does not retrospectively fill the missing observation in the first run.
