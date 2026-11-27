/* SPDX-License-Identifier: MIT */
/* Native Win32 Vulkan storage/image sharing. No D3D resource interoperability. */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include "kmt.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); ExitProcess(3); \
} } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "FAIL line %u: %s = %d\n", __LINE__, #x, r_); ExitProcess(4); \
} } while (0)

#define FUNCTIONS(X) \
   X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties2) \
   X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
   X(GetPhysicalDeviceExternalBufferProperties) X(GetPhysicalDeviceImageFormatProperties2) \
   X(CreateDevice) X(DestroyDevice) X(GetDeviceProcAddr) X(GetDeviceQueue) \
   X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) \
   X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
   X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit) \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
   X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
   X(CmdPipelineBarrier) X(CmdFillBuffer) X(CmdClearColorImage) \
   X(CmdCopyBuffer) X(CmdCopyImageToBuffer)
#define DECLARE(n) static PFN_vk##n vk##n;
FUNCTIONS(DECLARE)
#undef DECLARE
static PFN_vkGetMemoryWin32HandleKHR get_handle;

enum { WIDTH = 64, HEIGHT = 64, WORDS = WIDTH * HEIGHT, BYTES = WORDS * 4,
       GUARD_WORDS = 32, READ_WORDS = WORDS + 2 * GUARD_WORDS };
static const uint32_t guard = 0xcdcdcdcd;
static const VkExternalMemoryHandleTypeFlagBits opaque =
   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static VkPhysicalDevice physical;
static VkPhysicalDeviceMemoryProperties memory_props;
static uint32_t family;
static uint64_t checked_words;

struct device {
   VkDevice vk;
   VkQueue queue;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
};

struct resource {
   VkBuffer buffer;
   VkImage image;
   VkDeviceMemory memory;
   VkMemoryRequirements reqs;
   VkFormat format;
   uint32_t memory_type;
};

static struct device
create_device(void)
{
   const float priority = 1;
   const VkDeviceQueueCreateInfo queue = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
   };
   const char *extension = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
   const VkDeviceCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension,
   };
   struct device d = {0};
   VK(vkCreateDevice(physical, &info, NULL, &d.vk));
   get_handle = (void *)vkGetDeviceProcAddr(d.vk, "vkGetMemoryWin32HandleKHR");
   CHECK(get_handle);
   vkGetDeviceQueue(d.vk, family, 0, &d.queue);
   const VkCommandPoolCreateInfo pool = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
   };
   VK(vkCreateCommandPool(d.vk, &pool, NULL, &d.pool));
   const VkCommandBufferAllocateInfo cmd = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = d.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   VK(vkAllocateCommandBuffers(d.vk, &cmd, &d.cmd));
   const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VK(vkCreateFence(d.vk, &fence, NULL, &d.fence));
   return d;
}

static void
destroy_device(struct device *d)
{
   vkDestroyFence(d->vk, d->fence, NULL);
   vkDestroyCommandPool(d->vk, d->pool, NULL);
   vkDestroyDevice(d->vk, NULL);
   memset(d, 0, sizeof(*d));
}

static void
begin(struct device *d)
{
   VK(vkResetCommandPool(d->vk, d->pool, 0));
   VK(vkResetFences(d->vk, 1, &d->fence));
   const VkCommandBufferBeginInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VK(vkBeginCommandBuffer(d->cmd, &info));
}

static void
submit(struct device *d)
{
   VK(vkEndCommandBuffer(d->cmd));
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
      .pCommandBuffers = &d->cmd,
   };
   VK(vkQueueSubmit(d->queue, 1, &submit, d->fence));
   VK(vkWaitForFences(d->vk, 1, &d->fence, VK_TRUE, UINT64_C(10000000000)));
}

