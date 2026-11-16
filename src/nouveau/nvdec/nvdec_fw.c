/* SPDX-License-Identifier: MIT
 *
 * NVDEC Falcon firmware validation, load and boot for Tegra210.
 *
 * The image format and boot sequence are the ones used by NVIDIA's upstream
 * "drm/tegra: Add NVDEC driver" series and the Tegra Falcon driver:
 *   bin header v1: magic, version, size, os_header_offset, os_data_offset,
 *                  os_size
 *   os header:     code_offset, code_size, data_offset, data_size
 * All offsets are relative to the OS image, which starts at os_data_offset.
 */
#include "nvdec.h"

#include <string.h>

#define FALCON_MAGIC_NVIDIA       0x10deu
#define FALCON_MAGIC_NVIDIA_ALT   0x10feu

#define FALCON_IRQMSET            0x00001010u
#define FALCON_IRQMSET_WDTMR      (1u << 1)
#define FALCON_IRQMSET_HALT       (1u << 4)
#define FALCON_IRQMSET_EXTERR     (1u << 5)
#define FALCON_IRQMSET_SWGEN0     (1u << 6)
#define FALCON_IRQMSET_SWGEN1     (1u << 7)
#define FALCON_IRQMSET_EXT(v)     (((v) & 0xffu) << 8)

#define FALCON_IRQDEST            0x0000101cu
#define FALCON_IRQDEST_HALT       (1u << 4)
#define FALCON_IRQDEST_EXTERR     (1u << 5)
#define FALCON_IRQDEST_SWGEN0     (1u << 6)
#define FALCON_IRQDEST_SWGEN1     (1u << 7)
#define FALCON_IRQDEST_EXT(v)     (((v) & 0xffu) << 8)

#define FALCON_ITFEN              0x00001048u
#define FALCON_ITFEN_CTXEN        (1u << 0)
#define FALCON_ITFEN_MTHDEN       (1u << 1)

#define FALCON_IDLESTATE          0x0000104cu
#define FALCON_CPUCTL             0x00001100u
#define FALCON_CPUCTL_STARTCPU    (1u << 1)
#define FALCON_BOOTVEC            0x00001104u
#define FALCON_DMACTL             0x0000110cu
#define FALCON_DMACTL_DMEM_SCRUB  (1u << 1)
#define FALCON_DMACTL_IMEM_SCRUB  (1u << 2)
#define FALCON_DMATRFBASE         0x00001110u
#define FALCON_DMATRFMOFFS        0x00001114u
#define FALCON_DMATRFCMD          0x00001118u
#define FALCON_DMATRFCMD_IDLE     (1u << 1)
#define FALCON_DMATRFCMD_IMEM     (1u << 4)
#define FALCON_DMATRFCMD_SIZE_256B (6u << 8)
#define FALCON_DMATRFCMD_DMACTX(v) (((v) & 7u) << 12)
#define FALCON_DMATRFFBOFFS       0x0000111cu

struct fw_bin_header {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t os_header_offset;
    uint32_t os_data_offset;
    uint32_t os_size;
};

struct fw_os_header {
    uint32_t code_offset;
    uint32_t code_size;
    uint32_t data_offset;
    uint32_t data_size;
};

#define FW_IMEM_LIMIT (64u * 1024u)
#define FW_DMEM_LIMIT (64u * 1024u)
#define FW_BOOT_TIMEOUT_US 100000u
#define FW_DMA_TIMEOUT_US 100000u

enum tg_nvdec_result tg_nvdec_set_error(struct tg_nvdec *nvdec,
                                        enum tg_nvdec_result result,
                                        const char *what)
{
    size_t n;

    if (!nvdec)
        return result;
    if (what) {
        n = strlen(what);
        if (n >= sizeof(nvdec->error))
            n = sizeof(nvdec->error) - 1;
        memcpy(nvdec->error, what, n);
        nvdec->error[n] = '\0';
    } else {
        nvdec->error[0] = '\0';
    }
    return result;
}

