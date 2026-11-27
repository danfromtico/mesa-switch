/* SPDX-License-Identifier: MIT */
#include "command_stream.h"
#include "copy_stream.h"
#include "present_halfturn.h"
#include "gm20b_mmu.h"
#include "nvk_mme_programs.h"
#include "submission.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb0b5.h"
#include "nouveau/headers/nvidia/classes/clb197.h"
#include "nouveau/headers/nvidia/classes/clb1c0.h"
#include "nouveau/headers/nvidia/classes/clb1c0qmd.h"
#include "nouveau/vulkan/nvk_mme_slots.h"
#include "nouveau/vulkan/nvkmd/windows/submit_abi.h"

/* NVK's 3D, compute and copy subchannels on GM20B. */
static const uint32_t nvk_class[NVB06F_NUMBER_OF_SUBCHANNELS] = {
    [0] = MAXWELL_B,
    [1] = MAXWELL_COMPUTE_B,
    [4] = MAXWELL_DMA_COPY_A,
};

/* Complete, CPU-visible calls emitted by NVK on Windows GM20B. */
static const uint8_t nvk_mme_call_words[NVK_MME_COUNT] = {
    [NVK_MME_SELECT_CB0] = 1,
    [NVK_MME_CLEAR] = 2,
    [NVK_MME_UPDATE_WINDOW_CLIP] = 1,
    [NVK_MME_BIND_IB] = 4,
    [NVK_MME_BIND_VB] = 4,
    [NVK_MME_SET_VB_ENABLES] = 1,
    [NVK_MME_SET_VB_STRIDE] = 1,
    [NVK_MME_SET_TESS_PARAMS] = 1,
    [NVK_MME_SET_ANTI_ALIAS] = 1,
    [NVK_MME_DRAW] = 5,
    [NVK_MME_DRAW_INDEXED] = 6,
    [NVK_MME_ADD_CS_INVOCATIONS] = 2,
    [NVK_MME_WRITE_CS_INVOCATIONS] = 2,
    [NVK_MME_SET_WRITE_MASK] = 2,
    [NVK_MME_SET_VIEWPORT_MIN_MAX_Z] = 3,
    [NVK_MME_SET_Z_CLAMP] = 1,
    [NVK_MME_SET_STATISTICS_COUNTERS] = 2,
};

static bool method_allowed(uint32_t method)
{
    return method>=0x100 || method==NVB06F_SET_OBJECT || method==NVB06F_WFI;
}

static bool packet_last_method(uint32_t op, uint32_t method, uint32_t count, uint32_t *last)
{
    *last = method;
    if (op == NVB06F_DMA_INCR_OPCODE_VALUE) {
        if (count - 1 > (0x3ffc - method) / 4)
            return false;
        *last += (count - 1) * 4;
    } else if (op == NVB06F_DMA_ONEINCR_OPCODE_VALUE && count > 1) {
        if (method == 0x3ffc)
            return false;
        *last += 4;
    }
    return true;
}

bool tg_gpu_command_range_allowed(const struct tg_gpu_command_va *va, uint64_t address,
                                  uint64_t bytes, uint64_t *max_end)
{
    const uint64_t limit = va ? va->limit : TG_GPU_VA_LIMIT;
    if (!address || !bytes || address >= limit || bytes > limit - address)
        return false;
    const uint64_t end = address + bytes;
    if (va && va->reserved_base < va->reserved_end &&
        address < va->reserved_end && end > va->reserved_base)
        return false;
    if (end > *max_end)
        *max_end = end;
    return true;
}

static bool word_span(const struct tg_gpu_command_va *va, uint32_t hi, uint32_t lo,
                      uint64_t bytes, uint32_t alignment, uint64_t *max_end)
{
    return hi <= UINT8_MAX && !(lo & (alignment - 1)) &&
           tg_gpu_command_range_allowed(va, ((uint64_t)hi << 32) | lo, bytes, max_end);
}

struct tg_gpu_checked_ranges {
    const struct tg_gpu_command_va *va;
    uint64_t *max_end;
    tg_gpu_command_range_check check;
    void *data;
};

static bool checked_word_span(const struct tg_gpu_checked_ranges *ranges, uint32_t hi, uint32_t lo,
                              uint64_t bytes, uint32_t alignment, enum tg_gpu_pitch_access access)
{
    const uint64_t base = ((uint64_t)hi << 32) | lo;
    return word_span(ranges->va, hi, lo, bytes, alignment, ranges->max_end) &&
           (!ranges->check || ranges->check(ranges->data, base, base + bytes, access));
}

static bool halfturn(const uint32_t *words, size_t count, const struct tg_gpu_checked_ranges *ranges)
{
    struct tg_gpu_present_halfturn b;
    uint64_t source, destination;
    return tg_gpu_decode_present_halfturn(words, count, &source, &destination, &b) &&
        checked_word_span(ranges, (uint32_t)(source >> 32), (uint32_t)source,
            (uint64_t)b.source.pitch * b.source.height, 256, TG_GPU_PITCH_READ) &&
        checked_word_span(ranges, (uint32_t)(destination >> 32), (uint32_t)destination,
            (uint64_t)b.destination.pitch * b.destination.height, 256, TG_GPU_PITCH_WRITE);
}

static bool host_acquire(const uint32_t *args, const struct tg_gpu_command_va *va,
                         uint64_t *max_end)
{
    const uint32_t flags = NVDEF(NVB06F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED) |
                           NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);
    const uint32_t equal = flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQUIRE);
    const uint32_t geq = flags | NVDEF(NVB06F, SEMAPHORED, OPERATION, ACQ_GEQ);
    return (args[3] == equal || args[3] == geq) &&
           word_span(va, args[0], args[1], 4, 4, max_end);
}

static bool global_render_enable_span(uint32_t first, uint32_t last)
{
    return first <= NVB197_SET_GLOBAL_RENDER_ENABLE_C &&
           last >= NVB197_SET_GLOBAL_RENDER_ENABLE_A;
}

static bool global_render_enable(const uint32_t *args,
                                 const struct tg_gpu_checked_ranges *ranges)
{
    const uint32_t mode = args[2];
    if (mode == NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_FALSE ||
        mode == NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE)
        return !args[0] && !args[1];
    if (mode == NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_CONDITIONAL ||
        mode == NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_EQUAL ||
        mode == NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL)
        return checked_word_span(ranges, args[0], args[1], 32, 16,
                                 TG_GPU_PITCH_READ_ONLY);
    return false;
}

static uint32_t stream_out_buffer_in_span(uint32_t first, uint32_t last)
{
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t enable = NVB197_SET_STREAM_OUT_BUFFER_ENABLE(i);
        const uint32_t size_reg = NVB197_SET_STREAM_OUT_BUFFER_SIZE(i);
        if (first <= size_reg && last >= enable)
            return enable;
    }
    return 0;
}

static bool stream_out_buffer(const uint32_t *args,
                              const struct tg_gpu_checked_ranges *ranges)
{
    if (args[0] == NVB197_SET_STREAM_OUT_BUFFER_ENABLE_V_FALSE)
        return !args[1] && !args[2] && !args[3];
    if (args[0] == NVB197_SET_STREAM_OUT_BUFFER_ENABLE_V_TRUE)
        return checked_word_span(ranges, args[1], args[2], args[3], 4,
                                 TG_GPU_PITCH_WRITE);
    return false;
}

/* The B197 graphics and B1C0 compute engines share these I2M methods. */
static bool inline_upload_span(uint32_t first, uint32_t last)
{
    return (first <= NVB197_LOAD_INLINE_DATA && last >= NVB197_LINE_LENGTH_IN) ||
           (first <= NVB197_SET_I2M_SEMAPHORE_C && last >= NVB197_SET_I2M_SEMAPHORE_A);
}

static size_t inline_upload(uint32_t subchannel, const uint32_t *args, size_t remaining,
                            const struct tg_gpu_checked_ranges *ranges)
{
    if (remaining < 6 || !args[0] || (args[0] & 3) || args[1] != 1)
        return 0;
    const uint32_t bytes = args[0], words = bytes / 4;
    if (words + 1 > DRF_MASK(NVB06F_DMA_ONEINCR_COUNT) || words > remaining - 6)
        return 0;
    const uint32_t header = NVDEF(NVB06F, DMA_ONEINCR, OPCODE, VALUE) |
                            NVVAL(NVB06F, DMA_ONEINCR, SUBCHANNEL, subchannel) |
                            NVVAL(NVB06F, DMA_ONEINCR, COUNT, words + 1) |
                            NVVAL(NVB06F, DMA_ONEINCR, ADDRESS, NVB197_LAUNCH_DMA >> 2);
    const uint32_t launch = NVDEF(NVB197, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
                            NVDEF(NVB197, LAUNCH_DMA, COMPLETION_TYPE, FLUSH_ONLY);
    if (args[4] != header || args[5] != launch ||
        !checked_word_span(ranges, args[2], args[3], bytes, 4, TG_GPU_PITCH_WRITE))
        return 0;
    return 6 + words;
}

static bool report_span(uint32_t first, uint32_t last)
{
    return first <= NVB197_SET_REPORT_SEMAPHORE_D && last >= NVB197_SET_REPORT_SEMAPHORE_A;
}

static bool graphics_report_control(uint32_t control, uint32_t bytes)
{
    const uint32_t allowed = DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_OPERATION) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_FLUSH_DISABLE) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_RELEASE) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_REPORT) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_SUB_REPORT) |
                             DRF_SMASK(NVB197_SET_REPORT_SEMAPHORE_D_STRUCTURE_SIZE);
#define LOCATION_BIT(name) (1u << NVB197_SET_REPORT_SEMAPHORE_D_PIPELINE_LOCATION_##name)
    const uint32_t locations = LOCATION_BIT(NONE) | LOCATION_BIT(DATA_ASSEMBLER) |
        LOCATION_BIT(VERTEX_SHADER) | LOCATION_BIT(TESSELATION_INIT_SHADER) |
        LOCATION_BIT(TESSELATION_SHADER) | LOCATION_BIT(GEOMETRY_SHADER) |
        LOCATION_BIT(STREAMING_OUTPUT) | LOCATION_BIT(VPC) | LOCATION_BIT(ZCULL) |
        LOCATION_BIT(PIXEL_SHADER) | LOCATION_BIT(DEPTH_TEST) | LOCATION_BIT(ALL);
#undef LOCATION_BIT
    const uint32_t location = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION);
    if ((control & ~allowed) || !(locations & (1u << location)))
        return false;

    const uint32_t operation = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, OPERATION);
    const uint32_t report = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, REPORT);
    const uint32_t sub_report = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, SUB_REPORT);
    if (operation == NVB197_SET_REPORT_SEMAPHORE_D_OPERATION_RELEASE)
        return !report && !sub_report;
    if (operation != NVB197_SET_REPORT_SEMAPHORE_D_OPERATION_REPORT_ONLY)
        return false;

