/* SPDX-License-Identifier: MIT */
#ifndef TG_NVDEC_ESCAPE_ABI_H
#define TG_NVDEC_ESCAPE_ABI_H
#include <stdint.h>

#define TG_NVDEC_ESCAPE_MAGIC 0x4344564eu
#define TG_NVDEC_ESCAPE_VERSION 0x00010001u
#define TG_NVDEC_ESCAPE_MAX_PAYLOAD (4u * 1024u * 1024u)
enum tg_nvdec_escape_command {
    TG_NVDEC_ESCAPE_LOAD = 1,
    TG_NVDEC_ESCAPE_CONFIGURE,
    TG_NVDEC_ESCAPE_DECODE,
    TG_NVDEC_ESCAPE_STATUS,
    TG_NVDEC_ESCAPE_CLOSE,
};

/* Real WDDM hDevice owns the session; session is a generation identifier,
 * never an OS allocation/resource handle. Payloads use nvdec_control.h.
 * One buffer contains max(input_bytes,output_bytes) after this header.
 * DDI delivery success is distinct from result and hardware completion. */
struct tg_nvdec_escape {
    uint32_t magic, version, size, command;
    uint32_t input_bytes, output_bytes;
    int32_t result;
    uint32_t reserved;
    uint64_t session;
};
_Static_assert(sizeof(struct tg_nvdec_escape) == 40, "NVDEC escape packet ABI");
#endif
