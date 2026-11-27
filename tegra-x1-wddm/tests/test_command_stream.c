/* SPDX-License-Identifier: MIT */
#include "../core/command_stream.h"
#include "../core/nvk_mme_programs.h"
#include "../core/submission.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"
#include "nouveau/headers/nvidia/classes/clb197.h"
#include "nouveau/headers/nvidia/classes/clb1c0.h"
#include "nouveau/headers/nvidia/classes/clb1c0qmd.h"
#include "nouveau/vulkan/nvk_mme_slots.h"
#include "nouveau/vulkan/nvkmd/windows/submit_abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_mme_upload(void)
{
    enum { MME_WORDS = sizeof(tg_gpu_nvk_mme_init) / sizeof(uint32_t) };
    uint32_t stream[2 + MME_WORDS + 1] = {0x20010000, 0xb197};
    memcpy(stream + 2, tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    assert(tg_gpu_validate_command_stream(stream, sizeof(stream)));
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream) - 2 * sizeof(uint32_t)));

    stream[2 + MME_WORDS] = 0x80000045; /* Immediate write to instruction RAM pointer. */
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream)));
    stream[2 + MME_WORDS] = 0;

    stream[3] ^= 1; /* First macro slot. */
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream)));
    stream[3] ^= 1;
    stream[7] ^= 1; /* First instruction. */
    assert(!tg_gpu_validate_command_stream(stream, sizeof(stream)));

    const uint32_t span[] = {0x20060044, 0, 0, 0, 0, 0, 0}; /* 0x110 through 0x124. */
    const uint32_t oneinc[] = {0xa0020044, 0, 0}; /* 0x110 and 0x114. */
    const uint32_t nonincr[] = {0x60010046, 0}; /* Instruction RAM alone. */
    assert(!tg_gpu_validate_command_stream(span, sizeof(span)));
    assert(!tg_gpu_validate_command_stream(oneinc, sizeof(oneinc)));
    assert(!tg_gpu_validate_command_stream(nonincr, sizeof(nonincr)));
}

static bool check_mme_call(uint32_t slot, uint32_t count, uint32_t first, uint32_t second)
{
    uint32_t packet[7] = {
        0xa0000000u | (count << 16) | (NVB197_CALL_MME_MACRO(slot) >> 2),
        first, second,
    };
    assert(count < sizeof(packet) / sizeof(*packet));
    return tg_gpu_validate_command_stream(packet, (count + 1) * sizeof(*packet));
}

static void test_mme_calls(void)
{
    assert(check_mme_call(NVK_MME_DRAW, 5, 0, 3));
    assert(check_mme_call(NVK_MME_DRAW_INDEXED, 6, 0, 3));
    assert(!check_mme_call(NVK_MME_DRAW_INDEXED, 5, 0, 3));
    assert(!check_mme_call(NVK_MME_SET_PRIV_REG, 3, 0, 0));
    assert(!check_mme_call(NVK_MME_COUNT, 1, 0, 0));

    assert(check_mme_call(NVK_MME_CLEAR, 2, 2047u << 10, 1));
    assert(!check_mme_call(NVK_MME_CLEAR, 1, 0x3c, 0));
    assert(!check_mme_call(NVK_MME_CLEAR, 2, 2047u << 10, 2));
    assert(check_mme_call(NVK_MME_SET_WRITE_MASK, 2, 8, 0));
    assert(!check_mme_call(NVK_MME_SET_WRITE_MASK, 2, 9, 0));
    assert(check_mme_call(NVK_MME_SET_VIEWPORT_MIN_MAX_Z, 3, 15, 0));
    assert(!check_mme_call(NVK_MME_SET_VIEWPORT_MIN_MAX_Z, 3, 16, 0));

    const uint32_t immediate = 0x80000e00; /* CALL_MME_MACRO(0) requires ONEINCR. */
    const uint32_t data_only[] = {0xa0010e01, 0}; /* CALL_MME_DATA(0). */
    const uint32_t span[] = {0x20020dff, 0, 0}; /* Increment into the call range. */
    assert(!tg_gpu_validate_command_stream(&immediate, sizeof(immediate)));
    assert(!tg_gpu_validate_command_stream(data_only, sizeof(data_only)));
    assert(!tg_gpu_validate_command_stream(span, sizeof(span)));
}

static void test_mme_order(void)
{
    const uint32_t call[] = {0xa0010000u | (NVB197_CALL_MME_MACRO(NVK_MME_SELECT_CB0) >> 2), 0};
    uint32_t stream[(sizeof(tg_gpu_nvk_mme_init) + sizeof(call)) / sizeof(uint32_t)];
    struct tg_gpu_command_info info;

    assert(tg_gpu_inspect_command_stream(call, sizeof(call), NULL, &info));
    assert(info.requires_mme && !info.uploads_mme);

    memcpy(stream, tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    memcpy(stream + sizeof(tg_gpu_nvk_mme_init) / sizeof(*stream), call, sizeof(call));
    assert(tg_gpu_inspect_command_stream(stream, sizeof(stream), NULL, &info));
    assert(!info.requires_mme && info.uploads_mme);

    memcpy(stream, call, sizeof(call));
    memcpy(stream + sizeof(call) / sizeof(*stream), tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    assert(tg_gpu_inspect_command_stream(stream, sizeof(stream), NULL, &info));
    assert(info.requires_mme && info.uploads_mme);
}

#define INCR(subchannel, method, count) (0x20000000u | ((subchannel) << 13) | ((count) << 16) | ((method) >> 2))
#define NONINCR(subchannel, method, count) (0x60000000u | ((subchannel) << 13) | ((count) << 16) | ((method) >> 2))
#define IMMD(subchannel, method, data) (0x80000000u | ((subchannel) << 13) | ((data) << 16) | ((method) >> 2))
#define ONEINCR(subchannel, method, count) (0xa0000000u | ((subchannel) << 13) | ((count) << 16) | ((method) >> 2))
#define CE_INCR(method, count) INCR(4, NVB0B5_##method, count)
#define CE_IMMD(method, data) IMMD(4, NVB0B5_##method, data)

static void test_copy_stream(void)
{
    const uint32_t dma = NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
        NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE);
    const uint32_t multi = NVDEF(NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE);
    const uint32_t src_pitch = NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH);
    const uint32_t dst_pitch = NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH);
    const uint32_t remap = NVDEF(NVB0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE);
    struct tg_gpu_command_va va = {0x50460, 0x20000, 0x30000};
    struct tg_gpu_command_info info;
    uint32_t pitch[] = {
        CE_INCR(OFFSET_IN_UPPER, 8), 0, 0x10000, 0, 0x50000, 256, 512, 96, 3,
        CE_IMMD(LAUNCH_DMA, dma | multi | src_pitch | dst_pitch),
    };
    assert(tg_gpu_inspect_command_stream(pitch, sizeof(pitch), &va, &info));
    assert(info.va_end == va.limit && info.reserved_base == va.reserved_base);
    va.limit--;
    assert(!tg_gpu_inspect_command_stream(pitch, sizeof(pitch), &va, &info));
    va.limit++;
    pitch[4] = 0x1fff0;
    assert(!tg_gpu_inspect_command_stream(pitch, sizeof(pitch), &va, &info));
    pitch[4] = 0x50000;

    uint32_t tiled[] = {
        CE_INCR(OFFSET_IN_UPPER, 8), 0, 0x10000, 0, 0x50000, 128, 128, 64, 3,
        CE_INCR(SET_SRC_BLOCK_SIZE, 6), 0x1110, 128, 13, 3, 2, (5u << 16) | 32,
        CE_IMMD(LAUNCH_DMA, dma | multi | dst_pitch),
    };
    assert(tg_gpu_inspect_command_stream(tiled, sizeof(tiled), &va, &info));
    assert(info.va_end == 0x50140);
    const struct tg_gpu_command_va tile_padding = {0x60000, 0x11f00, 0x12000};
    assert(!tg_gpu_inspect_command_stream(tiled, sizeof(tiled), &tile_padding, &info));
    tiled[14] = 3; /* Layer must be below depth. */
    assert(!tg_gpu_inspect_command_stream(tiled, sizeof(tiled), &va, &info));

    const uint32_t fill[] = {
        CE_INCR(SET_REMAP_CONST_A, 1), 0x12345678,
        CE_INCR(SET_REMAP_COMPONENTS, 1), 0x34444,
        CE_INCR(OFFSET_OUT_UPPER, 6), 0, 0x50000, 512, 512, 64, 2,
        CE_IMMD(LAUNCH_DMA, dma | multi | src_pitch | dst_pitch | remap),
    };
    assert(tg_gpu_inspect_command_stream(fill, sizeof(fill), &va, &info));
    assert(info.va_end == 0x50300);

    const uint32_t release[] = {
        CE_INCR(SET_SEMAPHORE_A, 3), 0, 0x50000, 1,
        CE_IMMD(LAUNCH_DMA, NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_TYPE, RELEASE_FOUR_WORD_SEMAPHORE)),
    };
    assert(tg_gpu_inspect_command_stream(release, sizeof(release), &va, &info));
    assert(info.va_end == 0x50010);

    const uint32_t barrier[] = {
        CE_INCR(LINE_LENGTH_IN, 2), 0, 0,
        CE_IMMD(LAUNCH_DMA, dma | src_pitch | dst_pitch | remap),
    };
    assert(tg_gpu_inspect_command_stream(barrier, sizeof(barrier), &va, &info));
    assert(!info.va_end);

    const uint32_t phys_method[] = {CE_INCR(SET_SRC_PHYS_MODE, 1), 0};
    assert(!tg_gpu_validate_command_stream(phys_method, sizeof(phys_method)));
    assert(!tg_gpu_validate_command_stream(pitch + 9, sizeof(uint32_t))); /* Missing operands. */
    pitch[3] = 0xff;
    pitch[4] = 0xfffffff0;
    assert(!tg_gpu_validate_command_stream(pitch, sizeof(pitch))); /* 40-bit end overflow. */
    const uint32_t phys_launch[] = {
        CE_INCR(LAUNCH_DMA, 1), NVDEF(NVB0B5, LAUNCH_DMA, DST_TYPE, PHYSICAL),
    };
    assert(!tg_gpu_validate_command_stream(phys_launch, sizeof(phys_launch)));
}