static struct resource
create_resource(struct device *d, VkFormat format, bool shared)
{
   struct resource r = {.format = format};
   if (format == VK_FORMAT_UNDEFINED) {
      const VkExternalMemoryBufferCreateInfo external = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = opaque,
      };
      const VkBufferCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = shared ? &external : NULL,
         .size = shared ? BYTES : READ_WORDS * 4,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      };
      VK(vkCreateBuffer(d->vk, &info, NULL, &r.buffer));
      vkGetBufferMemoryRequirements(d->vk, r.buffer, &r.reqs);
   } else {
      const VkExternalMemoryImageCreateInfo external = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = opaque,
      };
      const VkImageCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
         .flags = VK_IMAGE_CREATE_ALIAS_BIT, .imageType = VK_IMAGE_TYPE_2D, .format = format,
         .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      };
      VK(vkCreateImage(d->vk, &info, NULL, &r.image));
      vkGetImageMemoryRequirements(d->vk, r.image, &r.reqs);
   }
   r.memory_type = UINT32_MAX;
   const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++) {
      if ((r.reqs.memoryTypeBits & (1u << i)) &&
          (memory_props.memoryTypes[i].propertyFlags & flags) == flags) {
         r.memory_type = i;
         break;
      }
   }
   CHECK(r.memory_type != UINT32_MAX);
   return r;
}

static void
allocate_resource(struct device *d, struct resource *r, bool shared,
                   HANDLE imported, const WCHAR *import_name, const WCHAR *export_name)
{
   const SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
   const VkExportMemoryWin32HandleInfoKHR win32 = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      .pAttributes = &security, .dwAccess = STANDARD_RIGHTS_READ | 1 /* SHARED_ALLOCATION_WRITE */,
      .name = export_name,
   };
   const VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = export_name ? &win32 : NULL, .handleTypes = opaque,
   };
   const VkImportMemoryWin32HandleInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      .pNext = shared ? &export_info : NULL, .handleType = opaque,
      .handle = imported, .name = import_name,
   };
   const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = imported || import_name ? (const void *)&import_info :
               shared ? (const void *)&export_info : NULL,
      .allocationSize = r->reqs.size, .memoryTypeIndex = r->memory_type,
   };
   VK(vkAllocateMemory(d->vk, &info, NULL, &r->memory));
   if (r->image)
      VK(vkBindImageMemory(d->vk, r->image, r->memory, 0));
   else
      VK(vkBindBufferMemory(d->vk, r->buffer, r->memory, 0));
}

static HANDLE
export_resource(struct device *d, const struct resource *r, bool inherit)
{
   const VkMemoryGetWin32HandleInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
      .memory = r->memory, .handleType = opaque,
   };
   HANDLE handle = NULL;
   VK(get_handle(d->vk, &info, &handle));
   DWORD flags;
   CHECK(handle && GetHandleInformation(handle, &flags));
   CHECK(!!(flags & HANDLE_FLAG_INHERIT) == inherit);
   return handle;
}

static void
destroy_resource(struct device *d, struct resource *r)
{
   if (r->image) vkDestroyImage(d->vk, r->image, NULL);
   if (r->buffer) vkDestroyBuffer(d->vk, r->buffer, NULL);
   vkFreeMemory(d->vk, r->memory, NULL);
   memset(r, 0, sizeof(*r));
}

static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

static void
barrier(struct device *d, struct resource *r, VkAccessFlags src, VkAccessFlags dst,
         VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
         uint32_t src_family, uint32_t dst_family, VkImageLayout old, VkImageLayout next)
{
   if (r->image) {
      const VkImageMemoryBarrier b = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
         .oldLayout = old, .newLayout = next, .srcQueueFamilyIndex = src_family,
         .dstQueueFamilyIndex = dst_family, .image = r->image, .subresourceRange = color_range,
      };
      vkCmdPipelineBarrier(d->cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
   } else {
      const VkBufferMemoryBarrier b = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
         .srcQueueFamilyIndex = src_family, .dstQueueFamilyIndex = dst_family,
         .buffer = r->buffer, .size = VK_WHOLE_SIZE,
      };
      vkCmdPipelineBarrier(d->cmd, src_stage, dst_stage, 0, 0, NULL, 1, &b, 0, NULL);
   }
}

static uint32_t
pattern(VkFormat format, bool second)
{
   if (format == VK_FORMAT_UNDEFINED) return second ? 0x4f17a239 : 0x9d62c503;
   const bool red_low = format == VK_FORMAT_R8G8B8A8_UNORM;
   return (second == red_low) ? 0xff0000ff : 0xffff0000;
}

