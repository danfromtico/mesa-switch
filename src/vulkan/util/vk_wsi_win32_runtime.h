/* SPDX-License-Identifier: MIT */
#ifndef VK_WSI_WIN32_RUNTIME_H
#define VK_WSI_WIN32_RUNTIME_H

#include <vulkan/vulkan_core.h>
#include <stdbool.h>
#include <stdint.h>

/* Private, in-tree transport. Contexts and monitored fences belong to the
 * runtime device identified by device_cookie. No runtime HANDLE is cast to
 * a KMT handle. All callbacks execute inline on the invoking DDI thread;
 * adopters must not defer them to a Vulkan submission worker.
 *
 * The frontend/state outlives the Vulkan device. A fence token owns one
 * reference; retain creates another and release consumes exactly one. GPU
 * uses must finish before the last reference is released. Failed destruction
 * is retained by the frontend until finish can prove it was released.
 */
#define WSI_WIN32_RUNTIME_VERSION 1u
#define WSI_WIN32_RUNTIME_3D 1u
#define WSI_WIN32_RUNTIME_COMPUTE 2u
#define WSI_WIN32_RUNTIME_COPY 4u
#define WSI_WIN32_RUNTIME_MAX_SYNCS 16u

struct wsi_win32_runtime_fence {
   void *token;
   uint32_t handle;
   const volatile uint64_t *value;
};

struct wsi_win32_runtime {
   uint32_t version;
   void *context;
   void *device_cookie;
   VkResult (*create_context)(void *, uint32_t types, void **token);
   VkResult (*destroy_context)(void *, void *token);
   VkResult (*submit)(void *, void *token, uint64_t address, const void *words,
                      uint32_t bytes, uint32_t count, const uint32_t *written);
   VkResult (*gpu_sync)(void *, void *token, bool signal, uint32_t count,
                        const uint32_t *handles, const uint64_t *values);
   VkResult (*create_fence)(void *, uint64_t initial, struct wsi_win32_runtime_fence *);
   VkResult (*retain_fence)(void *, void *token, struct wsi_win32_runtime_fence *);
   void (*release_fence)(void *, void *token);
   VkResult (*wait_cpu)(void *, void *token, uint64_t value, uint32_t timeout_ms);
   VkResult (*signal_cpu)(void *, void *token, uint64_t value);
   /* Stop admitting work and destroy every submission context. Success
    * proves queued work is finished; tokens remain valid for destruction.
    * Failure requires retaining all potentially in-flight GPU storage. */
   VkResult (*quiesce)(void *);
};

#endif