#define REPORT_BIT(name) (1u << NVB197_SET_REPORT_SEMAPHORE_D_REPORT_##name)
    const uint32_t queries = REPORT_BIT(ZPASS_PIXEL_CNT64) | REPORT_BIT(DA_VERTICES_GENERATED) |
        REPORT_BIT(DA_PRIMITIVES_GENERATED) | REPORT_BIT(VS_INVOCATIONS) | REPORT_BIT(GS_INVOCATIONS) |
        REPORT_BIT(GS_PRIMITIVES_GENERATED) | REPORT_BIT(CLIPPER_INVOCATIONS) |
        REPORT_BIT(CLIPPER_PRIMITIVES_GENERATED) | REPORT_BIT(PS_INVOCATIONS) |
        REPORT_BIT(TI_INVOCATIONS) | REPORT_BIT(TS_INVOCATIONS);
    const uint32_t streams = REPORT_BIT(VTG_PRIMITIVES_OUT) | REPORT_BIT(STREAMING_PRIMITIVES_SUCCEEDED) |
                             REPORT_BIT(STREAMING_PRIMITIVES_NEEDED);
#undef REPORT_BIT
    if (report == NVB197_SET_REPORT_SEMAPHORE_D_REPORT_STREAMING_BYTE_COUNT)
        return bytes == 4 && sub_report < 4;
    return bytes == 16 && ((sub_report == 0 && (queries & (1u << report))) ||
                           (sub_report < 4 && (streams & (1u << report))));
}

static bool engine_report(uint32_t subchannel, const uint32_t *args,
                          const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t control = args[3];
    const uint32_t bytes = NVVAL_GET(control, NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE) ? 4 : 16;
    const uint32_t compute_release = DRF_SMASK(NVB1C0_SET_REPORT_SEMAPHORE_D_STRUCTURE_SIZE) |
                                     DRF_SMASK(NVB1C0_SET_REPORT_SEMAPHORE_D_FLUSH_DISABLE);
    const bool valid = subchannel == 0 ? graphics_report_control(control, bytes) :
                                        !(control & ~compute_release);
    return valid && word_span(va, args[0], args[1], bytes, bytes, max_end);
}

/* B197 and B1C0 share the pool methods and 32-byte descriptor stride. */
static const struct {
    uint32_t base, index_mask;
} texture_pool_methods[TG_GPU_TEXTURE_POOLS] = {
    [TG_GPU_TEXTURE_HEADERS] = {NVB197_SET_TEX_HEADER_POOL_A,
                                DRF_MASK(NVB197_SET_TEX_HEADER_POOL_C_MAXIMUM_INDEX)},
    [TG_GPU_TEXTURE_SAMPLERS] = {NVB197_SET_TEX_SAMPLER_POOL_A,
                                 DRF_MASK(NVB197_SET_TEX_SAMPLER_POOL_C_MAXIMUM_INDEX)},
};

static enum tg_gpu_texture_pool texture_pool_in_span(uint32_t first, uint32_t last)
{
    for (unsigned pool = 0; pool < TG_GPU_TEXTURE_POOLS; pool++) {
        const uint32_t base = texture_pool_methods[pool].base;
        if (first <= base + 8 && last >= base)
            return pool;
    }
    return TG_GPU_TEXTURE_POOLS;
}

static bool texture_pool_range(enum tg_gpu_texture_pool pool, const uint32_t *args,
                               const struct tg_gpu_command_va *va,
                               struct tg_gpu_texture_pool_range *range)
{
    const uint32_t descriptor_bytes = 32;
    uint64_t end = 0;
    if ((args[2] & ~texture_pool_methods[pool].index_mask) ||
        !word_span(va, args[0], args[1], (args[2] + 1) * descriptor_bytes,
                   descriptor_bytes, &end))
        return false;
    *range = (struct tg_gpu_texture_pool_range){((uint64_t)args[0] << 32) | args[1], end};
    return true;
}

static void require_texture_pools(struct tg_gpu_command_info *info, uint32_t engine)
{
    for (unsigned pool = 0; pool < TG_GPU_TEXTURE_POOLS; pool++)
        info->requires_texture_pool[engine][pool] |= !info->texture_pool[engine][pool].end;
}

static bool pcas_span(uint32_t first, uint32_t last)
{
    return first <= NVB1C0_SEND_SIGNALING_PCAS_B && last >= NVB1C0_SEND_PCAS_A;
}

static size_t compute_dispatch(const uint32_t *args, size_t remaining)
{
    const uint32_t launch = NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) |
                            NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE);
    const uint32_t method = NVB1C0_SEND_SIGNALING_PCAS_B >> 2;
    const uint32_t immediate = NVDEF(NVB06F, DMA_IMMD, OPCODE, VALUE) |
                               NVVAL(NVB06F, DMA_IMMD, SUBCHANNEL, 1) |
                               NVVAL(NVB06F, DMA_IMMD, ADDRESS, method) |
                               NVVAL(NVB06F, DMA_IMMD, DATA, launch);
    const uint32_t incrementing = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                                  NVVAL(NVB06F, DMA_INCR, SUBCHANNEL, 1) |
                                  NVVAL(NVB06F, DMA_INCR, ADDRESS, method) |
                                  NVVAL(NVB06F, DMA_INCR, COUNT, 1);
    if (remaining < 2)
        return 0;
    if (args[1] == immediate)
        return 2;
    if (remaining >= 3 && args[1] == incrementing && args[2] == launch)
        return 3;
    return 0;
}

static bool mme_upload_span(uint32_t subchannel, uint32_t first, uint32_t last)
{
    return subchannel == 0 && first <= NVB197_LOAD_MME_START_ADDRESS_RAM &&
           last >= NVB197_LOAD_MME_INSTRUCTION_RAM_POINTER;
}

static uint32_t vertex_fallback_pair(uint32_t first, uint32_t last)
{
    if (first <= NVB197_SET_VERTEX_STREAM_SUBSTITUTE_B && last >= NVB197_SET_VERTEX_STREAM_SUBSTITUTE_A)
        return NVB197_SET_VERTEX_STREAM_SUBSTITUTE_A;
    if (first <= TG_GPU_NVK_MME_ZERO_ADDR_LO && last >= TG_GPU_NVK_MME_ZERO_ADDR_HI)
        return TG_GPU_NVK_MME_ZERO_ADDR_HI;
    return 0;
}

static bool direct_input_binding_span(uint32_t first, uint32_t last)
{
    if (first <= NVB197_SET_INDEX_BUFFER_E && last >= NVB197_SET_INDEX_BUFFER_A)
        return true;
    if (first <= NVB197_SET_VERTEX_STREAM_LIMIT_B_B(15) && last >= NVB197_SET_VERTEX_STREAM_LIMIT_A_A(0))
        return true;

    /* Format and frequency writes remain separate from the address pair
     * in each vertex stream's register block.
     */
    const uint32_t base = NVB197_SET_VERTEX_STREAM_A_FORMAT(0);
    const uint32_t stride = NVB197_SET_VERTEX_STREAM_A_FORMAT(1) - base;
    if (last < base || first > NVB197_SET_VERTEX_STREAM_B_LOCATION_B(15))
        return false;
    uint32_t index = first > base ? (first - base) / stride : 0;
    if (first > NVB197_SET_VERTEX_STREAM_A_LOCATION_B(index))
        index++;
    return last >= NVB197_SET_VERTEX_STREAM_A_LOCATION_A(index);
}

static bool input_binding_range(const uint32_t *args, const struct tg_gpu_command_va *va,
                                bool zero_addr_ready, uint64_t *max_end)
{
    /* The pre-Turing macros substitute the scratch address for an empty
     * range and encode the inclusive limit as that address minus one.
     */
    if (!args[2] || (!args[0] && !args[1]))
        return zero_addr_ready;
    return word_span(va, args[0], args[1], args[2], 1, max_end);
}

/* Byte widths for the renderable Maxwell formats in NIL's format table. */
static uint32_t color_target_element_bytes(uint32_t format)
{
    if (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_RF32_GF32_BF32_AF32 &&
        format <= NVB197_SET_COLOR_TARGET_FORMAT_V_RU32_GU32_BU32_X32)
        return 16;
    if (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_R16_G16_B16_A16 &&
        format <= NVB197_SET_COLOR_TARGET_FORMAT_V_RF16_GF16_BF16_X16)
        return 8;
    if ((format >= NVB197_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_AU2BU10GU10RU10) ||
        (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_A8B8G8R8 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_BF10GF11RF11) ||
        (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_RS32 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_X8RL8GL8BL8) ||
        format == NVB197_SET_COLOR_TARGET_FORMAT_V_X8B8G8R8 ||
        format == NVB197_SET_COLOR_TARGET_FORMAT_V_X8BL8GL8RL8)
        return 4;
    if ((format >= NVB197_SET_COLOR_TARGET_FORMAT_V_R5G6B5 &&
         format <= NVB197_SET_COLOR_TARGET_FORMAT_V_RF16) ||
        format == NVB197_SET_COLOR_TARGET_FORMAT_V_X1R5G5B5)
        return 2;
    if (format >= NVB197_SET_COLOR_TARGET_FORMAT_V_R8 && format <= NVB197_SET_COLOR_TARGET_FORMAT_V_A8)
        return 1;
    return 0;
}

static const uint8_t depth_target_element_bytes[32] = {
    [NVB197_SET_ZT_FORMAT_V_Z16] = 2,
    [NVB197_SET_ZT_FORMAT_V_Z24S8] = 4,
    [NVB197_SET_ZT_FORMAT_V_X8Z24] = 4,
    [NVB197_SET_ZT_FORMAT_V_S8Z24] = 4,
    [NVB197_SET_ZT_FORMAT_V_S8] = 1,
    [NVB197_SET_ZT_FORMAT_V_ZF32] = 4,
    [NVB197_SET_ZT_FORMAT_V_ZF32_X24S8] = 8,
};

static uint64_t tiled_target_layer_bytes(uint32_t width, uint32_t height,
                                         uint32_t element_bytes, uint32_t block_height)
{
    const uint64_t row_bytes = (uint64_t)width * element_bytes;
    const uint32_t row_mask = (8u << block_height) - 1;
    /* NVK programs width as a GOB-aligned stride in samples. */
    return row_bytes % 64 ? 0 : row_bytes * ((height + row_mask) & ~row_mask);
}

static uint64_t target_array_bytes(uint64_t layer_bytes, uint32_t pitch_words,
                                   uint32_t layers, uint32_t alignment)
{
    const uint64_t pitch = (uint64_t)pitch_words * 4;
    if (!layer_bytes || !layers || (layers > 1 && (pitch < layer_bytes || pitch % alignment)))
        return 0;
    return (layers - 1) * pitch + layer_bytes;
}

static bool color_target_span(uint32_t first, uint32_t last)
{
    const uint32_t base = NVB197_SET_COLOR_TARGET_A(0);
    const uint32_t stride = NVB197_SET_COLOR_TARGET_A(1) - base;
    if (last < base || first > NVB197_SET_COLOR_TARGET_LAYER(7))
        return false;
    const uint32_t slot = first > base ? (first - base) / stride : 0;
    return first <= NVB197_SET_COLOR_TARGET_LAYER(slot) || last >= NVB197_SET_COLOR_TARGET_A(slot + 1);
}

