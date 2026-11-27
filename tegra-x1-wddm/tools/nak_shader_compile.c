/* SPDX-License-Identifier: MIT */
/* Compiler-only diagnostic. No winsys, Vulkan ICD or device access. */
#include "compiler/nir/nir.h"
#include "compiler/spirv/nir_spirv.h"
#include "nouveau/compiler/nak.h"
#include "nouveau/headers/nv_device_info.h"
#include "util/ralloc.h"
#include "t214_nvk_constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_blob(const char *prefix, const char *suffix,
                      const void *data, size_t size)
{
   char path[4096];
   if (snprintf(path, sizeof(path), "%s%s", prefix, suffix) >= sizeof(path))
      return 1;
   FILE *f = fopen(path, "wb");
   if (!f) { perror(path); return 1; }
   int ret = fwrite(data, 1, size, f) != size;
   return fclose(f) != 0 || ret;
}

int main(int argc, char **argv)
{
   if (argc != 4 || (strcmp(argv[1], "vs") && strcmp(argv[1], "fs"))) {
      fprintf(stderr, "usage: %s vs|fs input.spv output-prefix\n", argv[0]);
      return 2;
   }
   mesa_shader_stage stage = !strcmp(argv[1], "vs") ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;
   FILE *f = fopen(argv[2], "rb");
   if (!f) { perror(argv[2]); return 1; }
   if (fseek(f, 0, SEEK_END)) return 1;
   long bytes = ftell(f);
   if (bytes < 20 || bytes > 1024 * 1024 || bytes % 4 || fseek(f, 0, SEEK_SET)) return 1;
   uint32_t *spv = malloc(bytes);
   if (!spv || fread(spv, 1, bytes, f) != (size_t)bytes || fclose(f)) return 1;
   if (spv[0] != 0x07230203) return 1;

   struct nv_device_info dev = {
      .type = NV_DEVICE_TYPE_SOC, .chipset = 0x12e,
      .sm = 53, .gpc_count = 1, .tpc_count = 2, .mp_per_tpc = 1,
      .max_warps_per_mp = 64, .cls_eng3d = 0xb197,
   };
   glsl_type_singleton_init_or_ref();
   struct nak_compiler *nak = nak_compiler_create(&dev);
   const struct spirv_to_nir_options opts = {
      .environment = NIR_SPIRV_VULKAN,
      .ignore_replacement = true,
   };
   nir_shader *nir = spirv_to_nir(spv, bytes / 4, NULL, stage, "main", &opts,
                                  nak_nir_options(nak));
   free(spv);
   if (!nir) { fprintf(stderr, "SPIR-V frontend failed\n"); return 1; }
   nak_preprocess_nir(nir, nak);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   const struct nak_fs_key fs_key = {0};
   struct nak_shader_bin *bin = nak_compile_shader(nir, true, nak, 0,
                                                   stage == MESA_SHADER_FRAGMENT ? &fs_key : NULL,
                                                   false);
   if (!bin || !bin->code_size || bin->info.sm != 53 || bin->info.slm_size ||
       bin->info.crs_size || nir->constant_data_size) {
      fprintf(stderr, "shader failed or needs unsupported storage\n"); return 1;
   }
   int ret = write_blob(argv[3], ".code.bin", bin->code, bin->code_size) |
             write_blob(argv[3], ".header.bin", bin->info.hdr, 80);
   if (bin->asm_str) ret |= write_blob(argv[3], ".asm", bin->asm_str, strlen(bin->asm_str));
   char info[512];
   int n = snprintf(info, sizeof(info),
      "{\"stage\":\"%s\",\"sm\":%u,\"gprs\":%u,\"instructions\":%u,"
      "\"code_size\":%u,\"header_size\":80,\"slm_size\":%u,\"crs_size\":%u}\n",
      argv[1], bin->info.sm, bin->info.num_gprs, bin->info.num_instrs,
      bin->code_size, bin->info.slm_size, bin->info.crs_size);
   if (n <= 0 || n >= sizeof(info)) ret = 1;
   else ret |= write_blob(argv[3], ".json", info, n);
   nak_shader_bin_destroy(bin);
   ralloc_free(nir);
   nak_compiler_destroy(nak);
   glsl_type_singleton_decref();
   return ret;
}
