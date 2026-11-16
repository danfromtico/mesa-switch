/* SPDX-License-Identifier: MIT
 *
 * Windows kernel bridge for the T210 NVDEC H.264 backend.  This is the
 * bring-up transport used by the standalone acceptance tool; it is not a
 * WDDM video allocation contract.  Only 1280x720, 8-bit 4:2:0 progressive
 * H.264 is admitted.
 */
#ifndef TGD3D_NVDEC_BACKEND_H
#define TGD3D_NVDEC_BACKEND_H

#include <ntddk.h>
#include "../../src/nouveau/nvdec/nvdec.h"

struct tg_nvdec_win;

NTSTATUS TgNvdecCreate(volatile UINT32 *nvdec_regs,
                                    volatile UINT32 *host1x_regs,
                                    volatile UINT32 *pmc_regs,
                                    volatile UINT32 *car_regs,
                                    volatile UINT32 *mc_regs,
                                    struct tg_nvdec_win **result);
NTSTATUS TgNvdecDestroy(struct tg_nvdec_win *win);

BOOLEAN TgNvdecFirmwareValid(const void *data, SIZE_T size,
                             const UINT8 expected_sha256[TG_NVDEC_SHA256_SIZE]);

NTSTATUS TgNvdecLoadFirmware(struct tg_nvdec_win *win, const void *data,
                             SIZE_T size,
                             const UINT8 expected_sha256[TG_NVDEC_SHA256_SIZE]);

NTSTATUS TgNvdecConfigure(struct tg_nvdec_win *win, UINT32 width, UINT32 height,
                          UINT32 *pitch, UINT32 *aligned_height,
                          UINT32 *frame_size);
NTSTATUS TgNvdecReleaseSurfaces(struct tg_nvdec_win *win);

NTSTATUS TgNvdecDecode(struct tg_nvdec_win *win,
                       const struct tg_nvdec_h264_pic *pic,
                       const UINT8 *bitstream, UINT32 bitstream_size,
                       const UINT32 *slice_offsets, UINT32 slice_count,
                       void *output, SIZE_T output_size,
                       struct tg_nvdec_status *status,
                       UINT32 *expected, UINT32 *observed);
NTSTATUS TgNvdecReset(struct tg_nvdec_win *win);
const char *TgNvdecLastError(struct tg_nvdec_win *win);
BOOLEAN TgNvdecFirmwareReady(struct tg_nvdec_win *win);
BOOLEAN TgNvdecSurfacesReady(struct tg_nvdec_win *win);
void TgNvdecGetConfig(struct tg_nvdec_win *win, UINT32 *width, UINT32 *height,
                      UINT32 *pitch, UINT32 *aligned_height, UINT32 *frame_size);

#endif