static bool color_target(const uint32_t *args, const struct tg_gpu_command_va *va,
                         uint64_t *max_end)
{
    const uint32_t width = args[2], height = args[3], format = args[4], layout = args[5];
    const uint32_t layers = args[6], pitch_words = args[7], layer = args[8];
    if (width > DRF_MASK(NVB197_SET_COLOR_TARGET_WIDTH_V) ||
        height > DRF_MASK(NVB197_SET_COLOR_TARGET_HEIGHT_V) ||
        layers > DRF_MASK(NVB197_SET_COLOR_TARGET_THIRD_DIMENSION_V) || layer)
        return false;
    if (format == NVB197_SET_COLOR_TARGET_FORMAT_V_DISABLED)
        return !args[0] && !args[1] && !height && !layout && !pitch_words;

    const uint32_t element_bytes = color_target_element_bytes(format);
    if (!element_bytes || !width || !height || !layers)
        return false;
    if (layout == NVDEF(NVB197, SET_COLOR_TARGET_MEMORY, LAYOUT, PITCH))
        return layers == 1 && !pitch_words && !(width % 128) &&
               word_span(va, args[0], args[1], (uint64_t)width * height, 128, max_end);

    const uint32_t mask = DRF_SMASK(NVB197_SET_COLOR_TARGET_MEMORY_BLOCK_HEIGHT) |
                          DRF_SMASK(NVB197_SET_COLOR_TARGET_MEMORY_BLOCK_DEPTH) |
                          DRF_SMASK(NVB197_SET_COLOR_TARGET_MEMORY_THIRD_DIMENSION_CONTROL);
    const uint32_t block_height = NVVAL_GET(layout, NVB197, SET_COLOR_TARGET_MEMORY, BLOCK_HEIGHT);
    const uint32_t block_depth = NVVAL_GET(layout, NVB197, SET_COLOR_TARGET_MEMORY, BLOCK_DEPTH);
    const bool volume = NVVAL_GET(layout, NVB197, SET_COLOR_TARGET_MEMORY, THIRD_DIMENSION_CONTROL);
    if ((layout & ~mask) || block_height > 5 || block_depth > 5 || (!volume && block_depth))
        return false;

    const uint32_t alignment = 512u << block_height;
    const uint64_t plane_bytes = tiled_target_layer_bytes(width, height, element_bytes, block_height);
    if (!volume)
        return word_span(va, args[0], args[1], target_array_bytes(plane_bytes, pitch_words, layers, alignment),
                         alignment, max_end);

    const uint32_t depth_mask = (1u << block_depth) - 1;
    const uint32_t padded_depth = (layers + depth_mask) & ~depth_mask;
    return plane_bytes <= TG_GPU_VA_LIMIT / padded_depth &&
           word_span(va, args[0], args[1], plane_bytes * padded_depth, alignment, max_end);
}

static bool depth_target_span(uint32_t first, uint32_t last)
{
    return (first <= NVB197_SET_ZT_ARRAY_PITCH && last >= NVB197_SET_ZT_A) ||
           (first <= NVB197_SET_ZT_SIZE_C && last >= NVB197_SET_ZT_SIZE_A) ||
           (first <= NVB197_SET_ZT_LAYER && last >= NVB197_SET_ZT_LAYER);
}

/* Read a single B197 method written by P_IMMD, which can fall back to INCR. */
static size_t read_3d_word(const uint32_t *words, size_t remaining,
                           uint32_t method, uint32_t *value)
{
    if (!remaining)
        return 0;
    const uint32_t address = method >> 2;
    const uint32_t immediate = NVDEF(NVB06F, DMA_IMMD, OPCODE, VALUE) |
                               NVVAL(NVB06F, DMA_IMMD, ADDRESS, address);
    if ((words[0] & ~DRF_SMASK(NVB06F_DMA_IMMD_DATA)) == immediate) {
        *value = NVVAL_GET(words[0], NVB06F, DMA_IMMD, DATA);
        return 1;
    }
    const uint32_t incrementing = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                                  NVVAL(NVB06F, DMA_INCR, ADDRESS, address) |
                                  NVVAL(NVB06F, DMA_INCR, COUNT, 1);
    if (remaining < 2 || words[0] != incrementing)
        return 0;
    *value = words[1];
    return 2;
}

static bool program_region_span(uint32_t first, uint32_t last)
{
    /* B197 and B1C0 use the same methods for the program region. */
    return first <= NVB197_SET_PROGRAM_REGION_B && last >= NVB197_SET_PROGRAM_REGION_A;
}

#define PROGRAM_REGION_BYTES (UINT64_C(1) << 32)

static size_t program_region(uint32_t engine, uint32_t count, const uint32_t *args,
                             size_t remaining, const struct tg_gpu_command_va *va,
                             uint64_t *end)
{
    uint32_t low;
    size_t used = 2;
    if (count == 2) {
        low = args[1];
    } else if (count == 1 && engine == TG_GPU_PROGRAM_3D) {
        /* The private draw diagnostic writes A and B in separate packets. */
        used = read_3d_word(args + 1, remaining - 1, NVB197_SET_PROGRAM_REGION_B, &low);
        if (!used)
            return 0;
        used++;
    } else {
        return 0;
    }
    uint64_t limit = 0;
    if (!word_span(va, args[0], low, PROGRAM_REGION_BYTES, 1u << TG_GPU_PAGE_SHIFT, &limit))
        return 0;
    *end = limit;
    return used;
}

static void require_program(struct tg_gpu_command_info *info, uint32_t engine)
{
    if (!info->programs.end[engine])
        info->requires_program |= 1u << engine;
}

static uint32_t pipeline_program_in_span(uint32_t first, uint32_t last)
{
    const uint32_t base = NVB197_SET_PIPELINE_PROGRAM(0);
    const uint32_t stride = NVB197_SET_PIPELINE_PROGRAM(1) - base;
    const uint32_t offset = first > base ? first - base : 0;
    const uint32_t method = base + ((offset + stride - 1) / stride) * stride;
    return method <= last && method <= NVB197_SET_PIPELINE_PROGRAM(TG_GPU_GRAPHICS_PROGRAMS - 1) ? method : 0;
}

#define B197_PROGRAM_HEADER_BYTES 80u
#define PROGRAM_FETCH_ALIGN       128u
#define PROGRAM_PREFETCH_GUARD    2048u

/* The Windows GM20B profile has two TPCs with 64 warps each. */
#define GM20B_TPCS 2u
#define GM20B_WARPS_PER_TPC 64u

static bool program_offset(uint32_t offset, uint32_t header_bytes)
{
    /* Leave room for the first aligned fetch and NVK's 2 KiB I-cache guard. */
    const uint64_t entry = (uint64_t)offset + header_bytes;
    return entry % PROGRAM_FETCH_ALIGN == 0 &&
           entry + PROGRAM_FETCH_ALIGN + PROGRAM_PREFETCH_GUARD <= PROGRAM_REGION_BYTES;
}

#define QMD_GET(field) NVVAL_MW_GET(qmd, NVB1C0, QMDV00_06, field)
static bool qmd_constant_buffers(const uint32_t *qmd, const struct tg_gpu_checked_ranges *ranges)
{
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t lo = QMD_GET(CONSTANT_BUFFER_ADDR_LOWER(i));
        const uint32_t hi = QMD_GET(CONSTANT_BUFFER_ADDR_UPPER(i));
        const uint32_t bytes = QMD_GET(CONSTANT_BUFFER_SIZE(i));
        if (QMD_GET(CONSTANT_BUFFER_VALID(i))) {
            if (bytes > 65536 || bytes % 256 ||
                !checked_word_span(ranges, hi, lo, bytes, 256, TG_GPU_PITCH_READ))
                return false;
        } else if (lo || hi || bytes) {
            return false;
        }
    }
    return true;
}

static bool qmd_0_6(const uint32_t *qmd, const struct tg_gpu_checked_ranges *ranges,
                    struct tg_gpu_command_info *info)
{
#define QMD_WORD(field) DRF_LW_IDX(qmd, NVB1C0_QMDV00_06_##field)
#define QMD_MASK(field) DRF_LW_SMASK(qmd, NVB1C0_QMDV00_06_##field)
#define QMD_CBUF(i) \
    [QMD_WORD(CONSTANT_BUFFER_ADDR_LOWER(i))] = QMD_MASK(CONSTANT_BUFFER_ADDR_LOWER(i)), \
    [QMD_WORD(CONSTANT_BUFFER_ADDR_UPPER(i))] = QMD_MASK(CONSTANT_BUFFER_ADDR_UPPER(i)) | \
                                              QMD_MASK(CONSTANT_BUFFER_SIZE(i))
    /* The QMD 0.6 fields initialized by NAK for a direct launch. */
    static const uint32_t allowed[TG_GPU_QMD_WORDS] = {
        [QMD_WORD(PROGRAM_OFFSET)] = QMD_MASK(PROGRAM_OFFSET),
        [QMD_WORD(API_VISIBLE_CALL_LIMIT)] = QMD_MASK(API_VISIBLE_CALL_LIMIT),
        [QMD_WORD(CTA_RASTER_WIDTH)] = QMD_MASK(CTA_RASTER_WIDTH),
        [QMD_WORD(CTA_RASTER_HEIGHT)] = QMD_MASK(CTA_RASTER_HEIGHT) | QMD_MASK(CTA_RASTER_DEPTH),
        [QMD_WORD(SHARED_MEMORY_SIZE)] = QMD_MASK(SHARED_MEMORY_SIZE),
        [QMD_WORD(QMD_VERSION)] = QMD_MASK(QMD_VERSION) | QMD_MASK(CTA_THREAD_DIMENSION0),
        [QMD_WORD(CTA_THREAD_DIMENSION1)] = QMD_MASK(CTA_THREAD_DIMENSION1) | QMD_MASK(CTA_THREAD_DIMENSION2),
        [QMD_WORD(CONSTANT_BUFFER_VALID(0))] = 0xff | QMD_MASK(L1_CONFIGURATION),
        QMD_CBUF(0), QMD_CBUF(1), QMD_CBUF(2), QMD_CBUF(3),
        QMD_CBUF(4), QMD_CBUF(5), QMD_CBUF(6), QMD_CBUF(7),
        [QMD_WORD(SHADER_LOCAL_MEMORY_LOW_SIZE)] = QMD_MASK(SHADER_LOCAL_MEMORY_LOW_SIZE) | QMD_MASK(BARRIER_COUNT),
        [QMD_WORD(REGISTER_COUNT)] = QMD_MASK(REGISTER_COUNT),
        [QMD_WORD(SASS_VERSION)] = QMD_MASK(SASS_VERSION) | QMD_MASK(SHADER_LOCAL_MEMORY_CRS_SIZE),
    };
    for (size_t i = 0; i < TG_GPU_QMD_WORDS; i++) {
        if (qmd[i] & ~allowed[i])
            return false;
    }
    const uint32_t x = QMD_GET(CTA_THREAD_DIMENSION0), y = QMD_GET(CTA_THREAD_DIMENSION1);
    const uint32_t z = QMD_GET(CTA_THREAD_DIMENSION2), shared = QMD_GET(SHARED_MEMORY_SIZE);
    const uint32_t slm = QMD_GET(SHADER_LOCAL_MEMORY_LOW_SIZE);
    const uint32_t crs = QMD_GET(SHADER_LOCAL_MEMORY_CRS_SIZE);
    const uint32_t l1 = shared <= 16 * 1024 ? 1 : shared <= 32 * 1024 ? 2 : 3;
    if (QMD_GET(QMD_VERSION) != 6 || QMD_GET(SASS_VERSION) != 0x30 ||
        QMD_GET(API_VISIBLE_CALL_LIMIT) != NVB1C0_QMDV00_06_API_VISIBLE_CALL_LIMIT_NO_CHECK ||
        !program_offset(QMD_GET(PROGRAM_OFFSET), 0) || QMD_GET(CTA_RASTER_WIDTH) > INT32_MAX ||
        !x || !y || !z || z > 64 || (uint64_t)x * y * z > 1024 ||
        shared > 48 * 1024 || shared % 256 || QMD_GET(L1_CONFIGURATION) != l1 ||
        slm % 16 || crs % 512 || crs > (1u << 20))
        return false;