const char *tg_nvdec_result_name(enum tg_nvdec_result result)
{
    switch (result) {
    case TG_NVDEC_OK: return "ok";
    case TG_NVDEC_ERR_ARGUMENT: return "invalid-argument";
    case TG_NVDEC_ERR_NO_HAL: return "no-hal";
    case TG_NVDEC_ERR_NO_MEMORY: return "out-of-memory";
    case TG_NVDEC_ERR_FIRMWARE_MISSING: return "firmware-missing";
    case TG_NVDEC_ERR_FIRMWARE_MAGIC: return "firmware-bad-magic";
    case TG_NVDEC_ERR_FIRMWARE_VERSION: return "firmware-bad-version";
    case TG_NVDEC_ERR_FIRMWARE_SIZE: return "firmware-bad-size";
    case TG_NVDEC_ERR_FIRMWARE_SECTIONS: return "firmware-bad-sections";
    case TG_NVDEC_ERR_FIRMWARE_HASH: return "firmware-hash-mismatch";
    case TG_NVDEC_ERR_FIRMWARE_DMA: return "firmware-dma";
    case TG_NVDEC_ERR_DMA_SCRUB: return "falcon-dma-scrub-timeout";
    case TG_NVDEC_ERR_DMA_TIMEOUT: return "falcon-dma-timeout";
    case TG_NVDEC_ERR_BOOT_TIMEOUT: return "falcon-boot-timeout";
    case TG_NVDEC_ERR_BOOT_HALT: return "falcon-boot-halt";
    case TG_NVDEC_ERR_SUBMIT: return "submit-failed";
    case TG_NVDEC_ERR_TIMEOUT: return "decode-timeout";
    case TG_NVDEC_ERR_CANCELLED: return "cancelled";
    case TG_NVDEC_ERR_ENGINE: return "engine-error";
    case TG_NVDEC_ERR_UNSUPPORTED: return "unsupported";
    case TG_NVDEC_ERR_DIMENSIONS: return "bad-dimensions";
    case TG_NVDEC_ERR_SURFACE: return "bad-surface";
    case TG_NVDEC_ERR_SLICE: return "bad-slice";
    case TG_NVDEC_ERR_DPB: return "bad-dpb";
    case TG_NVDEC_ERR_STATE: return "bad-state";
    }
    return "unknown";
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool range_ok(uint32_t offset, uint32_t length, uint32_t limit)
{
    return offset <= limit && length <= limit - offset;
}

static bool falcon_poll(struct tg_nvdec *nvdec, uint32_t reg, uint32_t mask,
                        bool clear, uint32_t timeout_us)
{
    uint32_t elapsed = 0;
    const uint32_t step = 10;

    for (;;) {
        const uint32_t value = nvdec->hal->read32(nvdec->hal->ctx, reg);
        nvdec->last_regs[0] = reg;
        nvdec->last_regs[1] = value;
        if (clear ? ((value & mask) == 0) : ((value & mask) != 0))
            return true;
        if (elapsed >= timeout_us)
            return false;
        nvdec->hal->delay_us(nvdec->hal->ctx, step);
        elapsed += step;
    }
}

void tg_nvdec_init(struct tg_nvdec *nvdec, const struct tg_nvdec_hal *hal,
                   uint32_t syncpt_id)
{
    if (!nvdec)
        return;
    memset(nvdec, 0, sizeof(*nvdec));
    nvdec->hal = hal;
    nvdec->syncpt_id = syncpt_id;
}

enum tg_nvdec_result tg_nvdec_load_firmware(struct tg_nvdec *nvdec,
                                            const void *data, size_t size,
                                            const uint8_t expected_sha256[TG_NVDEC_SHA256_SIZE])
{
    struct fw_bin_header bin;
    struct fw_os_header os;
    const uint8_t *p = data;
    uint8_t digest[TG_NVDEC_SHA256_SIZE];

    if (!nvdec || !nvdec->hal)
        return TG_NVDEC_ERR_NO_HAL;
    if (nvdec->in_flight || nvdec->booted)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_STATE,
                                  "quiesce the engine before replacing firmware");
    if (!data || size < sizeof(bin))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_MISSING,
                                  "firmware image is absent or truncated");

    bin.magic = read_le32(p + 0);
    bin.version = read_le32(p + 4);
    bin.size = read_le32(p + 8);
    bin.os_header_offset = read_le32(p + 12);
    bin.os_data_offset = read_le32(p + 16);
    bin.os_size = read_le32(p + 20);

    if (bin.magic != FALCON_MAGIC_NVIDIA && bin.magic != FALCON_MAGIC_NVIDIA_ALT)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_MAGIC,
                                  "firmware magic is not 0x10de/0x10fe");
    if (bin.version != 1)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_VERSION,
                                  "only Falcon bin format v1 is supported");
    if (bin.size < sizeof(bin) || bin.size > size)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_SIZE,
                                  "firmware header size exceeds the buffer");
    if (!range_ok(bin.os_header_offset, sizeof(os), bin.size) ||
        !range_ok(bin.os_data_offset, bin.os_size, bin.size))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_SECTIONS,
                                  "firmware OS header/data range is invalid");

    os.code_offset = read_le32(p + bin.os_header_offset + 0);
    os.code_size = read_le32(p + bin.os_header_offset + 4);
    os.data_offset = read_le32(p + bin.os_header_offset + 8);
    os.data_size = read_le32(p + bin.os_header_offset + 12);

    if (!os.code_size || !os.data_size ||
        !range_ok(os.code_offset, os.code_size, bin.os_size) ||
        !range_ok(os.data_offset, os.data_size, bin.os_size))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_SECTIONS,
                                  "firmware code/data section is invalid");
    /* Falcon DMA copies in 256-byte chunks. */
    if ((bin.os_data_offset & 255u) ||
        (os.code_size & 255u) || (os.data_size & 255u) ||
        (os.code_offset & 255u) || (os.data_offset & 255u))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_SECTIONS,
                                  "firmware sections are not 256-byte aligned");
    if (os.code_size > FW_IMEM_LIMIT || os.data_size > FW_DMEM_LIMIT)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_SECTIONS,
                                  "firmware section exceeds Falcon IMEM/DMEM");

    tg_nvdec_sha256(data, size, digest);
    if (expected_sha256 && memcmp(digest, expected_sha256, sizeof(digest)) != 0)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_HASH,
                                  "firmware SHA-256 does not match the pinned value");

    if (nvdec->firmware.cpu) {
        nvdec->hal->dma_free(nvdec->hal->ctx, &nvdec->firmware);
        memset(&nvdec->firmware, 0, sizeof(nvdec->firmware));
    }
    if (!nvdec->hal->dma_alloc(nvdec->hal->ctx, bin.size, &nvdec->firmware) ||
        !nvdec->firmware.cpu || nvdec->firmware.size < bin.size)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_DMA,
                                  "cannot allocate a DMA buffer for the firmware");

    memcpy(nvdec->firmware.cpu, data, bin.size);
    if (nvdec->hal->dma_sync)
        nvdec->hal->dma_sync(nvdec->hal->ctx, &nvdec->firmware, true);
    memcpy(nvdec->fw_sha256, digest, sizeof(digest));
    nvdec->fw_version = bin.version;
    nvdec->firmware_loaded = true;
    nvdec->booted = false;
    nvdec->stats.firmware_bytes = bin.size;
    nvdec->stats.fw_version = bin.version;
    return TG_NVDEC_OK;
}

