/* SPDX-License-Identifier: MIT */
#include "../core/present_blit.h"
#include "../core/command_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SOURCE_VA UINT64_C(0x100000)
#define DESTINATION_VA UINT64_C(0x500000)
static void execute(const uint32_t *words, uint32_t count, bool opaque,
    const uint8_t *source, size_t source_bytes, uint8_t *destination, size_t destination_bytes)
{
    if (count == TG_GPU_PRESENT_HALFTURN_WORDS) {
        assert(words[1] == 0x902d && words[12] == (opaque ? 0xe6u : 0xcfu));
        assert(words[49] == 0 && words[50] == UINT32_MAX && words[51] == 0 && words[52] == UINT32_MAX);
        assert(words[53] == 0x80000000 && words[55] == 0x80000000);
        for (unsigned y = 0; y < words[48]; ++y) for (unsigned x = 0; x < words[47]; ++x) {
            const uint64_t from = ((uint64_t)words[18] << 32) + words[19] - SOURCE_VA +
                (uint64_t)(words[56] - y) * words[15] + (words[54] - x) * 4;
            const uint64_t to = ((uint64_t)words[9] << 32) + words[10] - DESTINATION_VA +
                (uint64_t)(words[46] + y) * words[6] + (words[45] + x) * 4;
            assert(from + 4 <= source_bytes && to + 4 <= destination_bytes);
            memcpy(destination + to, source + from, 4);
            if (opaque) destination[to + 3] = 255;
        }
    } else {
    const unsigned shift = opaque ? 4 : 0;
    assert(count == 13 + shift && words[1] == 0xb0b5);
    const uint64_t src = ((uint64_t)words[3 + shift] << 32) | words[4 + shift];
    const uint64_t dst = ((uint64_t)words[5 + shift] << 32) | words[6 + shift];
    assert(src >= SOURCE_VA && dst >= DESTINATION_VA);
    const unsigned bytes = words[9 + shift] * (opaque ? 4 : 1);
    if (opaque) assert(words[3] == 255 && words[5] == 0x03304210);
    for (unsigned y = 0; y < words[10 + shift]; ++y) {
        const uint64_t from = src - SOURCE_VA + (uint64_t)y * words[7 + shift];
        const uint64_t to = dst - DESTINATION_VA + (uint64_t)y * words[8 + shift];
        assert(from <= source_bytes && bytes <= source_bytes - from);
        assert(to <= destination_bytes && bytes <= destination_bytes - to);
        memcpy(destination + to, source + from, bytes);
        for (unsigned x = 3; opaque && x < bytes; x += 4) destination[to + x] = 255;
    }
    }
    uint32_t captured[TG_GPU_PRESENT_HALFTURN_WORDS];
    memcpy(captured, words, count * 4);
    const struct tg_gpu_command_va va = {UINT64_C(0x10000000), UINT64_C(0x4000000), UINT64_C(0x8000000)};
    struct tg_gpu_command_info info;
    assert(tg_gpu_prepare_command_stream(captured, count * 4, &va, &info));
}

static void copies(const struct tg_gpu_present_blit *plan, struct tg_gpu_blit_rect rect,
    bool opaque, const uint8_t *source, size_t source_bytes, uint8_t *dst, size_t dst_bytes)
{
    for (uint32_t i = 0; i < tg_gpu_present_blit_stripes(plan, rect); ++i) {
        uint32_t words[TG_GPU_PRESENT_HALFTURN_WORDS], count;
        if (plan->rotation == 180) {
            struct tg_gpu_present_halfturn half;
            assert(tg_gpu_present_blit_halfturn(plan, rect, &half));
            count = tg_gpu_encode_present_halfturn(words, TG_GPU_PRESENT_HALFTURN_WORDS,
                SOURCE_VA, DESTINATION_VA, &half, opaque);
        } else {
            struct tg_gpu_present_copy copy;
            assert(tg_gpu_present_blit_copy(plan, rect, i, &copy));
            count = tg_gpu_encode_present_copy(words, TG_GPU_PRESENT_HALFTURN_WORDS,
                SOURCE_VA, DESTINATION_VA, &copy, opaque, false);
        }
        assert(count);
        execute(words, count, opaque, source, source_bytes, dst, dst_bytes);
    }
}