    if (!qmd_constant_buffers(qmd, ranges))
        return false;
    const uint64_t bytes_per_tpc = ((uint64_t)slm * 32 + crs) * GM20B_WARPS_PER_TPC;
    if (info->compute_slm.va_end)
        return bytes_per_tpc <= info->compute_slm.bytes_per_tpc;
    if (bytes_per_tpc > info->required_compute_slm_per_tpc)
        info->required_compute_slm_per_tpc = bytes_per_tpc;
    return true;
#undef QMD_CBUF
#undef QMD_GET
#undef QMD_MASK
#undef QMD_WORD
}

static size_t qmd_record(const uint32_t *args, size_t remaining,
                         const struct tg_gpu_checked_ranges *ranges, struct tg_gpu_command_info *info)
{
    const size_t record = TG_GPU_QMD_RECORD_WORDS;
    const uint32_t pcas_a = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                            NVVAL(NVB06F, DMA_INCR, COUNT, 1) |
                            NVVAL(NVB06F, DMA_INCR, SUBCHANNEL, 1) |
                            NVVAL(NVB06F, DMA_INCR, ADDRESS, NVB1C0_SEND_PCAS_A >> 2);
    if (remaining < record + 3 || args[record] != pcas_a || args[record + 1])
        return 0;
    const size_t used = compute_dispatch(args + record + 1, remaining - record - 1);
    return used && qmd_0_6(args, ranges, info) ? record + 1 + used : 0;
}

static size_t qmd_aligned_word(size_t first)
{
    return (first + TG_GPU_QMD_WORDS - 1) & ~(size_t)(TG_GPU_QMD_WORDS - 1);
}

static bool stage_qmd_addr(uint32_t *words, size_t first, const struct tg_gpu_command_va *va,
                           uint64_t *max_end, uint64_t *qmd_va_out)
{
    const size_t aligned = qmd_aligned_word(first);
    const size_t offset = TG_GPU_COMMAND_QMD + aligned * sizeof(*words);
    const size_t end = offset + TG_GPU_QMD_WORDS * sizeof(*words);
    if (!va || !va->reserved_base || end > TG_GPU_COMMAND_QMD + TG_GPU_COMMAND_QMD_BYTES ||
        end > va->reserved_end - va->reserved_base ||
        (va->reserved_base + offset) % (TG_GPU_QMD_WORDS * sizeof(*words)))
        return false;
    /* Keep a checked, aligned copy in the immutable no-op payload. PCAS uses
     * the separate writable mirror published by the serialized worker:
     * DFALCON's QMD writeback must never make command pages writable.
     */
    for (size_t i = TG_GPU_QMD_WORDS; i > 0; i--)
        words[aligned + i - 1] = words[first + i - 1];
    const uint64_t qmd_va = va->reserved_base + offset;
    words[first + TG_GPU_QMD_RECORD_WORDS + 1] = (uint32_t)(qmd_va >> 8);
    if (va->reserved_base + end > *max_end)
        *max_end = va->reserved_base + end;
    if (qmd_va_out)
        *qmd_va_out = qmd_va;
    return true;
}

static bool stage_qmd(uint32_t *words, size_t first, const struct tg_gpu_command_va *va,
                       uint64_t *max_end)
{
    return stage_qmd_addr(words, first, va, max_end, NULL);
}

static bool shader_local_memory_span(uint32_t engine, uint32_t first, uint32_t last)
{
    if (engine == TG_GPU_PROGRAM_3D)
        return first <= NVB197_SET_SHADER_LOCAL_MEMORY_E && last >= NVB197_SET_SHADER_LOCAL_MEMORY_A;
    return (first <= NVB1C0_SET_SHADER_LOCAL_MEMORY_B && last >= NVB1C0_SET_SHADER_LOCAL_MEMORY_A) ||
           (first <= NVB1C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_C &&
            last >= NVB1C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A);
}

static size_t shader_local_memory(uint32_t engine, uint32_t count, const uint32_t *args,
                                  size_t remaining, const struct tg_gpu_command_va *va,
                                  struct tg_gpu_command_info *info)
{
    const uint32_t tpc_alignment = 0x8000, warp_alignment = 0x200;
    uint64_t bytes, bytes_per_tpc = 0, end = 0;
    size_t used;
    if (engine == TG_GPU_PROGRAM_3D) {
        if (count != 5 || args[2] > DRF_MASK(NVB197_SET_SHADER_LOCAL_MEMORY_C_SIZE_UPPER) ||
            args[4] > DRF_MASK(NVB197_SET_SHADER_LOCAL_MEMORY_E_DEFAULT_SIZE_PER_WARP) ||
            !args[4] || args[4] % warp_alignment)
            return 0;
        bytes = ((uint64_t)args[2] << 32) | args[3];
        if (bytes % 0x20000 || bytes < (uint64_t)args[4] * GM20B_WARPS_PER_TPC * GM20B_TPCS)
            return 0;
        used = 5;
    } else {
        const uint32_t header = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                                NVVAL(NVB06F, DMA_INCR, SUBCHANNEL, 1) |
                                NVVAL(NVB06F, DMA_INCR, COUNT, 3);
        if (count != 2 || remaining < 10 ||
            args[2] != (header | (NVB1C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A >> 2)) ||
            args[6] != (header | (NVB1C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A >> 2)) ||
            args[3] > UINT8_MAX || args[4] % tpc_alignment || args[5] != 0xff ||
            args[7] != args[3] || args[8] != args[4] || args[9] != args[5])
            return 0;
        bytes_per_tpc = ((uint64_t)args[3] << 32) | args[4];
        bytes = bytes_per_tpc * GM20B_TPCS;
        used = 10;
    }
    if (!word_span(va, args[0], args[1], bytes, 1u << TG_GPU_PAGE_SHIFT, &end))
        return 0;
    if (bytes_per_tpc)
        info->compute_slm = (struct tg_gpu_compute_slm){bytes_per_tpc, end};
    if (end > info->va_end)
        info->va_end = end;
    return used;
}

static size_t color_target_setup(uint32_t method, uint32_t count, const uint32_t *args,
                                  size_t remaining, const struct tg_gpu_command_va *va,
                                  uint64_t *max_end)
{
    if (count == 9)
        return color_target(args, va, max_end) ? 9 : 0;
    if (count != 1)
        return 0;

    /* The private clear diagnostic writes the same nine registers singly. */
    uint32_t target[9] = {args[0]};
    size_t used = 1;
    for (uint32_t i = 1; i < 9; i++) {
        const size_t words = read_3d_word(args + used, remaining - used,
                                          method + i * 4, &target[i]);
        if (!words)
            return 0;
        used += words;
    }
    return color_target(target, va, max_end) ? used : 0;
}

static size_t depth_target(const uint32_t *args, size_t remaining,
                            const struct tg_gpu_command_va *va, uint64_t *max_end)
{
    const uint32_t format = args[2], block = args[3], pitch_words = args[4];
    const uint32_t block_height = NVVAL_GET(block, NVB197, SET_ZT_BLOCK_SIZE, HEIGHT);
    if (format >= sizeof(depth_target_element_bytes) || !depth_target_element_bytes[format] ||
        (block & ~DRF_SMASK(NVB197_SET_ZT_BLOCK_SIZE_HEIGHT)) || block_height > 5)
        return 0;

    uint32_t enabled, layer;
    const size_t select_words = read_3d_word(args + 5, remaining - 5, NVB197_SET_ZT_SELECT, &enabled);
    const size_t size_pos = 5 + select_words;
    const uint32_t size_header = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
                                 NVVAL(NVB06F, DMA_INCR, ADDRESS, NVB197_SET_ZT_SIZE_A >> 2) |
                                 NVVAL(NVB06F, DMA_INCR, COUNT, 3);
    if (!select_words || enabled != 1 || remaining - size_pos < 4 || args[size_pos] != size_header)
        return 0;
    const uint32_t width = args[size_pos + 1], height = args[size_pos + 2], layers = args[size_pos + 3];
    const size_t layer_pos = size_pos + 4;
    const size_t layer_words = read_3d_word(args + layer_pos, remaining - layer_pos, NVB197_SET_ZT_LAYER, &layer);
    if (!layer_words || width > DRF_MASK(NVB197_SET_ZT_SIZE_A_WIDTH) ||
        height > DRF_MASK(NVB197_SET_ZT_SIZE_B_HEIGHT) ||
        layers > DRF_MASK(NVB197_SET_ZT_SIZE_C_THIRD_DIMENSION) || layer >= layers)
        return 0;

    const uint32_t alignment = 512u << block_height;
    const uint64_t plane_bytes = tiled_target_layer_bytes(width, height, depth_target_element_bytes[format],
                                                          block_height);
    const uint64_t bytes = target_array_bytes(plane_bytes, pitch_words, layers, alignment);
    return word_span(va, args[0], args[1], bytes, alignment, max_end) ? layer_pos + layer_words : 0;
}

static bool cb0_scratch_span(uint32_t first, uint32_t last)
{
    return first <= TG_GPU_NVK_MME_CB0_ADDR_LO && last >= TG_GPU_NVK_MME_CB0_ADDR_HI;
}

static bool cbuf_data_span(uint32_t first, uint32_t last)
{
    return first < NVB197_BIND_GROUP_RESERVED_A(0) && last >= NVB197_SET_CONSTANT_BUFFER_SELECTOR_A;
}

