/* SPDX-License-Identifier: MIT */
/* Production common queue admission/merge/deferred storage, no GPU or KMT. */
#include "vk_queue.h"
#include "vk_device.h"
#include "vk_physical_device.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_internal_exts.h"
#include <windows.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t batches, expected_count, expected_first;
static HANDLE gate, finished;
static unsigned live_allocations;
static void *VKAPI_PTR counted_alloc(void *user, size_t size, size_t align,
                                     VkSystemAllocationScope scope)
{
   const VkAllocationCallbacks *base = vk_default_allocator();
   void *ptr = base->pfnAllocation(base->pUserData, size, align, scope);
   if (ptr) ++live_allocations;
   return ptr;
}
static void VKAPI_PTR counted_free(void *user, void *ptr)
{
   if (ptr) { CHECK(live_allocations); --live_allocations; }
   const VkAllocationCallbacks *base = vk_default_allocator();
   base->pfnFree(base->pUserData, ptr);
}
static VkResult capture(struct vk_queue *queue, struct vk_queue_submit *submit)
{
   if (gate) CHECK(WaitForSingleObject(gate, 10000) == WAIT_OBJECT_0);
   CHECK(submit->win32_written_count == expected_count);
   for (uint32_t i = 0; i < expected_count; ++i)
      CHECK(submit->win32_written_allocations[i] == expected_first + i);
   ++batches;
   if (finished) CHECK(SetEvent(finished));
   return VK_SUCCESS;
}

int main(void)
{
   struct vk_physical_device physical = {0};
   struct vk_device device = {0};
   vk_object_base_instance_init(NULL, &physical.base, VK_OBJECT_TYPE_PHYSICAL_DEVICE);
   vk_object_base_init(&device, &device.base, VK_OBJECT_TYPE_DEVICE);
   device.physical = &physical;
   device.alloc = *vk_default_allocator();
   device.alloc.pfnAllocation = counted_alloc; device.alloc.pfnFree = counted_free;
   device.timeline_mode = VK_DEVICE_TIMELINE_MODE_NATIVE;
   device.submit_mode = VK_QUEUE_SUBMIT_MODE_IMMEDIATE;
   list_inithead(&device.queues);
   struct vk_queue queue;
   const VkDeviceQueueCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
   };
   CHECK(vk_queue_init(&queue, &device, &create, 0) == VK_SUCCESS);
   queue.supports_wsi_written_primaries = true;
   queue.driver_submit = capture;
   struct vk_command_pool pool = {.queue_family_index = 0};
   struct vk_command_buffer cmd = {.pool = &pool};
   vk_object_base_init(&device, &cmd.base, VK_OBJECT_TYPE_COMMAND_BUFFER);
   VkCommandBufferSubmitInfo command = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = vk_command_buffer_to_handle(&cmd),
   };
   struct wsi_win32_written_primaries written = {
      .sType = VK_STRUCTURE_TYPE_WSI_WIN32_WRITTEN_PRIMARIES_MESA,
      .version = 1,
   };
   VkSubmitInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .pNext = &written, .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
   };
   for (uint32_t count = 0; count <= 16; count += 8) {
      written.count = count;
      for (uint32_t i = 0; i < count; ++i) written.allocations[i] = 100 + i;
      expected_count = count; expected_first = 100;
      CHECK(vk_common_QueueSubmit2(vk_queue_to_handle(&queue), 1, &info, VK_NULL_HANDLE) == VK_SUCCESS);
   }
   queue.submit.mode = VK_QUEUE_SUBMIT_MODE_IMMEDIATE;
   CHECK(batches == 3);
   for (unsigned failure = 0; failure < 6; ++failure) {
      written.version = 1; written.count = 2;
      written.allocations[0] = 100; written.allocations[1] = 101;
      info.commandBufferInfoCount = 1; queue.supports_wsi_written_primaries = true;
      switch (failure) {
      case 0: queue.supports_wsi_written_primaries = false; break;
      case 1: written.version = 2; break;
      case 2: written.count = 17; break;
      case 3: written.allocations[0] = 0; break;
      case 4: written.allocations[1] = 100; break;
      case 5: info.commandBufferInfoCount = 0; break;
      }
      CHECK(vk_common_QueueSubmit2(vk_queue_to_handle(&queue), 1, &info, VK_NULL_HANDLE) != VK_SUCCESS);
   }
   CHECK(batches == 3);
   queue.supports_wsi_written_primaries = true;
   written.version = 1; written.count = 16;
   for (uint32_t i = 0; i < 16; ++i) written.allocations[i] = 200 + i;
   const VkSubmitInfo2 pair[2] = {
      {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = &written,
       .commandBufferInfoCount = 1, .pCommandBufferInfos = &command},
      {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = &written,
       .commandBufferInfoCount = 1, .pCommandBufferInfos = &command},
   };
   expected_count = 16; expected_first = 200;
   CHECK(vk_common_QueueSubmit2(vk_queue_to_handle(&queue), 2, pair, VK_NULL_HANDLE) == VK_SUCCESS);
   CHECK(batches == 5); /* Both writer batches retained their own list. */
   struct wsi_win32_written_primaries invalid = written;
   invalid.count = 17;
   VkSubmitInfo2 bad_pair[2] = {pair[0], pair[1]}; bad_pair[1].pNext = &invalid;
   CHECK(!live_allocations);
   CHECK(vk_common_QueueSubmit2(vk_queue_to_handle(&queue), 2, bad_pair, VK_NULL_HANDLE) != VK_SUCCESS);
   CHECK(batches == 5 && !live_allocations); /* Unsubmitted first batch released. */
   VkSubmitInfo2 plain[2] = {pair[0], pair[1]};
   plain[0].pNext = plain[1].pNext = NULL; expected_count = 0;
   CHECK(vk_common_QueueSubmit2(vk_queue_to_handle(&queue), 2, plain, VK_NULL_HANDLE) == VK_SUCCESS);
   CHECK(batches == 6); /* Ordinary submissions still merge as before. */
   vk_queue_finish(&queue);
   /* Hold the real worker at the driver boundary. QueueSubmit2 must have
    * copied the stack list before returning to the caller. */
   device.submit_mode = VK_QUEUE_SUBMIT_MODE_THREADED;
   gate = CreateEventW(NULL, TRUE, FALSE, NULL);
   finished = CreateEventW(NULL, TRUE, FALSE, NULL);
   CHECK(gate && finished);
   CHECK(vk_queue_init(&queue, &device, &create, 0) == VK_SUCCESS);
   queue.supports_wsi_written_primaries = true; queue.driver_submit = capture;
   info.commandBufferInfoCount = 1; expected_count = 16; expected_first = 200;
   CHECK(vk_common_QueueSubmit2(vk_queue_to_handle(&queue), 1, &info, VK_NULL_HANDLE) == VK_SUCCESS);
   memset(written.allocations, 0, sizeof(written.allocations)); written.count = 0;
   CHECK(SetEvent(gate));
   CHECK(WaitForSingleObject(finished, 10000) == WAIT_OBJECT_0 && batches == 7);
   vk_queue_finish(&queue);
   CHECK(CloseHandle(finished) && CloseHandle(gate));
   vk_object_base_finish(&cmd.base);
   vk_object_base_finish(&device.base);
   vk_object_base_finish(&physical.base);
   CHECK(!live_allocations);
   puts("PASS: actual common queue copies write lists, rejects invalid input and preserves writer boundaries through threaded and multi-submit paths (CPU control)");
   return 0;
}
