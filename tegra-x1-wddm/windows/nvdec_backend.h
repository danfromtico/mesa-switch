/* SPDX-License-Identifier: MIT
 *
 * Retained Windows NVDEC H.264 backend for coexisting transport integration.
 * Parent-owned register/clock interfaces are required; production DMA and
 * WDDM video allocation wiring remains incomplete. Only 1280x720, 8-bit 4:2:0
 * progressive H.264 is admitted. The replacement probe cannot start it.
 */
#ifndef TGD3D_NVDEC_BACKEND_H
#define TGD3D_NVDEC_BACKEND_H

#include <ntddk.h>
#include "../../src/nouveau/nvdec/nvdec.h"
#include "../platform/t214_nvdec.h"
#include "nvdec_memory.h"

struct tg_nvdec_win;

/* The parent retains its translated register views, actual clock lease and
 * channel/syncpoint reservations until Destroy succeeds. Raw aperture
 * pointers from the retired replacement probe are not an ownership contract.
 * Production DMA/transport wiring remains required before advertising video. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecCreate(const DXGKRNL_INTERFACE *dxgk,
                       const struct tg_nvdec_platform_io *io,
                       const struct tg_nvdec_platform_owner *owner,
                       UINT32 channel, UINT32 syncpt,
                       struct tg_nvdec_win **result);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecDestroy(struct tg_nvdec_win *win);
/* Provider stop: stop actual DMA without freeing allocations or requesting
 * a new clock lease. Caller holds the shared hardware/MC gate. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecQuiesce(struct tg_nvdec_win *win);

BOOLEAN TgNvdecFirmwareValid(const void *data, SIZE_T size,
                             const UINT8 expected_sha256[TG_NVDEC_SHA256_SIZE]);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecLoadFirmware(struct tg_nvdec_win *win, const void *data,
                             SIZE_T size,
                             const UINT8 expected_sha256[TG_NVDEC_SHA256_SIZE]);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecConfigure(struct tg_nvdec_win *win, UINT32 width, UINT32 height,
                          UINT32 *pitch, UINT32 *aligned_height,
                          UINT32 *frame_size);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecReleaseSurfaces(struct tg_nvdec_win *win);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecDecode(struct tg_nvdec_win *win,
                       const struct tg_nvdec_h264_pic *pic,
                       const UINT8 *bitstream, UINT32 bitstream_size,
                       const UINT32 *slice_offsets, UINT32 slice_count,
                       void *output, SIZE_T output_size,
                       struct tg_nvdec_status *status,
                       UINT32 *expected, UINT32 *observed);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecReset(struct tg_nvdec_win *win);
const char *TgNvdecLastError(struct tg_nvdec_win *win);
BOOLEAN TgNvdecFirmwareReady(struct tg_nvdec_win *win);
BOOLEAN TgNvdecSurfacesReady(struct tg_nvdec_win *win);
void TgNvdecGetConfig(struct tg_nvdec_win *win, UINT32 *width, UINT32 *height,
                      UINT32 *pitch, UINT32 *aligned_height, UINT32 *frame_size);

#endif