struct range_checks {
    struct tg_gpu_texture_pool_range expected[8];
    enum tg_gpu_pitch_access access[8];
    unsigned next;
};

static bool check_range(void *data, uint64_t base, uint64_t end, enum tg_gpu_pitch_access access)
{
    struct range_checks *checks = data;
    if (checks->next >= 8)
        return false;
    const unsigned index = checks->next++;
    const struct tg_gpu_texture_pool_range *range = &checks->expected[index];
    return base == range->base && end == range->end && access == checks->access[index];
}

static void test_inline_upload(void)
{
    const uint32_t launch = NVDEF(NVB197, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
                            NVDEF(NVB197, LAUNCH_DMA, COMPLETION_TYPE, FLUSH_ONLY);
    const struct tg_gpu_command_va va = {0x50000, 0x20000, 0x30000};
    struct tg_gpu_command_info info;
    uint32_t upload[] = {
        INCR(0, NVB197_LINE_LENGTH_IN, 4), 8, 1, 0, 0x1fff8,
        ONEINCR(0, NVB197_LAUNCH_DMA, 3), launch, 0x20010014, 0x55aa, 0,
    };
    assert(tg_gpu_inspect_command_stream(upload, sizeof(upload), &va, &info));
    assert(info.va_end == va.reserved_base);
    struct tg_gpu_program_state programs = {0};
    struct range_checks checks = {.expected = {{0x1fff8, 0x20000}}, .access = {TG_GPU_PITCH_WRITE}};
    assert(tg_gpu_check_command_mappings(upload, sizeof(upload), &programs, check_range, &checks));
    assert(checks.next == 1);
    upload[0] = INCR(1, NVB197_LINE_LENGTH_IN, 4);
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload))); /* Wrong launch subchannel. */
    upload[5] = ONEINCR(1, NVB197_LAUNCH_DMA, 3);
    assert(tg_gpu_inspect_command_stream(upload, sizeof(upload), &va, &info));
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload) - 2 * sizeof(uint32_t)));
    assert(!tg_gpu_validate_command_stream(upload + 5, 4 * sizeof(uint32_t)));
    upload[1] = 4;
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload))); /* Length and payload differ. */
    upload[1] = 8;
    upload[4] += 4;
    assert(!tg_gpu_inspect_command_stream(upload, sizeof(upload), &va, &info));
    upload[4] -= 4;
    upload[6] = NVDEF(NVB197, LAUNCH_DMA, COMPLETION_TYPE, RELEASE_SEMAPHORE);
    assert(!tg_gpu_validate_command_stream(upload, sizeof(upload)));
}

static void test_report_and_acquire(void)
{
    const struct tg_gpu_command_va va = {0x50000, 0x20000, 0x30000};
    struct tg_gpu_command_info info;
    uint32_t report[] = {
        INCR(0, NVB197_SET_REPORT_SEMAPHORE_A, 4), 0, 0x4fff0, 0,
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, REPORT_ONLY) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, REPORT, ZPASS_PIXEL_CNT64),
    };
    assert(tg_gpu_inspect_command_stream(report, sizeof(report), &va, &info));
    assert(info.va_end == va.limit);
    report[4] |= NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, ONE_WORD);
    assert(!tg_gpu_validate_command_stream(report, sizeof(report)));
    report[0] = INCR(1, NVB197_SET_REPORT_SEMAPHORE_A, 4);
    assert(!tg_gpu_validate_command_stream(report, sizeof(report))); /* A graphics query on compute. */
    report[4] = 0; /* Compute release with a four-word report. */
    assert(tg_gpu_inspect_command_stream(report, sizeof(report), &va, &info));
    report[2] = va.reserved_base;
    assert(!tg_gpu_inspect_command_stream(report, sizeof(report), &va, &info));

    uint32_t macro[] = {ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_WRITE_CS_INVOCATIONS), 2), 0, 0x4fff8};
    assert(tg_gpu_inspect_command_stream(macro, sizeof(macro), &va, &info));
    assert(info.requires_mme && info.va_end == va.limit);
    macro[2] += 4;
    assert(!tg_gpu_inspect_command_stream(macro, sizeof(macro), &va, &info));

    const uint32_t acquire_flags = NVDEF(NVB06F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED) |
                                   NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);
    uint32_t wait[] = {
        INCR(4, NVB06F_SEMAPHOREA, 4), 0, 0x4fffc, 1,
        acquire_flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQUIRE),
    };
    assert(tg_gpu_inspect_command_stream(wait, sizeof(wait), &va, &info));
    assert(info.va_end == va.limit);
    wait[4] = acquire_flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQ_GEQ);
    assert(tg_gpu_inspect_command_stream(wait, sizeof(wait), &va, &info));
    wait[4] = acquire_flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE);
    assert(!tg_gpu_validate_command_stream(wait, sizeof(wait)));
}

