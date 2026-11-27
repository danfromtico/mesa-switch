# Synchronous display timing results

The .120 trial kept the Tegra adapter at Code 0 and reached
`DxgkDdiSetTimingsFromVidPn`. Its seven mode-setting events belong to the
GM20B adapter, LUID `8ec5d`. The driver returned success, but the interactive
display probe found zero active paths. SSH remained responsive while the
panel was black.

`set_timings` cleared the entire returned `TargetState`. This violates the
successful-result contract: NTSTATUS success does not itself report that
the requested link configuration completed. The .121 candidate returns a
nonzero, increasing `ConnectionChangeId`, the requested target ID, and
`LinkConfigurationSucceeded` after synchronous visibility programming
succeeds. It also reports no diagnostic glitch. Failed programming does
not publish a new successful result.

Microsoft's [timing-path contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_set_timing_path_info)
requires returning the resulting target state. The installed Windows
`dxgkrnl.sys`, SHA256
`99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`,
also checks the returned values directly:

- `VIDPN_MGR::CheckDdiSetTimingsViolationOnPathInfo`, RVA `0x173a80`, checks
  the immutable inputs, reserved fields, nonzero change ID and link result.
- When that check fails, the caller at RVA `0x268670` replaces the result
  with `LinkConfigurationFailed` and records `STATUS_DRIVER_INTERNAL_ERROR`.

The WDK header defines the numeric link result as 14 and the three
no-glitch diagnostic fields as 255. These enum constants are exposed only
to C++ in this WDK; the C miniport uses the corresponding values explicitly.

The new ARM64 fixture checks successful inherited activation, path removal,
unique IDs across targets, error propagation and preservation of inputs.
On the Switch, the original implementation fails the nonzero-ID check
(exit 51), while the fix passes (exit 0). The old panel module was rebuilt
and relinked to reproduce the .120 unsigned driver byte for byte before
building the candidate.

## Physical .121 result

The durable record confirms successful timing output with link status 14
and change ID 1. The admitted ETL identifies GM20B LUID `962ec`, followed
by a new `SetDisplayMode` event for 720x1280, format 22, 60 Hz and identity
orientation. This is further progress than .120's repeatedly rejected
timing result. Shared-primary data queries and allocation creation also
returned success.

The system then restarted. The normal record area filled all 224 slots,
mostly with repeated vblank enable/disable calls after mode activation.
The last retained entry is a 256-entry page-table update's entry marker;
the full area cannot record its return. That is not proof it caused the
restart. The admitted ETL is unfinalized and retains no first recovery or
Present failure; the fixed journal likewise has no failure attribution.
The next trial must retain the first unexpected paging/Present failure
independently of the ordinary callback log and limit repeated vblank logs.

After the unexpected restart, the consumed permit denied another .121
hardware start, producing Code 43 with a BasicDisplay desktop. The
original demand script's transient Code 0 was read before hardware startup
completed and must not be treated as stable startup proof. Planned
rollback restored .59 Code 0, the original ACPI table, and removed the test
grant/permit/tasks. Native presentation and startup stability remain open.

Recovery observation confirmed an active BasicDisplay path, LUID `533d`,
720x1280 and rotation 2. Seventeen live evidence files and eight recovery
files matched device-side SHA256 values. Kernel-Power 41 contains no
bugcheck code; timestamps also change across boot/time synchronization,
so they do not identify the faulting instruction or an exact crash delay.