static uint32_t cbuf_group_in_span(uint32_t first, uint32_t last)
{
    const uint32_t base = NVB197_BIND_GROUP_CONSTANT_BUFFER(0);
    const uint32_t stride = NVB197_BIND_GROUP_CONSTANT_BUFFER(1) - base;
    const uint32_t offset = first > base ? first - base : 0;
    const uint32_t group = base + ((offset + stride - 1) / stride) * stride;
    return group <= last && group <= NVB197_BIND_GROUP_CONSTANT_BUFFER(4) ? group : 0;
}

static bool cbuf_binding_value(uint32_t value, bool direct)
{
    const uint32_t mask = DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_VALID) |
                          DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_SHADER_SLOT);
    const uint32_t slot = NVVAL_GET(value, NVB197, BIND_GROUP_CONSTANT_BUFFER, SHADER_SLOT);
    const bool valid = NVVAL_GET(value, NVB197, BIND_GROUP_CONSTANT_BUFFER, VALID);
    return !(value & ~mask) && slot < 16 && (direct ? valid && slot : !valid || !slot);
}

static size_t direct_cbuf_binding(const uint32_t *args, size_t remaining,
                                  const struct tg_gpu_checked_ranges *ranges)
{
    const uint32_t alignment = 256;
    const uint32_t bytes = args[0];
    if (remaining < 6 || bytes > 65536 || (bytes & (alignment - 1)))
        return 0;

    const uint32_t method = NVVAL_GET(args[3], NVB06F, DMA_INCR, ADDRESS) * 4;
    if (!cbuf_group_in_span(method, method))
        return 0;
    uint32_t bind;
    const size_t bind_words = read_3d_word(args + 3, remaining - 3, method, &bind);
    const size_t next = 3 + bind_words;
    const uint32_t restore = NVDEF(NVB06F, DMA_ONEINCR, OPCODE, VALUE) |
                             NVVAL(NVB06F, DMA_ONEINCR, COUNT, 1) |
                             NVVAL(NVB06F, DMA_ONEINCR, ADDRESS, NVB197_CALL_MME_MACRO(NVK_MME_SELECT_CB0) >> 2);
    if (!bind_words || !cbuf_binding_value(bind, true) || remaining - next < 2 ||
        args[next] != restore || args[next + 1] ||
        !checked_word_span(ranges, args[1], args[2], bytes, alignment, TG_GPU_PITCH_READ))
        return 0;
    /* Leave SELECT_CB0 to the normal MME validation below. */
    return next;
}

static bool mme_call_allowed(uint32_t op, uint32_t method, uint32_t count,
                             const uint32_t *args, const struct tg_gpu_command_va *va,
                             bool zero_addr_ready, uint64_t *max_end)
{
    const uint32_t base = NVB197_CALL_MME_MACRO(0);
    if (op != NVB06F_DMA_ONEINCR_OPCODE_VALUE || method < base)
        return false;
    const uint32_t offset = method - base;
    const uint32_t slot = offset / 8;
    if (offset % 8 || slot >= NVK_MME_COUNT || count != nvk_mme_call_words[slot])
        return false;

    switch (slot) {
    case NVK_MME_SELECT_CB0:
        return !args[0];
    case NVK_MME_CLEAR: {
        const uint32_t base_layer = NVVAL_GET(args[0], NVB197, CLEAR_SURFACE, RT_ARRAY_INDEX);
        const uint32_t max_layers = 2048;
        return base_layer < max_layers && args[1] && args[1] <= max_layers - base_layer;
    }
    case NVK_MME_BIND_IB:
        return input_binding_range(args, va, zero_addr_ready, max_end);
    case NVK_MME_BIND_VB:
        return args[0] < 32 && input_binding_range(args + 1, va, zero_addr_ready, max_end);
    case NVK_MME_SET_WRITE_MASK:
        return args[0] <= 8;
    case NVK_MME_SET_VIEWPORT_MIN_MAX_Z:
        return args[0] < 16;
    case NVK_MME_WRITE_CS_INVOCATIONS:
        /* This macro writes two adjacent one-word reports. */
        return word_span(va, args[0], args[1], 8, 4, max_end);
    default:
        return true;
    }
}

#define TG_GPU_NVK_CS_GROUP_COUNT_OFFSET 12u
#define TG_GPU_NVK_CS_GROUP_COUNT_BYTES  12u
enum {
    XFB_POINTER_STRIDE_DWORDS = (NVB197_SET_STREAM_OUT_BUFFER_LOAD_WRITE_POINTER(1) -
                                NVB197_SET_STREAM_OUT_BUFFER_LOAD_WRITE_POINTER(0)) / sizeof(uint32_t),
};

static bool indirect_mme_call(uint32_t op, uint32_t method, uint32_t count,
                              const uint32_t *args, size_t inline_words,
                              const struct tg_gpu_submit_indirect *indirect,
                              struct tg_gpu_command_info *stream,
                              uint32_t xfb_selector_stride, uint32_t *slot_out)
{
    const uint32_t base = NVB197_CALL_MME_MACRO(0);
    if (op != NVB06F_DMA_ONEINCR_OPCODE_VALUE || method < base)
        return false;
    const uint32_t offset = method - base;
    const uint32_t slot = offset / 8;
    const uint32_t indirect_words = indirect->bytes / sizeof(*args);
    if (offset % 8 || slot >= NVK_MME_COUNT || !indirect_words)
        return false;

    switch (slot) {
    case NVK_MME_DRAW_INDIRECT:
    case NVK_MME_DRAW_INDEXED_INDIRECT: {
        const uint32_t cmd_words = slot == NVK_MME_DRAW_INDIRECT ? 4u : 5u;
        if (indirect->suffix_offset || inline_words != 2 ||
            count != inline_words + indirect_words || !args[0])
            return false;
        const uint32_t draw_count = args[0], pad_words = args[1];
        if (draw_count == 1) {
            if (pad_words || indirect_words != cmd_words)
                return false;
        } else {
            if (pad_words > indirect_words ||
                (uint64_t)draw_count * ((uint64_t)cmd_words + pad_words) != indirect_words)
                return false;
        }
        stream->requires_cb0 |= !stream->cb0_end;
        require_program(stream, TG_GPU_PROGRAM_3D);
        require_texture_pools(stream, TG_GPU_PROGRAM_3D);
        break;
    }
    case NVK_MME_XFB_DRAW_INDIRECT:
        if (indirect->suffix_offset || inline_words != 2 || indirect_words != 1 ||
            count != 3 || !args[0])
            return false;
        stream->requires_cb0 |= !stream->cb0_end;
        require_program(stream, TG_GPU_PROGRAM_3D);
        require_texture_pools(stream, TG_GPU_PROGRAM_3D);
        break;
    case NVK_MME_XFB_COUNTER_LOAD:
        if (indirect->suffix_offset || inline_words != 1 || indirect_words != 1 ||
            count != 2 || args[0] % xfb_selector_stride || args[0] / xfb_selector_stride >= 4)
            return false;
        break;
    case NVK_MME_DISPATCH_INDIRECT:
        if (!indirect->suffix_offset || inline_words != 0 || indirect_words != 3 ||
            count != 8)
            return false;
        break;
    default:
        return false;
    }
    stream->requires_mme |= !stream->uploads_mme;
    *slot_out = slot;
    return true;
}

static size_t wfi_scope_all_words(const uint32_t *words, size_t remaining)
{
    if (!remaining)
        return 0;
    const uint32_t header = words[0];
    if ((header & (1u << 12)) ||
        NVVAL_GET(header, NVB06F, DMA_INCR, SUBCHANNEL) != 0 ||
        NVVAL_GET(header, NVB06F, DMA_INCR, ADDRESS) * 4 != NVB06F_WFI)
        return 0;
    const uint32_t op = NVVAL_GET(header, NVB06F, DMA_INCR, OPCODE);
    const uint32_t count = NVVAL_GET(header, NVB06F, DMA_INCR, COUNT);
    if (op == NVB06F_DMA_IMMD_OPCODE_VALUE)
        return count == NVB06F_WFI_SCOPE_ALL ? 1 : 0;
    if ((op == NVB06F_DMA_INCR_OPCODE_VALUE || op == NVB06F_DMA_ONEINCR_OPCODE_VALUE) &&
        count == 1 && remaining >= 2 && words[1] == NVB06F_WFI_SCOPE_ALL)
        return 2;
    return 0;
}

static bool dispatch_indirect_suffix(const uint32_t *words, uint32_t *staging,
                                     size_t s, size_t word_count,
                                     const struct tg_gpu_checked_ranges *ranges,
                                     struct tg_gpu_command_info *stream)
{
    if (word_count - s < 7 + TG_GPU_QMD_RECORD_WORDS + 3)
        return false;
    /* MME writes group_count[3] into root_desc + 12 (256-byte aligned root). */
    if (words[s] > UINT8_MAX || (words[s + 1] & 0xff) || words[s + 2] || words[s + 3])
        return false;
    const uint64_t root_base = ((uint64_t)words[s] << 32) | words[s + 1];
    if (root_base > UINT64_MAX - (TG_GPU_NVK_CS_GROUP_COUNT_OFFSET + TG_GPU_NVK_CS_GROUP_COUNT_BYTES) ||
        !tg_gpu_command_range_allowed(ranges->va, root_base + TG_GPU_NVK_CS_GROUP_COUNT_OFFSET,
                                      TG_GPU_NVK_CS_GROUP_COUNT_BYTES, ranges->max_end))
        return false;
    const uint32_t local_size = words[s + 4];
    if (!local_size || local_size > 1024)
        return false;

    const size_t wfi_words = wfi_scope_all_words(words + s + 5, word_count - (s + 5));
    if (!wfi_words)
        return false;
    const size_t q_hdr = s + 5 + wfi_words;
    const uint32_t qmd_nop = NVDEF(NVB06F, DMA_NONINCR, OPCODE, VALUE) |
                             NVVAL(NVB06F, DMA_NONINCR, SUBCHANNEL, TG_GPU_PROGRAM_COMPUTE) |
                             NVVAL(NVB06F, DMA_NONINCR, COUNT, TG_GPU_QMD_RECORD_WORDS) |
                             NVVAL(NVB06F, DMA_NONINCR, ADDRESS, NVB1C0_NO_OPERATION >> 2);
    if (q_hdr >= word_count || words[q_hdr] != qmd_nop)
        return false;
    const size_t q_first = q_hdr + 1;
    const size_t used = qmd_record(words + q_first, word_count - q_first, ranges, stream);
    if (!used || q_first + used != word_count)
        return false;

    const uint32_t *raw_qmd = words + q_first;
    const uint32_t threads = NVVAL_MW_GET(raw_qmd, NVB1C0, QMDV00_06, CTA_THREAD_DIMENSION0) *
                             NVVAL_MW_GET(raw_qmd, NVB1C0, QMDV00_06, CTA_THREAD_DIMENSION1) *
                             NVVAL_MW_GET(raw_qmd, NVB1C0, QMDV00_06, CTA_THREAD_DIMENSION2);
    if (local_size != threads)
        return false;