static void test_texture_pools(void)
{
    const struct tg_gpu_command_va va = {0x100050000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    uint32_t pool[] = {INCR(0, NVB197_SET_TEX_HEADER_POOL_A, 3), 1, 0x40000, 2047};
    assert(tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));
    assert(info.va_end == va.limit &&
           info.texture_pool[TG_GPU_PROGRAM_3D][TG_GPU_TEXTURE_HEADERS].base == 0x100040000 &&
           info.texture_pool[TG_GPU_PROGRAM_3D][TG_GPU_TEXTURE_HEADERS].end == va.limit);
    pool[3]++;
    assert(!tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));

    pool[0] = INCR(1, NVB1C0_SET_TEX_SAMPLER_POOL_A, 3);
    pool[2] = 0x1ffe0;
    pool[3] = 0;
    assert(tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));
    assert(info.va_end == va.reserved_base &&
           info.texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_SAMPLERS].end == va.reserved_base &&
           !info.texture_pool[TG_GPU_PROGRAM_3D][TG_GPU_TEXTURE_SAMPLERS].end);
    pool[3] = 1;
    assert(!tg_gpu_inspect_command_stream(pool, sizeof(pool), &va, &info));
    pool[3] = DRF_MASK(NVB1C0_SET_TEX_SAMPLER_POOL_C_MAXIMUM_INDEX) + 1;
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));
    pool[3] = 0;
    pool[2] |= 4;
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));
    pool[2] &= ~4u;
    pool[1] = 0x100;
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));
    pool[1] = 1;
    pool[0] = ONEINCR(1, NVB1C0_SET_TEX_SAMPLER_POOL_A, 3);
    assert(!tg_gpu_validate_command_stream(pool, sizeof(pool)));

    const uint32_t standalone = IMMD(0, NVB197_SET_TEX_HEADER_POOL_C, 0);
    const uint32_t partial[] = {INCR(1, NVB1C0_SET_TEX_SAMPLER_POOL_B, 2), 0x10000, 0};
    assert(!tg_gpu_validate_command_stream(&standalone, sizeof(standalone)));
    assert(!tg_gpu_validate_command_stream(partial, sizeof(partial)));

    const uint32_t bind = INCR(0, NVB197_SET_TEX_HEADER_POOL_A, 3);
    uint32_t rebind[] = {
        bind, 1, 0x10000, 3,
        NONINCR(0, NVB197_NO_OPERATION, 4), bind, 1, 0x1ffe0, 0,
        bind, 1, 0x40000, 0,
    };
    struct range_checks checks = {.expected = {
        {0x100010000, 0x100010080}, {0x100040000, 0x100040020},
    }};
    struct tg_gpu_program_state programs = {0};
    assert(tg_gpu_prepare_command_stream(rebind, sizeof(rebind), &va, &info));
    assert(tg_gpu_check_command_mappings(rebind, sizeof(rebind), &programs, check_range, &checks));
    assert(checks.next == 2 && info.texture_pool[TG_GPU_PROGRAM_3D][TG_GPU_TEXTURE_HEADERS].end ==
           checks.expected[1].end);
    checks.next = 0;
    checks.expected[0].end = 0;
    assert(!tg_gpu_check_command_mappings(rebind, sizeof(rebind), &programs, check_range, &checks));
    assert(checks.next == 1);
}

static void test_compute_dispatch(void)
{
    struct tg_gpu_command_va va = {0x100050000, 0x100020080, 0x100030000};
    struct tg_gpu_command_info info;
    const uint32_t launch = NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) |
                            NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE);
    uint32_t dispatch[] = {
        INCR(1, NVB1C0_SEND_PCAS_A, 1), 0x10004ff,
        IMMD(1, NVB1C0_SEND_SIGNALING_PCAS_B, launch),
    };
    assert(tg_gpu_inspect_command_stream(dispatch, sizeof(dispatch), &va, &info));
    assert(info.va_end == va.limit);
    va.limit--;
    assert(!tg_gpu_inspect_command_stream(dispatch, sizeof(dispatch), &va, &info));
    va.limit++;
    dispatch[1] = 0x1000200;
    assert(!tg_gpu_inspect_command_stream(dispatch, sizeof(dispatch), &va, &info));
    dispatch[1] = 0x10004ff;
    assert(!tg_gpu_validate_command_stream(dispatch + 2, sizeof(uint32_t)));
    dispatch[2] = IMMD(1, NVB1C0_SEND_SIGNALING_PCAS_B, launch ^ 1u);
    assert(!tg_gpu_validate_command_stream(dispatch, sizeof(dispatch)));
    dispatch[2] = IMMD(0, NVB1C0_SEND_SIGNALING_PCAS_B, launch);
    assert(!tg_gpu_validate_command_stream(dispatch, sizeof(dispatch)));

    const uint32_t debug[] = {
        INCR(1, NVB1C0_SEND_PCAS_A, 1), 0x10004ff,
        INCR(1, NVB1C0_SEND_SIGNALING_PCAS_B, 1), launch,
    };
    assert(tg_gpu_inspect_command_stream(debug, sizeof(debug), &va, &info));
    assert(info.va_end == va.limit);
    assert(!tg_gpu_validate_command_stream(debug, sizeof(debug) - sizeof(uint32_t)));
    const uint32_t other_pcas[] = {INCR(1, NVB1C0_SEND_PCAS_B, 1), 0};
    assert(!tg_gpu_validate_command_stream(other_pcas, sizeof(other_pcas)));
}

static void test_input_bindings(void)
{
    const struct tg_gpu_command_va va = {0x100050000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    uint32_t vb[] = {ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_BIND_VB), 4),
                     31, 1, 0x4ff00, 0x100};
    assert(tg_gpu_inspect_command_stream(vb, sizeof(vb), &va, &info));
    assert(info.va_end == va.limit && info.requires_mme);
    vb[4]++;
    assert(!tg_gpu_inspect_command_stream(vb, sizeof(vb), &va, &info));
    vb[4]--;
    vb[1] = 32;
    assert(!tg_gpu_inspect_command_stream(vb, sizeof(vb), &va, &info));

    uint32_t ib[] = {ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_BIND_IB), 4),
                     1, 0x1fffc, 4, 0};
    assert(tg_gpu_inspect_command_stream(ib, sizeof(ib), &va, &info));
    assert(info.va_end == va.reserved_base);
    ib[3] += 2;
    assert(!tg_gpu_inspect_command_stream(ib, sizeof(ib), &va, &info));

    uint32_t empty[] = {
        INCR(0, TG_GPU_NVK_MME_ZERO_ADDR_HI, 2), 1, 0x4f000,
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_BIND_IB), 4), 0, 0, 128, 0,
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_BIND_VB), 4), 31, 1, 0x10000, 0,
    };
    assert(tg_gpu_inspect_command_stream(empty, sizeof(empty), &va, &info));
    assert(info.va_end == va.limit);
    assert(!tg_gpu_validate_command_stream(empty + 3, sizeof(empty) - 3 * sizeof(uint32_t)));
    empty[2] = 0x20000;
    assert(!tg_gpu_inspect_command_stream(empty, sizeof(empty), &va, &info));
    empty[2] = 0x4f000;
    empty[0] = INCR(0, NVB197_SET_VERTEX_STREAM_SUBSTITUTE_A, 2);
    assert(tg_gpu_inspect_command_stream(empty, 3 * sizeof(uint32_t), &va, &info));
    assert(info.va_end == va.limit);
    assert(!tg_gpu_validate_command_stream(empty, sizeof(empty))); /* MME reads its scratch pair. */
    empty[2] += 4;
    assert(!tg_gpu_validate_command_stream(empty, 3 * sizeof(uint32_t)));

    const uint32_t scratch_lo = IMMD(0, TG_GPU_NVK_MME_ZERO_ADDR_LO, 0);
    const uint32_t index_limit = IMMD(0, NVB197_SET_INDEX_BUFFER_D, 0);
    const uint32_t last_vb_limit[] = {INCR(0, NVB197_SET_VERTEX_STREAM_LIMIT_B_B(15), 1), 0};
    const uint32_t includes_location[] = {INCR(0, NVB197_SET_VERTEX_STREAM_A_FORMAT(3), 2), 0, 0};
    assert(!tg_gpu_validate_command_stream(&scratch_lo, sizeof(scratch_lo)));
    assert(!tg_gpu_validate_command_stream(&index_limit, sizeof(index_limit)));
    assert(!tg_gpu_validate_command_stream(last_vb_limit, sizeof(last_vb_limit)));
    assert(!tg_gpu_validate_command_stream(includes_location, sizeof(includes_location)));
    const uint32_t format_and_frequency[] = {
        IMMD(0, NVB197_SET_VERTEX_STREAM_A_FORMAT(0), 0),
        IMMD(0, NVB197_SET_VERTEX_STREAM_A_FREQUENCY(15), 1),
        INCR(0, NVB197_SET_VERTEX_STREAM_A_FREQUENCY(30), 2), 1, 0,
    };
    assert(tg_gpu_validate_command_stream(format_and_frequency, sizeof(format_and_frequency)));
}

