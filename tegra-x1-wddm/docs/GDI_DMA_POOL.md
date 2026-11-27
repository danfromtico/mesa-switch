# Virtual GDI DMA pool investigation

The .120 trial changes the GDI context's `DmaBufferSegmentSet` from zero to
the existing paging aperture, segment 2 (`0x2`). It retains 256 allocation
entries, 232 private bytes, virtual addressing, and the .119 presentation
capabilities. The physical trial passed the previous startup boundary but
did not establish an active desktop display path.

## Evidence behind the change

The .119 durable record ends its successful startup milestones with GDI
context flags `6`. Its missing later records do not establish where the
machine failed: the last ETW flush preceded the GDI journal timestamp, and
the scheduler journal depends on a separate writer thread.

The source returns a zero DMA segment set for that context. Microsoft's
[context contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_contextinfo)
defines zero as the contiguous, write-combined DMA path; nonzero sets may
contain aperture segments only. Segment 2 is already used by the system
paging context and implements aperture map/unmap and PFN translation.

Inspection of the exact installed Windows binaries identifies this path:

1. `DXGCONTEXT::Initialize` creates the privileged DMA pool for the virtual
   GDI context. `EnsurePriviledgedDmaPool` passes the returned segment set
   through to `VidMmCreateDmaPool`.
2. `VIDMM_DMA_POOL` records that the context uses GPU virtual addressing.
3. `AddDmaBufferToPool` creates and opens a VidMm allocation when the segment
   set is nonzero. With zero, it instead allocates contiguous memory and
   leaves the VidMm allocation field unset.
4. The GPU virtual-address path subsequently maps and commits the DMA
   allocation. It dereferences that allocation while obtaining its paging
   queue. The zero-segment path does not supply the object it requires.

This gives a concrete incompatible combination to fix. Without a captured
exception, it does not establish the actual .119 faulting instruction.

Exact image SHA256 values, independently matched against the device:

- `dxgkrnl.sys`: `99d425e1c997eae32909fa4f37d6c8e2e846a5374c602295546c8be2547aa5e6`
- `dxgmms2.sys`: `94fa72e5cee778b7cd12bedf13550a78414231df8612547c74f0bc3ed11a62c5`

Relevant image RVAs are `dxgkrnl!EnsurePriviledgedDmaPool` at `0x134de8`,
`dxgmms2!VIDMM_DMA_POOL`'s constructor at `0x91628`, and
`dxgmms2!VIDMM_DMA_POOL::AddDmaBufferToPool` at `0x80b00`. The latter's
zero-segment branch starts at `0x80ef0`; GPU VA handling starts at
`0x81100`, with the allocation dereference at `0x811d0`.

## Validation

The original context source and recorded .119 link inputs reproduce the
unsigned .119 driver byte for byte. The .120 candidate changes only the
GDI DMA segment selection, adds a record of the returned segment set, and
uses its own versioned test paths.

The ARM64 context fixture ran on the Switch under the .59 diagnostic
driver. With the old context implementation it fails at the new aperture
requirement (exit 890); with the fix it passes (exit 0). These are controlled
model checks, not a native WDDM startup or rendering result.

The physical trial retains the existing single-start grant, automatic
rollback, 512 MiB allocation budget, and sleep/firmware configuration.

On 2026-11-10 the demand start returned success and Code 0. The durable
record confirms GDI's segment set `0x2`, subsequent GPU-virtual page-table
updates, TLB flushes and virtual submissions. ETW retains eight successful
paging submissions after GDI, with no scheduler recovery event in this
capture. The previously observed immediate reboot did not occur; the
adapter remained Code 0 until the planned rollback, about five minutes
after the request.

The interactive probe found GM20B LUID `000000000008ec5d` but zero active
display paths. The user observed a black panel while SSH still responded.
Thus Code 0 is a startup milestone, not proof of native presentation.
The finalized ETL contains 85,562 exported records. The next investigation
is creation of the desktop session view and its display path.

Automatic rollback removed .120 and restored .59 plus the baseline ACPI
table, then performed a normal restart. The final check confirmed .59
Code 0, no remaining test permit/grant/tasks, and an active BasicDisplay
path with LUID `00000000000053b0`, 720x1280 and rotation 2. Sixteen live
evidence files and eight recovery files were downloaded with device-side
SHA256 comparison into the local `render120-live` evidence directory.