    if (staging) {
        uint64_t qmd_va = 0;
        if (!stage_qmd_addr(staging, q_first, ranges->va, ranges->max_end, &qmd_va))
            return false;
        staging[s + 2] = (uint32_t)(qmd_va >> 32);
        staging[s + 3] = (uint32_t)qmd_va;
    }
    stream->has_qmd = true;
    require_program(stream, TG_GPU_PROGRAM_COMPUTE);
    require_texture_pools(stream, TG_GPU_PROGRAM_COMPUTE);
    return true;
}

static bool graphics_draw_packet(uint32_t first, uint32_t last);

static bool inspect(const uint32_t *words, uint32_t *staging, size_t bytes,
                    const struct tg_gpu_submit_indirect *indirect,
                    const struct tg_gpu_command_va *va, struct tg_gpu_command_info *info)
{
    struct tg_gpu_command_info stream = {.reserved_base = va ? va->reserved_base : 0};
    const struct tg_gpu_checked_ranges ranges = {.va = va, .max_end = &stream.va_end};
    struct tg_gpu_copy_state copy = {0};
    uint64_t cb0_setup_end = 0;
    bool zero_addr_ready = false;
    bool host_wfi = false;
    uint32_t indirect_slot = NVK_MME_COUNT;
    if(!words || !bytes || (bytes&3) || ((uintptr_t)words&3))return false;
    if (va && (!va->limit || va->limit > TG_GPU_VA_LIMIT ||
               va->reserved_base > va->reserved_end || va->reserved_end > va->limit))
        return false;
    const size_t word_count = bytes / sizeof(*words);
    size_t prefix_words = word_count;
    if (indirect) {
        if (!indirect->bytes || (indirect->bytes & 3) ||
            indirect->bytes > TG_GPU_SUBMIT_MAX_BYTES || (indirect->address & 3) ||
            !tg_gpu_command_range_allowed(va, indirect->address, indirect->bytes, &stream.va_end))
            return false;
        if (indirect->suffix_offset) {
            if ((indirect->suffix_offset & 3) || indirect->suffix_offset >= bytes)
                return false;
            prefix_words = indirect->suffix_offset / sizeof(*words);
        }
        if (!prefix_words)
            return false;
    }
    for(size_t n=0;n<prefix_words;) {
        const size_t start = n;
        uint32_t header=words[n++],op=NVVAL_GET(header,NVB06F,DMA_INCR,OPCODE);
        uint32_t method=NVVAL_GET(header,NVB06F,DMA_INCR,ADDRESS)*4;
        uint32_t count=NVVAL_GET(header,NVB06F,DMA_INCR,COUNT),last;
        if(header==NVB06F_DMA_NOP)continue;
        uint32_t subchannel=NVVAL_GET(header,NVB06F,DMA_INCR,SUBCHANNEL);
        if (subchannel == 3) {
            if (!halfturn(words + start, word_count - start, &ranges)) return false;
            n = start + TG_GPU_PRESENT_HALFTURN_WORDS;
            continue;
        }
        if((header&(1u<<12)) || !nvk_class[subchannel])return false;
        if(op==NVB06F_DMA_IMMD_OPCODE_VALUE) {
            if(!method_allowed(method) || method==NVB06F_SET_OBJECT ||
               mme_upload_span(subchannel, method, method))return false;
            if (method == NVB06F_WFI) {
                if (count > NVB06F_WFI_SCOPE_ALL)
                    return false;
                if (count == NVB06F_WFI_SCOPE_ALL)
                    host_wfi = true;
                continue;
            }
            if (subchannel == 0 &&
                (method >= NVB197_CALL_MME_MACRO(0) || direct_input_binding_span(method, method) ||
                 vertex_fallback_pair(method, method) || color_target_span(method, method) ||
                 depth_target_span(method, method) || (method == NVB197_SET_ZT_SELECT && count) ||
                 cb0_scratch_span(method, method) || cbuf_data_span(method, method) ||
                 stream_out_buffer_in_span(method, method)))
                return false;
            if (subchannel == 0 && graphics_draw_packet(method, method))
                host_wfi = false;
            if (subchannel == 0 && cbuf_group_in_span(method, method)) {
                const uint32_t value = NVVAL_GET(header, NVB06F, DMA_IMMD, DATA);
                if (!cbuf_binding_value(value, false))
                    return false;
                stream.requires_cb0 |= (value & DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_VALID)) && !stream.cb0_end;
            }
            if (subchannel <= TG_GPU_PROGRAM_COMPUTE && (inline_upload_span(method, method) ||
                                    report_span(method, method) ||
                                    global_render_enable_span(method, method) ||
                                    texture_pool_in_span(method, method) != TG_GPU_TEXTURE_POOLS ||
                                    program_region_span(method, method) ||
                                    shader_local_memory_span(subchannel, method, method)))
                return false;
            if (subchannel == TG_GPU_PROGRAM_3D && pipeline_program_in_span(method, method)) {
                if (!program_offset(NVVAL_GET(header, NVB06F, DMA_IMMD, DATA), B197_PROGRAM_HEADER_BYTES))
                    return false;
                require_program(&stream, TG_GPU_PROGRAM_3D);
            }
            if (subchannel == 1 && pcas_span(method, method))
                return false;
            if (subchannel == 4 && method >= 0x100) {
                if (!tg_gpu_copy_method(&copy, method, NVVAL_GET(header, NVB06F, DMA_IMMD, DATA),
                                        va, &stream.va_end))
                    return false;
                if (method == NVB0B5_LAUNCH_DMA)
                    host_wfi = false;
            }
            continue;
        }
        if((op!=NVB06F_DMA_INCR_OPCODE_VALUE && op!=NVB06F_DMA_NONINCR_OPCODE_VALUE &&
            op!=NVB06F_DMA_ONEINCR_OPCODE_VALUE) || !count)return false;
        if (count > prefix_words - n) {
            if (!indirect || !host_wfi || subchannel != 0 ||
                !indirect_mme_call(op, method, count, words + n, prefix_words - n,
                                   indirect, &stream, 1, &indirect_slot))
                return false;
            n = prefix_words;
            break;
        }
        if(method==NVB06F_SET_OBJECT && (count!=1 || words[n]!=nvk_class[subchannel]))return false;
        if (!packet_last_method(op, method, count, &last)) return false;
        if (method <= NVB06F_SEMAPHORED && last >= NVB06F_SEMAPHOREA) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB06F_SEMAPHOREA ||
                count != 4 || !host_acquire(words + n, va, &stream.va_end))
                return false;
            n += count;
            continue;
        }
        if(!method_allowed(method) || !method_allowed(last))return false;
        /* Incrementing packets can span a forbidden host register even if
         * their first and last methods are both individually permitted. */
        if(last!=method && method<0x100)return false;
        if (method == NVB06F_WFI) {
            for (uint32_t i = 0; i < count; i++) {
                if (words[n + i] > NVB06F_WFI_SCOPE_ALL)
                    return false;
                if (words[n + i] == NVB06F_WFI_SCOPE_ALL)
                    host_wfi = true;
            }
            n += count;
            continue;
        }
        if (subchannel <= 1 && global_render_enable_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_GLOBAL_RENDER_ENABLE_A ||
                count != 3 || !global_render_enable(words + n, &ranges))
                return false;
            n += count;
            continue;
        }
        if (subchannel <= 1 && inline_upload_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_LINE_LENGTH_IN || count != 4)
                return false;
            const size_t used = inline_upload(subchannel, words + n, prefix_words - n, &ranges);
            if (!used)
                return false;
            host_wfi = false;
            n += used;
            continue;
        }
        if (subchannel <= 1 && report_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_REPORT_SEMAPHORE_A ||
                count != 4 || !engine_report(subchannel, words + n, va, &stream.va_end))
                return false;
            host_wfi = false;
        }
        const enum tg_gpu_texture_pool pool = subchannel <= TG_GPU_PROGRAM_COMPUTE ?
            texture_pool_in_span(method, last) : TG_GPU_TEXTURE_POOLS;
        if (pool != TG_GPU_TEXTURE_POOLS) {
            struct tg_gpu_texture_pool_range *range = &stream.texture_pool[subchannel][pool];
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || count != 3 ||
                method != texture_pool_methods[pool].base ||
                !texture_pool_range(pool, words + n, va, range))
                return false;
            if (range->end > stream.va_end)
                stream.va_end = range->end;
        }
        if (subchannel <= TG_GPU_PROGRAM_COMPUTE && program_region_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_PROGRAM_REGION_A)
                return false;
            uint64_t *end = &stream.programs.end[subchannel];
            const size_t used = program_region(subchannel, count, words + n, prefix_words - n, va, end);
            if (!used)
                return false;
            if (*end > stream.va_end)
                stream.va_end = *end;
            n += used;
            continue;
        }
        if (subchannel <= TG_GPU_PROGRAM_COMPUTE && shader_local_memory_span(subchannel, method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_SHADER_LOCAL_MEMORY_A)
                return false;
            const size_t used = shader_local_memory(subchannel, count, words + n, prefix_words - n,
                                                    va, &stream);
            if (!used)
                return false;
            n += used;
            continue;
        }
        if (subchannel == TG_GPU_PROGRAM_COMPUTE && method == NVB1C0_NO_OPERATION &&
            op == NVB06F_DMA_NONINCR_OPCODE_VALUE && count == TG_GPU_QMD_RECORD_WORDS) {
            const size_t used = qmd_record(words + n, prefix_words - n, &ranges, &stream);
            if (!used || (staging && !stage_qmd(staging, n, va, &stream.va_end)))
                return false;
            stream.has_qmd = true;
            host_wfi = false;
            require_program(&stream, TG_GPU_PROGRAM_COMPUTE);
            require_texture_pools(&stream, TG_GPU_PROGRAM_COMPUTE);
            n += used;
            continue;
        }
        if (subchannel == TG_GPU_PROGRAM_COMPUTE && pcas_span(method, last)) {
            /* Read-only replay can inspect the old external-QMD packet. */
            if (staging || op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB1C0_SEND_PCAS_A || count != 1)
                return false;
            const size_t used = compute_dispatch(words + n, prefix_words - n);
            if (!used || !tg_gpu_command_range_allowed(va, (uint64_t)words[n] << 8,
                                                     TG_GPU_QMD_WORDS * sizeof(*words), &stream.va_end))
                return false;
            host_wfi = false;
            require_program(&stream, TG_GPU_PROGRAM_COMPUTE);
            n += used;
            continue;
        }
        if (subchannel == 0) {
            const uint32_t xfb_enable = stream_out_buffer_in_span(method, last);
            if (xfb_enable) {
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != xfb_enable ||
                    count != 4 || !stream_out_buffer(words + n, &ranges))
                    return false;
                n += count;
                continue;
            }
            if (graphics_draw_packet(method, last) && last < NVB197_CALL_MME_MACRO(0))
                host_wfi = false;
            const uint32_t program = pipeline_program_in_span(method, last);
            if (program) {
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != program || count != 1 ||
                    !program_offset(words[n], B197_PROGRAM_HEADER_BYTES))
                    return false;
                require_program(&stream, TG_GPU_PROGRAM_3D);
            }
            if (color_target_span(method, last)) {
                const uint32_t base = NVB197_SET_COLOR_TARGET_A(0);
                const uint32_t stride = NVB197_SET_COLOR_TARGET_A(1) - base;
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method < base || (method - base) % stride)
                    return false;
                const size_t used = color_target_setup(method, count, words + n, prefix_words - n,
                                                        va, &stream.va_end);
                if (!used)
                    return false;
                n += used;
                continue;
            }
            if (depth_target_span(method, last)) {
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_ZT_A || count != 5)
                    return false;
                const size_t used = depth_target(words + n, prefix_words - n, va, &stream.va_end);
                if (!used)
                    return false;
                n += used;
                continue;
            }
            if (method <= NVB197_SET_ZT_SELECT && last >= NVB197_SET_ZT_SELECT &&
                (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_ZT_SELECT || count != 1 || words[n]))
                return false;
            if (cbuf_data_span(method, last)) {
                if (op == NVB06F_DMA_INCR_OPCODE_VALUE && method == NVB197_SET_CONSTANT_BUFFER_SELECTOR_A && count == 3) {
                    const size_t used = direct_cbuf_binding(words + n, prefix_words - n, &ranges);
                    if (!used)
                        return false;
                    n += used;
                    continue;
                }
                if (op != NVB06F_DMA_ONEINCR_OPCODE_VALUE || method != NVB197_LOAD_CONSTANT_BUFFER_OFFSET ||
                    count < 2 || (words[n] & 3) || words[n] >= TG_GPU_NVK_DRAW_CB0_BYTES ||
                    count - 1 > (TG_GPU_NVK_DRAW_CB0_BYTES - words[n]) / 4)
                    return false;
                stream.requires_cb0 |= !stream.cb0_end;
            }
            const uint32_t group = cbuf_group_in_span(method, last);
            if (group) {
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != group || count != 1 ||
                    !cbuf_binding_value(words[n], false))
                    return false;
                stream.requires_cb0 |= (words[n] & DRF_SMASK(NVB197_BIND_GROUP_CONSTANT_BUFFER_VALID)) && !stream.cb0_end;
            }
            if (cb0_scratch_span(method, last)) {
                if (!stream.uploads_mme || cb0_setup_end ||
                    op != NVB06F_DMA_INCR_OPCODE_VALUE || method != TG_GPU_NVK_MME_CB0_ADDR_HI || count != 2 ||
                    !word_span(va, words[n], words[n + 1], TG_GPU_NVK_DRAW_CB0_BYTES, 256, &cb0_setup_end))
                    return false;
                if (cb0_setup_end > stream.va_end)
                    stream.va_end = cb0_setup_end;
            }
            if (direct_input_binding_span(method, last))
                return false;
            const uint32_t pair = vertex_fallback_pair(method, last);
            if (pair) {
                const uint32_t page_bytes = 1u << TG_GPU_PAGE_SHIFT;
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != pair || count != 2 ||
                    !word_span(va, words[n], words[n + 1], page_bytes, page_bytes, &stream.va_end))
                    return false;
                zero_addr_ready |= pair == TG_GPU_NVK_MME_ZERO_ADDR_HI;
            }
        }
        if (mme_upload_span(subchannel, method, last)) {
            /* NVK uploads its programs and their slot bindings together. */
            const size_t mme_words = sizeof(tg_gpu_nvk_mme_init) / sizeof(*words);
            if (mme_words > prefix_words - start)
                return false;
            for (size_t i = 0; i < mme_words; i++) {
                if (words[start + i] != tg_gpu_nvk_mme_init[i])
                    return false;
            }
            stream.uploads_mme = true;
            n = start + mme_words;
            continue;
        }
        if (subchannel == 0 && last >= NVB197_CALL_MME_MACRO(0)) {
            if (!mme_call_allowed(op, method, count, words + n, va, zero_addr_ready, &stream.va_end))
                return false;
            const uint32_t slot = (method - NVB197_CALL_MME_MACRO(0)) / 8;
            if (slot == NVK_MME_SELECT_CB0 && cb0_setup_end)
                stream.cb0_end = cb0_setup_end;
            if (slot == NVK_MME_SELECT_CB0 || slot == NVK_MME_SET_ANTI_ALIAS ||
                slot == NVK_MME_DRAW || slot == NVK_MME_DRAW_INDEXED)
                stream.requires_cb0 |= !stream.cb0_end;
            if (slot == NVK_MME_DRAW || slot == NVK_MME_DRAW_INDEXED) {
                require_program(&stream, TG_GPU_PROGRAM_3D);
                require_texture_pools(&stream, TG_GPU_PROGRAM_3D);
            }
            if (slot == NVK_MME_CLEAR || slot == NVK_MME_DRAW ||
                slot == NVK_MME_DRAW_INDEXED || slot == NVK_MME_WRITE_CS_INVOCATIONS)
                host_wfi = false;
            stream.requires_mme |= !stream.uploads_mme;
        }
        if (subchannel == 4) {
            if (method == NVB06F_SET_OBJECT)
                copy.written = 0;
            if (method >= 0x100) {
                for (uint32_t i = 0; i < count; i++) {
                    if (!tg_gpu_copy_method(&copy, method, words[n + i], va, &stream.va_end))
                        return false;
                    if (method == NVB0B5_LAUNCH_DMA)
                        host_wfi = false;
                    if (method < last)
                        method += 4;
                }
            }
        }
        n+=count;
    }
    if (indirect) {
        if (indirect_slot >= NVK_MME_COUNT)
            return false;
        if (indirect->suffix_offset &&
            (indirect_slot != NVK_MME_DISPATCH_INDIRECT ||
             !dispatch_indirect_suffix(words, staging, prefix_words, word_count, &ranges, &stream)))
            return false;
    }
    if (cb0_setup_end && !stream.cb0_end)
        return false;
    if (staging && indirect_slot == NVK_MME_XFB_COUNTER_LOAD) {
        /* Windows ICDs send a buffer index in the last inline word. The
         * installed Maxwell macro adds a dword offset to the slot 0 method. */
        staging[prefix_words - 1] = words[prefix_words - 1] * XFB_POINTER_STRIDE_DWORDS;
    }
    if (info)
        *info = stream;
    return true;
}

