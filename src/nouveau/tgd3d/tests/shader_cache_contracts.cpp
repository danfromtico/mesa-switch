/* SPDX-License-Identifier: MIT
 * Real Windows cache files and separate processes; Vulkan is substituted.
 */
#include "tgd3d.h"
#include "util/disk_cache.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace tgd3d;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

static unsigned spvCalls, binaryCalls, destroyed;
static VkResult binaryResult = VK_SUCCESS, exportResult = VK_SUCCESS;
static const uint32_t opaque[] = {0x54475348, 1, 2, 3, 4, 5, 6, 7};

void shader_cache_contracts(bool child)
{
   wchar_t root[MAX_PATH], temp[MAX_PATH];
   if (!child) {
      CHECK(GetTempPathW(MAX_PATH, temp));
      CHECK(swprintf(root, MAX_PATH, L"%lstgd3d-cache-%lu-%llu", temp,
         GetCurrentProcessId(), (unsigned long long)GetTickCount64()) > 0);
      CHECK(CreateDirectoryW(root, nullptr));
      CHECK(SetEnvironmentVariableW(L"MESA_SHADER_CACHE_DIR", root));
      CHECK(SetEnvironmentVariableW(L"MESA_SHADER_CACHE_DISABLE", L"false"));
   }
   VkPhysicalDeviceShaderObjectPropertiesEXT props = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
   memset(props.shaderBinaryUUID, 0x11, VK_UUID_SIZE);
   Adapter adapter;
   adapter.vk.shaderCache = create_shader_cache(props);
   CHECK(adapter.vk.shaderCache);
   Device dev;
   dev.adapter = &adapter;
   dev.caps = &adapter.vk.caps;
   adapter.vk.caps.cbSlots = D3D_CB_SLOTS;
   dev.vkdev = (VkDevice)(uintptr_t)1;
   dev.vk.CreateShadersEXT = [](VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *info,
                               const VkAllocationCallbacks *, VkShaderEXT *out) {
      CHECK(count == 1);
      *out = VK_NULL_HANDLE;
      if (info->codeType == VK_SHADER_CODE_TYPE_BINARY_EXT) {
         ++binaryCalls;
         CHECK(!((uintptr_t)info->pCode & 15) && info->codeSize == sizeof(opaque));
         CHECK(!memcmp(info->pCode, opaque, sizeof(opaque)));
         if (binaryResult != VK_SUCCESS)
            return binaryResult;
      } else {
         ++spvCalls;
         CHECK(info->codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT);
         CHECK(static_cast<const uint32_t *>(info->pCode)[0] == 0x07230203);
      }
      *out = (VkShaderEXT)((uintptr_t)device * 100 + spvCalls + binaryCalls);
      return VK_SUCCESS;
   };
   dev.vk.GetShaderBinaryDataEXT = [](VkDevice, VkShaderEXT, size_t *size, void *data) {
      if (exportResult != VK_SUCCESS)
         return exportResult;
      if (data) {
         CHECK(!((uintptr_t)data & 15) && *size >= sizeof(opaque));
         memcpy(data, opaque, sizeof(opaque));
      }
      *size = sizeof(opaque);
      return VK_SUCCESS;
   };
   dev.vk.DestroyShaderEXT = [](VkDevice, VkShaderEXT shader, const VkAllocationCallbacks *) {
      CHECK(shader); ++destroyed;
   };
   /* Use the real frontend's VS emitted earlier in this executable. */
   FILE *file = fopen("tgd3d-vs.spv", "rb"); CHECK(file);
   CHECK(!fseek(file, 0, SEEK_END)); const long length = ftell(file);
   CHECK(length >= 20 && !(length & 3) && !fseek(file, 0, SEEK_SET));
   std::vector<uint32_t> code(length / 4);
   CHECK(fread(code.data(), 1, length, file) == (size_t)length && !fclose(file));
   VkDescriptorSetLayout layouts[SET_COUNT] = {};
   VkShaderCreateInfoEXT info = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
   info.stage = VK_SHADER_STAGE_VERTEX_BIT;
   info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
   info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
   info.codeSize = length; info.pCode = code.data(); info.pName = "main";
   info.setLayoutCount = SET_COUNT; info.pSetLayouts = layouts;
   VkShaderEXT first, next;
   CHECK(create_shader_cached(&dev, info, &first) == VK_SUCCESS);
   if (child) {
      CHECK(binaryCalls == 1 && spvCalls == 0);
      adapter.vk.close();
      std::puts("PASS: tgd3d binary cache hit in a fresh process (CPU control)");
      return;
   }
   CHECK(spvCalls == 1 && binaryCalls == 0);
   adapter.vk.close(); /* waits for the copied CPU blob, without retaining a Vulkan handle */
   wchar_t exe[MAX_PATH], command[MAX_PATH + 64];
   CHECK(GetModuleFileNameW(nullptr, exe, MAX_PATH));
   CHECK(swprintf(command, MAX_PATH + 64, L"\"%ls\" --shader-cache-child", exe) > 0);
   STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
   PROCESS_INFORMATION process = {};
   CHECK(CreateProcessW(exe, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
   CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
   DWORD status; CHECK(GetExitCodeProcess(process.hProcess, &status) && !status);
   CloseHandle(process.hThread); CloseHandle(process.hProcess);
   adapter.vk.shaderCache = create_shader_cache(props); CHECK(adapter.vk.shaderCache);
   dev.vkdev = (VkDevice)(uintptr_t)2;
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS && next != first);
   CHECK(spvCalls == 1 && binaryCalls == 1);
   binaryResult = VK_INCOMPATIBLE_SHADER_BINARY_EXT;
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS);
   CHECK(spvCalls == 2 && binaryCalls == 2);
   disk_cache_wait_for_idle(adapter.vk.shaderCache);
   for (VkResult error : {VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
      binaryResult = error;
      CHECK(create_shader_cached(&dev, info, &next) == error && !next);
      CHECK(spvCalls == 2);
   }
   binaryResult = VK_SUCCESS;
   ++code[3]; /* a distinct SPIR-V bound changes the key */
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS && spvCalls == 3);
   --code[3];
   info.nextStage |= VK_SHADER_STAGE_GEOMETRY_BIT;
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS && spvCalls == 4);
   info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
   adapter.vk.close();
   ++props.shaderBinaryVersion;
   adapter.vk.shaderCache = create_shader_cache(props);
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS && spvCalls == 5);
   adapter.vk.close();
   ++props.shaderBinaryUUID[0];
   adapter.vk.shaderCache = create_shader_cache(props);
   exportResult = VK_ERROR_OUT_OF_HOST_MEMORY;
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS && spvCalls == 6);
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS && spvCalls == 7);
   exportResult = VK_ERROR_DEVICE_LOST;
   CHECK(create_shader_cached(&dev, info, &next) == VK_ERROR_DEVICE_LOST && !next);
   CHECK(spvCalls == 8 && destroyed == 1);
   adapter.vk.close();
   CHECK(create_shader_cached(&dev, info, &next) == VK_SUCCESS && spvCalls == 9);
   std::puts("PASS: binary cache cold/warm devices, SPIR-V/stage/driver keys, incompatible fallback, OOM/loss propagation and optional export failure (CPU control)");
}