static void test_color_targets(void)
{
    const struct tg_gpu_command_va va = {0x100050000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    uint32_t color[] = {
        INCR(0, NVB197_SET_COLOR_TARGET_A(7), 9), 1, 0x4e000, 128, 64,
        NVB197_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8,
        NVDEF(NVB197, SET_COLOR_TARGET_MEMORY, LAYOUT, PITCH), 1, 0, 0,
    };
    assert(tg_gpu_inspect_command_stream(color, sizeof(color), &va, &info));
    assert(info.va_end == va.limit);
    color[4]++;
    assert(!tg_gpu_inspect_command_stream(color, sizeof(color), &va, &info));

    color[2] = 0x4b000;
    color[3] = 32;
    color[4] = 33;
    color[6] = NVDEF(NVB197, SET_COLOR_TARGET_MEMORY, BLOCK_HEIGHT, FOUR_GOBS);
    color[7] = 2;
    color[8] = 0xc00;
    assert(tg_gpu_inspect_command_stream(color, sizeof(color), &va, &info));
    assert(info.va_end == va.limit);
    color[2] = 0x1e000;
    assert(!tg_gpu_inspect_command_stream(color, sizeof(color), &va, &info));
    color[2] = 0x4b000;
    color[8] = 0x400;
    assert(!tg_gpu_validate_command_stream(color, sizeof(color)));

    color[2] = 0x4f800;
    color[3] = 16;
    color[4] = 8;
    color[6] = NVDEF(NVB197, SET_COLOR_TARGET_MEMORY, BLOCK_DEPTH, TWO_GOBS) |
               NVDEF(NVB197, SET_COLOR_TARGET_MEMORY, THIRD_DIMENSION_CONTROL,
                     THIRD_DIMENSION_DEFINES_DEPTH_SIZE);
    color[7] = 3;
    assert(tg_gpu_inspect_command_stream(color, sizeof(color), &va, &info));
    assert(info.va_end == va.limit);
    color[2] += 512;
    assert(!tg_gpu_inspect_command_stream(color, sizeof(color), &va, &info));
    color[2] -= 512;
    color[5] = 0xd3; /* Unassigned Maxwell format. */
    assert(!tg_gpu_validate_command_stream(color, sizeof(color)));

    const uint32_t format_only = IMMD(0, NVB197_SET_COLOR_TARGET_FORMAT(7),
                                      NVB197_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8);
    assert(!tg_gpu_validate_command_stream(&format_only, sizeof(format_only)));
}

static void test_depth_targets(void)
{
    const struct tg_gpu_command_va va = {0x100050000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    uint32_t depth[] = {
        INCR(0, NVB197_SET_ZT_A, 5), 1, 0x45000, NVB197_SET_ZT_FORMAT_V_Z16,
        NVDEF(NVB197, SET_ZT_BLOCK_SIZE, HEIGHT, FOUR_GOBS), 0x1000,
        IMMD(0, NVB197_SET_ZT_SELECT, 1),
        INCR(0, NVB197_SET_ZT_SIZE_A, 3), 64, 65, 3,
        IMMD(0, NVB197_SET_ZT_LAYER, 2),
    };
    assert(tg_gpu_inspect_command_stream(depth, sizeof(depth), &va, &info));
    assert(info.va_end == va.limit);
    depth[2] += 0x800;
    assert(!tg_gpu_inspect_command_stream(depth, sizeof(depth), &va, &info));
    depth[2] -= 0x800;
    depth[11] = IMMD(0, NVB197_SET_ZT_LAYER, 3);
    assert(!tg_gpu_validate_command_stream(depth, sizeof(depth)));
    assert(!tg_gpu_validate_command_stream(depth, sizeof(depth) - sizeof(uint32_t)));

    const uint32_t debug[] = {
        INCR(0, NVB197_SET_ZT_A, 5), 1, 0x45000, NVB197_SET_ZT_FORMAT_V_Z16,
        NVDEF(NVB197, SET_ZT_BLOCK_SIZE, HEIGHT, FOUR_GOBS), 0x1000,
        INCR(0, NVB197_SET_ZT_SELECT, 1), 1,
        INCR(0, NVB197_SET_ZT_SIZE_A, 3), 64, 65, 3,
        INCR(0, NVB197_SET_ZT_LAYER, 1), 2,
    };
    assert(tg_gpu_inspect_command_stream(debug, sizeof(debug), &va, &info));
    assert(info.va_end == va.limit);
    assert(!tg_gpu_validate_command_stream(debug + 6, 2 * sizeof(uint32_t)));
    const uint32_t disable[] = {IMMD(0, NVB197_SET_ZT_SELECT, 0)};
    assert(tg_gpu_validate_command_stream(disable, sizeof(disable)));
}

static void test_constant_buffers(void)
{
    const struct tg_gpu_command_va va = {0x100050000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    const uint32_t select = ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_SELECT_CB0), 1);
    const uint32_t load = ONEINCR(0, NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 2);
    uint32_t root[] = {load, TG_GPU_NVK_DRAW_CB0_BYTES - 4, 0xdeadbeef};
    assert(tg_gpu_inspect_command_stream(root, sizeof(root), &va, &info));
    assert(info.requires_cb0 && !info.cb0_end);
    root[1] += 4;
    assert(!tg_gpu_validate_command_stream(root, sizeof(root)));
    root[1] -= 6;
    assert(!tg_gpu_validate_command_stream(root, sizeof(root)));

    uint32_t bind[] = {
        INCR(0, NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, 3), 0x100, 1, 0x4ff00,
        INCR(0, NVB197_BIND_GROUP_CONSTANT_BUFFER(4), 1), 0x11,
        select, 0,
    };
    assert(tg_gpu_inspect_command_stream(bind, sizeof(bind), &va, &info));
    assert(info.va_end == va.limit && info.requires_cb0 && info.requires_mme);
    struct tg_gpu_program_state programs = {0};
    struct range_checks checks = {.expected = {{0x10004ff00, 0x100050000}}, .access = {TG_GPU_PITCH_READ}};
    assert(tg_gpu_check_command_mappings(bind, sizeof(bind), &programs, check_range, &checks));
    assert(checks.next == 1);
    assert(!tg_gpu_validate_command_stream(bind, sizeof(bind) - 2 * sizeof(uint32_t)));
    bind[3] = 0x20000;
    assert(!tg_gpu_inspect_command_stream(bind, sizeof(bind), &va, &info));
    bind[3] = 0x4ff00;
    bind[5] = 1;
    assert(!tg_gpu_validate_command_stream(bind, sizeof(bind)));
    bind[5] = 0x11;
    assert(!tg_gpu_validate_command_stream(bind + 4, 2 * sizeof(uint32_t)));

    enum { MME_WORDS = sizeof(tg_gpu_nvk_mme_init) / sizeof(uint32_t) };
    const uint32_t setup[] = {
        INCR(0, TG_GPU_NVK_MME_CB0_ADDR_HI, 2), 1, 0x4f800,
        select, 0,
        IMMD(0, NVB197_BIND_GROUP_CONSTANT_BUFFER(4), 1),
        load, TG_GPU_NVK_DRAW_CB0_BYTES - 4, 0xdeadbeef,
    };
    uint32_t init[MME_WORDS + sizeof(setup) / sizeof(uint32_t)];
    memcpy(init, tg_gpu_nvk_mme_init, sizeof(tg_gpu_nvk_mme_init));
    memcpy(init + MME_WORDS, setup, sizeof(setup));
    assert(tg_gpu_inspect_command_stream(init, sizeof(init), &va, &info));
    assert(info.uploads_mme && !info.requires_mme && !info.requires_cb0);
    assert(info.cb0_end == va.limit && info.va_end == va.limit);
    assert(!tg_gpu_validate_command_stream(setup, sizeof(setup)));
    assert(!tg_gpu_validate_command_stream(init, (MME_WORDS + 3) * sizeof(uint32_t)));
    init[MME_WORDS + 2] += 256;
    assert(!tg_gpu_inspect_command_stream(init, sizeof(init), &va, &info));
}

static void test_program_regions(void)
{
    const struct tg_gpu_command_va va = {0x200040000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    uint32_t shader[] = {
        INCR(0, NVB197_SET_PROGRAM_REGION_A, 2), 1, 0x40000,
        IMMD(0, NVB197_SET_PIPELINE_PROGRAM(1), 0x30),
        INCR(0, NVB197_SET_PIPELINE_PROGRAM(5), 1), 0x1030,
    };
    assert(tg_gpu_inspect_command_stream(shader, sizeof(shader), &va, &info));
    assert(info.va_end == va.limit && info.programs.end[TG_GPU_PROGRAM_3D] == va.limit);
    assert(!info.requires_program && !info.programs.end[TG_GPU_PROGRAM_COMPUTE]);
    assert(tg_gpu_inspect_command_stream(shader + 3, sizeof(shader) - 3 * sizeof(uint32_t), &va, &info));
    assert(info.requires_program == (1u << TG_GPU_PROGRAM_3D));
    shader[2] += 4096;
    assert(!tg_gpu_inspect_command_stream(shader, sizeof(shader), &va, &info));
    shader[2] = 0x10000;
    assert(!tg_gpu_inspect_command_stream(shader, sizeof(shader), &va, &info));
    shader[2] = 0x40000;
    shader[5] = 0xffffffb0;
    assert(!tg_gpu_validate_command_stream(shader, sizeof(shader)));
    shader[5] = 0x1040;
    assert(!tg_gpu_validate_command_stream(shader, sizeof(shader)));

    const uint32_t region_only[] = {INCR(1, NVB1C0_SET_PROGRAM_REGION_B, 1), 0x40000};
    assert(!tg_gpu_validate_command_stream(region_only, sizeof(region_only)));
    const uint32_t dispatch[] = {
        INCR(1, NVB1C0_SET_PROGRAM_REGION_A, 2), 1, 0x40000,
        INCR(1, NVB1C0_SEND_PCAS_A, 1), 0x1000400,
        IMMD(1, NVB1C0_SEND_SIGNALING_PCAS_B, 3),
    };
    assert(tg_gpu_inspect_command_stream(dispatch, sizeof(dispatch), &va, &info));
    assert(info.programs.end[TG_GPU_PROGRAM_COMPUTE] == va.limit && !info.requires_program);
    assert(tg_gpu_inspect_command_stream(dispatch + 3, sizeof(dispatch) - 3 * sizeof(uint32_t), &va, &info));
    assert(info.requires_program == (1u << TG_GPU_PROGRAM_COMPUTE));

    uint32_t setup[] = {
        INCR(0, NVB197_SET_PROGRAM_REGION_A, 2), 1, 0x40000,
        IMMD(0, NVB197_SET_PIPELINE_SHADER(1), 0x11),
        IMMD(0, NVB197_SET_PIPELINE_PROGRAM(1), 0x30),
        INCR(0, NVB197_SET_PIPELINE_SHADER(5), 1), 0x51,
        INCR(0, NVB197_SET_PIPELINE_PROGRAM(5), 1), 0x1030,
    };
    struct range_checks checks = {.expected = {
        {0x100040030, 0x100040900}, {0x100041030, 0x100041900},
    }};
    struct tg_gpu_program_state programs = {0};
    assert(tg_gpu_prepare_command_stream(setup, sizeof(setup), &va, &info));
    assert(tg_gpu_check_command_mappings(setup, sizeof(setup), &programs, check_range, &checks));
    assert(!checks.next && programs.graphics_enabled == ((1u << 1) | (1u << 5)));
    const uint32_t draw = IMMD(0, NVB197_DRAW_VERTEX_ARRAY, 3);
    assert(tg_gpu_check_command_mappings(&draw, sizeof(draw), &programs, check_range, &checks));
    assert(checks.next == 2);

    /* A rebind before a draw must use the new entry. Failed validation
     * leaves the retired bindings intact. */
    uint32_t rebind[] = {INCR(0, NVB197_SET_PIPELINE_PROGRAM(5), 1), 0x3030, draw};
    assert(tg_gpu_prepare_command_stream(rebind, sizeof(rebind), &va, &info));
    checks.next = 0;
    assert(!tg_gpu_check_command_mappings(rebind, sizeof(rebind), &programs, check_range, &checks));
    assert(checks.next == 2 && programs.graphics_offset[5] == 0x1030);
    const uint32_t disable[] = {IMMD(0, NVB197_SET_PIPELINE_SHADER(5), 0x50), draw};
    checks.next = 0;
    assert(tg_gpu_check_command_mappings(disable, sizeof(disable), &programs, check_range, &checks));
    assert(checks.next == 1 && programs.graphics_enabled == (1u << 1));
}

static void test_shader_local_memory(void)
{
    const struct tg_gpu_command_va va = {0x100080000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    uint32_t gfx[] = {
        INCR(0, NVB197_SET_SHADER_LOCAL_MEMORY_A, 5), 1, 0x60000, 0, 0x20000, 0x400,
    };
    assert(tg_gpu_inspect_command_stream(gfx, sizeof(gfx), &va, &info));
    assert(info.va_end == va.limit);
    gfx[5] += 0x200;
    assert(!tg_gpu_validate_command_stream(gfx, sizeof(gfx)));

    uint32_t compute[] = {
        INCR(1, NVB1C0_SET_SHADER_LOCAL_MEMORY_A, 2), 1, 0x60000,
        INCR(1, NVB1C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A, 3), 0, 0x10000, 0xff,
        INCR(1, NVB1C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A, 3), 0, 0x10000, 0xff,
    };
    assert(tg_gpu_inspect_command_stream(compute, sizeof(compute), &va, &info));
    assert(info.va_end == va.limit);
    assert(!tg_gpu_validate_command_stream(compute, sizeof(compute) - 4 * sizeof(uint32_t)));
    assert(!tg_gpu_validate_command_stream(compute + 3, 4 * sizeof(uint32_t)));
    compute[9] += 0x8000;
    assert(!tg_gpu_validate_command_stream(compute, sizeof(compute)));
    compute[9] -= 0x8000;
    compute[2] = 0x1f000;
    assert(!tg_gpu_inspect_command_stream(compute, sizeof(compute), &va, &info));
}

static void test_captured_qmd(void)
{
    enum { PCAS = 1 + TG_GPU_QMD_RECORD_WORDS };
    const struct tg_gpu_command_va va = {0x300000000, 0x80000000, 0x80100000};
    struct tg_gpu_command_info info;
    uint32_t push[PCAS + 3] = {
        NONINCR(1, NVB1C0_NO_OPERATION, TG_GPU_QMD_RECORD_WORDS),
        [PCAS] = INCR(1, NVB1C0_SEND_PCAS_A, 1),
        [PCAS + 2] = IMMD(1, NVB1C0_SEND_SIGNALING_PCAS_B, 3),
    };
    uint32_t *qmd = push + 1;
#define SET_QMD(field, value) NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, field, value)
    SET_QMD(PROGRAM_OFFSET, 0xed80);
    SET_QMD(API_VISIBLE_CALL_LIMIT, 1);
    SET_QMD(CTA_RASTER_WIDTH, 20);
    SET_QMD(CTA_RASTER_HEIGHT, 30);
    SET_QMD(CTA_RASTER_DEPTH, 1);
    SET_QMD(QMD_VERSION, 6);
    SET_QMD(CTA_THREAD_DIMENSION0, 16);
    SET_QMD(CTA_THREAD_DIMENSION1, 8);
    SET_QMD(CTA_THREAD_DIMENSION2, 1);
    SET_QMD(L1_CONFIGURATION, 1);
    SET_QMD(REGISTER_COUNT, 24);
    SET_QMD(SASS_VERSION, 0x30);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CONSTANT_BUFFER_VALID, 0, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CONSTANT_BUFFER_ADDR_LOWER, 0, 0x30000);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CONSTANT_BUFFER_SIZE, 0, 2048);

    assert(tg_gpu_inspect_command_stream(push, sizeof(push), &va, &info));
    assert(info.va_end == 0x30800 && info.requires_program == (1u << TG_GPU_PROGRAM_COMPUTE) &&
           !info.required_compute_slm_per_tpc);
    assert(info.requires_texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_HEADERS] &&
           info.requires_texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_SAMPLERS]);
    uint32_t textured[8 + PCAS + 3] = {
        INCR(1, NVB1C0_SET_TEX_HEADER_POOL_A, 3), 0, 0x40000, 0,
        INCR(0, NVB197_SET_TEX_SAMPLER_POOL_A, 3), 0, 0x41000, 0,
    };
    memcpy(textured + 8, push, sizeof(push));
    assert(tg_gpu_inspect_command_stream(textured, sizeof(textured), &va, &info));
    assert(!info.requires_texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_HEADERS] &&
           info.requires_texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_SAMPLERS]);
    textured[4] = INCR(1, NVB1C0_SET_TEX_SAMPLER_POOL_A, 3);
    assert(tg_gpu_inspect_command_stream(textured, sizeof(textured), &va, &info));
    assert(!info.requires_texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_SAMPLERS]);
    memmove(textured + PCAS + 3, textured, 8 * sizeof(uint32_t));
    memcpy(textured, push, sizeof(push));
    assert(tg_gpu_inspect_command_stream(textured, sizeof(textured), &va, &info));
    assert(info.requires_texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_HEADERS] &&
           info.requires_texture_pool[TG_GPU_PROGRAM_COMPUTE][TG_GPU_TEXTURE_SAMPLERS]);
    uint32_t snapshot[PCAS + 3];
    memcpy(snapshot, push, sizeof(push));
    assert(tg_gpu_prepare_command_stream(snapshot, sizeof(snapshot), &va, &info));
    const uint64_t qmd_va = va.reserved_base + TG_GPU_COMMAND_QMD + 256;
    assert(info.has_qmd && qmd_va >= va.reserved_base + TG_GPU_COMMAND_CAPTURE + TG_GPU_COMMAND_CAPTURE_BYTES);
    assert(snapshot[PCAS + 1] == qmd_va >> 8 && !push[PCAS + 1]);
    assert(info.va_end == qmd_va + 256 && !memcmp(snapshot + TG_GPU_QMD_WORDS, qmd, 256));
    struct tg_gpu_program_state programs = {.end = {[TG_GPU_PROGRAM_COMPUTE] = 0x200000000}};
    struct range_checks checks = {
        .expected = {{0x10000ed80, 0x10000f600}, {0x30000, 0x30800}},
        .access = {TG_GPU_PITCH_READ_ONLY, TG_GPU_PITCH_READ},
    };
    /* The abandoned original offset must not drive the check. */
    NVVAL_MW_SET(snapshot + 1, NVB1C0, QMDV00_06, PROGRAM_OFFSET, 0x100);
    assert(tg_gpu_check_command_mappings(snapshot, sizeof(snapshot), &programs, check_range, &checks));
    assert(checks.next == 2);

    memcpy(snapshot, push, sizeof(push));
    assert(!tg_gpu_prepare_command_stream(snapshot, sizeof(snapshot) - 4, &va, NULL));
    struct tg_gpu_command_va short_va = va;
    short_va.reserved_end = qmd_va + 255;
    assert(!tg_gpu_prepare_command_stream(snapshot, sizeof(snapshot), &short_va, NULL));

    SET_QMD(SHADER_LOCAL_MEMORY_LOW_SIZE, 16);
    SET_QMD(SHADER_LOCAL_MEMORY_CRS_SIZE, 512);
    assert(tg_gpu_inspect_command_stream(push, sizeof(push), &va, &info));
    assert(info.required_compute_slm_per_tpc == 0x10000 && !info.compute_slm.va_end);

    uint32_t bound[11 + PCAS + 3] = {
        INCR(1, NVB1C0_SET_SHADER_LOCAL_MEMORY_A, 2), 0, 0x100000,
        INCR(1, NVB1C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A, 3), 0, 0x10000, 0xff,
        INCR(1, NVB1C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A, 3), 0, 0x10000, 0xff,
    };
    memcpy(bound + 11, push, sizeof(push));
    assert(tg_gpu_inspect_command_stream(bound, sizeof(bound), &va, &info));
    assert(info.compute_slm.bytes_per_tpc == 0x10000 && info.compute_slm.va_end == 0x120000 &&
           !info.required_compute_slm_per_tpc);
    bound[5] = bound[9] = 0x8000;
    assert(!tg_gpu_inspect_command_stream(bound, sizeof(bound), &va, NULL));

    SET_QMD(CTA_THREAD_DIMENSION0, 1024);
    assert(!tg_gpu_inspect_command_stream(push, sizeof(push), &va, NULL));
    SET_QMD(CTA_THREAD_DIMENSION0, 16);
    SET_QMD(RELEASE0_ADDRESS_LOWER, 0x10000);
    assert(!tg_gpu_inspect_command_stream(push, sizeof(push), &va, NULL));
    SET_QMD(RELEASE0_ADDRESS_LOWER, 0);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CONSTANT_BUFFER_ADDR_LOWER, 0, va.reserved_base - 256);
    assert(!tg_gpu_inspect_command_stream(push, sizeof(push), &va, NULL));

    push[PCAS + 1] = 0x300;
    assert(tg_gpu_inspect_command_stream(push + PCAS, 12, &va, NULL));
    assert(!tg_gpu_prepare_command_stream(push + PCAS, 12, &va, NULL));