bool tg_gpu_inspect_command_stream(const uint32_t *words, size_t bytes,
                                   const struct tg_gpu_command_va *va,
                                   struct tg_gpu_command_info *info)
{
    return inspect(words, NULL, bytes, NULL, va, info);
}

bool tg_gpu_inspect_indirect_command_stream(const uint32_t *words, size_t bytes,
                                            const struct tg_gpu_submit_indirect *indirect,
                                            const struct tg_gpu_command_va *va,
                                            struct tg_gpu_command_info *info)
{
    return indirect && inspect(words, NULL, bytes, indirect, va, info);
}

bool tg_gpu_prepare_command_stream(uint32_t *words, size_t bytes,
                                   const struct tg_gpu_command_va *va,
                                   struct tg_gpu_command_info *info)
{
    return bytes <= TG_GPU_COMMAND_CAPTURE_BYTES && inspect(words, words, bytes, NULL, va, info);
}

bool tg_gpu_prepare_indirect_command_stream(uint32_t *words, size_t bytes,
                                            const struct tg_gpu_submit_indirect *indirect,
                                            const struct tg_gpu_command_va *va,
                                            struct tg_gpu_command_info *info)
{
    return indirect && bytes <= TG_GPU_COMMAND_CAPTURE_BYTES &&
           inspect(words, words, bytes, indirect, va, info);
}

static bool shader_entry_mapped(uint64_t program_end, uint32_t offset, uint32_t header_bytes,
                                tg_gpu_command_range_check check, void *data)
{
    if (program_end <= PROGRAM_REGION_BYTES || program_end > TG_GPU_VA_LIMIT ||
        !program_offset(offset, header_bytes))
        return false;
    const uint64_t base = program_end - PROGRAM_REGION_BYTES + offset;
    return check(data, base, base + header_bytes + PROGRAM_FETCH_ALIGN + PROGRAM_PREFETCH_GUARD,
                 TG_GPU_PITCH_READ_ONLY);
}

/* The last value written to a method in this packet. IMMD stores its value
 * in the count field. In ONEINCR only the first word uses the first method.
 */
static uint32_t packet_value(const uint32_t *args, uint32_t op, uint32_t count,
                             uint32_t first, uint32_t method)
{
    if (op == NVB06F_DMA_IMMD_OPCODE_VALUE)
        return count;
    if (op == NVB06F_DMA_INCR_OPCODE_VALUE)
        return args[(method - first) / 4];
    if (op == NVB06F_DMA_ONEINCR_OPCODE_VALUE && method == first)
        return args[0];
    return args[count - 1];
}

static bool graphics_draw_packet(uint32_t first, uint32_t last)
{
    static const uint32_t draws[] = {
        NVB197_DRAW_ZERO_INDEX, NVB197_DRAW_VERTEX_ARRAY,
        NVB197_DRAW_VERTEX_ARRAY_BEGIN_END_INSTANCE_FIRST,
        NVB197_DRAW_VERTEX_ARRAY_BEGIN_END_INSTANCE_SUBSEQUENT,
        NVB197_DRAW_AUTO, NVB197_DRAW_INLINE_INDEX4X8,
        NVB197_DRAW_INLINE_INDEX, NVB197_DRAW_INLINE_INDEX2X16,
        NVB197_CALL_MME_MACRO(NVK_MME_DRAW), NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDEXED),
        NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDIRECT),
        NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDEXED_INDIRECT),
        NVB197_CALL_MME_MACRO(NVK_MME_XFB_DRAW_INDIRECT),
    };
    for (size_t i = 0; i < sizeof(draws) / sizeof(draws[0]); ++i) {
        if (first <= draws[i] && last >= draws[i])
            return true;
    }
    return first <= NVB197_DRAW_INDEX_BUFFER8_BEGIN_END_INSTANCE_SUBSEQUENT &&
           last >= NVB197_DRAW_INDEX_BUFFER;
}

static bool graphics_program_tracking(const struct tg_gpu_program_state *state)
{
    return state->shadow_mode == NVB197_SET_MME_SHADOW_RAM_CONTROL_MODE_METHOD_TRACK ||
           state->shadow_mode == NVB197_SET_MME_SHADOW_RAM_CONTROL_MODE_METHOD_TRACK_WITH_FILTER;
}

static bool check_enabled_graphics_programs(const struct tg_gpu_program_state *state,
                                            tg_gpu_command_range_check check, void *data)
{
    for (unsigned stage = 0; stage < TG_GPU_GRAPHICS_PROGRAMS; ++stage) {
        if ((state->graphics_enabled & (1u << stage)) &&
            !shader_entry_mapped(state->end[TG_GPU_PROGRAM_3D], state->graphics_offset[stage],
                                 B197_PROGRAM_HEADER_BYTES, check, data))
            return false;
    }
    return true;
}