static void labelled_rectangle(unsigned rotation, bool opaque)
{
    /* Six distinct pixels; padding must never become an image pixel. */
    uint32_t source[16], destination[24];
    for (unsigned i = 0; i < 16; ++i) source[i] = 0xdeadbeef;
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 3; ++x)
        source[y*8+x] = 0x12000000 | (y*3+x+1);
    for (unsigned i = 0; i < 24; ++i) destination[i] = 0xcafebabe;
    const bool quarter = rotation == 90 || rotation == 270;
    const unsigned width = quarter ? 2 : 3, height = quarter ? 3 : 2;
    struct tg_gpu_present_blit plan;
    const struct tg_gpu_blit_rect full = {0,0,3,2};
    assert(tg_gpu_present_blit_init(&plan, (struct tg_gpu_blit_surface){3,2,32},
        (struct tg_gpu_blit_surface){width,height,32}, full, full, rotation));
    copies(&plan, full, opaque, (const uint8_t *)source, sizeof(source),
        (uint8_t *)destination, sizeof(destination));
    const unsigned identity[6] = {1,2,3,4,5,6}, ccw90[6] = {3,6,2,5,1,4};
    const unsigned ccw180[6] = {6,5,4,3,2,1}, ccw270[6] = {4,1,5,2,6,3};
    const unsigned *expected = rotation == 90 ? ccw90 : rotation == 180 ? ccw180 : rotation == 270 ? ccw270 : identity;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x)
            assert(destination[y*8+x] == ((opaque ? 0xff000000u : 0x12000000u) | expected[y*width+x]));
        for (unsigned x = width; x < 8; ++x) assert(destination[y*8+x] == 0xcafebabe);
    }
}

static void clipped_offsets(unsigned rotation)
{
    uint32_t source[96], destination[144], expected[144];
    for (unsigned i = 0; i < 96; ++i) source[i] = 0xff000000 | i;
    for (unsigned i = 0; i < 144; ++i) destination[i] = expected[i] = 0xabcdef01;
    const bool quarter = rotation == 90 || rotation == 270;
    const unsigned width = quarter ? 5 : 9, height = quarter ? 9 : 5, stride = quarter ? 8 : 16;
    struct tg_gpu_present_blit plan;
    const struct tg_gpu_blit_rect src = {1,1,6,4}, dst = {2,0,7,3}, dirty = {3,1,6,3};
    assert(tg_gpu_present_blit_init(&plan, (struct tg_gpu_blit_surface){8,6,64},
        (struct tg_gpu_blit_surface){width,height,stride*4}, src, dst, rotation));
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const unsigned lx = rotation == 90 ? 8-y : rotation == 270 ? y : rotation == 180 ? 8-x : x;
        const unsigned ly = rotation == 90 ? x : rotation == 270 ? 4-x : rotation == 180 ? 4-y : y;
        if (lx >= 3 && lx < 6 && ly >= 1 && ly < 3)
            expected[y*stride+x] = source[(ly+1)*16+(lx-1)];
    }
    copies(&plan, dirty, false, (const uint8_t *)source, sizeof(source),
        (uint8_t *)destination, sizeof(destination));
    assert(!memcmp(destination, expected, sizeof(destination)));
}

static unsigned mapping_calls;
static bool deny_write;
static bool mapping(void *data, uint64_t from, uint64_t end, enum tg_gpu_pitch_access access)
{
    (void)data; ++mapping_calls;
    assert((access == TG_GPU_PITCH_READ && from == SOURCE_VA && end == SOURCE_VA + 64) ||
           (access == TG_GPU_PITCH_WRITE && from == DESTINATION_VA && end == DESTINATION_VA + 64));
    return !deny_write || access != TG_GPU_PITCH_WRITE;
}