#undef SET_QMD
}

static void test_native_umd_barrier(void)
{
    /* Exact 592-byte NVK_DEBUG=push stream from the .134 native D3D clear.
     * QMD data occupies the NO_OPERATION payload at DWORD 0x0b.  The final
     * MME-data invalidation used SET_REFERENCE, which is reserved for KMD
     * retirement.  Changing only that barrier must preserve QMD relocation.
     */
    uint32_t push[148] = {
        [0x00] = 0x20010040,
        [0x02] = 0x800020a6, [0x03] = 0x80002509, [0x04] = 0x80002091,
        [0x05] = 0x20020509,
        [0x08] = 0x90000369, [0x09] = 0x80012651, [0x0a] = 0x607f2040,
        [0x13] = 0x0000f480,
        [0x16] = 0x04000000, [0x17] = 0x00000004, [0x18] = 0x00010008,
        [0x1d] = 0x00100006, [0x1e] = 0x00010008, [0x1f] = 0x20000003,
        [0x28] = 0x027b0100, [0x29] = 0x04000000,
        [0x2a] = 0x027b0000, [0x2b] = 0x00800000,
        [0x39] = 0x18000000, [0x3a] = 0x30000000,
        [0x8a] = 0x200120ad,
        [0x8c] = 0x800320af, [0x8d] = 0x80002651,
        [0x8e] = 0x80142087, [0x8f] = 0x80142087,
        [0x90] = 0x800024a2, [0x91] = 0x901025a6,
        [0x92] = IMMD(0, NVB06F_SET_REFERENCE, 0),
        [0x93] = 0x800020a6,
    };
    const struct tg_gpu_command_va va = {TG_GPU_VA_LIMIT, 0x7000000000, 0x7004000000};
    struct tg_gpu_command_info info;
    assert(!tg_gpu_inspect_command_stream(push, sizeof(push), &va, NULL));
    push[0x92] = IMMD(0, NVB06F_WFI, NVB06F_WFI_SCOPE_ALL);
    uint32_t qmd[TG_GPU_QMD_WORDS];
    memcpy(qmd, push + 0x0b, sizeof(qmd));
    assert(tg_gpu_prepare_command_stream(push, sizeof(push), &va, &info));
    const uint64_t qmd_va = va.reserved_base + TG_GPU_COMMAND_QMD + 256;
    assert(info.has_qmd);
    assert(push[0x8b] == qmd_va >> 8);
    assert(info.va_end == qmd_va + 256);
    assert(info.requires_program == (1u << TG_GPU_PROGRAM_COMPUTE));
    assert(!memcmp(push + TG_GPU_QMD_WORDS, qmd, sizeof(qmd)));
}