static bool check_graphics_programs(struct tg_gpu_program_state *state,
                                    const uint32_t *args, uint32_t op, uint32_t count,
                                    uint32_t first, uint32_t last,
                                    tg_gpu_command_range_check check, void *data)
{
    const uint32_t shadow = NVB197_SET_MME_SHADOW_RAM_CONTROL;
    if (first <= shadow && last >= shadow) {
        const uint32_t value = packet_value(args, op, count, first, shadow);
        state->shadow_mode = NVVAL_GET(value, NVB197, SET_MME_SHADOW_RAM_CONTROL, MODE);
    }
    if (graphics_draw_packet(first, last) &&
        !check_enabled_graphics_programs(state, check, data))
        return false;
    if (last < NVB197_SET_PIPELINE_SHADER(0) ||
        first > NVB197_SET_PIPELINE_PROGRAM(TG_GPU_GRAPHICS_PROGRAMS - 1))
        return true;
    for (unsigned stage = 0; stage < TG_GPU_GRAPHICS_PROGRAMS; ++stage) {
        const uint32_t control = NVB197_SET_PIPELINE_SHADER(stage);
        const uint32_t program = NVB197_SET_PIPELINE_PROGRAM(stage);
        /* NVK uses shadow replay and passthrough for meta counters. Its
         * program bindings are normal tracked writes in both modes of tracking.
         */
        if (first <= control && last >= control) {
            if (!graphics_program_tracking(state))
                return false;
            const uint32_t value = packet_value(args, op, count, first, control);
            const uint32_t enabled = NVVAL_GET(value, NVB197, SET_PIPELINE_SHADER, ENABLE);
            state->graphics_enabled = (state->graphics_enabled & ~(1u << stage)) | (enabled << stage);
        }
        if (first <= program && last >= program) {
            if (!graphics_program_tracking(state))
                return false;
            state->graphics_offset[stage] = packet_value(args, op, count, first, program);
        }
    }
    return true;
}

static bool check_packet_range(const uint32_t *words, size_t start_word, size_t end_word,
                               const struct tg_gpu_submit_indirect *indirect,
                               struct tg_gpu_program_state *state,
                               const struct tg_gpu_checked_ranges *ranges,
                               uint32_t *indirect_slot_out)
{
    for (size_t n = start_word; n < end_word;) {
        const uint32_t header = words[n++];
        if (header == NVB06F_DMA_NOP)
            continue;
        const uint32_t op = NVVAL_GET(header, NVB06F, DMA_INCR, OPCODE);
        const uint32_t method = NVVAL_GET(header, NVB06F, DMA_INCR, ADDRESS) * 4;
        const uint32_t count = NVVAL_GET(header, NVB06F, DMA_INCR, COUNT);
        const bool immediate = op == NVB06F_DMA_IMMD_OPCODE_VALUE;
        const uint32_t subchannel = NVVAL_GET(header, NVB06F, DMA_INCR, SUBCHANNEL);
        if (subchannel == 3) {
            if (!halfturn(words + n - 1, end_word - n + 1, ranges)) return false;
            n += TG_GPU_PRESENT_HALFTURN_WORDS - 1;
            continue;
        }
        uint32_t last = method;
        if (!immediate) {
            if (!count ||
                (op != NVB06F_DMA_INCR_OPCODE_VALUE && op != NVB06F_DMA_NONINCR_OPCODE_VALUE &&
                 op != NVB06F_DMA_ONEINCR_OPCODE_VALUE))
                return false;
            if (count > end_word - n) {
                struct tg_gpu_command_info unused = {0};
                uint32_t slot = NVK_MME_COUNT;
                if (!indirect || !indirect_slot_out || subchannel != 0 ||
                    !indirect_mme_call(op, method, count, words + n, end_word - n,
                                       indirect, &unused, XFB_POINTER_STRIDE_DWORDS, &slot))
                    return false;
                if ((slot == NVK_MME_DRAW_INDIRECT ||
                     slot == NVK_MME_DRAW_INDEXED_INDIRECT ||
                     slot == NVK_MME_XFB_DRAW_INDIRECT) &&
                    !check_enabled_graphics_programs(state, ranges->check, ranges->data))
                    return false;
                *indirect_slot_out = slot;
                return true;
            }
            if (!packet_last_method(op, method, count, &last))
                return false;
        }
        if (subchannel <= 1 && !immediate && global_render_enable_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_GLOBAL_RENDER_ENABLE_A ||
                count != 3 || !global_render_enable(words + n, ranges))
                return false;
            n += count;
            continue;
        }
        if (subchannel <= TG_GPU_PROGRAM_COMPUTE && op == NVB06F_DMA_INCR_OPCODE_VALUE &&
            method == NVB197_LINE_LENGTH_IN && count == 4) {
            const size_t used = inline_upload(subchannel, words + n, end_word - n, ranges);
            if (!used)
                return false;
            n += used;
            continue;
        }
        if (subchannel == TG_GPU_PROGRAM_3D && !immediate) {
            const uint32_t xfb_enable = stream_out_buffer_in_span(method, last);
            if (xfb_enable) {
                if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != xfb_enable ||
                    count != 4 || !stream_out_buffer(words + n, ranges))
                    return false;
                n += count;
                continue;
            }
        }
        if (subchannel == TG_GPU_PROGRAM_3D && op == NVB06F_DMA_INCR_OPCODE_VALUE &&
            method == NVB197_SET_CONSTANT_BUFFER_SELECTOR_A && count == 3) {
            const size_t used = direct_cbuf_binding(words + n, end_word - n, ranges);
            if (!used)
                return false;
            n += used;
            continue;
        }
        if (subchannel <= TG_GPU_PROGRAM_COMPUTE && program_region_span(method, last)) {
            if (op != NVB06F_DMA_INCR_OPCODE_VALUE || method != NVB197_SET_PROGRAM_REGION_A ||
                (subchannel == TG_GPU_PROGRAM_3D && !graphics_program_tracking(state)))
                return false;
            const size_t used = program_region(subchannel, count, words + n, end_word - n,
                                               NULL, &state->end[subchannel]);
            if (!used)
                return false;
            n += used;
            continue;
        }
        if (subchannel <= TG_GPU_PROGRAM_COMPUTE && op == NVB06F_DMA_INCR_OPCODE_VALUE && count == 3) {
            const enum tg_gpu_texture_pool pool = texture_pool_in_span(method, method);
            if (pool != TG_GPU_TEXTURE_POOLS && method == texture_pool_methods[pool].base) {
                struct tg_gpu_texture_pool_range range;
                if (!texture_pool_range(pool, words + n, NULL, &range) ||
                    !ranges->check(ranges->data, range.base, range.end, TG_GPU_PITCH_READ_ONLY))
                    return false;
            }
        }
        if (subchannel == TG_GPU_PROGRAM_3D &&
            !check_graphics_programs(state, words + n, op, count, method, last,
                                     ranges->check, ranges->data))
            return false;
        if (subchannel == TG_GPU_PROGRAM_COMPUTE && method == NVB1C0_NO_OPERATION &&
            op == NVB06F_DMA_NONINCR_OPCODE_VALUE && count == TG_GPU_QMD_RECORD_WORDS) {
            /* Admission moved the QMD within its immutable NO_OPERATION payload. */
            const uint32_t *qmd = words + qmd_aligned_word(n);
            const uint32_t offset = NVVAL_MW_GET(qmd, NVB1C0, QMDV00_06, PROGRAM_OFFSET);
            if (!shader_entry_mapped(state->end[TG_GPU_PROGRAM_COMPUTE], offset, 0,
                                     ranges->check, ranges->data) ||
                !qmd_constant_buffers(qmd, ranges))
                return false;
        }
        if (!immediate)
            n += count;
    }
    return !indirect || !indirect_slot_out;
}

bool tg_gpu_check_stream_mappings(const uint32_t *words, size_t bytes,
                                  const struct tg_gpu_submit_indirect *indirect,
                                  struct tg_gpu_program_state *programs,
                                  tg_gpu_command_range_check check, void *data)
{
    if (!words || !bytes || bytes > TG_GPU_COMMAND_CAPTURE_BYTES ||
        (bytes & 3) || ((uintptr_t)words & 3) || !programs || !check)
        return false;
    struct tg_gpu_program_state state = *programs;
    uint64_t max_end = 0;
    const struct tg_gpu_checked_ranges ranges = {.max_end = &max_end, .check = check, .data = data};
    const size_t word_count = bytes / sizeof(*words);
    if (!indirect) {
        if (!check_packet_range(words, 0, word_count, NULL, &state, &ranges, NULL))
            return false;
        *programs = state;
        return true;
    }
    if (!indirect->bytes || (indirect->bytes & 3) || indirect->bytes > TG_GPU_SUBMIT_MAX_BYTES ||
        (indirect->address & 3) || !tg_gpu_command_range_allowed(NULL, indirect->address, indirect->bytes, &max_end) ||
        !check(data, indirect->address, indirect->address + indirect->bytes, TG_GPU_PITCH_READ))
        return false;
    size_t prefix_words = word_count;
    if (indirect->suffix_offset) {
        if ((indirect->suffix_offset & 3) || indirect->suffix_offset >= bytes)
            return false;
        prefix_words = indirect->suffix_offset / sizeof(*words);
    }
    uint32_t slot = NVK_MME_COUNT;
    if (!prefix_words ||
        !check_packet_range(words, 0, prefix_words, indirect, &state, &ranges, &slot) ||
        slot >= NVK_MME_COUNT)
        return false;
    if (indirect->suffix_offset) {
        if (slot != NVK_MME_DISPATCH_INDIRECT || word_count - prefix_words < 7 + TG_GPU_QMD_RECORD_WORDS + 3 ||
            words[prefix_words] > UINT8_MAX || (words[prefix_words + 1] & 0xff))
            return false;
        const uint64_t root_base = ((uint64_t)words[prefix_words] << 32) | words[prefix_words + 1];
        const uint64_t cs_base = root_base + TG_GPU_NVK_CS_GROUP_COUNT_OFFSET;
        if (root_base > UINT64_MAX - (TG_GPU_NVK_CS_GROUP_COUNT_OFFSET + TG_GPU_NVK_CS_GROUP_COUNT_BYTES) ||
            !tg_gpu_command_range_allowed(NULL, cs_base, TG_GPU_NVK_CS_GROUP_COUNT_BYTES, &max_end) ||
            !check(data, cs_base, cs_base + TG_GPU_NVK_CS_GROUP_COUNT_BYTES, TG_GPU_PITCH_WRITE))
            return false;
        const size_t wfi_words = wfi_scope_all_words(words + prefix_words + 5, word_count - (prefix_words + 5));
        if (!wfi_words ||
            !check_packet_range(words, prefix_words + 5 + wfi_words, word_count, NULL, &state, &ranges, NULL))
            return false;
    }
    *programs = state;
    return true;
}

bool tg_gpu_check_command_mappings(const uint32_t *words, size_t bytes,
                                   struct tg_gpu_program_state *programs,
                                   tg_gpu_command_range_check check, void *data)
{
    return tg_gpu_check_stream_mappings(words, bytes, NULL, programs, check, data);
}