static void
write_resource(struct device *d, struct resource *r, bool second)
{
   if (r->image) {
      const VkClearColorValue color = {.float32 = {second ? 1 : 0, 0, second ? 0 : 1, 1}};
      vkCmdClearColorImage(d->cmd, r->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &color, 1, &color_range);
   } else {
      vkCmdFillBuffer(d->cmd, r->buffer, 0, BYTES, pattern(r->format, second));
   }
}

static void
produce(struct device *d, struct resource *r)
{
   begin(d);
   barrier(d, r, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   write_resource(d, r, false);
   barrier(d, r, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            family, VK_QUEUE_FAMILY_EXTERNAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
   submit(d);
}

static void
consume(struct device *d, struct resource *r, bool second)
{
   struct resource readback = create_resource(d, VK_FORMAT_UNDEFINED, false);
   allocate_resource(d, &readback, false, NULL, NULL, NULL);
   uint32_t *map;
   VK(vkMapMemory(d->vk, readback.memory, 0, READ_WORDS * 4, 0, (void **)&map));
   for (unsigned i = 0; i < READ_WORDS; i++) map[i] = guard;
   begin(d);
   barrier(d, r, 0, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_QUEUE_FAMILY_EXTERNAL, family,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   if (r->image) {
      const VkBufferImageCopy copy = {
         .bufferOffset = GUARD_WORDS * 4,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {WIDTH, HEIGHT, 1},
      };
      vkCmdCopyImageToBuffer(d->cmd, r->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &copy);
   } else {
      const VkBufferCopy copy = {0, GUARD_WORDS * 4, BYTES};
      vkCmdCopyBuffer(d->cmd, r->buffer, readback.buffer, 1, &copy);
   }
   barrier(d, &readback, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, 0, 0);
   if (!second) {
      barrier(d, r, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      write_resource(d, r, true);
   }
   barrier(d, r, second ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            family, VK_QUEUE_FAMILY_EXTERNAL,
            second ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL);
   submit(d);
   unsigned mismatches = 0;
   for (unsigned i = 0; i < READ_WORDS; i++) {
      uint32_t expected = i < GUARD_WORDS || i >= GUARD_WORDS + WORDS ? guard : pattern(r->format, second);
      if (map[i] != expected) {
         if (!mismatches) fprintf(stderr, "first mismatch at %u: %08x expected %08x\n", i, map[i], expected);
         mismatches++;
      }
   }
   printf("{\"format\":%u,\"generation\":%u,\"words\":%u,\"guards\":%u,\"mismatches\":%u}\n",
           r->format, second + 1, WORDS, 2 * GUARD_WORDS, mismatches);
   CHECK(!mismatches);
   checked_words += READ_WORDS;
   vkUnmapMemory(d->vk, readback.memory);
   destroy_resource(d, &readback);
}

static void
run_case(VkFormat format, bool named)
{
   printf("case format=%u named=%u\n", format, named);
   struct device producer = create_device(), consumer = create_device();
   struct resource a = create_resource(&producer, format, true);
   WCHAR name[96];
   swprintf(name, 96, L"Local\\nvk-shared-%lu-%u", GetCurrentProcessId(), format);
   allocate_resource(&producer, &a, true, NULL, NULL, named ? name : NULL);
   produce(&producer, &a);
   HANDLE handle = export_resource(&producer, &a, named);
   struct resource b = create_resource(&consumer, format, true);
   CHECK(a.reqs.size == b.reqs.size && a.memory_type == b.memory_type);
   allocate_resource(&consumer, &b, true, named ? NULL : handle, named ? name : NULL, NULL);
   /* Both the exporting VkDevice and the application's handle go away. */
   destroy_resource(&producer, &a);
   destroy_device(&producer);
   CHECK(CloseHandle(handle));
   consume(&consumer, &b, false);
   handle = export_resource(&consumer, &b, false);
   producer = create_device();
   a = create_resource(&producer, format, true);
   CHECK(a.reqs.size == b.reqs.size && a.memory_type == b.memory_type);
   allocate_resource(&producer, &a, true, handle, NULL, NULL);
   destroy_resource(&consumer, &b);
   destroy_device(&consumer);
   CHECK(CloseHandle(handle));
   consume(&producer, &a, true);
   destroy_resource(&producer, &a);
   destroy_device(&producer);
   printf("PASS format=%u named=%u producer-release/import/re-export/GPU-write-read\n", format, named);
}

static void
check_properties(void)
{
   const VkExternalMemoryHandleTypeFlagBits types[] = {
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
   };
   for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
      const VkPhysicalDeviceExternalBufferInfo info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .handleType = types[i],
      };
      VkExternalBufferProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
      vkGetPhysicalDeviceExternalBufferProperties(physical, &info, &props);
      const VkExternalMemoryProperties *p = &props.externalMemoryProperties;
      CHECK(p->externalMemoryFeatures == (i ? 0 : VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                                                  VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT));
      if (!i) CHECK(p->compatibleHandleTypes == opaque && p->exportFromImportedHandleTypes == opaque);
   }
   const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
   for (unsigned i = 0; i < 2; i++) {
      const VkPhysicalDeviceExternalImageFormatInfo external = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .handleType = opaque,
      };
      const VkPhysicalDeviceImageFormatInfo2 info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &external,
         .format = formats[i], .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .flags = VK_IMAGE_CREATE_ALIAS_BIT,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      };
      VkExternalImageFormatProperties ext_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
      VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext_props};
      VK(vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &props));
      CHECK(ext_props.externalMemoryProperties.externalMemoryFeatures ==
             (VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT));
   }
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   CHECK(argc == 2);
   struct tg_kmt_scan scan;
   CHECK(tg_gpu_kmt_scan_system(&scan) >= 0 && scan.native_ready == 1);
   HMODULE loader = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
   CHECK(loader);
   PFN_vkGetInstanceProcAddr gipa = (void *)GetProcAddress(loader, "vkGetInstanceProcAddr");
   CHECK(gipa);
   PFN_vkCreateInstance create = (void *)gipa(NULL, "vkCreateInstance");
   CHECK(create);
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  .pApplicationName = "NVK Win32 shared memory", .apiVersion = VK_API_VERSION_1_1};
   const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance instance;
   VK(create(&info, NULL, &instance));
