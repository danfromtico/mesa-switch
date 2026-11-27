/* SPDX-License-Identifier: MIT */
#include "copy_stream.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"

/* Index the contiguous method banks used by NVK. */
enum copy_register {
    SRC_HI, SRC_LO, DST_HI, DST_LO, SRC_PITCH, DST_PITCH, LINE_LENGTH, LINE_COUNT,
    SEM_HI, SEM_LO, SEM_PAYLOAD, REMAP,
    DST_TILE, DST_WIDTH, DST_HEIGHT, DST_DEPTH, DST_LAYER, DST_ORIGIN,
    SRC_TILE, SRC_WIDTH, SRC_HEIGHT, SRC_DEPTH, SRC_LAYER, SRC_ORIGIN,
    COPY_REGISTER_COUNT,
};
_Static_assert(COPY_REGISTER_COUNT == sizeof(((struct tg_gpu_copy_state *)0)->registers) /
                                    sizeof(uint32_t), "B0B5 register state");

static bool have(const struct tg_gpu_copy_state *state, unsigned first, unsigned count)
{
    const uint32_t mask = ((1u << count) - 1) << first;
    return (state->written & mask) == mask;
}

static uint64_t align_up(uint64_t n, uint64_t alignment)
{
    return (n + alignment - 1) & ~(alignment - 1);
}

static bool copy_surface(const struct tg_gpu_copy_state *state, bool source, bool pitch,
                         uint32_t units, uint32_t lines, uint32_t element_bytes,
                         const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t *r = state->registers;
    const unsigned offset = source ? SRC_HI : DST_HI;
    if (!have(state, offset, 2))
        return false;
    const uint64_t address = ((uint64_t)r[offset] << 32) | r[offset + 1];

    if (pitch) {
        const unsigned stride = source ? SRC_PITCH : DST_PITCH;
        if (lines > 1 && (!have(state, stride, 1) || !r[stride]))
            return false;
        const uint64_t last_row = (uint64_t)(lines - 1) * r[stride];
        const uint64_t row_bytes = (uint64_t)units * element_bytes;
        return last_row < TG_GPU_VA_LIMIT && row_bytes <= TG_GPU_VA_LIMIT - last_row &&
               tg_gpu_command_range_allowed(va, address, last_row + row_bytes, max_end);
    }

    const unsigned tile = source ? SRC_TILE : DST_TILE;
    if (!have(state, tile, 6))
        return false;
    const uint32_t block = r[tile];
    const uint32_t height_log2 = NVVAL_GET(block, NVB0B5, SET_SRC_BLOCK_SIZE, HEIGHT);
    const uint32_t depth_log2 = NVVAL_GET(block, NVB0B5, SET_SRC_BLOCK_SIZE, DEPTH);
    const uint32_t width = r[tile + 1], height = r[tile + 2], depth = r[tile + 3];
    const uint32_t layer = r[tile + 4], origin = r[tile + 5];
    const uint32_t x = NVVAL_GET(origin, NVB0B5, SET_SRC_ORIGIN, X);
    const uint32_t y = NVVAL_GET(origin, NVB0B5, SET_SRC_ORIGIN, Y);
    const uint32_t supported_block = NVDEF(NVB0B5, SET_SRC_BLOCK_SIZE, GOB_HEIGHT, GOB_HEIGHT_FERMI_8) |
        NVVAL(NVB0B5, SET_SRC_BLOCK_SIZE, HEIGHT, height_log2) |
        NVVAL(NVB0B5, SET_SRC_BLOCK_SIZE, DEPTH, depth_log2);
    /* NVK uses 64-byte-wide Fermi GOBs. Check the rectangle and conservatively
     * cover the padded mip level containing it, including its depth tiles. */
    if (height_log2 > 5 || depth_log2 > 5 ||
        block != supported_block ||
        x >= width || units > width - x || y >= height || lines > height - y ||
        layer >= depth)
        return false;
    const uint64_t row = align_up((uint64_t)width * element_bytes, 64);
    const uint64_t rows = align_up(height, 8u << height_log2);
    const uint64_t layers = align_up(depth, 1u << depth_log2);
    if (row > TG_GPU_VA_LIMIT / rows || row * rows > TG_GPU_VA_LIMIT / layers)
        return false;
    return tg_gpu_command_range_allowed(va, address, row * rows * layers, max_end);
}

