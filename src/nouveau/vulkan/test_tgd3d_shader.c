/* SPDX-License-Identifier: MIT
 * Compile tgd3d's exported SPIR-V with the production NVK/NAK SM53 backend.
 * No adapter, KMT device, GPU memory or command queue is created. This checks
 * compiler acceptance, not GPU execution or the correctness of rendered pixels.
 */
#include "nvk_device.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"
#include "nvk_shader.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_entrypoints.h"
#include "compiler/glsl_types.h"
#include "nir.h"
#include "compiler/spirv/nir_spirv.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_nir.h"
#include "vk_pipeline.h"
#include "vulkan/util/vk_internal_exts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void
make_layouts(struct nvk_device *dev, VkDescriptorSetLayout sets[3])
{
   /* The production tgd3d layout: 3 stages x 14 CBs, 16 samplers/128 SRVs,
    * and 128 texel buffers. Ordinary descriptor sets, not push descriptors. */
   const VkShaderStageFlagBits stages[3] = {
      VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   for (unsigned set = 0; set < 3; set++) {
      VkDescriptorSetLayoutBinding bindings[3 * 144];
      unsigned count = 0;
      const unsigned slots = set == 0 ? 14 : set == 1 ? 144 : 128;
      for (unsigned s = 0; s < 3; s++) {
         for (unsigned i = 0; i < slots; i++) {
            bindings[count++] = (VkDescriptorSetLayoutBinding) {
               .binding = s * slots + i,
               .descriptorType = set == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                  set == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                  i < 16 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
               .descriptorCount = 1, .stageFlags = stages[s],
            };
         }
      }
      const VkDescriptorSetLayoutCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = count, .pBindings = bindings,
      };
      CHECK(nvk_CreateDescriptorSetLayout(nvk_device_to_handle(dev), &info, NULL, &sets[set]) == VK_SUCCESS);
   }
}

