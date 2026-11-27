/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 * NETB layout and bootstrap ABI follow Switchroot nvgpu 1ae0167d:
 * gk20a/gr_ctx_gk20a.{c,h}, gr_gk20a.{c,h}. See ../NOTICE for the MIT license.
 */
#include "gr_firmware.h"
#include <stdbool.h>

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned width(uint32_t id)
{
    if (id < 4) return 1; /* Falcon code is a byte stream, not padded in NETB. */
    if (id >= 15 && id <= 18) return 4;
    if (id == 4 || id == 6 || id == 7 || id == 28) return 8;
    return 12;
}
enum tg_gpu_gr_firmware_result tg_gpu_gr_netlist_parse(
    struct tg_gpu_gr_blob input, struct tg_gpu_gr_netlist *out)
{
    struct tg_gpu_gr_netlist v = {0};
    uint64_t seen = 0;
    uint32_t regions, i, id, size, offset;
    size_t table_end;
    if (!out) return TG_GPU_GR_FW_ARGUMENT;
    *out = v;
    if (!input.data) return TG_GPU_GR_FW_ARGUMENT;
    if (input.size < 8 || input.size > UINT32_MAX) return TG_GPU_GR_FW_FORMAT;
    v.version = le32(input.data);
    if (v.version != 0) return TG_GPU_GR_FW_FORMAT;
    regions = le32(input.data + 4);
    if (!regions || regions > (input.size - 8) / 12) return TG_GPU_GR_FW_FORMAT;
    table_end = 8 + (size_t)regions * 12;
    for (i = 0; i < regions; ++i) {
        const uint8_t *r = input.data + 8 + (size_t)i * 12;
        id = le32(r); size = le32(r + 4); offset = le32(r + 8);
        if (offset > input.size || size > input.size - offset ||
            (size && offset < table_end)) return TG_GPU_GR_FW_FORMAT;
        if (id >= TG_GPU_GR_REGION_COUNT) continue; /* Upstream ignores unknown regions. */
        if (seen & (UINT64_C(1) << id)) return TG_GPU_GR_FW_FORMAT;
        seen |= UINT64_C(1) << id;
        if (size % width(id) || (id >= 15 && id <= 18 && size != 4))
            return TG_GPU_GR_FW_FORMAT;
        v.region[id] = (struct tg_gpu_gr_blob){input.data + offset, size};
    }
    /* The firmware and four initialization streams are needed by a GR boot;
     * other regions include optional performance/context-register descriptions.
     */
    if ((seen & 0x780ff) != 0x780ff) return TG_GPU_GR_FW_FORMAT;
    for (i = 0; i < 8; ++i) if (!v.region[i].size) return TG_GPU_GR_FW_FORMAT;
    v.major = le32(v.region[15].data);
    v.buffer_size = le32(v.region[16].data);
    v.ctxsw_base = le32(v.region[17].data);
    v.number = le32(v.region[18].data);
    *out = v;
    return TG_GPU_GR_FW_OK;
}
static bool boot_prefix(unsigned falcon, uint32_t signature, uint32_t *prefix)
{
    if ((!falcon && (signature == 0x9121ab5c || signature == 0x9125ab5c)) ||
        (falcon == 1 && signature == 0x3d3d65e2)) { *prefix = 4; return true; }
    if ((!falcon && (signature == 0x93671b7d || signature == 0x4d6cbc10)) ||
        (falcon == 1 && signature == 0x393161da)) { *prefix = 0; return true; }
    return false;
}
static bool segment(uint32_t *cursor, size_t bytes, struct tg_gpu_gr_segment *s)
{
    uint32_t size;
    if (!bytes || bytes > UINT32_MAX - 3) return false;
    size = ((uint32_t)bytes + 3) & ~3u;
    if (size > UINT32_MAX - 255 || *cursor > UINT32_MAX - ((size + 255) & ~255u))
        return false;
    s->offset = *cursor; s->size = size;
    *cursor += (size + 255) & ~255u;
    return true;
}
static void copy(uint8_t *to, const uint8_t *from, size_t size)
{ for (size_t i = 0; i < size; ++i) to[i] = from[i]; }