static void halfturn_admission(void)
{
    uint32_t words[TG_GPU_PRESENT_HALFTURN_WORDS], changed[TG_GPU_PRESENT_HALFTURN_WORDS];
    const struct tg_gpu_present_halfturn half = {{3,2,32},{3,2,32},2,1,0,0,3,2};
    assert(tg_gpu_encode_present_halfturn(words, TG_GPU_PRESENT_HALFTURN_WORDS,
        SOURCE_VA, DESTINATION_VA, &half, false) == TG_GPU_PRESENT_HALFTURN_WORDS);
    assert(tg_gpu_inspect_command_stream(words, sizeof(words), NULL, NULL));
    for (unsigned size = 1; size < TG_GPU_PRESENT_HALFTURN_WORDS; ++size)
        assert(!tg_gpu_inspect_command_stream(words, size*4, NULL, NULL));
    /* Reject changed derivatives, methods, conditional rendering and an
     * otherwise valid address in the KMD's private VA region. */
    const unsigned corrupt[] = {0,1,4,13,21,23,25,27,29,31,33,35,37,39,41,43,44,49,50,51,52,53,55};
    for (unsigned i = 0; i < sizeof(corrupt)/sizeof(corrupt[0]); ++i) {
        memcpy(changed, words, sizeof(words)); changed[corrupt[i]] ^= 1;
        assert(!tg_gpu_inspect_command_stream(changed, sizeof(changed), NULL, NULL));
    }
    const struct tg_gpu_command_va va = {0x10000000,DESTINATION_VA,DESTINATION_VA+0x1000};
    assert(!tg_gpu_inspect_command_stream(words, sizeof(words), &va, NULL));
    memcpy(changed, words, sizeof(words)); changed[54] = 1;
    assert(!tg_gpu_inspect_command_stream(changed, sizeof(changed), NULL, NULL));
    struct tg_gpu_program_state state = {0};
    assert(tg_gpu_check_command_mappings(words, sizeof(words), &state, mapping, NULL) && mapping_calls == 2);
    deny_write = true;
    assert(!tg_gpu_check_command_mappings(words, sizeof(words), &state, mapping, NULL));
    assert(!tg_gpu_encode_present_halfturn(changed, TG_GPU_PRESENT_HALFTURN_WORDS-1,
        SOURCE_VA, DESTINATION_VA, &half, false));
    assert(!tg_gpu_encode_present_halfturn(changed, TG_GPU_PRESENT_HALFTURN_WORDS,
        SOURCE_VA, SOURCE_VA, &half, false));
}

int main(void)
{
    for (unsigned angle = 0; angle < 360; angle += 90) {
        labelled_rectangle(angle, false); labelled_rectangle(angle, true); clipped_offsets(angle);
    }
    halfturn_admission();
    struct tg_gpu_present_blit plan;
    const struct tg_gpu_blit_surface s = {3,2,16};
    const struct tg_gpu_blit_rect r = {0,0,3,2};
    assert(tg_gpu_present_blit_init(&plan, s, s, r, r, 0));
    assert(tg_gpu_present_blit_stripes(&plan, r) == 1);
    struct tg_gpu_present_copy copy;
    assert(tg_gpu_present_blit_copy(&plan, r, 0, &copy));
    assert(copy.row_bytes == 12 && copy.lines == 2 && copy.source_pitch == 16 && copy.destination_pitch == 16);
    assert(!tg_gpu_present_blit_copy(&plan, r, 1, &copy));
    assert(!tg_gpu_present_blit_stripes(&plan, (struct tg_gpu_blit_rect){-1,0,3,2}));
    assert(!tg_gpu_present_blit_init(&plan, s, s, r, (struct tg_gpu_blit_rect){0,0,2,2}, 0));
    assert(!tg_gpu_present_blit_init(&plan, s, s, r, r, 45));
    uint32_t words[17]; for (unsigned i = 0; i < 17; ++i) words[i] = 0x12345678;
    assert(!tg_gpu_encode_present_copy(words, 12, SOURCE_VA, DESTINATION_VA, &copy, false, false));
    for (unsigned i = 0; i < 17; ++i) assert(words[i] == 0x12345678);
    puts("PASS: 0/90/180/270 CE/2D packets, clipped pixels, pitch/alpha, bounded stream and mapping checks (CPU model only)");
    return 0;
}