static void test_indirect_and_xfb_streams(void)
{
    const struct tg_gpu_command_va va = {0x300000000, 0x80000000, 0x80100000};
    struct tg_gpu_command_info info;
    struct tg_gpu_program_state programs = {
        .end = {[TG_GPU_PROGRAM_3D] = 0x200000000, [TG_GPU_PROGRAM_COMPUTE] = 0x200000000},
        .graphics_enabled = 1u << 1,
        .graphics_offset = {[1] = 0x30},
    };

    /* Stream-out buffer bindings and conditional rendering. */
    uint32_t xfb_cond[] = {
        INCR(0, NVB197_SET_STREAM_OUT_BUFFER_ENABLE(0), 4), 1, 0, 0x50000, 64,
        INCR(0, NVB197_SET_STREAM_OUT_BUFFER_ENABLE(1), 4), 0, 0, 0, 0,
        INCR(0, NVB197_SET_GLOBAL_RENDER_ENABLE_A, 3), 0, 0x60000,
        NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_CONDITIONAL,
        INCR(0, NVB197_SET_GLOBAL_RENDER_ENABLE_A, 3), 0, 0,
        NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE,
    };
    assert(tg_gpu_inspect_command_stream(xfb_cond, sizeof(xfb_cond), &va, &info));
    assert(info.va_end == 0x60020);
    struct range_checks xfb_checks = {
        .expected = {{0x50000, 0x50040}, {0x60000, 0x60020}},
        .access = {TG_GPU_PITCH_WRITE, TG_GPU_PITCH_READ_ONLY},
    };
    assert(tg_gpu_check_command_mappings(xfb_cond, sizeof(xfb_cond), &programs, check_range, &xfb_checks));
    assert(xfb_checks.next == 2);
    xfb_cond[3] = 0x50002; /* unaligned XFB address */
    assert(!tg_gpu_validate_command_stream(xfb_cond, sizeof(xfb_cond)));
    xfb_cond[3] = 0x50000;
    xfb_cond[8] = 0x50000; /* disabled slot must zero address */
    assert(!tg_gpu_validate_command_stream(xfb_cond, sizeof(xfb_cond)));
    xfb_cond[8] = 0;
    xfb_cond[12] = 0x60008; /* conditional render enable requires 16B alignment */
    assert(!tg_gpu_validate_command_stream(xfb_cond, sizeof(xfb_cond)));
    xfb_cond[12] = 0x60000;

    /* Indirect Draw / DrawIndexed / XFB Draw / XFB Counter Load. */
    uint32_t draw_ind[] = {
        IMMD(0, NVB06F_WFI, NVB06F_WFI_SCOPE_ALL),
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDIRECT), 12),
        2, 1, /* 2 draws, 1 pad word -> 2 * (4 + 1) = 10 indirect words */
    };
    struct tg_gpu_submit_indirect ind = {.address = 0x70000, .bytes = 40, .suffix_offset = 0};
    assert(tg_gpu_inspect_indirect_command_stream(draw_ind, sizeof(draw_ind), &ind, &va, &info));
    assert(info.va_end == 0x70028 && info.requires_mme && info.requires_cb0 &&
           info.requires_program == (1u << TG_GPU_PROGRAM_3D));
    programs.end[TG_GPU_PROGRAM_3D] = 0x200000000;
    struct range_checks ind_checks = {
        .expected = {{0x70000, 0x70028}, {0x100000030, 0x100000900}},
        .access = {TG_GPU_PITCH_READ, TG_GPU_PITCH_READ_ONLY},
    };
    assert(tg_gpu_check_stream_mappings(draw_ind, sizeof(draw_ind), &ind, &programs, check_range, &ind_checks));
    assert(ind_checks.next == 2);

    /* Missing WFI or intervening draw before indirect call must be rejected. */
    assert(!tg_gpu_inspect_indirect_command_stream(draw_ind + 1, sizeof(draw_ind) - 4, &ind, &va, &info));
    const uint32_t bad_intervening[] = {
        IMMD(0, NVB06F_WFI, NVB06F_WFI_SCOPE_ALL),
        IMMD(0, NVB197_DRAW_VERTEX_ARRAY, 3),
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDIRECT), 6),
        1, 0,
    };
    const struct tg_gpu_submit_indirect single_draw = {.address = 0x70000, .bytes = 16, .suffix_offset = 0};
    assert(!tg_gpu_inspect_indirect_command_stream(bad_intervening, sizeof(bad_intervening), &single_draw, &va, &info));

    /* DrawIndexedIndirect, XfbDrawIndirect, XfbCounterLoad. */
    const uint32_t idx_ind[] = {
        INCR(0, NVB06F_WFI, 1), NVB06F_WFI_SCOPE_ALL,
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDEXED_INDIRECT), 7),
        1, 0,
    };
    const struct tg_gpu_submit_indirect single_idx = {.address = 0x71000, .bytes = 20, .suffix_offset = 0};
    assert(tg_gpu_inspect_indirect_command_stream(idx_ind, sizeof(idx_ind), &single_idx, &va, &info));
    assert(info.va_end == 0x71014);

    const uint32_t xfb_draw[] = {
        IMMD(0, NVB06F_WFI, NVB06F_WFI_SCOPE_ALL),
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_XFB_DRAW_INDIRECT), 3),
        4, 16,
    };
    const struct tg_gpu_submit_indirect word_ind = {.address = 0x72000, .bytes = 4, .suffix_offset = 0};
    assert(tg_gpu_inspect_indirect_command_stream(xfb_draw, sizeof(xfb_draw), &word_ind, &va, &info));
    assert(info.va_end == 0x72004 && info.requires_program == (1u << TG_GPU_PROGRAM_3D));

    uint32_t xfb_load[] = {
        IMMD(0, NVB06F_WFI, NVB06F_WFI_SCOPE_ALL),
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_XFB_COUNTER_LOAD), 2),
        3,
    };
    assert(tg_gpu_inspect_indirect_command_stream(xfb_load, sizeof(xfb_load), &word_ind, &va, &info));
    assert(info.va_end == 0x72004 && !info.requires_program);
    for (uint32_t slot = 0; slot < 4; ++slot) {
        xfb_load[2] = slot;
        uint32_t prepared[3];
        memcpy(prepared, xfb_load, sizeof(prepared));
        assert(tg_gpu_prepare_indirect_command_stream(prepared, sizeof(prepared), &word_ind, &va, &info));
        assert(prepared[2] == 8 * slot && xfb_load[2] == slot);
        struct range_checks counter = {
            .expected = {{0x72000, 0x72004}}, .access = {TG_GPU_PITCH_READ},
        };
        assert(tg_gpu_check_stream_mappings(prepared, sizeof(prepared), &word_ind,
                                            &programs, check_range, &counter));
        assert(counter.next == 1);
    }
    xfb_load[2] = 4; /* stream index >= 4 */
    assert(!tg_gpu_inspect_indirect_command_stream(xfb_load, sizeof(xfb_load), &word_ind, &va, &info));

    /* DispatchIndirect: prefix + 3-word indirect + suffix with [root, qmd_zero, local_size, WFI, QMD, PCAS]. */
    enum { SUFFIX = 3, Q_HDR = SUFFIX + 7, PCAS = Q_HDR + 1 + TG_GPU_QMD_RECORD_WORDS };
    uint32_t disp[PCAS + 3] = {
        [0] = INCR(0, NVB06F_WFI, 1),
        [1] = NVB06F_WFI_SCOPE_ALL,
        [2] = ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DISPATCH_INDIRECT), 8),
        [SUFFIX + 0] = 0,
        [SUFFIX + 1] = 0x90000, /* root_desc VA (256B aligned) */
        [SUFFIX + 2] = 0,
        [SUFFIX + 3] = 0,
        [SUFFIX + 4] = 64,      /* local_size = 8 * 8 * 1 */
        [SUFFIX + 5] = INCR(0, NVB06F_WFI, 1),
        [SUFFIX + 6] = NVB06F_WFI_SCOPE_ALL,
        [Q_HDR] = NONINCR(1, NVB1C0_NO_OPERATION, TG_GPU_QMD_RECORD_WORDS),
        [PCAS] = INCR(1, NVB1C0_SEND_PCAS_A, 1),
        [PCAS + 2] = IMMD(1, NVB1C0_SEND_SIGNALING_PCAS_B, 3),
    };
    uint32_t *qmd = disp + Q_HDR + 1;
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, PROGRAM_OFFSET, 0x200);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, API_VISIBLE_CALL_LIMIT, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CTA_RASTER_WIDTH, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CTA_RASTER_HEIGHT, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CTA_RASTER_DEPTH, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, QMD_VERSION, 6);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CTA_THREAD_DIMENSION0, 8);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CTA_THREAD_DIMENSION1, 8);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CTA_THREAD_DIMENSION2, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, L1_CONFIGURATION, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, REGISTER_COUNT, 16);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, SASS_VERSION, 0x30);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CONSTANT_BUFFER_VALID, 0, 1);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CONSTANT_BUFFER_ADDR_LOWER, 0, 0x90000);
    NVVAL_MW_SET(qmd, NVB1C0, QMDV00_06, CONSTANT_BUFFER_SIZE, 0, 256);

    const struct tg_gpu_submit_indirect disp_ind = {
        .address = 0x73000, .bytes = 12, .suffix_offset = SUFFIX * sizeof(uint32_t),
    };
    assert(tg_gpu_inspect_indirect_command_stream(disp, sizeof(disp), &disp_ind, &va, &info));
    assert(info.requires_mme && info.requires_program == (1u << TG_GPU_PROGRAM_COMPUTE));

    uint32_t staged[PCAS + 3];
    memcpy(staged, disp, sizeof(disp));
    assert(tg_gpu_prepare_indirect_command_stream(staged, sizeof(staged), &disp_ind, &va, &info));
    const uint64_t qmd_va = ((va.reserved_base + TG_GPU_COMMAND_QMD + (Q_HDR + 1) * 4u) + 255u) & ~UINT64_C(255);
    assert(info.has_qmd);
    assert(staged[SUFFIX + 2] == (uint32_t)(qmd_va >> 32));
    assert(staged[SUFFIX + 3] == (uint32_t)qmd_va);
    assert(staged[PCAS + 1] == (uint32_t)(qmd_va >> 8));

    programs.end[TG_GPU_PROGRAM_COMPUTE] = 0x200000000;
    struct range_checks disp_checks = {
        .expected = {
            {0x73000, 0x7300c},         /* indirect buffer */
            {0x9000c, 0x90018},         /* root_desc cs.group_count[3] writable */
            {0x100000200, 0x100000a80}, /* compute program */
            {0x90000, 0x90100},         /* QMD cbuf 0 */
        },
        .access = {
            TG_GPU_PITCH_READ,
            TG_GPU_PITCH_WRITE,
            TG_GPU_PITCH_READ_ONLY,
            TG_GPU_PITCH_READ,
        },
    };
    assert(tg_gpu_check_stream_mappings(staged, sizeof(staged), &disp_ind, &programs,
                                        check_range, &disp_checks));
    assert(disp_checks.next == 4);

    /* Corrupt local_size mismatch or nonzero unpatched qmd_addr must fail. */
    disp[SUFFIX + 4] = 32;
    assert(!tg_gpu_inspect_indirect_command_stream(disp, sizeof(disp), &disp_ind, &va, &info));
    disp[SUFFIX + 4] = 64;
    disp[SUFFIX + 3] = 0x1000;
    assert(!tg_gpu_inspect_indirect_command_stream(disp, sizeof(disp), &disp_ind, &va, &info));
}

