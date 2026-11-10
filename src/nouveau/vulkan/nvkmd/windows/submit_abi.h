/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_SUBMIT_ABI_H
#define TG_GPU_SUBMIT_ABI_H
#include <stdint.h>
#define TG_GPU_SUBMIT_ABI 1u
#define TG_GPU_SUBMIT_MAX_BYTES (64u*1024u)
/* D3DKMTSubmitCommand private bytes: header followed by command_bytes of
 * linear NVK push words. Dxgkrnl copies this payload into kernel storage;
 * the KMD snapshots it again for the entire asynchronous queue lifetime.
 * Commands/CommandLength still identify the originating GPU VA for the OS.
 * No CPU address, physical address, privilege bit or kernel pointer is input.
 */
struct tg_gpu_submit_request {uint32_t version,size,command_bytes,reserved;};
_Static_assert(sizeof(struct tg_gpu_submit_request)==16,"KMD/NVK submit ABI");
#endif