static enum tg_nvdec_result falcon_copy_chunk(struct tg_nvdec *nvdec,
                                              uint32_t image_offset,
                                              uint32_t falcon_offset,
                                              bool imem)
{
    uint32_t cmd = FALCON_DMATRFCMD_SIZE_256B | FALCON_DMATRFCMD_DMACTX(1);

    if (imem)
        cmd |= FALCON_DMATRFCMD_IMEM;
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_DMATRFMOFFS, falcon_offset);
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_DMATRFFBOFFS, image_offset);
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_DMATRFCMD, cmd);
    if (!falcon_poll(nvdec, FALCON_DMATRFCMD, FALCON_DMATRFCMD_IDLE, false,
                     FW_DMA_TIMEOUT_US))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_DMA_TIMEOUT,
                                  "Falcon DMA did not return to idle");
    return TG_NVDEC_OK;
}

enum tg_nvdec_result tg_nvdec_boot(struct tg_nvdec *nvdec)
{
    const uint8_t *image;
    uint32_t os_data_offset, os_size;
    uint32_t code_offset, code_size, data_offset, data_size;
    uint32_t offset;

    if (!nvdec || !nvdec->hal)
        return TG_NVDEC_ERR_NO_HAL;
    if (!nvdec->firmware_loaded || !nvdec->firmware.cpu)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_FIRMWARE_MISSING,
                                  "load the firmware before booting");
    if (nvdec->in_flight || nvdec->booted)
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_STATE,
                                  "quiesce the engine before booting again");

    image = nvdec->firmware.cpu;
    os_data_offset = read_le32(image + 16);
    os_size = read_le32(image + 20);
    code_offset = read_le32(image + read_le32(image + 12) + 0);
    code_size = read_le32(image + read_le32(image + 12) + 4);
    data_offset = read_le32(image + read_le32(image + 12) + 8);
    data_size = read_le32(image + read_le32(image + 12) + 12);
    (void)os_size;

    if (!falcon_poll(nvdec, FALCON_DMACTL,
                     FALCON_DMACTL_DMEM_SCRUB | FALCON_DMACTL_IMEM_SCRUB,
                     true, FW_DMA_TIMEOUT_US))
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_DMA_SCRUB,
                                  "Falcon memory scrub did not complete");
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_DMACTL, 0);
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_DMATRFBASE,
                        (uint32_t)((nvdec->firmware.bus + os_data_offset) >> 8));

    /* A DMA or boot timeout does not prove that firmware storage is idle. */
    nvdec->in_flight = true;
    for (offset = 0; offset < data_size; offset += 256) {
        enum tg_nvdec_result r = falcon_copy_chunk(nvdec,
            data_offset + offset, offset, false);
        if (r != TG_NVDEC_OK)
            return r;
    }
    for (offset = 0; offset < code_size; offset += 256) {
        enum tg_nvdec_result r = falcon_copy_chunk(nvdec,
            code_offset + offset, offset, true);
        if (r != TG_NVDEC_OK)
            return r;
    }

    nvdec->hal->write32(nvdec->hal->ctx, FALCON_IRQMSET,
                        FALCON_IRQMSET_EXT(0xff) | FALCON_IRQMSET_SWGEN1 |
                        FALCON_IRQMSET_SWGEN0 | FALCON_IRQMSET_EXTERR |
                        FALCON_IRQMSET_HALT | FALCON_IRQMSET_WDTMR);
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_IRQDEST,
                        FALCON_IRQDEST_EXT(0xff) | FALCON_IRQDEST_SWGEN1 |
                        FALCON_IRQDEST_SWGEN0 | FALCON_IRQDEST_EXTERR |
                        FALCON_IRQDEST_HALT);
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_ITFEN,
                        FALCON_ITFEN_MTHDEN | FALCON_ITFEN_CTXEN);
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_BOOTVEC, 0);
    nvdec->hal->write32(nvdec->hal->ctx, FALCON_CPUCTL, FALCON_CPUCTL_STARTCPU);

    if (!falcon_poll(nvdec, FALCON_IDLESTATE, 0xffffffffu, true,
                     FW_BOOT_TIMEOUT_US)) {
        nvdec->last_regs[2] = nvdec->hal->read32(nvdec->hal->ctx, FALCON_CPUCTL);
        nvdec->last_regs[3] = nvdec->hal->read32(nvdec->hal->ctx, FALCON_ITFEN);
        return tg_nvdec_set_error(nvdec, TG_NVDEC_ERR_BOOT_TIMEOUT,
                                  "Falcon did not reach idle after STARTCPU");
    }
    nvdec->last_regs[2] = nvdec->hal->read32(nvdec->hal->ctx, FALCON_CPUCTL);
    nvdec->last_regs[3] = nvdec->hal->read32(nvdec->hal->ctx, FALCON_ITFEN);
    nvdec->last_regs[4] = nvdec->hal->read32(nvdec->hal->ctx, FALCON_DMATRFBASE);
    nvdec->booted = true;
    nvdec->in_flight = false;
    return TG_NVDEC_OK;
}