static void test_pipeline_statistics_stream(void)
{
    /* Combined 3D + compute VK_QUERY_TYPE_PIPELINE_STATISTICS stream emitted
     * for DXVK D3D11_QUERY_PIPELINE_STATISTICS: clears 3D report + CS scratch,
     * enables 3D counters via MME, accumulates direct CS invocations via MME,
     * writes 3D 4-word report + CS 2-word report, disables counters, releases
     * 1-word availability, and acquires availability before copy-query meta.
     */
    const struct tg_gpu_command_va va = {0x100050000, 0x100020000, 0x100030000};
    struct tg_gpu_command_info info;
    const uint32_t stat_report =
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, REPORT_ONLY) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, VERTEX_SHADER) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, REPORT, VS_INVOCATIONS) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, FOUR_WORDS) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, FLUSH_DISABLE, TRUE);
    const uint32_t avail_release =
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, RELEASE) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, RELEASE, AFTER_ALL_PRECEEDING_WRITES_COMPLETE) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL) |
        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, ONE_WORD);
    const uint32_t avail_acquire =
        NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQ_GEQ) |
        NVDEF(NVB06F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED) |
        NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);

    uint32_t stream[] = {
        IMMD(0, NVB197_CLEAR_REPORT_VALUE, NVB197_CLEAR_REPORT_VALUE_TYPE_VS_INVOCATIONS),
        IMMD(0, NVB197_SET_MME_SHADOW_SCRATCH(3), 0),
        IMMD(0, NVB197_SET_MME_SHADOW_SCRATCH(4), 0),
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_SET_STATISTICS_COUNTERS), 2), 1, 0x4,
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_ADD_CS_INVOCATIONS), 2), 0, 8,
        INCR(0, NVB197_SET_REPORT_SEMAPHORE_A, 4), 1, 0x40100, 0, stat_report,
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_WRITE_CS_INVOCATIONS), 2), 1, 0x40110,
        IMMD(0, NVB197_FLUSH_PENDING_WRITES, 0),
        ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_SET_STATISTICS_COUNTERS), 2), 0, 0x4,
        INCR(0, NVB197_SET_REPORT_SEMAPHORE_A, 4), 1, 0x40000, 1, avail_release,
        INCR(0, NVB06F_SEMAPHOREA, 4), 1, 0x40000, 1, avail_acquire,
    };
    assert(tg_gpu_inspect_command_stream(stream, sizeof(stream), &va, &info));
    assert(info.requires_mme && info.va_end == 0x100040118);

    /* Unaligned CS report address must fail. */
    stream[16] = 0x40112;
    assert(!tg_gpu_inspect_command_stream(stream, sizeof(stream), &va, &info));
}

