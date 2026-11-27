/* SPDX-License-Identifier: MIT
 * CPU regression for meta operations followed by partial shader-object binds.
 * Include the production implementation to exercise its private save/restore
 * helpers without adding test entrypoints to the driver. Rename its exported
 * entrypoints so the rest of libnvk can be linked normally.
 */
#define nvk_device_init_meta test_device_init_meta
#define nvk_device_finish_meta test_device_finish_meta
#define nvk_meta_resolve_rendering test_meta_resolve_rendering
#define nvk_CmdBlitImage2 test_CmdBlitImage2
#define nvk_CmdResolveImage2 test_CmdResolveImage2
#define nvk_CmdCopyImageToBuffer2 test_CmdCopyImageToBuffer2
#define nvk_CmdCopyBufferToImage2 test_CmdCopyBufferToImage2
#define nvk_CmdCopyImage2 test_CmdCopyImage2
#define nvk_CmdCopyBuffer2 test_CmdCopyBuffer2
#define nvk_CmdFillBuffer test_CmdFillBuffer
#include "nvk_cmd_meta.c"

#include "nvk_cmd_pool.h"
#include "nvk_shader.h"
#include "vk_common_entrypoints.h"
#include "vk_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

int main(void)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   setvbuf(stdout, NULL, _IONBF, 0);
   puts("CPU meta-state control: initialize");
   struct nvk_device *dev = calloc(1, sizeof(*dev));
   struct nvk_physical_device *physical = calloc(1, sizeof(*physical));
   struct nvk_cmd_buffer *cmd = calloc(1, sizeof(*cmd));
   struct nvk_cmd_pool pool = {0};
   /* Empty GM20B vertex bindings encode the device's scratch VA. These
    * CPU fixtures only record it in the local push array; no BO is created.
    */
   struct nvkmd_va zero_va = {.addr = 0x10000};
   struct nvkmd_mem zero_page = {.va = &zero_va};
   struct nvk_shader vs = {0}, ps = {0}, next_ps = {0}, cs = {0};
   CHECK(dev && physical && cmd);
   dev->vk.physical = &physical->vk;
   dev->vk.alloc = *vk_default_allocator();
   dev->vk.shader_ops = &nvk_device_shader_ops;
   dev->zero_page = &zero_page;
   physical->info.cls_eng3d = 0xb197;
   physical->info.cls_compute = 0xb1c0;
   vk_object_base_init(&dev->vk, &dev->vk.base, VK_OBJECT_TYPE_DEVICE);
   vk_object_base_init(&dev->vk, &cmd->vk.base, VK_OBJECT_TYPE_COMMAND_BUFFER);
   pool.vk.alloc = dev->vk.alloc;
   cmd->vk.pool = &pool.vk;
   cmd->vk.dynamic_graphics_state.vi = &cmd->state.gfx._dynamic_vi;
   cmd->vk.dynamic_graphics_state.ms.sample_locations = &cmd->state.gfx._dynamic_sl;
   uint32_t push[4096];
   nv_push_init(&cmd->push, push, ARRAY_SIZE(push), UINT8_MAX);
   cmd->push_mem_limit = push + ARRAY_SIZE(push);

   struct nvk_shader *objects[] = {&vs, &ps, &next_ps, &cs};
   const mesa_shader_stage object_stages[] = {
      MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_FRAGMENT, MESA_SHADER_COMPUTE,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(objects); i++) {
      vk_object_base_init(&dev->vk, &objects[i]->vk.base, VK_OBJECT_TYPE_SHADER_EXT);
      objects[i]->vk.stage = object_stages[i];
   }
   VkCommandBuffer handle = nvk_cmd_buffer_to_handle(cmd);
   const VkShaderStageFlagBits stages[] = {
      VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   const VkShaderEXT shaders[] = {
      vk_shader_to_handle(&vs.vk), vk_shader_to_handle(&ps.vk),
   };
   puts("CPU meta-state control: initial shader bind");
   vk_common_CmdBindShadersEXT(handle, 2, stages, shaders);
   CHECK(cmd->state.gfx.shaders[MESA_SHADER_VERTEX] == &vs);

   /* Preserve an unrelated compute pipeline binding across the graphics
    * operation. The temporary pipeline binding is the only substituted
    * operation: save/restore and subsequent CmdBindShadersEXT are real.
    */
   cmd->vk.pipeline_shader_stages = VK_SHADER_STAGE_COMPUTE_BIT;
   struct nvk_meta_save_gfx gfx;
   puts("CPU meta-state control: graphics save");
   nvk_meta_begin_gfx(cmd, &gfx);
   cmd->vk.pipeline_shader_stages |= VK_SHADER_STAGE_VERTEX_BIT |
                                     VK_SHADER_STAGE_FRAGMENT_BIT;
   nvk_cmd_bind_graphics_shader(cmd, MESA_SHADER_VERTEX, NULL);
   nvk_cmd_bind_graphics_shader(cmd, MESA_SHADER_FRAGMENT, &next_ps);
   puts("CPU meta-state control: graphics restore");
   nvk_meta_end_gfx(cmd, &gfx);
   CHECK(cmd->state.gfx.shaders[MESA_SHADER_VERTEX] == &vs);
   const VkShaderEXT replacement = vk_shader_to_handle(&next_ps.vk);
   puts("CPU meta-state control: partial fragment bind");
   vk_common_CmdBindShadersEXT(handle, 1, &stages[1], &replacement);
   /* Without restoring pipeline_shader_stages, the common runtime unbinds
    * VS here. The next NVK draw then dereferences a null last_vtgm.
    */
   CHECK(cmd->state.gfx.shaders[MESA_SHADER_VERTEX] == &vs);
   CHECK(cmd->state.gfx.shaders[MESA_SHADER_FRAGMENT] == &next_ps);
   CHECK(cmd->vk.pipeline_shader_stages == VK_SHADER_STAGE_COMPUTE_BIT);
   puts("PASS: meta restore followed by PS-only bind retains the original VS");

   /* Also retain actual pipeline provenance, including mixed graphics and
    * compute state, rather than unconditionally clearing the mask.
    */
   const VkShaderStageFlags mixed = VK_SHADER_STAGE_VERTEX_BIT |
                                    VK_SHADER_STAGE_FRAGMENT_BIT |
                                    VK_SHADER_STAGE_COMPUTE_BIT;
   cmd->vk.pipeline_shader_stages = mixed;
   nvk_meta_begin_gfx(cmd, &gfx);
   cmd->vk.pipeline_shader_stages = VK_SHADER_STAGE_FRAGMENT_BIT;
   nvk_meta_end_gfx(cmd, &gfx);
   CHECK(cmd->vk.pipeline_shader_stages == mixed);

   cmd->vk.pipeline_shader_stages = VK_SHADER_STAGE_FRAGMENT_BIT;
   cmd->state.cs.shader = &cs;
   struct nvk_meta_save_compute compute;
   nvk_meta_begin_compute(cmd, &compute);
   cmd->vk.pipeline_shader_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
   nvk_meta_end_compute(cmd, &compute);
   CHECK(cmd->state.cs.shader == &cs);
   CHECK(cmd->vk.pipeline_shader_stages == VK_SHADER_STAGE_FRAGMENT_BIT);
   puts("PASS: graphics/compute meta preserve shader-object and pipeline provenance (CPU only)");

   CHECK(!dev->nvkmd && !cmd->push_mem);
   free(cmd); free(physical); free(dev);
   return 0;
}