enum tg_nvdec_result tg_nvdec_close(struct tg_nvdec *nvdec)
{
    if (!nvdec)
        return TG_NVDEC_ERR_ARGUMENT;
    if ((nvdec->in_flight || nvdec->booted) &&
        tg_nvdec_cancel(nvdec) != TG_NVDEC_OK)
        return TG_NVDEC_ERR_SUBMIT;
    if (nvdec->hal && nvdec->hal->dma_free) {
        if (nvdec->arena.cpu)
            nvdec->hal->dma_free(nvdec->hal->ctx, &nvdec->arena);
        if (nvdec->scratch.cpu)
            nvdec->hal->dma_free(nvdec->hal->ctx, &nvdec->scratch);
        if (nvdec->firmware.cpu)
            nvdec->hal->dma_free(nvdec->hal->ctx, &nvdec->firmware);
    }
    memset(&nvdec->arena, 0, sizeof(nvdec->arena));
    memset(&nvdec->scratch, 0, sizeof(nvdec->scratch));
    memset(&nvdec->firmware, 0, sizeof(nvdec->firmware));
    nvdec->arena_used = 0;
    nvdec->picture_index = 0;
    nvdec->firmware_loaded = false;
    nvdec->booted = false;
    nvdec->in_flight = false;
    return TG_NVDEC_OK;
}