enum tg_gpu_gr_firmware_result tg_gpu_gr_image_prepare(
    struct tg_gpu_gr_blob netlist, const struct tg_gpu_gr_blob boot[2],
    void *storage, size_t capacity, struct tg_gpu_gr_image *out)
{
    struct tg_gpu_gr_netlist n;
    struct tg_gpu_gr_image v = {0};
    uint32_t boot_bytes[2], prefix, cursor = 0;
    enum tg_gpu_gr_firmware_result result;
    if (!out) return TG_GPU_GR_FW_ARGUMENT;
    *out = v;
    if (!boot || (!storage && capacity)) return TG_GPU_GR_FW_ARGUMENT;
    result = tg_gpu_gr_netlist_parse(netlist, &n);
    if (result != TG_GPU_GR_FW_OK) return result;
    for (unsigned i = 0; i < 2; ++i) {
        struct tg_gpu_gr_falcon_image *f = &v.falcon[i];
        if (!boot[i].data) return TG_GPU_GR_FW_ARGUMENT;
        if (boot[i].size < 16) return TG_GPU_GR_FW_FORMAT;
        boot_bytes[i] = le32(boot[i].data + 4);
        f->imem_offset = le32(boot[i].data + 8);
        f->entry = le32(boot[i].data + 12);
        if (!boot_bytes[i] || boot_bytes[i] > boot[i].size - 16 ||
            (f->imem_offset & 255) || f->imem_offset >= 0x20000 ||
            boot_bytes[i] > 0x20000 - f->imem_offset ||
            f->entry < f->imem_offset || f->entry - f->imem_offset >= boot_bytes[i])
            return TG_GPU_GR_FW_FORMAT;
        /* The descriptor's start_offset is not used by Switchroot. The boot
         * bytes immediately follow its four words; later file padding is not
         * part of the signature. Partial words are zero-padded like kzalloc.
         */
        for (uint32_t j = 0; j < boot_bytes[i]; ++j)
            f->boot_signature += (uint32_t)boot[i].data[16 + j] << ((j & 3) * 8);
        if (!boot_prefix(i, f->boot_signature, &prefix)) return TG_GPU_GR_FW_BOOT_VERSION;
        if (!segment(&cursor, boot_bytes[i], &f->boot) ||
            !segment(&cursor, n.region[i * 2 + 1].size, &f->code) ||
            !segment(&cursor, n.region[i * 2].size, &f->data)) return TG_GPU_GR_FW_SIZE;
    }
    v.size = cursor;
    if (!storage) { *out = v; return TG_GPU_GR_FW_OK; }
    if (capacity < v.size) return TG_GPU_GR_FW_SIZE;
    for (uint32_t i = 0; i < v.size; ++i) ((uint8_t *)storage)[i] = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const struct tg_gpu_gr_falcon_image *f = &v.falcon[i];
        copy((uint8_t *)storage + f->boot.offset, boot[i].data + 16, boot_bytes[i]);
        copy((uint8_t *)storage + f->code.offset, n.region[i * 2 + 1].data, n.region[i * 2 + 1].size);
        copy((uint8_t *)storage + f->data.offset, n.region[i * 2].data, n.region[i * 2].size);
    }
    *out = v;
    return TG_GPU_GR_FW_OK;
}
enum tg_gpu_gr_firmware_result tg_gpu_gr_boot_header(
    const struct tg_gpu_gr_image *image, unsigned falcon, uint64_t gpu_va,
    uint32_t words[17], uint32_t *count)
{
    const struct tg_gpu_gr_falcon_image *f;
    const struct tg_gpu_gr_segment *parts[3];
    uint32_t prefix;
    if (!count || !words) return TG_GPU_GR_FW_ARGUMENT;
    *count = 0;
    for (unsigned i = 0; i < 17; ++i) words[i] = 0;
    if (!image || falcon > 1 || !image->size) return TG_GPU_GR_FW_ARGUMENT;
    if (!gpu_va || (gpu_va & 255) || gpu_va >= (UINT64_C(1) << 40) ||
        image->size > (UINT64_C(1) << 40) - gpu_va) return TG_GPU_GR_FW_ADDRESS;
    f = &image->falcon[falcon];
    parts[0] = &f->boot; parts[1] = &f->code; parts[2] = &f->data;
    for (unsigned i = 0; i < 3; ++i)
        if (!parts[i]->size || (parts[i]->size & 3) || (parts[i]->offset & 255) ||
            parts[i]->offset > image->size || parts[i]->size > image->size - parts[i]->offset)
            return TG_GPU_GR_FW_FORMAT;
    if (!boot_prefix(falcon, f->boot_signature, &prefix)) return TG_GPU_GR_FW_BOOT_VERSION;
    words[prefix + 4] = 4; /* Virtual-DMA context selected by the T21x bootloader. */
    words[prefix + 5] = (uint32_t)((gpu_va + f->code.offset) >> 8);
    words[prefix + 7] = f->code.size;
    words[prefix + 11] = (uint32_t)((gpu_va + f->data.offset) >> 8);
    words[prefix + 12] = f->data.size;
    *count = prefix + 13;
    return TG_GPU_GR_FW_OK;
}
