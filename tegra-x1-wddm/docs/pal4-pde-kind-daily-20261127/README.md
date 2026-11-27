# PAL4 page-directory repair and daily diagnostics

The scoped PAL4 startup repair was selected after a clean, one-use Switch Lite
qualification at boot **106486**. The protected on-device policy returned
`QualifiedPromotion` and granted permanent native startup for the exact
component profile. A subsequent permanent boot **106579** retained Code 0,
the exact loaded native components and `PagingStopProbeArmed=0`; its interval
contains the requested controlled restart and no abnormal-shutdown events.
[Qualification](qualification.json) · [Permanent approval](permanent-approval.json) ·
[Permanent-boot verification](permanent-boot.json)

- KMD: `1d88065b2e4a8ed777c40da0802331ed542763ca1520ee5630c2a150185813d5`.
- Functional source: `d1c07b6a8c4cc812ba8fdbdd53a3780dcac9c7a2`.
- Actual ETW input: operation 11, level 1, GPU_PHYSICAL mode 2, no allocation,
  kind `0xfe`, return status 0. No events or log buffers were lost.
- A 69,206,016-byte optimal image and a 4 KiB working control each rendered
  and correctly read back 1024 pixels and 128 guard bytes. Both processes
  exited with code 0; the boot record remained unchanged and DWM was available.
- The startup and paging unit tests ran on the Switch; the package SYS/CAT
  signatures were valid. The project owner explicitly attested that review
  was complete. The review record identifies that human attestation and does
  not claim a completed external-model audit.

[Native case](native-case.json) · [ETW parameters and loss counters](etw-summary.json) ·
[Build and signatures](build.json) · [Owner review attestation](code-review.json).

Native KMD builds now retain bounded ETW diagnostics: event 11 records the
first unsupported paging return per StartDevice, and event 12 records the
first accepted nonzero-kind directory update. Collection requires an enabled
ETW session; this is not a durable disk log. Successful requests are not
logged individually. The explicit `--paging-stop-probe` build remains a
separate one-use diagnostic option that rejects permanent permission; it
is **not enabled** in this daily KMD. Startup reported
`PagingDiagnosticsVersion=1` and `PagingStopProbeArmed=0`.

Two-level paging remains enabled. Known leaf kinds are accepted on the
GPU-physical directory path, but PDEs still contain only child-table pointers.
CPU initialization restrictions, the kind whitelist and existing validation
remain in force. NVK, tgd3d and their architecture-specific implementations
are unchanged at their accepted hashes.

The original game startup, movie and gameplay observations remain in the
[game report](../game-runs-20261127/README.md). PAL4 audio and both games'
performance remain unresolved. All historical open faults are retained;
this promotion is limited to the independently exercised paging repair and
default dormant diagnostics. It does not close broad presentation, restart
or full-game acceptance issues.
