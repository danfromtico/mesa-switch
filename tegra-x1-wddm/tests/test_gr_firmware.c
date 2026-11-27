/* SPDX-License-Identifier: MIT */
#include "../core/gr_firmware.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void word(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8)); }
static size_t fixture(uint8_t *net, uint8_t boot[2][20])
{
    const unsigned ids[] = {1,0,3,2,4,5,6,7,15,16,17,18};
    const unsigned sizes[] = {5,7,9,4,8,12,8,8,4,4,4,4};
    size_t offset = 8 + 12 * sizeof(ids)/sizeof(ids[0]);
    memset(net, 0, 512); memset(boot, 0, 40);
    word(net + 4, 12);
    for (unsigned i = 0; i < 12; ++i) {
        word(net + 8 + 12*i, ids[i]);
        word(net + 12 + 12*i, sizes[i]);
        word(net + 16 + 12*i, (uint32_t)offset);
        for (unsigned j = 0; j < sizes[i]; ++j) net[offset+j] = (uint8_t)(i*16+j);
        offset += sizes[i];
    }
    for (unsigned i = 0; i < 2; ++i) {
        word(boot[i], 123); /* Ignored descriptor start_offset, like Switchroot. */
        word(boot[i] + 4, 4);
        word(boot[i] + 8, 0x5c00);
        word(boot[i] + 12, 0x5c00);
        word(boot[i] + 16, i ? 0x3d3d65e2 : 0x9121ab5c);
    }
    return offset;
}
static void check_payload(const uint8_t *image, struct tg_gpu_gr_segment part,
                          struct tg_gpu_gr_blob expected)
{
    assert(part.offset % 256 == 0 && part.size % 4 == 0);
    assert(part.size >= expected.size && part.size < expected.size + 4);
    assert(!memcmp(image + part.offset, expected.data, expected.size));
    for (size_t j = expected.size; j < ((part.size + 255u) & ~255u); ++j)
        assert(image[part.offset+j] == 0);
}
static void check_image(struct tg_gpu_gr_blob net, const struct tg_gpu_gr_blob boot[2])
{
    struct tg_gpu_gr_netlist parsed;
    struct tg_gpu_gr_image image;
    uint32_t words[17], count;
    uint8_t *storage;
    const uint64_t va = UINT64_C(0x1234500000);
    assert(tg_gpu_gr_netlist_parse(net, &parsed) == TG_GPU_GR_FW_OK);
    assert(tg_gpu_gr_image_prepare(net, boot, NULL, 0, &image) == TG_GPU_GR_FW_OK);
    storage = malloc(image.size); assert(storage);
    memset(storage, 0x7d, image.size);
    assert(tg_gpu_gr_image_prepare(net, boot, storage, image.size, &image) == TG_GPU_GR_FW_OK);
    for (unsigned i = 0; i < 2; ++i) {
        const struct tg_gpu_gr_falcon_image *f = &image.falcon[i];
        check_payload(storage, f->code, parsed.region[i*2+1]);
        check_payload(storage, f->data, parsed.region[i*2]);
        assert(tg_gpu_gr_boot_header(&image, i, va, words, &count) == TG_GPU_GR_FW_OK);
        assert(count == 17 && words[8] == 4);
        assert(words[9] == (uint32_t)((va + f->code.offset) >> 8));
        assert(words[11] == f->code.size);
        assert(words[15] == (uint32_t)((va + f->data.offset) >> 8));
        assert(words[16] == f->data.size);
        assert(!words[0] && !words[7] && !words[10] && !words[12] && !words[14]);
    }
    free(storage);
}
static struct tg_gpu_gr_blob file(const char *name)
{
    FILE *fp = fopen(name, "rb"); long size; uint8_t *data;
    assert(fp && !fseek(fp, 0, SEEK_END)); size = ftell(fp);
    assert(size > 0 && !fseek(fp, 0, SEEK_SET));
    data = malloc((size_t)size); assert(data);
    assert(fread(data, 1, (size_t)size, fp) == (size_t)size); fclose(fp);
    return (struct tg_gpu_gr_blob){data, (size_t)size};
}
int main(int argc, char **argv)
{
    uint8_t raw[513], boots[2][20], backup[512], storage[1536];
    uint8_t *net = raw + 1; /* Deliberately unaligned header and payloads. */
    size_t size = fixture(net, boots);
    struct tg_gpu_gr_blob input = {net, size}, boot[] = {{boots[0],20},{boots[1],20}};
    struct tg_gpu_gr_image image;
    struct tg_gpu_gr_netlist parsed;
    uint32_t words[17], count;
    check_image(input, boot); memcpy(backup, net, 512);
    assert(tg_gpu_gr_image_prepare(input, boot, NULL, 0, &image) == TG_GPU_GR_FW_OK);
    assert(image.size == sizeof(storage));
    assert(image.falcon[0].code.size == 8 && image.falcon[1].code.size == 12);
    memset(storage, 0x7d, sizeof(storage));
    assert(tg_gpu_gr_image_prepare(input, boot, storage, sizeof(storage)-1, &image) == TG_GPU_GR_FW_SIZE);
    for (unsigned i = 0; i < sizeof(storage); ++i) assert(storage[i] == 0x7d);
    assert(image.size == 0);
    /* Short/overflowing tables, invalid/duplicate/missing entries, malformed
     * typed streams, and out-of-file ranges must fail before exposing views.
     */
    for (unsigned n = 0; n < 8; ++n)
        assert(tg_gpu_gr_netlist_parse((struct tg_gpu_gr_blob){net,n}, &parsed) == TG_GPU_GR_FW_FORMAT);
    word(net+4, UINT32_MAX);
    assert(tg_gpu_gr_netlist_parse(input,&parsed)==TG_GPU_GR_FW_FORMAT);
    memcpy(net,backup,512); word(net+8+12, 1);
    assert(tg_gpu_gr_netlist_parse(input,&parsed)==TG_GPU_GR_FW_FORMAT);
    memcpy(net,backup,512); word(net+8, 40);
    assert(tg_gpu_gr_netlist_parse(input,&parsed)==TG_GPU_GR_FW_FORMAT);
    memcpy(net,backup,512); word(net+12+4*12, 7);
    assert(tg_gpu_gr_netlist_parse(input,&parsed)==TG_GPU_GR_FW_FORMAT);
    memcpy(net,backup,512); word(net+16, 4);
    assert(tg_gpu_gr_netlist_parse(input,&parsed)==TG_GPU_GR_FW_FORMAT);
    memcpy(net,backup,512); word(net+16, UINT32_MAX);
    assert(tg_gpu_gr_netlist_parse(input,&parsed)==TG_GPU_GR_FW_FORMAT);
    memcpy(net,backup,512); word(net+12, UINT32_MAX);
    assert(tg_gpu_gr_netlist_parse(input,&parsed)==TG_GPU_GR_FW_FORMAT);
    assert(parsed.region[0].data == NULL);
    memcpy(net,backup,512); boots[1][16] ^= 1;
    assert(tg_gpu_gr_image_prepare(input,boot,storage,sizeof(storage),&image)==TG_GPU_GR_FW_BOOT_VERSION);
    assert(image.size == 0 && storage[0] == 0x7d);
    boots[1][16] ^= 1; word(boots[1]+12,0x5c04);
    assert(tg_gpu_gr_image_prepare(input,boot,NULL,0,&image)==TG_GPU_GR_FW_FORMAT);
    word(boots[1]+12,0x5c00);
    assert(tg_gpu_gr_image_prepare(input,boot,NULL,0,&image)==TG_GPU_GR_FW_OK);
    assert(tg_gpu_gr_boot_header(&image,0,0,words,&count)==TG_GPU_GR_FW_ADDRESS && !count);
    assert(tg_gpu_gr_boot_header(&image,0,0x100001,words,&count)==TG_GPU_GR_FW_ADDRESS);
    assert(tg_gpu_gr_boot_header(&image,0,(UINT64_C(1)<<40)-256,words,&count)==TG_GPU_GR_FW_ADDRESS);
    image.falcon[0].boot_signature = 0x93671b7d;
    assert(tg_gpu_gr_boot_header(&image,0,0x100000,words,&count)==TG_GPU_GR_FW_OK);
    assert(count == 13 && words[4] == 4 && words[5] == 0x1001 && words[7] == 8);
    image.falcon[0].boot_signature = 0x3d3d65e2; /* A GPCCS checksum cannot select FECS. */
    assert(tg_gpu_gr_boot_header(&image,0,0x100000,words,&count)==TG_GPU_GR_FW_BOOT_VERSION && !count);
    image.falcon[0].boot_signature = 0x9121ab5c;
    image.falcon[0].code.offset = UINT32_MAX;
    assert(tg_gpu_gr_boot_header(&image,0,0x100000,words,&count)==TG_GPU_GR_FW_FORMAT);
    assert(argc == 1 || argc == 4);
    if (argc == 4) {
        struct tg_gpu_gr_blob real_net = file(argv[1]);
        struct tg_gpu_gr_blob real_boot[] = {file(argv[2]),file(argv[3])};
        check_image(real_net,real_boot);
        free((void *)real_net.data); free((void *)real_boot[0].data); free((void *)real_boot[1].data);
        puts("PASS: supplied NETB/FECS/GPCCS parsing and DMA surface/header encoding (no GPU load)");
    }
    puts("PASS: GR firmware byte alignment, zero padding, range rejection and bootstrap ABI");
    return 0;
}
