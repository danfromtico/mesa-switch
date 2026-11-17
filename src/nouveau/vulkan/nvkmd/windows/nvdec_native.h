/* SPDX-License-Identifier: MIT */
#ifndef TG_NVDEC_NATIVE_H
#define TG_NVDEC_NATIVE_H
#include "kmt.h"
#include "nvdec_escape_abi.h"
struct tg_nvdec_native {
   uint32_t adapter, device;
   uint64_t session;
};
#ifdef __cplusplus
extern "C" {
#endif
/* A real KMT device on the selected native adapter owns the decoder. These
 * calls do not advertise or implement the D3D11 video DDI. */
LONG tg_nvdec_native_open(struct tg_nvdec_native *, const struct tg_gpu_adapter_info *);
LONG tg_nvdec_native_call(struct tg_nvdec_native *, uint32_t command,
                         const void *input, uint32_t input_size,
                         void *output, uint32_t output_size);
/* On failure, handles remain available for actual cleanup, never zeroed to
 * turn a failed hardware stop into apparent success. */
LONG tg_nvdec_native_close(struct tg_nvdec_native *);
#ifdef __cplusplus
}
#endif
#endif