int main(int argc, char **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   if ((argc != 4 && argc != 5) ||
       (strcmp(argv[1], "vs") && strcmp(argv[1], "ps")) ||
       (argc == 5 && (strcmp(argv[1], "vs") || strlen(argv[4]) != 1 ||
                     argv[4][0] < '1' || argv[4][0] > '3'))) {
      fprintf(stderr, "usage: %s vs|ps input.spv output-prefix [rotation 1|2|3 for vs]\n", argv[0]);
      return 2;
   }
   const mesa_shader_stage stage = !strcmp(argv[1], "vs") ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;
   FILE *input = fopen(argv[2], "rb"); CHECK(input);
   CHECK(!fseek(input, 0, SEEK_END));
   long size = ftell(input); CHECK(size >= 20 && !(size & 3));
   rewind(input);
   uint32_t *words = malloc(size); CHECK(words);
   CHECK(fread(words, 1, size, input) == (size_t)size); fclose(input);

   glsl_type_singleton_init_or_ref();
   struct nvk_instance *instance = calloc(1, sizeof(*instance));
   struct nvk_physical_device *physical = calloc(1, sizeof(*physical));
   struct nvk_device *dev = calloc(1, sizeof(*dev));
   CHECK(instance && physical && dev);
   const struct vk_instance_extension_table extensions = {0};
   const struct vk_instance_dispatch_table dispatch = {
      .EnumerateInstanceVersion = nvk_EnumerateInstanceVersion,
   };
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
   };
   VkResult instance_result = vk_instance_init(&instance->vk, &extensions, &dispatch,
      &instance_info, vk_default_allocator());
   if (instance_result != VK_SUCCESS)
      fprintf(stderr, "Instance setup result=%d\n", instance_result);
   CHECK(instance_result == VK_SUCCESS);
   /* Match nvkmd_windows's GM20B compiler topology without opening KMT. */
   physical->info = (struct nv_device_info) {
      .type = NV_DEVICE_TYPE_SOC, .device_id = 0x0fe0, .chipset = 0x12e,
      .device_name = "NVIDIA Tegra X1 (GM20B)", .chipset_name = "GM20B",
      .sm = 53, .gpc_count = 1, .tpc_count = 2, .mp_per_tpc = 1,
      .max_warps_per_mp = 64, .nc_atom_size_B = 128,
      .cls_eng3d = 0xb197, .cls_compute = 0xb1c0,
      .cls_copy = 0xb0b5, .cls_eng2d = 0x902d, .cls_m2mf = 0xa140, .cls_gpfifo = 0xb06f,
      .max_smem_per_wg_kB = 48, .sm_smem_sizes_kB = {64}, .sm_smem_size_count = 1,
   };
   physical->vk.instance = &instance->vk;
   vk_object_base_instance_init(&instance->vk, &physical->vk.base, VK_OBJECT_TYPE_PHYSICAL_DEVICE);
   physical->nak = nak_compiler_create(&physical->info); CHECK(physical->nak);
   physical->vk.supported_features.vulkanMemoryModel = true;
   physical->vk.supported_features.bufferDeviceAddress = true;
   physical->vk.supported_features.geometryShader = true;
   physical->vk.supported_features.shaderClipDistance = true;
   physical->vk.supported_features.shaderCullDistance = true;
   physical->vk.supported_features.shaderDemoteToHelperInvocation = true;
   physical->vk.supported_features.shaderImageGatherExtended = true;
   dev->vk.physical = &physical->vk;
   dev->vk.alloc = *vk_default_allocator();
   vk_object_base_init(&dev->vk, &dev->vk.base, VK_OBJECT_TYPE_DEVICE);
   (void)nvk_device_to_handle(dev);
   /* A null nvkmd uses NVK's existing compile-only branch: no shader upload. */
   CHECK(!dev->nvkmd);
   const struct vk_pipeline_robustness_state robustness = {
      .uniform_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
      .storage_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
      .vertex_inputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
      .images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT,
      .null_uniform_buffer_descriptor = true,
      .null_storage_buffer_descriptor = true,
   };
   const struct vk_device_shader_ops *ops = &nvk_device_shader_ops;
   struct spirv_to_nir_options options = ops->get_spirv_options(&physical->vk, stage, &robustness);
   /* Use the same specialization mechanism as tgd3d's rotated Blt, with the
    * original SPIR-V bytes. Rewriting/folding the source would not check it. */
   const uint32_t rotation = argc == 5 ? argv[4][0] - '0' : 0;
   const VkSpecializationMapEntry map = { .constantID = 0, .size = sizeof(rotation) };
   const VkSpecializationInfo specialization = {
      .mapEntryCount = 1, .pMapEntries = &map,
      .dataSize = sizeof(rotation), .pData = &rotation,
   };
   nir_shader *nir = vk_spirv_to_nir(&dev->vk, words, size, stage, "main",
      argc == 5 ? &specialization : NULL, &options,
      ops->get_nir_options(&physical->vk, stage, &robustness), false, NULL);
   CHECK(nir);
   vk_set_subgroup_size(nir, 32, 32, 32, vk_spirv_version(words, size), NULL, false, false);
   free(words);
   ops->preprocess_nir(&physical->vk, nir, &robustness);
   VkDescriptorSetLayout sets[3]; make_layouts(dev, sets);
   struct vk_descriptor_set_layout *layouts[3];
   for (unsigned i = 0; i < 3; i++)
      layouts[i] = vk_descriptor_set_layout_from_handle(sets[i]);
   struct vk_shader_compile_info compile = {
      .stage = stage, .nir = nir, .robustness = &robustness,
      .flags = VK_SHADER_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_MESA,
      .set_layout_count = 3, .set_layouts = layouts,
   };
   struct vk_shader *shader = NULL;
   CHECK(ops->compile(&dev->vk, 1, &compile, NULL, NULL, NULL, &shader) == VK_SUCCESS);
   struct nvk_shader *nvk = container_of(shader, struct nvk_shader, vk);
   CHECK(nvk->info.sm == 53 && nvk->code_ptr && nvk->code_size && !nvk->upload_size);
   char path[1024];
   CHECK(snprintf(path, sizeof(path), "%s.bin", argv[3]) < (int)sizeof(path));
   FILE *output = fopen(path, "wb"); CHECK(output);
   CHECK(fwrite(nvk->code_ptr, 1, nvk->code_size, output) == nvk->code_size); fclose(output);
   CHECK(snprintf(path, sizeof(path), "%s.nak.txt", argv[3]) < (int)sizeof(path));
   output = fopen(path, "w"); CHECK(output);
   CHECK(nvk->asm_str); fputs(nvk->asm_str, output); fclose(output);
   printf("PASS: SM%u %s compiler emitted %u bytes, %u instructions, %u GPRs; no GPU execution\n",
          nvk->info.sm, argv[1], nvk->code_size, nvk->info.num_instrs, nvk->info.num_gprs);
   if (argc == 5)
      printf("Specialization constant 0 = %u (DXGI %u degrees counter-clockwise)\n", rotation, rotation * 90);
   vk_shader_destroy(&dev->vk, shader, NULL);
   for (unsigned i = 0; i < 3; i++)
      vk_common_DestroyDescriptorSetLayout(nvk_device_to_handle(dev), sets[i], NULL);
   nak_compiler_destroy(physical->nak);
   vk_instance_finish(&instance->vk);
   free(dev); free(physical); free(instance);
   glsl_type_singleton_decref();
   return 0;
}
