/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_SUBMIT_ABI_H
#define TG_GPU_SUBMIT_ABI_H
#include <stdint.h>
#define TG_GPU_SUBMIT_ABI 1u
#define TG_GPU_SUBMIT_ABI_INDIRECT 2u
#define TG_GPU_SUBMIT_MAX_BYTES (64u*1024u)
/* A B1C0 NO_OPERATION packet carries QMD 0.6 and room to align it at capture. */
#define TG_GPU_QMD_WORDS 64u
#define TG_GPU_QMD_RECORD_WORDS (2u * TG_GPU_QMD_WORDS - 1u)
/* D3DKMTSubmitCommand private bytes: header (plus tg_gpu_submit_indirect when
 * version == TG_GPU_SUBMIT_ABI_INDIRECT) followed by command_bytes of linear
 * NVK push words. Dxgkrnl copies this payload into kernel storage; the KMD
 * snapshots it again for the entire asynchronous queue lifetime.
 * Commands/CommandLength still identify the originating GPU VA for the OS.
 * Indirect segments reference application GPU VA only; no CPU address,
 * physical address, privilege bit or kernel pointer is input.
 */
struct tg_gpu_submit_request {uint32_t version,size,command_bytes,reserved;};
struct tg_gpu_submit_indirect {uint64_t address;uint32_t bytes,suffix_offset;};
_Static_assert(sizeof(struct tg_gpu_submit_request)==16,"KMD/NVK submit ABI");
_Static_assert(sizeof(struct tg_gpu_submit_indirect)==16,"KMD/NVK indirect submit ABI");
#endif
