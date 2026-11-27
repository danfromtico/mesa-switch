## Current hardware status

The project overview lists the latest published Switch Lite permanent-boot result, the package manifest and the Switch OLED port status. [Hardware status and baseline hashes](../../README.md#hardware-status)

## Switch Lite graphics baseline recorded on 2026-11-20

Windows starts the GM20B with KMD `fc6e8d08`, ARM64 tgd3d `cd352fa8` and NVK `51ff97f6`. On AC-powered boot `84195`, DWM loaded the selected native modules at Code 0. The protected boot tool issued permanent permission for the seven verified component hashes and preserved the previous complete recovery profile. [Qualification for this profile](dxvk-games-20261118/dxgi-copy-cd352fa8/qualification.json) · [Permanent permission and rollback](dxvk-games-20261118/dxgi-copy-cd352fa8/permanent-boot.json) · [第四步交接](SWITCH_GRAPHICS_HANDOFF.md)

Two historical display KMD repairs were retested with the selected native user-mode components. The ARM64 models distinguish the old and corrected results for a context-free MMIO flip and a rotated primary mode-size query. Windows supplied a context for all 171 instrumented no-DMA flips. All 171 primary queries had identity storage and `CheckDisplayMode=0`. Both the candidate and the permanent baseline returned `E_FAIL` on entry to the same exclusive fullscreen window. Their ordinary and 90-degree borderless windows displayed the expected colors and exited normally. The two repairs remain candidates for their target paths. The selected seven-component profile was restored on boot `84936`, after eight requested boots and one additional unexplained restart. Windows started boot `84914` following the last planned restore, then started `84936` approximately 47 seconds later without another shutdown request. Events 41 and 6008 are registered separately in the open-fault ledger. [KMD display trials and restoration](KMD_DISPLAY_CONTRACTS_20261120.json)

The accepted component hashes and open faults are tracked in the repository. Native legacy `SEQUENTIAL` restarted on three separate AC-powered boots. Keep that route out of daily use. For applications with a verified flip path, the legacy `FLIP_SEQUENTIAL` control displayed six frames and matched all 15 desktop samples. An application with no qualified alternative remains a diagnostic workload. The process-scoped WDDM 2.0 candidate also has an open first-Present device-removal fault. [Accepted graphics baseline](graphics-baseline.json) · [Open faults and daily workarounds](open-faults.json) · [Physical test records](LOAD_STABILITY_FOLLOWUP_20261116.json)

The DXGI 1.3 and 1.4 Present1 tables forward optional dirty regions to the runtime through its original context. Both callback tests passed on the Switch in ARM64 and ARM64EC. [Callback qualification](dxvk-games-20261118/present1-dirty-hints-baseline.json)

On boot `82805`, the installed permission tool restored the accepted package after verifying all seven components. DWM loaded the four native user-mode modules. A negative acceptance test for the open presentation faults preserved the existing permanent grant. The desktop stayed at Code 0 on external power throughout installation, restoration and retirement of the previous tool. [Policy and hardware receipts](dxvk-games-20261118/baseline-promotion-gate.json)

On a one-time WDDM 2.0 trial, ARM64 tgd3d `9fb6cef7` accepted the observed Present1 input and passed the original context to the Windows callback. The empty flip window exited normally, and the single-clear window matched all five desktop samples. The six-frame scene returned success on its first two presents and then rebooted. Its trace ended 8.643 ms before the last application call. The accepted package returned to Code 0 with permanent permission on boot `83151`. [Native-window observations and restoration](dxvk-games-20261118/wddm20-present1-native-window.json)

The known-good legacy flip window negotiated DDI `0x000b000f` and DXGI 1.2 on boot `83327`. Its write-through journal recorded entry into the tgd3d Present function, invocation and successful return of the Windows callback, and return from tgd3d on the same drawing thread. The same bytes were read from the Switch after restoring the accepted desktop on boot `83414`. The three earlier legacy SEQUENTIAL interruptions still have no driver record covering their first public call. [Legacy callback qualification and original trace limits](LOAD_STABILITY_FOLLOWUP_20261116.json)

On one-use boot `83590`, the same signed public client presented three single frames using the legacy GM20B flip path. Windows linked each presentation packet to a graphics context and device owned by the drawing process. DWM completed the matching presentation-history token each time. The two clear windows matched all five desktop pixel samples, including the run with the ordinary UMD entry. All observed miniport Present callbacks ran on DWM threads. The accepted seven-component profile returned to Code 0 with permanent permission on boot `83765`. The original legacy sequential restart remains open. [Hardware observations and restoration](LOAD_STABILITY_FOLLOWUP_20261116.json)

The previous ARM64 driver changed all 88 encoded subrectangle pixels and all 748 scaled interior pixels. The complete `e6852733` build passed the original inputs three times. The unscaled copy was exact, the maximum filtered channel error was one, all seven copy cases passed and both displayed color cases matched five desktop samples each. A separate hardware-only FL11_1 draw matched all 589 pixels and a public flip window matched all five samples. The sRGB copy fault is closed. [Physical qualification](dxvk-games-20261118/dxgi-copy-cd352fa8/qualification.json) · [Before-and-after results](dxvk-games-20261118/dxgi-copy-cd352fa8/causal-comparison.json)

The source branch also contains the scoped native WDDM 2.0 Present1 input correction from the `9fb6cef7` trial and the EMC provider/vote repair `454ea62d`. Their combined images have separate hardware acceptance pending. [Merged source and model results](dxvk-games-20261118/dxgi-copy-cd352fa8/merged-source-checks.json)

The three original sequential restarts followed an empty first Present with an UNORM application back buffer. The internal redirection copy at that boundary remains unobserved. The accepted seven-component profile returned to Code 0 with permanent permission on boot `84029`. On that boot both sequential and flip-sequential windows returned `S_OK` to `DXGI_PRESENT_TEST` after leaving the minimized state. Both presentation counters stayed at zero. [Original incidents, status-only tests and restoration](LOAD_STABILITY_FOLLOWUP_20261116.json)

On this kernel the preceding ARM64 UMD `c954304f` obtained a hardware device and context with FL11_1 as its only requested level and matched all 589 triangle readback pixels. The first-Present restart remains under investigation. [Kernel drawing qualification](WINDOW_COPY_BASELINE_20261120.json) · [Presentation observations](LOAD_STABILITY_FOLLOWUP_20261116.json)

## Previous Switch graphics baseline · 2026-11-19

The selected ARM64 combination is KMD `6c86354e`, tgd3d `49fa08a2` and NVK `e563a81d`. The GM20B texture-copy probe verified 28 copied pixels and 63 guard pixels. Native GPU activation is manual pending physical FL11_1 qualification. [Component hashes and hardware readback](dxvk-games-20261118/native-code0-copy-flags.json)

## Previous native 1080p H.264 acceptance

The native D3D11VA decoder returned six 1920×1080 pictures from a 1920×1088 NV12 pool. Every frame matched the software reference. All six Presents completed in a 640×360 desktop window. The final GPU image differed from the reference by at most one value per channel. The paced run lasted 16.093 seconds, including an eight-second final hold. The window closed normally. [1080p evidence and signed component identities](NVDEC_1080_D3D11VA_20261117.json)

The KMD, UMD and ICD used for this video run were `9ef86d7e`, `c8a8eb9a` and `e563a81d`. Fixed 90-degree row scan and the Handheld power configuration were verified after playback. The accepted 720p package remains available for recovery. 2K and 4K native validation and unpaced load stability remain deferred. [Resolution roadmap](NVDEC_INTEGRATION_20261116.json)

### Previous 720p functional acceptance

**The native 720p H.264 functional test passed** on the de8bb90b KMD, c7ef263c UMD and e563a81d ICD. All six NV12 frames matched the independent reference. The GPU-converted pattern appeared on the panel in a 640×360 window. Six Presents, the GPU event, DwmFlush, decoder close and window release completed in the paced run. The client measured 15.109 seconds including an eight-second final hold. [Components, per-frame evidence and physical observation](NVDEC_720_COMPOSITOR_OPEN_20261117.json)

DWM opened both buffers on interface `0xA0009` with a 2560-byte pitch. Two runs without artificial frame holds ended in unexpected restarts. The last persisted record in the light replay was entry into the fifth Present. Continuous playback stability remains open. [Deferred restart observations](LOAD_STABILITY_FOLLOWUP_20261116.json)

### Preceding runtime-window package

On the preceding de8bb90b KMD, 864ff770 UMD and e563a81d ICD, the native 720p H.264 client decoded six frames with exact NV12 output. The first and last 640×360 GPU-converted frames matched the reference within one level per RGB channel. Two runs completed six redirected window flips, the GPU event, DwmFlush, decoder close and normal device destruction. The observed replay showed a black window followed by a full-panel blackout before returning to the desktop. The 720p video window failed physical acceptance. A separate compositor trace captured six DWM OpenResource failures on interface 0xA0009 before residency. That diagnostic boot ended unexpectedly during the final hold. [Component identities, timing, traces and panel observation](NVDEC_720_RUNTIME_WINDOW_20261117.json)

## Earlier five-patch observations

On the selected de8bb90b KMD, 45683c21 UMD and e563a81d ICD, native D3D11VA/NVDEC decoded six 720p H.264 frames with byte-exact NV12 output and retired the decode session. The decode loop took 860 ms. [Six-frame record](NVDEC_720_WINDOW_BOUNDARY_20261117.json)

Two interactive previews produced the same 640×360 first frame through the native D3D11 pixel shader. All 230,400 pixels were within one 8-bit level per RGB channel of an independent reference. The latest run created the swapchain with the factory of the actual decoder device. It entered its first windowed Present and the system restarted without a recorded return. The five-patch package remains selected. Decoded-window composition, physical display and the normal closure of this window path are pending. [GPU readback](NVDEC_720_FIRST_SUBMIT_20261117.json) · [DXGI factory correction and incident](NVDEC_720_DEVICE_FACTORY_20261117.json)

The two-buffer composed-flip client exited on the original boot. Its first 640×360 resource received an allocation, then failed the displayable-resource ownership check before residency. CreateSwapChainForHwnd returned 0x887A0005 before decode or Present. A subsequent unexpected boot occurred after the evidence transfer. The installed five-patch package and power policy were rechecked. [Completed driver trace and runtime boundary](NVDEC_720_COMPOSED_FLIP_20261117.json)

Earlier observations follow.

The active desktop baseline contains all five presentation and ownership patches. After one planned boot, DWM loaded the signed 45683c21 UMD and e563a81d ICD on the de8bb90b KMD. Keep this package selected during subsequent observations. The preceding window and decode results below retain their original component identities. [Active baseline, exact hashes and five patches](TGD3D_PRESENT_BASELINE_20261117.json)

An unexpected restart interrupted the subsequent ordinary-window trial after task dispatch. Window readiness and exit were unobserved. The partial trace yielded zero tgd3d events, and no matching dump was found. DWM loaded the same pair on the next boot, and all eleven installed files matched. The decoded preview was not launched. [Incident and retained baseline](TGD3D_PRESENT_BASELINE_20261117.json)

The presentation and resource ownership fixes are integrated on the controlled video branch. The ARM64 tgd3d and NVK synchronization contract suites passed on the Switch. A 320×240 Win32 cube selected the signed candidate NVK by driver UUID and adapter LUID and completed 120 successful Presents in an 8.19-second process run. DWM retained its installed 3a8ed9ff UMD and 95b97df4 ICD throughout. The candidate UMD still needs ordinary DWM validation. [Integrated commits, build identities and window results](TGD3D_PRESENT_OWNERSHIP_20261117.json)

On November 17, 2026 the Switch decoded six 1280×720 H.264 frames through the Windows D3D11VA runtime and native NVDEC. The synchronous libavcodec client used the codec-provided 17-surface pool. All six Host1x targets completed, the session closed, and all 8,294,400 NV12 bytes matched an independent software decode. The observed run lasted 4.374 seconds including fixture setup and cleanup. [D3D11VA acceptance and component identities](NVDEC_D3D11VA_720_20261117.json)

Media Foundation Source Reader initialized the decoder and its buffers, then failed in its first EndFrame. A separate GPU color conversion and window trial ended in an unexpected system restart. The partial trace includes two native decode completions and preview resource creation. The CSC output, first Present and physical window remain unverified. [Native runs and recovery](NVDEC_D3D11VA_720_20261117.json)

The recovered desktop uses the controlled 3a8ed9ff UMD, de8 KMD and 95b ICD. Interactive desktop settings show 1280×720 at 90°, with a 720×1280 panel signal. The Handheld policy and 85% charge limit are active. Video remains process opt-in. The completed fixed-desktop and vkcube records still apply to their original combination. Higher resolutions await joint acceptance. [Integration version table](NVDEC_INTEGRATION_20261116.json)

Entries below are historical.

**Owner priority update:** repair the current pre-logon reboot first; then complete shader caching and measured ~60 Hz VBlank/presentation pacing before hardware rotation. Idle desktops should submit on demand. The owner explicitly requires native startup to stay enabled and authorizes deploying the next reviewed fix directly for their tests. The SD is currently mounted for offline diagnosis.

**Shader cache implementation (not deployed):** tgd3d now saves/reloads Vulkan
Shader Object binaries using the branch's existing Windows blob-cache backend.
SPIR-V/state/layout/driver identities select entries; each hit creates a new
device-local shader. Incompatible binaries fall back to compilation, while
creation OOM/device loss stays an error. CPU controls cover a fresh process,
different devices and keys, failure paths, plus existing backend integrity and
concurrent-writer tests. Native cache hits, compilation-time improvement and
~60 Hz display acceptance remain outstanding. The separately frozen
`9de42c82` standard-surface candidate is still the next startup test; its files
are not replaced by this cache build.

**Latest installation, November 13, 17:26 UTC:** the selected test baseline is
`.158 + bit14 + persistent native startup`, `fcc53bf1` CPU-sync UMD and
`d3423d26` ICD. The actual installed kernel is now `oem94.inf`, SHA256
`c629328e…`; registration and package hashes pass. KMD patch `b916f531`
adds explicit persistent `AllowNativeBoot` authorization. No startup/logon
GPU-switching task or one-use permit is needed. One Windows restart was
requested at 17:27:36 UTC to verify this path. The owner reported another
automatic restart before the username appeared. This startup attempt failed;
there is no post-restart Code 0 or DWM loaded-module proof. Offline recovery of
a copied SYSTEM hive finds successful persisted initialization-stage statuses,
which do not establish desktop success. Native startup authorization remains
enabled at the owner's request. Do not call this stable or claim that the
earlier Code 43 desktop used tgd3d.

**Next UMD candidate:** accept the frozen KMD's standard primary/shadow
allocation descriptor when resource-level private data is empty. Preserve the
consumer's allocation handle, backing and primary identity. The same CPU
contract fails on the old implementation and passes on the fix; ARM64 and x64
cross-builds pass. This is an interface correction, not a demonstrated cause
of the pre-logon restart. KMD, ICD and native boot settings stay unchanged.

**Superseded startup attempts:** the bit14-only kernel `2e4ae808…` installed
successfully, but its post-logon activation restarted Windows at 16:56:40.
The owner's subsequent 17:09 boot still returned Code 43: the one-use permit
returned `STATUS_ACCESS_DENIED` before that StartDevice reached hardware.
The attempted persistent-grant/permit-rearm task was therefore inadequate
and has been removed. The replacement KMD's repeated-start control fails
with the old source (exit 99) and passes with the new source (exit 0), without
GPU execution. A separate installer identity failure returned success while
retaining `oem93.inf`; a distinct INF identity corrected it, and installed SYS
hash verification confirms the replacement. CPU1497600 kHz, fan13 and sleep
settings remain unchanged. See [the trial record](TGD3D_NATIVE_20261113.md).

**Earlier handoff, November 13, 16:10 UTC:** SSH and the owner's physical
confirmation show an operable desktop after a real Windows restart. Frozen
`.158` remains installed with the native gate closed (Code 43); automatic
enablement is disabled and the one-shot update task is removed. CPU1497600
kHz, fan13 and display/sleep settings are unchanged. The candidate package
hashes pass, but four newly copied recovery files were corrupted by the
interruption. They have been rewritten with WriteThrough/Flush(true) from
the independently verified prior snapshot; Check-Recovery now passes.
No second activation, Windows reboot command or `.59` installation was issued.

**November 13, meta-state candidate failed during activation/observation:**
`d3423d26` restores the common runtime's pipeline-binding mask around NVK
meta operations. ARM64 CPU controls reproduce VS loss with the old helpers
(exit 1) and retain the VS with the fix (exit 0). Only the ICD changes in the
new package; KMD, UMD and helpers are byte-identical to the prior round.
After one requested PnP device restart, Windows automatically restarted.
The new ICD's loaded-module proof was not obtained. This attempt is failed;
the returned state is recorded above. Do not repeat activation or
claim a Home-page regression pass. The separate bit-14 KMD patch remains
undeployed. No Windows reboot or `.59` rollback was requested by this chat.

**November 13, 15:44 UTC follow-up failure:** after the successful bounded
interaction stages, the owner reported a frozen folder window, a desktop
refresh that closed it, and another freeze on opening a folder again.
Explorer restarted at 15:42:37 UTC while DWM PID504 remained alive. SSH works;
memory has 1852 MiB available and 40% commit usage. Explorer private bytes
remain about 93.1 MiB across the short sample. This does not support immediate
memory exhaustion, and does not rule out a slower leak. The overall candidate
is not accepted as stable; stop expanding to pixels/rotation/vkcube while
investigating the application exception. The five-minute result below remains
valid only for its recorded interval.

**November 13, 15:35 UTC desktop result:** the `fcc53bf1` CPU-sync UMD
has passed the physical 30-second static, 60-second window and five-minute
interaction stages on the unchanged frozen `.158` KMD / `7c677212` ICD.
The owner confirmed the moving/resizing/occluding/minimizing/restoring window,
Start menu, quick settings and CPU/fan program were all visible and operable.
They subsequently reported several manual weather/browser attempts were normal.
This passed the bounded desktop stage, not sustained stability or proof of the
previous freeze's root cause. The bidirectional GPU bridge remains disabled in this diagnostic
binary and has not passed; the repository default remains enabled.
Known-pixel, hardware rotation and vkcube acceptance remain outstanding.

**Earlier separate source candidate (subsequently installed at 16:55 UTC):** the GM20B golden context now applies NVK's
shader-object output-exception policy by clearing only `0x419e44` bit 14.
The real-NETB model fails before the fix and passes after it; the changed
ARM64 kernel translation unit compiles. This KMD change was not deployed during
the five-minute desktop result above. Its later installation does not establish stability. See
[the context-policy review](GR_HELPER_LOADS.md#november-13-shader-object-output-exception-policy).

**November 13, 15:08 UTC DDI fix:** device-loss notifications now use
`D3DDDIERR_DEVICEREMOVED` at SetErrorCb instead of DXGI API error codes.
Shader creation preserves Vulkan host/device OOM and device-loss status; a lost
device cannot compile another variant or reuse a cached one. The new callback
assertion fails with the old source and passes with the fix. ARM64 CPU controls
and ARM64/x64 cross-builds pass. This corrects failure propagation, not the cause
of the weather/page-close freeze. The running `.158` CPU-sync UMD is unchanged;
the unbounded teardown wait is still unresolved.

**November 13 shader audit:** the owner also reports freezing when closing a
web page, with large-area changes appearing easier to trigger. Shader execution
and shared-surface retirement remain hypotheses, not established causes.
The four CPU fixtures (VS, PS, sampled PS with CB13, and a loop/indexing/clip PS)
pass SPIR-V validation and production NVK/NAK SM53 compilation. No GPU execution
or new desktop acceptance is implied. The compiler control opens no adapter or
KMT device; the running `.158`/CPU-sync package has not been replaced.

**November 13, 12:54 UTC source fix:** failed shader creation now destroys its
token/signature storage before reporting the error; the runtime never calls
DestroyShader for a failed handle. A production-DDI failure injection fails
against the previous shader source and passes with the fix, observing actual
deallocations before SetErrorCb. ARM64 and x64 cross-builds pass; the ARM64 CPU
controls pass. This is not a GPU/NAK execution or weather-flyout stability result.
The fix has not replaced the owner's running UMD.

**Current device handoff (November 13, 12:51 UTC):** the installed default-start
package's restore entry had removed its startup task and returned `.158` to
Code 43. Re-registering the task and starting it once on the existing Windows
boot succeeded: Code 0, hardware/panel 0, task exit 0, and DWM PID 484 plus
Explorer and shell processes load TegraGpuD3DVk.dll. The logon trigger is
registered again. CPU-sync UMD remains `d06e29e2`; the new shader fix is not
deployed. No new physical stability acceptance or Windows reboot is claimed.

**Latest owner report:** opening the large weather flyout can freeze/restart
the machine before it appears or while only part is visible. This follows
the requested CPU-sync activation; the post-activation DWM module query did
not complete, so retain that observation separately from loaded-module proof.
CPU-sync mode is not accepted as stable. The requested package enables tgd3d
after logon, still with the bridge disabled. Self-review must focus on this chat's tgd3d
changes; no further unchanged-build comparisons or new dump searches.

**Earlier device handoff (November 13, 12:14 UTC):** the owner requested
manual testing with the unfixed bidirectional bridge disabled. Frozen `.158`
is active (Code 0, hardware/panel status 0) with CPU-sync UMD `d06e29e2`
(`b3ba7de9…`). Automatic trial/recovery tasks and trace sessions were stopped;
Windows was not rebooted. Leave the device to the owner until they hand it
back. Source/audit work may continue. The bridge's implementation has not
been fixed; the repository default remains GPU bridging, and this explicit
CPU-control build is the current manual hardware configuration.

**November 13, tgd3d-vulkan update:** native tgd3d has produced a usable portrait
desktop, but stability has failed. The latest `94cc811a` trial passed its short
physical static observation and then unexpectedly restarted before the colored
window appeared. `.59 / Code0` and the original display configuration were
verified at 10:03:56 UTC and the owner confirmed usable output. The owner now
requests source comparison and a single-variable synchronous submission control,
not further dump/log investigation. Keep CPU **1497600 kHz**, fan
driver **13**, the current sleep settings and the dynamic **512 MiB** GPU budget.
The older CPU/fan values and parallel-chat workflow below are historical, not
current instructions. This branch owns native tests and recovery; `main` is paused.
**Latest recovery policy:** retain the active frozen `.158` after testing.
Do not schedule automatic recovery or reboot after a stage or its failure;
manual recovery remains available and does not reinstall `.59`.
The CPU-sync diagnostic completed one short window trial (eight moves and
eight resizes; the owner confirmed the window appeared). This is a comparison,
not a root-cause fix. The GPU fence bridge remains enabled by default.
See [the full tgd3d trial record](TGD3D_NATIVE_20261113.md) for frozen hashes,
the separate `.59` collection-process OOM incident, and current diagnostic work.
Those earlier rounds obtained no five-minute desktop acceptance. The later
`fcc53bf1` result is recorded above; pixel, rotation and vkcube remain unaccepted.

## Historical status before the November 13 tgd3d trials

**Latest hardware results** on Nintendo Switch Lite HDH-001, Windows 11 ARM64
23H2 (22631.2861).

**Current priority:** complete the desktop presentation paths and validate
hardware **1280x720 landscape**. Display-controller rotation, VidPN, allocation
geometry and legacy/MPO integration exist in source and pass the documented
models/ARM64 controls; rotated physical output remains unvalidated. Portrait
desktop or cube results cannot satisfy this requirement. The frozen `.158`
components remain a short-observation baseline, not a stable desktop release.
The latest native trial has ended; the device is restored to `.59`.
November 12 inventory confirms GPU `0.1.0.59`, Wi-Fi `0.5.6.0` and fan
`1.0.0.12`. At the owner's request the CPU returned to 1996.8 MHz, verified by
the fan driver's clock readback; thermal and screen-off policy remain enabled.
Windows CPU/API controls run on that recovery configuration before another
native rendering trial. No `.160` GPU package has been installed.

**Desktop direction updated November 12:** the user authorizes replacing the
Zink desktop route with a native Vulkan hardware backend and asks Claude to
choose a concrete implementation. Do not use the current Gallium `d3d10umd`
route or software rendering for the replacement. DXVK is a reuse candidate,
not a substitute for the required Windows UMD/DDI, shared-resource and DWM
presentation contracts. Direct Flip remains deferred. Preserve the `.158`
baseline while developing and validating the replacement separately.

The existing Zink desktop baseline is also being repaired in parallel, as
requested: shared-image correctness, synchronization and persistent shader
caching. This is distinct from the replacement architecture. No new native
desktop acceptance is claimed by the source and cache controls below.

The old remote Claude task has ended at the user's request. Six completed
commits through `9051f49e` are retained in `fable-vps/dxvk-games`. The imported
sRGB-view fix is adapted into `main` as `5538ace5`; the rest of that branch is
not merged. Its final collection contains 965 files /
381,337,146 bytes, all checked against remote hashes and lengths. The sixteen
unfinished indirect-submission files are preserved separately as a patch and
source snapshot. The additional completed work implements `BltDXGI` and
imported-image sRGB views; its controls do not establish native desktop or
DXVK acceptance. The new Claude architecture review completed, and the user
then explicitly delegated implementation as well. That same Fable 5.2/max
session began a separate native DDI/Vulkan backend in an isolated checkout
based on main `8fcf9e07`. It stopped on account quota at 09:37:04 UTC;
the reported reset is 13:30 UTC on November 12. No associated remote process
remains. All 30 collected files / 518,466 bytes match their recorded hashes.
The partial backend has build errors and no new commits; it is not integrated
or accepted on hardware. Local integration and hardware tests remain the
main agent's responsibility.

**Zink texture/cache work:** runtime-owned BGRA/RGBA imports now declare their
UNORM/sRGB views at image creation and retain the original allocation when a
view is added. The format-rule host control passes; this has not yet passed
native shared-image rendering. The UMD and ICD must be updated together.

Mesa's Windows blob-cache backend is implemented and enabled by the UMD/ICD
build tools. It uses asynchronous writes, atomic file replacement, a shared
key index and coordinated eviction; Zink includes its own DLL identity in the
cache namespace. The ARM64 cache control exits 0 on `.59`/Code 0 at
09:21:04 UTC: a new process reads a previous process's entry, two simultaneous
writers produce 64 verified entries, and namespace separation, corruption,
eviction and disable-option checks pass. Executable SHA256:
`de5824ed13ac67bcd488c3785a2799947cbb75f2d79fb943fff5f7698cb93fbb`.
This is Windows CPU/filesystem evidence, not GPU execution or a measured
shader-compilation speedup. The record is retained in
`build/zink-sync-texture-cache-20261112/cache-control-concurrent.json`.
Cross-queue synchronization and native visual/performance acceptance remain
in progress. [Windows cache configuration](PORTING.md#windows-shader-cache)

**Zink synchronization source repair:** Win32 fence import is now gated by
the semaphore extension, creates the correct binary or timeline semaphore,
and releases the prior fence reference. Repeated timeline waits retain the
largest value in each batch, including when a previous asynchronous batch
still owns the deferred-context marker. A signal keeps its semaphore alive
until batch completion instead of relying on the caller's lifetime.
The ARM64 UMD and `test-zink-win32-sync.exe` compile and link successfully.
The fixture invokes actual Zink entry points with Vulkan dispatch substitutes;
it is not a GPU test. Its SHA256 is
`7eb6289113528ffd93038ab4942291f69a7f913f3f64d040a58bac26850ca03f`.
After Wi-Fi recovery, this Zink control exits 0 on the device. Its intentional
`vkCreateSemaphore` out-of-host-memory injection writes an expected Mesa error
to stderr; the process still reaches its final PASS and returns zero. This is
not an actual device-memory exhaustion or a GPU rendering test.

**NVK Win32 timeline sharing implemented:** the SDK bridge now creates and
opens NT-shared monitored fences, including named/security-qualified exports.
Failed imports preserve the old payload, caller handles retain their ownership,
and mapped values use acquire reads on ARM64. The provider exposes timeline
OPAQUE_WIN32 and D3D12_FENCE import/export; unsupported binary sharing returns
zero capability bits. The semaphore runtime accepts export parameters while
the object is shareable, before the first handle export.

On the physical Windows system, SDK/KMT and actual Vulkan/NVK controls both
exit 0. They exercise Win32 handle lifetime, import failure preservation,
bidirectional payload visibility, and the production queue wait/signal
callbacks using KMT substitutes. No GPU command is executed. The final NVK
control SHA256 is
`aaee9386065887fe602f7a5737347c2b60dc76049f8be5b5603fa38c63927ba0`;
the SDK control is
`f57975f9e9d1c7813b2c01976364d043bcbc5caffc1bb03f055f42ff2bd4f255`.
The Win32 image-flags and pacing controls also exit 0; sixty paced calls take
1.003993 seconds, with thread CPU below the timer's measurement resolution.
Results are retained in `build/win32-sync-controls-20261112/`.
The updated ICD links, but is not installed. The runtime/NVK per-submission
queue bridge remains to be connected: `RuntimeDeviceFinish` still performs
its CPU fence wait. These controls do not establish desktop correctness,
native shared-fence behavior, or a measured rendering speedup.

**Five-minute observation completed; visual acceptance failed:** the frozen
`.158` set remains Code 0 during a 311.057-second observation, but the user's
physical photographs show severe white polygons/corruption in Quick Settings.
This is not a stable desktop pass. All 196 returned files / 112,332,491 bytes
match device hashes and lengths. Recovery to `.59` completes normally with
unchanged sleep/display policy. Hardware landscape and vkcube remain pending.
[Five-minute evidence](NATIVE_DESKTOP_158.md#five-minute-observation-and-physical-corruption)

**Fan policy deployed separately:** SwitchFan `1.0.0.12` uses the hotter of
SoC and board sensors: 35/38/40/43/45 C command approximately
32/50/75/90/100% PWM (76/118/177/212/236). At or below 32 C it stops;
33–34 C retains the prior duty capped at 76. The existing two-second polling,
CPU policy and sleep behavior are unchanged. ASan/UBSan curve-boundary,
hysteresis and PWM controls pass, as do WDK ARM64 build and package signatures.
Deployment at 08:04:09 UTC records Code 0, version 12, 37 C SoC and duty 76,
with unchanged display/sleep settings. At 08:20:19, the live poll count has
advanced to 484, sensor failures remain zero, and 34/33 C holds duty 76.
These are commanded PWM values, not measured fan RPM or a physical 45 C test.
The signed SYS SHA256 is
`6a1c51a93ef0521482cff45bdb74442dd4aa22711f525adbede603dfc8e896ca`;
nine returned deployment records match their device hashes. Source and INF
are identical in the main integration and companion WindowsDrivers tree.

**Win32 pacing candidate, not a desktop acceptance result:** the NVK CPU/GDI
presentation path now uses a private blocking timer capped at 60 Hz and joins
the compositor through `DwmFlush`. It does not wake or render an idle desktop.
The full ARM64 ICD links after propagating the Win32 WSI library dependency;
SHA256 `db77e2d4e8e27671eefcdcbed0e7b53720ea0ebe14af8641c7a20bccb1e74f34`.
A standalone Windows ARM64 timer control runs on `.59` with exit 0, covering
the cap, blocking wait, late-frame behavior and timer failure; sixty calls
take 1.004768 seconds and thread CPU time is below the measurement resolution.
Its executable SHA256 is
`b5d271af8f38a967df4344229d142c81f92bfc7c7afd32b96a8962b5f07ba16c`.
The candidate ICD is not installed and this control executes no GPU work.
It neither replaces DWM's native resource/fence contract nor proves that
physical corruption or tearing is fixed. Direct Flip remains deferred.

**Latest `.158` cube attempt is incomplete:** `native-vkcube158-20261112`
reaches native Code 0 at 07:05:29 UTC on November 12 with the frozen KMD/UMD/ICD.
No cube launch/result, native PNG or budget-probe result is retained. The user
reports two automatic restarts. This trial requests no rotation and proves
neither visible cube presentation nor hardware landscape. Recovery checks pass
at 07:14:04 UTC: `.59` Code 0, original ACPI, temporary-registration/task cleanup
and unchanged power. Saved portrait is unchanged at 07:14:19; the inspected
07:15:22 recovery PNG has the complete taskbar/tray. All 137 returned files /
113,708,587 bytes match device hashes. No restart-cause investigation is performed.
[Trial scope and evidence](NATIVE_DESKTOP_158.md#later-attempts)

**Current stabilization build:** `.158` combines the isolated sampler-view
fix, restored SAMPLE_I lowering (`71d67992`), execution-ordered inherited-state
validation (`6c854e9f`), and the GR helper-load correction with readback below.
Host regressions, actual NETB models, WDK ARM64 build and the Windows context
fixture pass. Its Mesa `src/` is identical to `ee7b0d50`, retaining UMD
`77dea046dde0`; ICD `6e2fad99d742` is unchanged. The signed SYS is
`46d1757674ff`. It excludes `.155–.156` diagnostics, the remote SM exception
policy and unfinished remote worker edits. Build: `stabilize158-20261112`.

**`.158` short native desktop observation passes:** four hash-verified PNGs
at 0/15/45/90 seconds show the complete taskbar/tray, with one Explorer PID
and a normal composition content bridge. The user also reports normal,
comparatively stable physical output. DWM selects Tegra LUID `bc22f` at FL10_0;
79 successful flips span 100.544 seconds, all with matching subsequent native
VSync addresses. The ETL is finalized with zero reported loss and no retained
native-era device error, UMD failure or KMD recovery. All 184 returned files
match device hashes. Scheduled `.59` Code 0 recovery/cleanup/unchanged power
passes at 03:48:56 UTC, portrait at 03:49:33, and the recovery screenshot has a
complete taskbar. No trial remains active. This is not a long-duration or
interaction pass. [Exact components, evidence and limits](NATIVE_DESKTOP_158.md)

**Frozen `.153` observation finished, desktop still incomplete:**
`stabilize153-native-20261112` reaches Code 0 at 02:42:09 UTC on November 12,
LUID `ba232`. All 35 retained MPO3 flips contain one full-screen 720x1280
plane, with each address confirmed by a subsequent native VSync. Native shell
geometry at 02:42:38 places the taskbar at the bottom but shows a zero-size
Start child and no normal composition content bridge. No native PNG completes;
an extra boot occurs at 02:43:15. The trace is unfinalized, with no retained
RuntimeError/ZinkFailure or KMD recovery event. It is not a stable desktop pass.
All 159 returned files (137,379,552 bytes, including later shell-event collection)
match device hashes. `.59` Code 0/cleanup/unchanged power are verified at
02:50:03, saved portrait at 02:50:25. The recovery PNG at 02:52:10 has a complete
taskbar/tray. No trial remains active. [MPO contract and evidence](MPO_CONTRACT_AUDIT.md)

**First isolated stabilization correction:** branch commit `b0d377f6` ports
only the sampler-view operand fix onto `931383e6`, without SAMPLE_I lowering
or later KMD changes. New UMD `9cdf03748210` builds; its exact DLL/shared/primary/
Present controls pass at 02:58:42 UTC. All six assertion-enabled `TgsiSample`
tests pass at 03:00:01, with `.59` Code 0 unchanged. Native validation is pending;
the known missing SAMPLE_I path is not fixed by this one-line correction.

**GR policy correction:** Fable's first independent source commit `52e35569`
is fetched over SSH and selectively ported. The actual NETB writes `0x10beb`
to `0x419f78`, disabling helper-invocation memory loads; Windows also skips
NVK's update of that bit. The KMD now clears only bit 3 before golden save,
with independent write-readback verification. Host regressions, the real NETB
stream and WDK ARM64 build pass. The `.153`-based `.157` package is signed and
verified at 03:12:34 UTC but not installed. Native stability and taskbar
correctness remain pending. [Source, validation and scope](GR_HELPER_LOADS.md)

**`.156` retained candidate, not installed:** retain the stalled application's owned
command snapshot and read-only PBDMA/USERD fetch state after joining the worker,
before destructive reset. Host checks, decoder checks and the WDK build pass;
Windows ARM64 context/reset models exit 0 at 02:13:09/02:13:18 UTC on November
12, with `.59` Code 0 unchanged. Native collection remains pending; this
candidate is not the selected stabilization package.
[Packet capture contract](RECOVERY_TRACE.md#reset-packet-capture-in-156)

**Latest `.155` result: failed native desktop, first stalled work retained.**
`execution155-native-20261112` uses UMD `77dea046dde0`/ICD `6e2fad99d742`.
On native LUID `ba67c`, reset entry captures a 64,864-byte render packet at
fence **749**, last completed **748**, waiting for GPU marker **80** while
the observed marker remains **0** after 140 polls. The DWM device-error mark
occurs about 2.085 seconds after submission; generic OOM follows 281 ms later.
The command words/fetch position are not retained, so the stalled instruction
and underlying cause remain unknown. Native Code 0 is recorded at 01:46:05 UTC
on November 12, after the first error; the user reports black output/reboot,
and an extra boot occurs at 01:46:50. No native PNG or Load result is retained.
[Execution-state evidence and limits](RECOVERY_TRACE.md#native-155-result)

**Recovery complete:** all 159 files (136,536,269 bytes) match device hashes.
`.59` Code 0, cleanup and unchanged power are verified at 01:52:36 UTC;
saved portrait is unchanged at 01:52:55. The actual restored desktop PNG at
01:58:42 has the complete bottom taskbar and right-side tray. No Explorer
restart is needed this time. No native test remains active.

**Current correction:** Carbon High review identifies a missing
`TGSI_FILE_SAMPLER_VIEW` resource-operand case. Independent Windows ARM64
debug builds reproduce `bad src file` on the old ordinary `SAMPLE` test;
the one-line correction passes all twelve translator tests at 00:53:40 UTC
on November 12. Release UMD `77dea046dde0` builds and its exact DLL/callback
controls pass at 00:55:26. The native retry below still fails. Confirmed undefined
behavior is corrected; the desktop failure's cause remains unresolved.
[Correction and limits](D3D_RUNTIME_SHARING.md#sampler-view-operand-correction)

**Previous native run:** `sampler-view154-native-20261112` uses unchanged
signed `.154` KMD/ICD and corrected UMD `77dea046dde0`. Native LUID `bcf28`
reaches Code 0 at 01:02:54, followed by user-confirmed black output and an extra
boot at 01:03:41. No Load result or native PNG is retained. The interrupted
trace has ten DWM flips, a system-preemption packet 2.091 seconds before the
first DWM device-error mark, then generic OOM 291 ms later. All 158 files match
device hashes. `.59` Code 0, cleanup and unchanged power are verified at
01:09:49; saved portrait at 01:10:25. No GPU test remains active.
[Native result and limits](D3D_RUNTIME_SHARING.md#sampler-view-native-retest)

**Recovered shell repaired:** the user then reports the taskbar missing on
the `.59` software desktop. The before PNG confirms it; Explorer and DWM load
WARP. Restarting only Explorer restores the full taskbar/tray in the after PNG
at 01:15:31, also confirmed by the user. Resolution/DPI remain 720x1280/96.
This is a software-shell recovery result, not native GPU acceleration or proof
of the earlier physical taskbar's cause.
[Before/after evidence](DESKTOP_CAPTURE.md#recovered-shell-with-missing-taskbar)

**Old UMD comparison:** fixed `.154` KMD/ICD with `e99798fa9668` reaches native
LUID `bb8de` at 00:32:31 UTC, then an extra boot at 00:33:30. It retains 44 DWM
flip callbacks but no completed native screenshot. All 162 files match device
hashes; `.59` Code 0, cleanup and unchanged power are verified at 00:47:55,
saved portrait at 00:48:32. The prepared `8f9a286a0e2b` desktop-only counterpart
is not started after the confirmed source defect is found. This is an incomplete
A/B comparison. The user reports missing right-side content and left-shifted
central icons; scaling and exact photo phase remain unconfirmed.
[Comparison evidence](D3D_RUNTIME_SHARING.md#old-umd-comparison-on-154)

**Capture geometry:** work-area/screen bounds, window DPI and taskbar child
rectangles are now flushed before pixel capture. The `.59` control passes at
00:50:21 UTC, with a complete taskbar/tray PNG and DPI 96. Native validation
of this extension remains pending.

**Desktop evidence:** automated PNG capture is verified on the recovered `.59`
at 23:30:12 UTC. Direct image inspection finds the bottom taskbar, Start button
and right-side tray/clock. Native trials must retain and inspect screenshots;
shell rectangles and successful Presents do not establish visible content.
The user's historical screenshot, identified by them as `.146`, lacks the
taskbar. Wallpaper/icons alone were not a complete desktop pass.
[Capture procedure and evidence limits](DESKTOP_CAPTURE.md)

**`.154` candidate:** inherited MME/CB0/program state is now validated when
queued work reaches execution, rather than against stale retired state during
admission. On `.59`, the old backend rejects the second valid queued MME packet
in the regression; the corrected ARM64 model passes at 23:47:39 UTC. Host
checks and the WDK build pass. The native retry below does not complete;
the model result does not establish the `.153` physical failure's cause.
[Queue correction and Carbon High review](D3D_RUNTIME_SHARING.md#queued-inherited-state-validation)

**`.154` native retry remains incomplete:** native LUID `baf72` reaches Code 0
at 23:59:21 UTC on November 11, followed by an extra boot at 00:00:09 on
November 12. No native Load result, screenshot or observation-start record is
retained. The trace has 12 successful DWM flips and a DWM-associated device
error preceding generic OOM and five later paging-submit recovery events. All 159
returned files match device hashes. `.59` Code 0, cleanup and unchanged power
are verified at 00:06:36 UTC; portrait verification at 00:07:19 is a no-op.
The trial is finished. The queue regression is fixed, but native desktop
composition has not passed.
[Native retry and trace limits](D3D_RUNTIME_SHARING.md#queued-state-native-retest)

**Latest physical desktop result: failed.** In the 120-second `.153` viewing
trial on LUID `b946a`, the user confirms an incomplete/displaced taskbar followed
by its disappearance, with four photos retained locally. Most shell snapshots
still report the taskbar at the bottom, but four distinct Explorer owners
appear. Successful shared-pixel checks below do not establish correct DWM output.
All 169 device files are verified; `.59` Code 0, cleanup and unchanged power
are verified at 22:27:50 UTC, saved portrait at 22:28:33.
[Physical observation and trace limits](D3D_RUNTIME_SHARING.md#physical-desktop-observation-after-the-pixel-pass)

**Old-stack comparison ended at the user's request:**
`desktop-147-compare-native-20261111` uses the archived `.147` KMD
`e491932a9fd3`, UMD `79fa8169fb00` and ICD `f828845c38b7`. The user reports
an automatic reboot before the host receives a native viewing-start record.
All 146 returned files (123,733,692 bytes) match device hashes. `.59` Code 0,
cleanup and unchanged power are verified at 22:45:26 UTC, saved portrait at
22:45:52. No physical `.147` pass is established and no GPU source is reverted.
Continue `.153` investigation with Explorer exception/termination capture
using the unchanged pixel-tested stack.

**Explorer failure identified:** `explorer-exit153-native-20261111` captures
Explorer PID 356 aborting on native LUID `b2d44`. The exact UMD's return RVA
`0x221928`, saved opcode register `0x92` and source identify unsupported
`TGSI_OPCODE_SAMPLE_I` in the TGSI-to-NIR converter. This proves the captured
process exit, not the separate taskbar-position failure. All 185 returned
files match device hashes; `.59` Code 0, cleanup and unchanged power are
verified at 23:03:02 UTC and saved portrait at 23:06:09.

**Current WIP:** samplerless integer texture loads and a D3D
`Texture.Load` pixel probe are implemented. ARM64 builds, twelve translator checks and
24,576 WARP pixel comparisons pass. The first native trial
`sample-i153-native-20261111` reaches Code 0 at 23:15:32 UTC, then the user
reports black output. An additional boot occurs at 23:16:23 before any
Load/desktop-observation result is retained. Native LUID `b02db` records a
DWM-associated device error, then a 128x64 texture's generic OOM 272 ms later,
followed by five paging-submit recovery requests with `c00000a3`. All 352
retained resource reservations succeed; budget exhaustion is not established.
All 156 returned files match device hashes. `.59` Code 0, cleanup and unchanged
power are verified at 23:22:05 UTC, saved portrait at 23:22:30. The trial is
finished without a native Load or physical desktop pass.
The user cancels the proposed `.148` comparison before installation. The old
translator prints `unknown TGSI opcode: SAMPLE_I` in the minimal regression
and is stopped after its post-abort process fails to exit within the bound.
The exact new UMD's callback/loading controls pass at 23:08:54 UTC.
[Captured abort, correction and review limits](D3D_RUNTIME_SHARING.md#explorer-sample-i-abort)
[Native retest and first-device-error boundary](D3D_RUNTIME_SHARING.md#sample-i-native-retest)

**Latest `.153` result:** `shared-usage153-native-20261111` tests UMD
`buffer-release-v2-20261111-umd+e99798fa9668` from `931383e6`, with unchanged
KMD/ICD and probe `5fcfd6671788`. Native LUID `af442` reaches Code 0 at
21:55:00 UTC. Original shared layout and all ordered BGRA/RGBA comparisons
pass **21,758,982 pixel checks with zero mismatches**. Both processes complete
device teardown and exit 0, without timeout or the prior access violation.

This validates the combined image-usage and buffer-ownership corrections for
these native cases. DWM has one successful device creation, 113 successful
resource creations and 78 successful flip callbacks spanning 72.794 seconds;
Explorer has two successful windowed Presents. The trace retains no RuntimeError,
ZinkFailure or KMD recovery event. The user missed this native interval;
the subsequent viewing run above establishes a physical desktop failure.
[Native pixel results and limits](D3D_RUNTIME_SHARING.md#native-shared-sampling-and-cleanup-pass)

All 167 returned files (110,632,441 bytes), including six early copies, match
device lengths/hashes. The ETL is finalized with no reported loss. Collection
completes at 21:56:46 UTC; an additional startup occurs during recovery at
21:57:58, cause unconfirmed. `.59` is staged at 22:00:48, boots at 22:01:26,
and is verified Code 0 with cleanup and unchanged power at 22:03:37. Saved
portrait is unchanged at 22:04:35. No native test remains running.

**Next:** retain the stalled packet's validated command snapshot and hardware
fetch position before reset destroys them. `.155` identifies GPU-fence wait,
but not the exact instruction. Preserve the underlying resource-create failure
instead of interpreting generic OOM as budget exhaustion.
Then complete physical desktop composition, hardware landscape and Win32 `vkcube`.
The successful shared-pixel probes need not be rerun without a relevant change.
[Buffer ownership](D3D_RUNTIME_SHARING.md#buffer-release-ownership-correction) ·
[Shared image usage](D3D_RUNTIME_SHARING.md#native-shared-versus-private-sampling)

**Prior `.152` result:** native LUID `bbde7` reaches Code 0 at 19:57:57 UTC.
Explorer's shared Blt-mode Present now returns S_OK through the runtime; the
corresponding kernel event is a successful `RedirectedFlip`. DWM opens that
image and retains 63 successful flip callbacks, followed by device loss. The
patterned probe fails `vkCreateDevice` with `VK_ERROR_DEVICE_LOST`, without a
pixel result. The user reports an initially correct taskbar position with its
right-side tray missing, then no taskbar from the second of about three flashes.
Complete native desktop composition remains unverified.
All 152 returned files (102,245,042 bytes) match device lengths/hashes. `.59`
Code 0, cleanup and unchanged power settings are verified at 20:05:11 UTC;
portrait verification at 20:06:35 is a no-op. No native test remains running.
[Native result and restart timeline](D3D_RUNTIME_SHARING.md#native-windowed-present-retest)

**Diagnostic implementation (`.153`, `110d9882`):** record the first KMD
recovery status and triggering path in ETW before notifying Dxgkrnl, including
DISPATCH_LEVEL preemption and legacy-submit failures. Host/WDK builds and
Windows ARM64 context/worker controls pass at 20:16:20 UTC. They check first
error retention, lock release and notification ordering. The later
`sample-i153-native-20261111` trace verifies live kernel delivery with five
records, but they occur after the first device error and do not explain it.
The first deployment registers the provider
successfully (`TraceStatus=0`) but aborts before native grant: the operator
called the low-level bind/arm helpers without the UMD-registration wrapper.
`.59` Code 0, cleanup and unchanged power are verified at 20:25:41 UTC;
portrait verification at 20:26:00 is a no-op. The retry uses `bind-cube.ps1`
and checks registration metadata before staging boot state; the signed driver
is unchanged. The missing-registration check passes on `.59` at 20:27:06 UTC
without changing boot state. The corrected retry completes the native probe
and recovery described above. Evidence directories:
`recovery153-native-20261111` (89 verified files from aborted setup),
`recovery153b-native-20261111` (completed retry).
[Payload and capture limits](RECOVERY_TRACE.md)

**Latest `.151` result:** native LUID `baff1` completes legacy BGRA/RGBA shared
GPU writes/readbacks in both directions and after producer-device release:
24,576 matching pixels. The full probe exits 1 because the NT/keyed-mutex
texture creation returns `80070057`. The user reports approximately three
flashes, an initially misplaced and incomplete taskbar, then no taskbar; two
photos retain wallpaper/icons with the taskbar displaced/incomplete or absent.
The trace now exceeds the old table-pool limit (2.75 MiB resident, no retained
ReserveResource failure). DWM opens Explorer's shared image successfully 26
times, but all 26 Explorer Blt-mode Presents return `887a0004` at the UMD's
flip-only guard. This rejection is addressed by the `.152` candidate above.
All 161 returned files match device lengths/hashes; `.59` Code 0, cleanup,
unchanged power settings and portrait are verified at 19:40:17–31 UTC.
[Native result and remaining windowed Present contract](D3D_RUNTIME_SHARING.md#native-table-pool-retest)

**Latest `.150` native result (2026-11-11): page-table capacity reached.**
On LUID `c0728`, Segment 1's 2-MiB backing reaches exactly 2 MiB resident
before 12 paired `ReserveResource` failures (`c0000001`, all Segment 1).
Ordinary system-memory residency is 22,900,736 bytes before the first failure;
this is distinct from the 512-MiB application resource limit. DWM retains 14
successful UMD Presents followed by device-removed, but no native shared-pixel
result or desktop screenshot survives. The user sees the Start menu only in
the unverified/recovered interval; complete native desktop remains unproven.
All 147 returned files match device hashes. `.59` Code 0, cleanup, unchanged
power settings and portrait are verified at 19:23:24–36 UTC.
[Native shared trial and page-table evidence](D3D_RUNTIME_SHARING.md#native-shared-trial-and-page-table-capacity)

**Latest `.149` native desktop result (2026-11-11):** the runtime access fix
from `5852cfcc` is tested with unchanged KMD `ebfd5142`, UMD `413d32bb6034`
and ICD `f828845c38b7`, on native LUID `bb153`. StartMenuExperienceHost now
loads the UMD and creates D3D devices successfully. The user still confirms
missing/incomplete taskbar content during the active trial. Six snapshots find
its window visible at `(0,1232)-(720,1280)`; window geometry is not pixel proof.
No rotation or cube is requested in this trial.

**.149 boundaries (superseded by .151 above):** Explorer's three observed instances each request a
576x960 BGRA shared surface, then the D3D runtime reports
`ConfigureSharedResource / ShareObjects = c000000d`. Some SearchHost and
TextInputHost instances fail Zink's `vkCreateDevice` with result `-3`;
the underlying KMT status is not captured. The retained trace has no
`LoadUMD / 80070005` error. This validates the runtime-directory access fix,
not complete desktop composition or a repaired taskbar.
[Native loading retest and process-specific evidence](D3D_RUNTIME_SHARING.md#native-appcontainer-loading-retest)

**.149 recovery:** all 163 returned files (75,003,540 bytes) match device lengths and
SHA256. `.59` Code 0, baseline hashes, cleanup and unchanged power settings are
verified at 18:11:24 UTC; portrait restore/verify is a no-op at 18:12:17 UTC.
Artifacts are in `desktop-acl-native-20261111`; no native test remains running.

**Shared-texture candidate:** runtime-owned non-primary BGRA/RGBA allocation,
serialized resource metadata and consumer OpenResource are implemented. UMD,
ICD and WDK ARM64 builds pass; controlled callbacks pass ownership, paging and
failure cleanup. The upgraded probe passes all 49,152 shared-content checks on
Microsoft WARP, including producer-device release. This is not native Tegra
pixel evidence. The candidate also retains the failing KMT API/NTSTATUS during
device initialization. Source `be55a95b` is published on `main`.
[Implementation and controls](D3D_RUNTIME_SHARING.md#runtime-shared-texture-bridge)

**Page-table correction:** increase the early single-process pool from 16 to 128
128-KiB tables (2 to 16 MiB). This is contiguous MMU infrastructure; application
storage retains its existing dynamic 512-MiB cap. Host models, WDK ARM64 build
and the paging/context controls on `.59` pass, including translations beyond
the former limit and rejection past the new backing. Source `71322147` is
published; `.151` passes the native capacity comparison above with the exact
same UMD/ICD as `.150`. Complete desktop rendering remains unfinished.

**Windowed Present candidate (`24c1ca9a`):** the UMD accepts the observed Blt flag for
runtime-owned shared surfaces, completes GPU use and forwards their allocation,
context and opaque DXGI token to the runtime callback. Primary-only validation
still applies to flips. The ARM64 build and Windows callback controls pass.
A new `--shared-layout` probe renders a nonuniform shared pattern
and samples it with translated/scaled/clipped viewports and rebound constants
on a 720x1280 target; all 3,686,400 WARP pixel checks pass at 19:50:09 UTC.
The `.152` native trial confirms successful Explorer and DWM callbacks, followed
by device loss; it is recovered to `.59`. Native layout checks and complete
windowed composition still require validation; callback success is not pixel proof.

**Next validation:** verify complete physical desktop composition, then hardware landscape and
Win32 `vkcube`. Native shared patterns, offset/scale/clipping and normal teardown pass above. NT-handle/keyed-mutex sharing remains a separate D3D interface
contract to implement. Do not treat raising Feature Level as a fix for the
observed Present rejection.

**Landscape remains unverified:** the preceding `.149` source-mode trial on
LUID `bd778` still returns CCD error 1610 at 17:32:03 UTC. It applies no mode
and launches no cube. The 1280x720 source/rotation correction passes host and
ARM64 controls but is insufficient by itself to fix native mode negotiation.
[Source-mode correction and retest](D3D_RUNTIME_SHARING.md#native-source-mode-retest)

**Latest `.148` native regression (2026-11-11):** all 40,960 draw/sample pixels
match on LUID `9a1e7`, with both exit-0 files retained. DWM has 469 successful
flip Presents at the KMD, UMD and DXGI layers across 42.235 seconds of retained
events. All 469 target physical addresses match a subsequent VSync scanout
report before the next flip. This validates the new allocation/scanout path
in portrait. Collection is interrupted by a further startup; the native ETL
is unfinalized and no desktop image survives. The user confirms only the
restored desktop, not the native interval. Physical landscape and sustained
stability remain unverified. All 152 returned files match device hashes;
`.59` Code 0, cleanup, unchanged power settings and portrait are verified.
[Native regression and evidence limits](D3D_RUNTIME_SHARING.md#native-explicit-layout-regression)

**Latest `.147` native result (2026-11-11): desktop lights up.** DWM selects
Tegra X1 / FL10_0 on LUID `a2008`. The retained trace contains 108 successful
Present calls at all three layers (KMD, UMD callback and outer DXGI), plus
108 successful rotations of the three swapchain buffers, spanning 24.725
seconds. The user confirms a normal physical desktop and supplies a photograph
of wallpaper, Computer Management and Tegra X1 (GM20B) under Display adapters.
The ordinary and sampling logs match all 40,960 pixels and reach explicit
teardown and their success messages.

**Scope:** this validates the no-wait MMIO-flip fix and an initial visible
DWM desktop, not long-term stability. A further boot at 15:03:07 interrupts
collection; the result/exit files are zero-filled and the native ETL is not
finalized. Its last event is not a fault location. The second ETL is from the
closed startup gate and Basic Render, not native rendering. All 151 returned
files match device hashes. `.59` Code 0, complete cleanup and unchanged power
policy are verified at 15:08:11 UTC; portrait verification at 15:08:32 is a
no-op. The test package is no longer installed.
[Native Present result, photo provenance and limits](D3D_RUNTIME_SHARING.md#native-mmio-present-result)

**Remaining desktop work:** complete visible composition, hardware landscape,
sustained operation and visible Win32 `vkcube`.
The current swapchain identity rotation moves storage/views; it does not
rotate the physical screen. FL10_0 is sufficient for the desktop milestone
observed here. The `.147` run changed only the KMD package from `.146` to
`.147`, retaining the same UMD and ICD binaries and their passed controls.

**Hardware rotation WIP:** allocation geometry, UMD imports, Display A fetch,
legacy/MPO flips and full-layout latch confirmation are connected. The candidate
VidPN implementation advertises the four normal rotations; the fixed DSI signal
and standard shared primary retain their unrotated dimensions. The actual
primary's stored geometry controls scanout, so a new mode does not reinterpret
an old buffer. Smooth rotation and rotated GDI blits remain unsupported.
The host address models and real-WDK ARM64 controls pass on `.59`, including
same-address direction changes, incomplete latches, mode support/pivots,
immediate retries and POST restoration. `.148` passes the recorded native
portrait drawing/flip checks; physical landscape validation is still pending.
The new 48-byte primary producer requires
the matching updated KMD and must not be paired with the old `.147` package.
[Implementation and remaining contracts](D3D_RUNTIME_SHARING.md#hardware-rotation-backend)

The preceding `.146` trial on LUID `94aa5` reached the real runtime Present
callback but rejected `FlipWithNoWait`, returning `c00000bb` and producing
black physical output. `.147` supersedes that boundary. The earlier LUID
`92611` trial passed the allocation-rotation mode check but reached the old
UMD Present stub. Detailed history remains in the linked implementation notes.
[Earlier Present failure](D3D_RUNTIME_SHARING.md#runtime-present-native-trial) ·
[Mode-setting fix](D3D_RUNTIME_SHARING.md#native-identity-rotation-trial)

**Final orientation requested by the user:** hardware rotation with a
1280x720 landscape desktop, left/right controls in their normal handheld
orientation. Validate the source dimensions, pitch, display fetch/latch and
complete visible image together, then leave Windows in landscape. The saved
portrait snapshot is only the recovery target during unvalidated experiments.

**Previous `.144` runtime-context result (2026-11-11):** the UMD now registers a real
D3D runtime context and completes NVK GPU work before runtime synchronization.
On native LUID `9b7ff`, all 40,960 ordinary/sampled pixels match and both probes
release their devices and exit 0. DWM issues 261 GPU synchronization signals
with nonempty context lists; its 401 recorded GPU waits return S_OK. The
finalized trace reports no loss, and all 145 returned files match device hashes.
.59 Code 0, cleanup, portrait and unchanged power policy are verified.

**Pre-fix `.144` desktop boundary:** the D3D11 journal on native LUID `953d1`
identifies 144 `SetDisplayMode` failures as `c01e0005`
(`STATUS_GRAPHICS_PRESENT_MODE_CHANGED`). The runtime saves this failure,
returns S_OK to the UMD, then reports `887a0025` to DXGI. Callback S_OK does
not establish kernel mode-setting success. All four providers decode fully;
the finalized ETL reports no loss. The 132 returned files match device hashes,
and .59 Code 0, portrait and unchanged power policy are restored.

This was a short observation with unchanged binaries, not another drawing
test. Its native `Default` desktop capture was black. The `.146` result above
supersedes this mode-setting boundary; shared-image integration and sustained
presentation remain unfinished. These earlier failures were not host/device OOM or
invalid-parameter return codes.
[Runtime context evidence and implementation](D3D_RUNTIME_SHARING.md#runtime-context-registration)

**Primary bridge native result (2026-11-11):** the driver now creates the portrait
primary through the D3D runtime, imports that same allocation into NVK through
Mesa-internal owner callbacks, and routes SetDisplayMode to the real runtime
callback. Zink/NVK retain the explicit 2880-byte pitch and use GPU tiled-shadow
rendering. The owner releases GPUVA reservations before runtime deallocation,
so delayed object cleanup cannot free a reused address. After fixing the
incorrect rejection of `E_PENDING`, native LUID `98df6` completes 116 primary
allocations, 116 GPUVA maps and 232 paging waits; 115 actual runtime
SetDisplayMode callbacks return S_OK. The finalized 12,816-event trace has
no recorded loss or DWM runtime-error callback. This validates allocation and
callback routing, not kernel mode-setting success, sustained Present or a
visible desktop; see the runtime error-masking finding above.
All 120 returned files match device hashes; .59, portrait and power policy are
restored. A bounded-wait rerun on LUID `954c5` validates device release but
reveals a sampler binding error in the returned SPIR-V. After using Zink's
Vulkan binding instead of the Gallium slot, LUID `92166` matches 24,576
independent `t5/s2` sample pixels (wrap/clamp/rebinding, RGBA/BGRA) plus the
16,384 ordinary draw pixels. Both probes complete explicit teardown and exit
0 without timeout. All 144 final-trial files match device hashes; .59 Code 0,
cleanup, portrait and unchanged power policy are verified. That trial's desktop
capture remained black while DWM cycled primary resources/mode setting. Actual
Present and shared-image opens were unfinished at that point.
[Primary bridge contract and controls](D3D_RUNTIME_SHARING.md#runtime-primary-bridge-work)

**Version labels:** Device Manager shows the installed INF package's version.
Earlier experiments reused the signed `.144` KMD while replacing the test UMD
and ICD. The latest native KMD candidate is `.154`, correcting inherited-state
validation order and retaining `.153` recovery diagnostics. Standalone build manifests
name their component and include a readable `artifact_id` with the DLL hash.
The completed `.154` trial used these artifacts; completing the trial does not
mean its native rendering or desktop checks passed:

| Component | Candidate identifier |
| --- | --- |
| Signed KMD package | `0.1.0.154` (source `760d8405`, SYS `dec82546353e`; execution-order state validation) |
| D3D UMD / Zink | `sampler-view-20261112-umd+77dea046dde0` (source `ee7b0d50`) |
| NVK ICD | `runtime-shared-20261111-icd+6e2fad99d742` |
| Runtime probe | `7a9752a1e128` (source `3531f0ee`); Load WARP control passes, no retained native Load result |

These are test-deployment identifiers. Rollback reinstalls `.59`; a future
complete driver-package release must version its INF and catalog together
with the bundled UMD/ICD, even if a component's binary is reused.

**DWM false OOM located (2026-11-11):** loader diagnostics identify Windows
error 87 while opening `./libvulkan_nouveau.dll`. The Windows manifest
generator used a POSIX separator; changing it to a backslash removes the
working-directory dependency in a four-case physical control. The loader
also misclassifies error 87 as OOM because its string test for `error 8`
matches both. This is separate from GPU allocation and the 512 MiB budget.

**The packaging fix passes the DWM initialization boundary on hardware:**
with unchanged binaries and LUID `994f3`, DWM discovers all 16 instance
extensions, creates its instances/Zink screens and receives three successful
D3D UMD CreateDevice returns. The trace records six fresh and six cached
snapshots, all successful. Native FL10_0 rendering still matches 16,384
pixels. The desktop capture remains black, and DWM subsequently exits with
`c0000409`; the later exception capture below identifies an unsupported
shader opcode. Composition and Present remain unverified.
.59 Code 0, environment cleanup, portrait preference and unchanged power
settings are verified after rollback.
[Diagnosis, controls and validation](PORTING.md#windows-icd-manifest-path)

**Post-device result (2026-11-11):** new resource/format/teardown and
source-located runtime-error records pass the ARM64 controls and native
regression. On LUID `90f60`, three DWM processes each successfully create
four constant buffers and two vertex buffers. The finalized trace has
22,407 valid events and no recorded loss; it contains no DWM runtime-error
callback. DWM still exits with `c0000409`, and the per-DWM WER attempt
produces no dump. The last successful buffer is not a fault location.
The subsequent exception capture below covers the shader failure. All 134
returned files match device hashes; .59, portrait,
power policy, temporary environment and dump settings/ACLs are restored.
[Post-device evidence](PORTING.md#post-device-ddi-trace)

**DWM sampling fix reaches the next contract boundary:** the matched ProcDump for PID
484 identifies fast-fail 7 in `ucrtbase`, called from the UMD's
`ttn_parse_tgsi` at RVA `21de44`. Register w26 is 145 (`TGSI_OPCODE_SAMPLE`),
and disassembly matches the unsupported-opcode `abort()` branch. The
`dwm-exception-auto-20261111` run has been recovered to .59; its dump hash
matches the device. SAMPLE lowering and independent texture/sampler
descriptors are implemented. Six ARM64 semantic controls and a WARP sampling
control (24,576 matching pixels) pass. On LUID `9a316`, the subsequent native
trial records 172 successful DWM resource creations, including 36 full-size
720x1280 BGRA textures across repeated device attempts. Nine `_SetDisplayMode`
calls explicitly return `DXGI_ERROR_UNSUPPORTED` from the existing hardware
stub. This is a concrete remaining presentation boundary.

The device restarts before the native drawing probes finish. The retained
34,774-event trace is not finalized; absence of a later event is not proof
of nonexecution. Native sampling, the latest ordinary-draw regression and
desktop composition remain unverified. All 121 returned files match device
hashes; .59 Code 0, portrait and unchanged power policy are restored.
The subsequent primary bridge connects the actual runtime allocation and
display-mode callback. The cause of this earlier trial's restart is not
established by its record.
[Exception context and sampling work](PORTING.md#dwm-tgsi-sample-abort)

**Host-memory audit (2026-11-11):** no page-size/alignment or 512 MiB unit
error was found in the inspected paths. The current instance path uses
ordinary CRT allocation; GPU section allocation uses committed,
write-combined pages without large-page flags. A separate bug collapsed
section access/privilege errors into host OOM; the source now retains the
Windows code and distinguishes real memory errors. The SDK control passes
thirteen injected errors, budget rollback, the exact 512 MiB boundary and GPU
VA alignment checks on .59. The subsequent round above supplies the new
ICD's native rendering regression; the injected OS failures remain
controlled tests. This is not a DWM root-cause fix.
[Scope and evidence](PORTING.md#host-allocation-and-budget-audit)

**Win32 presentation investigation (2026-11-11):** the GDI WSI now pairs
window DC acquisition/release on the presenting thread and flushes GDI
before reusing its DIB. On the unchanged .144 miniport, LUID `90f83`, 24
presents produce 24 different GPU-buffer hashes; all sampled window-center
pixels match the corresponding CPU readback RGB values. Every blit/flush
reports success. The desktop capture is still black, so visible cube/DWM
presentation is not established. Check the input desktop and capture
DPI/coordinates alongside composition and scanout; the samples do not
validate the whole image.

**Earlier Present audit:** the LUID `90f83` .144 journal retains an owned, visible
`Displayed` address `fc9a6000`, populated after active-bank confirmation.
This establishes an initial latch, not continuous refresh or correct cube
pixels. That trial's Application log has 35 Tegra-attributed DWM exits with
`8898008d`. A subsequent SYSTEM control cannot load `vulkan-1.dll` by name
(error 126), but can load the explicit app-local copy; the experimental NVK
opt-in is also absent there. The follow-up above supplies that environment
and locates the first failed UMD stage. Present/shared-resource contracts
remain unfinished as well.
[Feature-level, loader and latch audit](PORTING.md#present-contract-audit)

**User display baseline:** the previous 90-degree rollback did not restore
the user's portrait preference. Their manually corrected 720x1280,
orientation-0 mode is saved and again matches the registry on .59 Code 0
after the automatic trial.
Rollback during unvalidated experiments must restore that snapshot. The mode passes non-applying
CCD validation on `Default`; the later `Screen-saver` desktop observation
also requires an active-desktop check for visible GPU tests. It does not
prove what caused the earlier black cube captures.

**Screen-saver handling:** first attempt ordinary touch wake in the
interactive session and recheck the input desktop before asking the user
to intervene. Ask only when that attempt fails or authentication is needed.
The user need not keep the screen lit during local development. The prior
mouse attempt was denied. On 2026-11-11, the ordinary touch-down injection
also returned access denied (5) on `Screen-saver`; the user then returned
to `Default` and portrait verification passed. Remote touch dismissal is
not established on this configuration.

**The final display target is hardware 90-degree landscape rotation**, as the
user reaffirmed: implement and verify rotation with source dimensions,
stride and scanout layout, alongside the presentation work. The current
native path still accepts only 720x1280 identity scanout. Validate complete,
correctly oriented visible output and sustained refresh before claiming
an on-screen pass. The user's saved portrait setting remains the rollback
target while landscape capability is developed. Once the hardware landscape
path is verified, preserve landscape as the final system setting. The earlier
request to avoid speculative rotation-register changes during black-screen
diagnosis does not remove the requested hardware-rotation implementation.
[Build and physical evidence](PORTING.md#win32-vkcube)

**Current work: D3D UMD/DWM.** .144 passes native Vulkan OPAQUE_WIN32
memory sharing on GM20B LUID `98914`. Named/unnamed buffers and optimal
RGBA/BGRA images retain their contents after the exporting VkDevice and
NT handle are released. GPU read/write and re-export check 32,768 payload
words and 512 guards with zero mismatches; exit 0 without timeout.
[Win32 Vulkan memory result](VULKAN_WIN32_MEMORY.md)

**Current authorized route: a Vulkan desktop backend without Zink.**
The November 12 instruction supersedes the earlier desktop-DXVK approval gate.
Choose and implement the Windows UMD/DDI backend needed for hardware DWM;
DXVK may supply reusable code. The current Gallium `d3d10umd` is excluded from
the replacement. Zink remains an optional later OpenGL compatibility route.

.145 adds a diagnostic runtime-resource test: legacy allocation/open/release
passes, but NT shared texture creation returns `E_INVALIDARG`. These are raw
storage/association checks, not shared-image rendering. .144 remains the
GPU rendering/sharing baseline. [Runtime contract result](D3D_RUNTIME_SHARING.md)

**Current WIP (2026-11-11):** the KMD now accepts a versioned resource-level
storage descriptor on allocation creation and open. It checks the atomic
allocation set and retains its size/count contract across device opens.
The ARM64 controlled fixture passes on .59, including partial-failure
cleanup and cross-device read-only opens. This source change is not installed
as a live driver; the D3D UMD's image-layout and memory binding bridge remains
pending. [Implementation and validation](D3D_RUNTIME_SHARING.md#resource-metadata-work)

The rebuilt UMD/ICD also passes a native Windows D3D11 runtime regression
at feature level 10_0: 16,384 RGBA/BGRA clear/triangle pixels match, implicit
release completes and the process exits 0. The preceding .143 trial passed
three such runs, totaling 49,152 pixels.

The UMD fixes a Mesa barrier lifetime error: the Windows fallback returned
a serial result to every participant, and queue workers could destroy the
barrier while peers still reacquired its mutex. The caller now destroys it
after all worker fences, and each generation has one serial result.
The original ARM64 control crashes without GPU use; the patched control
passes 512 four-worker finishes and 2,048 jobs.
[Runtime lifetime evidence](D3D_UMD_LIFETIME.md)

Next: D3D runtime allocations/shared-image metadata, synchronization and
sustained DWM presentation, then the separate Win32 Vulkan `vkcube` test. These remain
unverified. .144 keeps the KMD core unchanged and uses an app-local
Vulkan loader; system-loader packaging for DWM is still pending.
New build artifacts are stored under
`/Users/rujian/.local/share/mesa-switch-woa-build/runs/`.
The original `/private/tmp/t214-gpu-round` directory is no longer present on
the build host; the paths in earlier reports identify their original locations.

The .132 trial passes 16 KiB NT-handle
storage sharing between two KMT devices on GM20B, including CPU readback
after releasing the producer and closing the exported handle. Both device
budgets return to zero. The matched adapter/active VidPN LUID is `949ff`.
D3D runtime resource integration, shared-texture synchronization and DWM
composition remain pending. The separate game compatibility path will use
DXVK over NVK. See the [shared-storage result](SHARED_STORAGE_RESULT.md).
After .145 testing, .59 Code 0, the original ACPI/driver hashes and unchanged
AC/DC display/sleep settings were verified. The start permit, test tasks and
temporary UMD/ICD registration have been removed (22:30:30 UTC).

| Driver | Result | Remaining work |
| --- | --- | --- |
| 0.1.0.145 | Code 0, matching LUID `9edf6`. Diagnostic runtime allocation/open/release of legacy shared storage succeeds; NT texture creation returns `80070057`. Probe exits 1 without timeout. | Connect real shared images and resolve the negotiated DDI/handle contract. No shared GPU rendering is claimed for this probe. |
| 0.1.0.144 | Code 0, matching LUID `98914`. Vulkan NT memory sharing, GPU buffer/image read/write, producer-device release and re-export pass 32,768 payload words plus 512 guards. D3D11 runtime regression matches another 16,384 pixels; both processes exit 0. | Cross-process sharing, D3D runtime resource/layout integration, DWM and Win32 `vkcube`. |
| 0.1.0.143 | Code 0, matching LUID `a59ef`, real D3D11 runtime at feature level 10_0. Three implicit-release runs match 49,152 pixels and exit 0 after the barrier lifetime fix. | Runtime sharing/synchronization, DWM and Win32 `vkcube` remain pending. |
| 0.1.0.142 | Code 0, matching LUID `a7cce`; 16,384 pixels match before exit `c0000005`. Raw frames reach the UMD's cnd_wait. CPU-only controls independently reproduce the barrier defects and pass after the source fix. | Native validation of the fix follows in .143. |
| 0.1.0.141 | Code 0, matching LUID `97642`. Three native runtime runs each match 16,384 pixels; implicit release faults once and two raw-address diagnostic runs exit 0. | Barrier defect reproduced in .142 and fixed in .143. Shared resources, DWM and Win32 `vkcube` remain pending. |
| 0.1.0.140 | Code 0, matching KMT/active VidPN LUID `9acf3`, real D3D11 hardware device at feature level 10_0. Both native runs match 16,384 RGBA/BGRA pixels; first exits `c0000005`, diagnostic run exits 0 with explicit release markers. | Barrier fix validated in .143; runtime sharing/synchronization, DWM and Win32 `vkcube` remain pending. |
| 0.1.0.139 | Code 0, matching KMT/active VidPN LUID `a2dc3`, 512 MiB ceiling. Three color passes and two SM4 triangle passes check 20,480 pixels with zero mismatches; exit 0 without timeout. | Windows D3D runtime/shared resources and DWM; Win32 `vkcube` remains pending. |
| 0.1.0.138 | Three-color readback passes, then first Draw exits `c0000005`. UMD RVA `1256e4` identifies Zink writing resource metadata through a CPU dummy vertex-buffer pointer. | CSO/u_vbuf routing fix passes natively in .139. |
| 0.1.0.137 | Code 0, matching KMT/active VidPN LUID `a3dbb`, 512 MiB ceiling. Native D3D UMD clear/copy/readback passes three colors with 12,288 matching pixels; exit 0 without timeout. | Native draw/shaders, D3D runtime/shared resources and DWM; Win32 `vkcube` remains pending. |
| 0.1.0.136 | Code 0, matching LUID `97b6f`; worker recovery at fence `7d` captures MMU address `04004000`, info `1dc6`, instance `fc241`. Release logging confirms later Lock2/allocation calls return `c00002b6`. Windows subsequently restarts. | QMD mirror passes in .137; native draw and runtime/DWM integration remain. |
| 0.1.0.135 | Code 0, matching LUID `98054`; the barrier uses WFI. Native fence `2e` completes; worker recovery at fence `43` records `c000009c`, FIFO `10000000`, MMU mask `1`. Windows later restarts. | Capture the fault address/type and fix the failing mapping; clear/readback remains unverified. |
| 0.1.0.134 | Code 0, LUID `9d74b`; the 592-byte rejection is reproduced offline at SET_REFERENCE(0). Changing only that DWORD to WFI passes unchanged KMD validation. Native mapping reports `88760870` after device loss. | Barrier fix physically tested in .135; further GPU execution work remains. |
| 0.1.0.133 | Code 0, matched LUID `9493e`, native-submit flag, real NVK UMD adapter/device and texture creation. Native fence `14f` completes with Reference `131` and proof mask 7. Fence `163` is rejected with `c000000d`; all 4,096 first-pass pixels mismatch. | Rejection identified in .134. Native clear/draw/readback, DWM and Vulkan presentation remain pending. |
| 0.1.0.132 | Code 0, LUID `949ff`, successful section-backed shared storage and 4,096 matching data words before/after producer release and NT-handle closure. GDI fence 23 completed with proof mask 7. | Cross-device GPU rendering/completion, native D3D UMD/DWM, and Win32 `vkcube`. The storage probe submits no drawing work. |
| 0.1.0.130 | Code 0, GM20B LUID `8e7d4`, matching active VidPN, completed GDI copies, successful MPO3 and native scanout at `fb9c7000`. Pixel readback and the user confirm visible RGB bars on the internal panel. | DWM exits with `MILERR_NO_HARDWARE_DEVICE`; D3D11 device creation on GM20B returns `DXGI_ERROR_UNSUPPORTED`, while WARP succeeds. Continuous desktop refresh remains unresolved. See [native startup result](NATIVE_STARTUP_RESULT.md). |
| 0.1.0.121 | The timing callback returned link success with a nonzero change ID. GM20B LUID `962ec` reached 720x1280, 60 Hz mode setup and successful shared-primary allocation creation. | Windows restarted later. The normal journal filled before the first failure; the next boot was gated to Code 43. Stable native presentation remains open. See [display timing result](DISPLAY_TIMING_RESULT.md). |
| 0.1.0.120 | GDI DMA now uses aperture segment 2. The live adapter stayed Code 0 through the test, progressed to GPU-virtual paging submissions, and exposed GM20B LUID `8ec5d`. | The interactive probe found zero active display paths and the panel was black while SSH responded. Native desktop presentation remains unverified. See [GDI DMA pool investigation](GDI_DMA_POOL.md). |
| 0.1.0.119 | The live driver-caps query returned `NoCacheCoherentApertureMemory` (`0x20000000`). GDI creation succeeded with 232 DMA private bytes and 256 allocation entries. | Windows restarted to BasicDisplay. Both ETLs associate the retained mode switches with BasicDisplay. |
| 0.1.0.118 | The write-through scheduler journal retained the writer's ready marker and the successful GDI return with 256 allocation entries. | Windows restarted to BasicDisplay. The journal has no submit, preempt, recovery or reset record, and the first internal failure remains unidentified. |
| 0.1.0.117 | Tegra StartDevice returned success with one source and one child. The GDI context returned 232 bytes of DMA private data and 256 allocation entries. | Windows restarted to BasicDisplay. The callback record contains no Tegra VidPN commit. |
| 0.1.0.116 | The live `GPUVERSION` query returned 128 bytes with `Maxwell GM20B`. Tegra StartDevice succeeded and GDI again reported 256 allocation entries. | Windows restarted and BasicDisplay resumed. The GDI callback record is the last retained Tegra startup milestone. |
| 0.1.0.115 | The desktop demand start returned success with one source and one child. DxgKrnl recorded a LUID for `SWOA0F01`. The driver again reached GDI with 256 allocation entries. | Windows restarted and BasicDisplay resumed. The callback record contains no Tegra VidPN commit. |
| 0.1.0.114 | Holding the grant until consumption let the same driver start from the BasicDisplay desktop. GPU and panel initialization and the system paging root succeeded. The live GDI context returned `AllocationListSize=256`. | Windows restarted. The durable record contains no GDI root setup, Present or VidPN commit. The next start was gated and BasicDisplay resumed. |
| 0.1.0.113 | The permitted start installed the 16,384-entry system paging root in segment 1. The GDI device and virtual-addressed GDI context were created after mode enumeration. | The callback record contains no GDI root setup, Present or VidPN commit. The second start was denied by the test budget. |
| 0.1.0.112 | One permitted start initialized the panel and completed the first CPU page-table update. After five successful mode enumerations, Windows created a GDI device and virtual-addressed GDI context. | No VidPN commit or timing submission appears in the callback record. The next start was denied by the test budget and the desktop used BasicDisplay. |
| 0.1.0.110 / .111 | Both permitted starts in each trial initialized the panel and completed five mode enumerations. The .111 ETLs retained successful StartDevice results. | The callback records show no entry into the hardware capability query, POST release or VidPN submission. The next start was denied by the test budget and the desktop used BasicDisplay. |
| 0.1.0.108 / .109 | In both trials, each permitted start initialized the panel and completed five mode enumerations. The persisted counter denied the third start, and Windows reached the BasicDisplay desktop. | No completion was recorded for VidPN commit or timing submission. |
| 0.1.0.107 | The startup trace recorded four complete rounds of successful DSI link queries and VidPN enumeration. | Windows repeatedly restarted before a recorded VidPN commit. The same boot sequence value kept the test permit active across the observed restarts. |
| 0.1.0.105 / .106 | The DSI link callback succeeded on hardware. The .106 trace recorded the first start and five successful VidPN enumeration calls. | A later driver load was denied by the test gate. The interactive probe saw Code 43 and the fallback display path. |
| 0.1.0.87 | Khronos ARM64 loader and NVK submitted a Vulkan triangle through the GM20B LUID. GPU fence 27 completed. Readback matched 300 red pixels, 724 blue pixels and 128 guards. The separate Win32 GDI preview displayed the verified pixels. | Native Win32 swapchain presentation. |
| 0.1.0.101 | `ACPI\SWOA0F01\0` appeared as **Tegra X1 (GM20B)** in the **Display** class with **OK / Code 0**. The GPU initialized, the miniport acquired the POST scanout, and the display core accepted the callbacks. | The internal adapter startup later reported `STATUS_NOT_SUPPORTED` after monitor-mode queries. The capture had no active VidPN mode commit or WDDM flip. |
| 0.1.0.59 | After the .121 rollback, the Lite returned to the diagnostic driver with Code 0 and the baseline ACPI table. | Continue from .121's accepted timing and shared-primary creation, preserving first-failure evidence. |

The test decrements and flushes the start budget before GPU initialization.
The .109, .110 and .111 callback records were identical.
AutoLogger preallocation made the first two files readable in .110. The
.111 configuration used 8 KiB buffers with circular preallocation. Each
of its first two ETLs retained StartDevice and three diagnostics with
`STATUS_NOT_SUPPORTED`. These ETLs lack a final log header and the
diagnostics do not identify the callback.
The .112 and .113 admitted-boot ETLs retained GPU VA events and still had
unfinalized headers. The .112 mode-switch events were in the fallback boot.
The .114 desktop-start ETL retained the Tegra StartDevice entry in an
unfinalized trace. Its mode-switch events and those in the finalized
fallback trace belong to BasicDisplay. The restart interrupted demand-script
logging.
The .115 write-through request and desktop records survived the restart.
The .115 and .116 admitted ETLs retained only registration events from the
scheduler provider. The .116 flusher logged 14 successful flushes in the
780 ms following the PnP request. Its first sample already counted 3,091
lost events. The sampled window added zero. The durable callback records
supply the GDI results.
[AutoLogger file rotation](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-an-autologger-session)

In .117, the callback record confirms the GDI marker was enabled and
`EtwWrite` returned success. The admitted ETL header is unfinalized and its
scheduler events are limited to registration. The flusher recorded 14
successful flushes in the 779 ms following the PnP request. Both loss
counters were zero throughout the sampled window. The first internal
failure remains unidentified.
[ETW session counters](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_properties)

The .118 journal's GDI marker was about 37 ms after the final ETW flush
sample by the boot-relative counters. Mode-switch events in both traces
match BasicDisplay. The fallback trace records Tegra's denied start.

The .113 package predates the GDI allocation-list change. The first .114
demand request left the permit unused.
[WDK context requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_contextinfo)

The .115 callback record shows `STATUS_NOT_SUPPORTED` for static adapter
performance caps, GPU version and DisplayID before GDI. The .116 test
verified the GPU version response on the Lite.
[WDK GPU version](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ns-d3dkmdt-_dxgk_gpuversion)

GDI now reserves one kernel queue entry of DMA private data. The Windows
ARM64 fixture passed with controlled substitutes under .59. The .117
package is the first physical test of this size change.

The ARM64 NVK build includes Win32 surface and swapchain support. Mesa's
GDI presentation waits for the GPU copy fence before reading the image.
The 2026-11-11 `vkcube` run exits normally but produces black captures;
visible presentation remains unverified, as detailed above.

## Long-term objective

Deliver a complete native Vulkan driver for Windows 11 ARM64 on GM20B.
The .87 offscreen draw/readback and .130 native scanout passes are the
hardware baselines. Complete the Vulkan core functionality, supported
extensions, memory and synchronization contracts, and Win32 presentation,
with explicit API-version and feature reporting. Validate each claimed
version and extension with the corresponding VK-GL-CTS coverage and
physical rendering, readback and sustained-presentation tests. Track
outstanding conformance failures and distinguish local fixtures from
hardware results. The D3D UMD and DWM work below is an intermediate milestone.

The November 12 instruction replaces the earlier Zink-first desktop plan:
**Windows D3D runtime → native UMD/DDI backend → Vulkan/NVK → Tegra KMD**,
without Zink or software rendering in the desktop path. The existing Gallium
`d3d10umd` implementation is excluded from the replacement. Claude is asked
to assess the concrete backend independently; desktop DXVK development is
already authorized as a candidate. The DX9/10/11 application route remains
DXVK over NVK, and Zink remains a later OpenGL compatibility option.

Implement the WDDM resource/synchronization/presentation work needed to
establish actual DWM rendering and basic desktop refresh on the GM20B LUID.
Preserve the existing result as a regression baseline. The replacement's
native DDI, shared-resource and sustained-composition paths require their own
implementation and validation. Do not enable Direct Flip before image
correctness and performance are established.

Autonomously analyze the WDDM contracts and the Mesa/NVK, Microsoft driver
samples, lfrb/mesa `wddm2` and deko3D sources, and implement the required changes
in the relevant upstream and downstream code. The GPU development and
publication repository is `ruje0504-2/mesa-switch-woa`, branch `main`.
Preserve upstream attribution and licensing, and update the published
status as implementation and evidence advance.
[Canonical GPU repository](https://github.com/ruje0504-2/mesa-switch-woa/tree/main)

WinX and Windows-for-Switch-Drivers remain companion firmware and board
integration repositories. Their GPU integration references this project.
[Repository relationships](../UPSTREAM.md)

Keep the 512 MiB dynamically allocated GPU budget and the existing
AC/DC display and sleep settings. Black-screen investigation belongs to
the separate task.

Use deko3D when comparing Maxwell command encoding, descriptors, graphics
state, synchronization and cache operations. Preserve its zlib notice and
identify adaptations when reusing source.
[deko3D upstream](https://github.com/devkitPro/deko3d)
[License](https://github.com/devkitPro/deko3d/blob/master/LICENSE)

## Development order

1. Replace the desktop translation path with a native Vulkan hardware backend
   without Zink or the existing Gallium `d3d10umd`.
   Build the Direct3D UMD device,
   rendering, shared-resource and synchronization paths needed by DWM on
   the GM20B LUID. Verify continuous window composition and landscape
   presentation. ARM64 Khronos `vkcube` through NVK's Win32 surface and
   swapchain remains a separate pending Vulkan test.
2. Direct3D 9, 10 and 11 game compatibility through DXVK over NVK, alongside `VK_EXT_memory_budget`,
   `VK_EXT_pageable_device_local_memory` and `VK_KHR_maintenance5`.
3. OpenGL compatibility through Mesa Zink over NVK.
4. Other Vulkan extensions.
5. Direct3D 12.

Use the [build and porting guide](PORTING.md), [driver contracts](DRIVER.md)
and [RADV Windows reuse notes](PORTING.md#radv-windows-reference) for details.
DXVK is the selected backend for the DX9/10/11 application path and a candidate
for the authorized desktop replacement. Zink is retained for later OpenGL
compatibility; the previous desktop-DXVK approval gate is superseded.
The native UMD and DWM requirements are tracked in the
[Direct3D plan](PORTING.md#direct3d-umd-and-dwm).
