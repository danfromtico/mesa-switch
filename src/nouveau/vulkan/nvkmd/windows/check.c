/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "kmt.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   if (argc != 2) {
      fprintf(stderr, "usage: nvk-windows-check.exe <absolute-path-to-NVK-DLL>\n");
      return 2;
   }
   struct tg_kmt_scan scan;
   LONG status = tg_gpu_kmt_scan_system(&scan);
   if (status < 0) {
      fprintf(stderr, "KMT scan failed: %08lx\n", (unsigned long)status);
      return 3;
   }
   HMODULE lib = LoadLibraryExA(argv[1], NULL,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!lib) {
      fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
      return 4;
   }
   PFN_vkGetInstanceProcAddr gipa =
      (void *)GetProcAddress(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { FreeLibrary(lib); return 5; }
   PFN_vkEnumerateInstanceVersion version = (void *)gipa(NULL, "vkEnumerateInstanceVersion");
   PFN_vkCreateInstance create = (void *)gipa(NULL, "vkCreateInstance");
   if (!version || !create) { FreeLibrary(lib); return 6; }
   uint32_t api = 0;
   VkResult r = version(&api);
   if (r != VK_SUCCESS || api < VK_API_VERSION_1_0) { FreeLibrary(lib); return 7; }
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "T214 NVK identity check", .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   r = create(&ci, NULL, &instance);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance failed: %d\n", r);
      FreeLibrary(lib); return 8;
   }
   PFN_vkDestroyInstance destroy = (void *)gipa(instance, "vkDestroyInstance");
   PFN_vkEnumeratePhysicalDevices enumerate = (void *)gipa(instance, "vkEnumeratePhysicalDevices");
   if (!destroy || !enumerate) { FreeLibrary(lib); return 9; }
   uint32_t count = 0;
   r = enumerate(instance, &count, NULL);
   const struct tg_gpu_adapter_info *adapter = scan.native_ready ? &scan.first_ready : &scan.first_match;
   const uint64_t expected = ((uint64_t)(uint32_t)adapter->luid_high << 32) | adapter->luid_low;
   bool luid_match = false;
   if (r == VK_SUCCESS && count == 1 && scan.native_ready) {
      VkPhysicalDevice physical;
      uint32_t capacity = 1;
      r = enumerate(instance, &capacity, &physical);
      PFN_vkGetPhysicalDeviceProperties2 properties =
         (void *)gipa(instance, "vkGetPhysicalDeviceProperties2");
      if (r == VK_SUCCESS && capacity == 1 && properties) {
         VkPhysicalDeviceIDProperties id = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
         };
         VkPhysicalDeviceProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id,
         };
         properties(physical, &props);
         uint64_t actual;
         memcpy(&actual, id.deviceLUID, sizeof(actual));
         luid_match = id.deviceLUIDValid && id.deviceNodeMask == 1 &&
                      props.properties.vendorID == 0x10de && actual == expected;
      }
   }
   destroy(instance, NULL);
   FreeLibrary(lib);
   printf("{\"scope\":\"NVK Windows ICD identity\",\"api_version\":%u,"
          "\"kmt_adapters\":%u,\"private_queries_ok\":%u,\"matching_kmd\":%u,"
          "\"native_ready_kmd\":%u,\"enumerate_result\":%d,\"physical_devices\":%u,"
          "\"kmt_luid\":\"%016llx\",\"matching_luid\":%s}\n",
          api, scan.enumerated, scan.queried, scan.matched, scan.native_ready, r, count,
          (unsigned long long)expected, luid_match ? "true" : "false");
   return r == VK_SUCCESS && (scan.native_ready ? luid_match : count == 0) ? 0 : 10;
}