int main(void)
{
    uint32_t w[16]={0x20010000,0xb197,0x20020100,0x20010014,0xdeadbeef,0};
    assert(tg_gpu_validate_command_stream(w,24)); /* Data may resemble a header. */
    assert(!tg_gpu_validate_command_stream(NULL,4));
    assert(!tg_gpu_validate_command_stream(w,0) && !tg_gpu_validate_command_stream(w,23));
    assert(!tg_gpu_validate_command_stream((void *)((char *)w+1),4));
    const uint32_t classes[8]={[0]=0xb197,[1]=0xb1c0,[4]=0xb0b5};
    for(unsigned sub=0;sub<8;sub++)for(unsigned op=1;op<=5;op++) {
        if(op==2)continue;
        w[0]=(op<<29)|(sub<<13)|(1<<16)|(0x50>>2);w[1]=123;
        assert(!tg_gpu_validate_command_stream(w,op==4?4:8));
        w[0]=(w[0]&~0xfffu)|(0x100>>2);
        assert(tg_gpu_validate_command_stream(w,op==4?4:8)==(classes[sub]!=0));
        if(op!=4)assert(!tg_gpu_validate_command_stream(w,4));
        w[0]|=1u<<12;assert(!tg_gpu_validate_command_stream(w,op==4?4:8));
    }
    for(unsigned sub=0;sub<8;sub++) {
        w[0]=0x20010000 | (sub<<13);
        w[1]=classes[sub];
        assert(tg_gpu_validate_command_stream(w,8)==(classes[sub]!=0));
        w[1]=classes[sub] | (1u<<16);
        assert(!tg_gpu_validate_command_stream(w,8));
    }
    w[0]=0x20018000;w[1]=0xb197; /* A copy subchannel must bind the copy class. */
    assert(!tg_gpu_validate_command_stream(w,8));
    w[0]=0x80010000;assert(!tg_gpu_validate_command_stream(w,4)); /* Immediate SET_OBJECT. */
    w[0]=0x60020000;w[1]=w[2]=0xb197;
    assert(!tg_gpu_validate_command_stream(w,12)); /* One object binding per packet. */
    w[0]=0x20030000;assert(!tg_gpu_validate_command_stream(w,16)); /* Host span. */
    /* Check method wrap on the compute subchannel. */
    w[0]=0x20012fff;assert(tg_gpu_validate_command_stream(w,8));
    w[0]=0x20022fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0xa0022fff;assert(!tg_gpu_validate_command_stream(w,12));
    w[0]=0x60022fff;assert(tg_gpu_validate_command_stream(w,12));
    w[0]=0x00010000;assert(!tg_gpu_validate_command_stream(w,4)); /* Subdevice. */
    w[0]=0xe0000000;assert(!tg_gpu_validate_command_stream(w,4)); /* End segment. */
    w[0]=0xc0000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Reserved. */
    w[0]=0x20000040;assert(!tg_gpu_validate_command_stream(w,4)); /* Zero count. */
    test_mme_upload();
    test_mme_calls();
    test_mme_order();
    test_copy_stream();
    test_inline_upload();
    test_report_and_acquire();
    test_texture_pools();
    test_compute_dispatch();
    test_input_bindings();
    test_color_targets();
    test_depth_targets();
    test_constant_buffers();
    test_program_regions();
    test_shader_local_memory();
    test_captured_qmd();
    test_native_umd_barrier();
    test_indirect_and_xfb_streams();
    test_pipeline_statistics_stream();
    puts("B06F packets, GM20B MME and command address ranges passed");return 0;
}
