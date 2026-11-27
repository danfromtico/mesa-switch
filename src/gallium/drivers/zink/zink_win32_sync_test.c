/* SPDX-License-Identifier: MIT */
/* Exercise Zink's actual import/wait/signal functions with Vulkan spies.
 * This tests recording and ownership, not GPU completion. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include "zink_context.h"
#include "zink_fence.h"
#include "zink_screen.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static VkResult create_result, import_result;
static unsigned creates, imports, destroys, flushes;
static VkSemaphoreType created_type;
static VkExternalSemaphoreHandleTypeFlagBits imported_type;
static HANDLE imported_handle;
static const wchar_t *imported_name;

static VKAPI_ATTR VkResult VKAPI_CALL
create_semaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
                 const VkAllocationCallbacks *alloc, VkSemaphore *out)
{
   creates++;
   const VkSemaphoreTypeCreateInfo *type = info->pNext;
   CHECK(type && type->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
   created_type = type->semaphoreType;
   if (create_result == VK_SUCCESS)
      *out = (VkSemaphore)(uintptr_t)(0x1000 + creates);
   return create_result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
import_semaphore(VkDevice device, const VkImportSemaphoreWin32HandleInfoKHR *info)
{
   imports++;
   CHECK(info->sType == VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR);
   CHECK(info->flags == 0 && info->semaphore);
   imported_type = info->handleType;
   imported_handle = info->handle;
   imported_name = info->name;
   return import_result;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_semaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *alloc)
{
   CHECK(semaphore);
   destroys++;
}

static void
flush(struct pipe_context *pipe, struct pipe_fence_handle **fence, unsigned flags)
{
   flushes++;
   CHECK(!fence);
}

static void
release_batch(struct zink_screen *screen, struct zink_batch_state *batch)
{
   util_dynarray_foreach(&batch->fences, struct zink_tc_fence *, fence)
      zink_fence_reference(screen, fence, NULL);
   util_dynarray_fini(&batch->fences);
   util_dynarray_fini(&batch->wait_semaphores);
   util_dynarray_fini(&batch->wait_semaphore_stages);
   util_dynarray_fini(&batch->wait_semaphore_values);
   util_dynarray_fini(&batch->user_signal_semaphores);
   util_dynarray_fini(&batch->user_signal_semaphore_values);
}

int main(void)
{
   struct zink_screen *screen = calloc(1, sizeof(*screen));
   struct zink_context *ctx = calloc(1, sizeof(*ctx));
   struct zink_batch_state *first = calloc(1, sizeof(*first));
   struct zink_batch_state *second = calloc(1, sizeof(*second));
   CHECK(screen && ctx && first && second);
   screen->vk.CreateSemaphore = create_semaphore;
   screen->vk.ImportSemaphoreWin32HandleKHR = import_semaphore;
   screen->vk.DestroySemaphore = destroy_semaphore;
   zink_screen_fence_init(&screen->base);
   struct pipe_fence_handle *fence = NULL;
   HANDLE handle = (HANDLE)(uintptr_t)0x1234;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(!fence && !creates && !imports);
   screen->info.have_KHR_external_semaphore_win32 = true;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(fence && created_type == VK_SEMAPHORE_TYPE_TIMELINE);
   CHECK(imported_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT);
   CHECK(imported_handle == handle && !imported_name);

   ctx->base.screen = &screen->base;
   ctx->base.flush = flush;
   ctx->bs = first;
   zink_fence_server_sync(&ctx->base, fence, 3);
   zink_fence_server_sync(&ctx->base, fence, 9);
   zink_fence_server_sync(&ctx->base, fence, 5);
   CHECK(util_dynarray_num_elements(&first->wait_semaphores, VkSemaphore) == 1);
   CHECK(*(uint64_t *)first->wait_semaphore_values.data == 9);
   /* The old batch has not submitted yet. Its deferred-context marker must
    * not discard the next batch's timeline wait. */
   ctx->bs = second;
   zink_fence_server_sync(&ctx->base, fence, 11);
   CHECK(util_dynarray_num_elements(&second->wait_semaphores, VkSemaphore) == 1);
   CHECK(*(uint64_t *)second->wait_semaphore_values.data == 11);
   zink_fence_server_signal(&ctx->base, fence, 12);
   CHECK(flushes == 1 && second->has_work);
   CHECK(*(uint64_t *)second->user_signal_semaphore_values.data == 12);
   screen->base.fence_reference(&screen->base, &fence, NULL);
   CHECK(destroys == 0);
   release_batch(screen, first);
   CHECK(destroys == 0);
   release_batch(screen, second);
   CHECK(destroys == 1);

   zink_create_fence_win32(&screen->base, &fence, NULL, L"named-fence",
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_VK);
   CHECK(fence && created_type == VK_SEMAPHORE_TYPE_TIMELINE);
   CHECK(imported_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT);
   CHECK(!imported_handle && !wcscmp(imported_name, L"named-fence"));
   unsigned before = destroys;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL, PIPE_FD_TYPE_SYNCOBJ);
   CHECK(fence && destroys == before + 1 && created_type == VK_SEMAPHORE_TYPE_BINARY);
   CHECK(imported_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT);
   screen->base.fence_reference(&screen->base, &fence, NULL);

   before = imports;
   create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(!fence && imports == before);
   create_result = VK_SUCCESS;
   import_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
   before = destroys;
   zink_create_fence_win32(&screen->base, &fence, handle, NULL,
                           PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
   CHECK(!fence && destroys == before + 1);

   free(second);
   free(first);
   free(ctx);
   free(screen);
   puts("PASS: actual Zink Win32 imports, timeline wait ordering, and signal lifetime; Vulkan calls mocked, no GPU execution");
   return 0;
}
