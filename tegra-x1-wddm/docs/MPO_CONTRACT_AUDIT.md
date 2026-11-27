# MPO and desktop composition audit

November 12, 2026; frozen `.153` KMD / `e99798fa9668` UMD /
`6e2fad99d742` ICD, source `931383e6`. This audit distinguishes a missing
desktop visual from a missing hardware overlay plane. Neither is inferred
from a successful Present alone.

## Contract and implementation

| Boundary | Official contract and current behavior |
| --- | --- |
| UMD presentation | `RuntimePresent` waits for the source resource's GPU work, then calls the runtime's `pfnPresentCb` with its allocation/context and opaque DXGI context. This UMD does not implement the separate DXGI MPO-present callback path. |
| Overlay support check | `check_mpo` returns `Supported=FALSE` for more than one plane or post-composition requests. It accepts one layer-0 primary only when its allocation geometry, full source/destination rectangle, color space and non-blended attributes match. It does not claim arbitrary multi-plane composition. |
| Scheduled scanout | `set_primary_mpo` handles `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3`, validates the owned scanout allocation and segment address, then queues the display flip. Immediate calls above PASSIVE request retry with `PrePresentNeeded`; no pixel copying is done in this DDI. |
| Completion | `TgGpuPanelReadLatched` checks the active scanout layout before moving Pending to Displayed. The VSync handler reports the displayed layer-0 PresentId, not a freshly submitted ID. Existing models check no premature notification and mismatched layout. |
| Post-present | `DxgkDdiPostMultiplaneOverlayPresent` is optional, for hardware optimization after the new configuration takes effect. It is requested through VSync's `PostPresentNeeded`. This implementation leaves that flag clear and does not register the callback. Its absence is not, by itself, a missing composition/copy implementation. |

Microsoft's [MPO support overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/multiplane-overlay-support),
[MPO3 address callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddresswithmultiplaneoverlay3),
[VSync plane information](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_multiplane_overlay_vsync_info2)
and [post-present callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_postmultiplaneoverlaypresent)
define these separate responsibilities. Supporting a visual tree in
[DirectComposition](https://learn.microsoft.com/en-us/windows/win32/directcomp/basic-concepts)
does not mean every visual is assigned a hardware overlay plane. Inspect
the submitted plane list before attributing a missing taskbar to a dropped
plane. This does not establish that all composition and synchronization
contracts are implemented correctly elsewhere in the stack.

## Frozen-stack observation

`stabilize153-native-20261112` reaches native Code 0 at 02:42:09 UTC, LUID
`ba232`, with unchanged archived components. It runs no new pixel probe or
mode change. The first desktop capture starts at 02:42:32; its flushed
geometry record at 02:42:38 describes 720x1280, DPI 96, work area 720x1232,
and a bottom taskbar rectangle `(0,1232)-(720,1280)` owned by Explorer 4884.
The Start child has zero extent and the normal composition content bridge
is absent. A native PNG never completes. These window records are evidence
of incomplete shell content, not a pixel capture or proof of its cause.

The retained native trace contains 35 DWM flip callbacks over 20.593 seconds.
All 35 MPO3 submissions use exactly one layer-0 plane. Source and destination
rectangles are `(0,0)-(720,1280)`. Each target physical address appears in a
subsequent native-adapter VSync report before the next flip. Eight retained
DXGI capability records after native startup report MaxPlanes=1, zero RGB/YUV
overlay planes and zero overlay flags; these capability records have no LUID
field. Thus the retained trace does not show a second taskbar plane being
accepted and then discarded. It also does not prove correct composited pixels.

The UMD trace has 33,624 events, including 178 resource-create completions,
one Explorer Present and no RuntimeError/ZinkFailure. Explorer device creation
appears under PID 4452, then 4884. No matching Application error/hang record
is returned for the collected desktop-process/time filter. The only subsequent
Dxgkrnl device-error reason in this trace is numeric 14, left uninterpreted;
it must not be conflated with `.155`'s reason 7. The `.153` first-recovery
provider has no retained event. Absence is not proof of no failure.

An additional boot occurs at 02:43:15. The automatic controller restores `.59`;
Code 0, cleanup and unchanged power are verified at 02:50:03, with saved
portrait unchanged at 02:50:25. The restored desktop PNG at 02:52:10 is
visually checked and has the complete taskbar/tray, without restarting Explorer.
Its SHA256 is `204ee62e46c85b1fdc114c48e7550a8a305f7139a5138b6ec927a1fc19c2a3ce`.
This is software recovery evidence, not a native desktop pass.

The native export has 270,961 internally consistent records, is unfinalized
and has no reported loss. SHA256:
`7b47382cceb39e713d278c83f53940400f4977c3cd6fba5be06c3902c1d4a8d9`.
The recovery export has 148,056 records and a finalized header (461 buffers):
`4a0f54e99f7c562a90a8b5a5f8b942366fd5278d2d51f31b9d7bee03fb989020`.
1,384 Dwm-Core event-203 payload/template mismatches remain excluded from
interpretation. Original evidence, `native-summary.json` and per-flip
`mpo-audit.json` are retained in the external run directory.

Next: correct demonstrated shell-rendering/compiler defects independently,
retain source-resource synchronization, and obtain a complete native desktop
image. Do not add fake post-present success or advertise extra planes to
make capability checks pass. The `.154` queue fix, `.155` diagnostics and
confirmed compiler fixes remain separately available for selective validation;
they are not automatically merged into this frozen baseline.