static bool copy_launch(const struct tg_gpu_copy_state *state, uint32_t launch,
                        const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t allowed = DRF_SMASK(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE) |
                             DRF_SMASK(NVB0B5_LAUNCH_DMA_FLUSH_ENABLE) |
                             DRF_SMASK(NVB0B5_LAUNCH_DMA_SEMAPHORE_TYPE) |
                             DRF_SMASK(NVB0B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT) |
                             DRF_SMASK(NVB0B5_LAUNCH_DMA_DST_MEMORY_LAYOUT) |
                             DRF_SMASK(NVB0B5_LAUNCH_DMA_MULTI_LINE_ENABLE) |
                             DRF_SMASK(NVB0B5_LAUNCH_DMA_REMAP_ENABLE);
    const uint32_t transfer = NVVAL_GET(launch, NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE);
    const uint32_t semaphore = NVVAL_GET(launch, NVB0B5, LAUNCH_DMA, SEMAPHORE_TYPE);
    const uint32_t *r = state->registers;
    if ((launch & ~allowed) || transfer > NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED ||
        semaphore > NVB0B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_FOUR_WORD_SEMAPHORE)
        return false;

    if (semaphore) {
        const uint32_t bytes = semaphore == NVB0B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_ONE_WORD_SEMAPHORE ? 4 : 16;
        const uint64_t address = ((uint64_t)r[SEM_HI] << 32) | r[SEM_LO];
        if (!have(state, SEM_HI, 3) || (address & (bytes - 1)) ||
            !tg_gpu_command_range_allowed(va, address, bytes, max_end))
            return false;
    }
    if (transfer == NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE)
        return true;

    if (!have(state, LINE_LENGTH, 2))
        return false;
    const bool multi = NVVAL_GET(launch, NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE);
    const uint32_t units = r[LINE_LENGTH];
    if (!units) {
        /* Zero-length non-pipelined launches implement NVK's CE barriers. */
        return !multi && !r[LINE_COUNT] && transfer == NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED &&
               NVVAL_GET(launch, NVB0B5, LAUNCH_DMA, FLUSH_ENABLE);
    }
    const uint32_t lines = multi ? r[LINE_COUNT] : 1;
    if (!lines)
        return false;

    uint32_t src_bytes = 1, dst_bytes = 1;
    bool reads_source = true;
    if (NVVAL_GET(launch, NVB0B5, LAUNCH_DMA, REMAP_ENABLE)) {
        const uint32_t remap = r[REMAP];
        const uint32_t mask = DRF_SMASK(NVB0B5_SET_REMAP_COMPONENTS_DST_X) |
                              DRF_SMASK(NVB0B5_SET_REMAP_COMPONENTS_DST_Y) |
                              DRF_SMASK(NVB0B5_SET_REMAP_COMPONENTS_DST_Z) |
                              DRF_SMASK(NVB0B5_SET_REMAP_COMPONENTS_DST_W) |
                              DRF_SMASK(NVB0B5_SET_REMAP_COMPONENTS_COMPONENT_SIZE) |
                              DRF_SMASK(NVB0B5_SET_REMAP_COMPONENTS_NUM_SRC_COMPONENTS) |
                              DRF_SMASK(NVB0B5_SET_REMAP_COMPONENTS_NUM_DST_COMPONENTS);
        if (!have(state, REMAP, 1) || (remap & ~mask))
            return false;
        const uint32_t component = 1 + NVVAL_GET(remap, NVB0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE);
        const uint32_t src_count = 1 + NVVAL_GET(remap, NVB0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS);
        const uint32_t dst_count = 1 + NVVAL_GET(remap, NVB0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS);
        src_bytes = component * src_count;
        dst_bytes = component * dst_count;
        reads_source = false;
        for (uint32_t i = 0; i < dst_count; i++) {
            const uint32_t select = (remap >> (i * 4)) & 7;
            if (select > NVB0B5_SET_REMAP_COMPONENTS_DST_X_NO_WRITE)
                return false;
            if (select < NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A) {
                if (select >= src_count)
                    return false;
                reads_source = true;
            }
        }
    }

    const bool src_pitch = NVVAL_GET(launch, NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT);
    const bool dst_pitch = NVVAL_GET(launch, NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT);
    if (!multi && ((!src_pitch && reads_source) || !dst_pitch))
        return false;
    return (!reads_source || copy_surface(state, true, src_pitch, units, lines, src_bytes, va, max_end)) &&
           copy_surface(state, false, dst_pitch, units, lines, dst_bytes, va, max_end);
}

bool tg_gpu_copy_method(struct tg_gpu_copy_state *state, uint32_t method, uint32_t value,
                        const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    unsigned index;
    if (method >= NVB0B5_OFFSET_IN_UPPER && method <= NVB0B5_LINE_COUNT)
        index = SRC_HI + (method - NVB0B5_OFFSET_IN_UPPER) / 4;
    else if (method >= NVB0B5_SET_SEMAPHORE_A && method <= NVB0B5_SET_SEMAPHORE_PAYLOAD)
        index = SEM_HI + (method - NVB0B5_SET_SEMAPHORE_A) / 4;
    else if (method >= NVB0B5_SET_DST_BLOCK_SIZE && method <= NVB0B5_SET_DST_ORIGIN)
        index = DST_TILE + (method - NVB0B5_SET_DST_BLOCK_SIZE) / 4;
    else if (method >= NVB0B5_SET_SRC_BLOCK_SIZE && method <= NVB0B5_SET_SRC_ORIGIN)
        index = SRC_TILE + (method - NVB0B5_SET_SRC_BLOCK_SIZE) / 4;
    else if (method == NVB0B5_SET_REMAP_COMPONENTS)
        index = REMAP;
    else if (method == NVB0B5_LAUNCH_DMA) {
        if (!copy_launch(state, value, va, max_end))
            return false;
        state->written = 0;
        return true;
    } else {
        return method == NVB0B5_NOP || method == NVB0B5_SET_REMAP_CONST_A ||
               method == NVB0B5_SET_REMAP_CONST_B;
    }

    if ((index == SRC_HI || index == DST_HI || index == SEM_HI) && value > 0xff)
        return false;
    state->registers[index] = value;
    state->written |= 1u << index;
    return true;
}