#define LOAD(n) vk##n = (void *)gipa(instance, "vk" #n); CHECK(vk##n);
   FUNCTIONS(LOAD)
#undef LOAD
   uint32_t count = 0;
   VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
   CHECK(count == 1);
   VK(vkEnumeratePhysicalDevices(instance, &count, &physical));
   VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
   VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
   vkGetPhysicalDeviceProperties2(physical, &props);
   uint64_t actual;
   memcpy(&actual, id.deviceLUID, sizeof(actual));
   const uint64_t expected = (uint64_t)(uint32_t)scan.first_ready.luid_high << 32 | scan.first_ready.luid_low;
   CHECK(id.deviceLUIDValid && actual == expected && props.properties.vendorID == 0x10de);
   printf("{\"luid\":\"%016" PRIx64 "\",\"device\":\"%s\",\"budget\":%" PRIu64 "}\n",
           actual, props.properties.deviceName, scan.first_ready.memory_budget_bytes);
   vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
   uint32_t queues = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &queues, NULL);
   CHECK(queues && queues <= 8);
   VkQueueFamilyProperties families[8];
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &queues, families);
   family = UINT32_MAX;
   for (unsigned i = 0; i < queues; i++) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {family = i; break;}
   CHECK(family != UINT32_MAX);
   check_properties();
   run_case(VK_FORMAT_UNDEFINED, false);
   run_case(VK_FORMAT_UNDEFINED, true);
   run_case(VK_FORMAT_R8G8B8A8_UNORM, false);
   run_case(VK_FORMAT_B8G8R8A8_UNORM, true);
   vkDestroyInstance(instance, NULL);
   CHECK(FreeLibrary(loader));
   printf("{\"result\":\"PASS\",\"checked_words_with_guards\":%" PRIu64 "}\n", checked_words);
   return 0;
}
