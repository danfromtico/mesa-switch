/*
 * Copyright 2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef ZINK_PUBLIC_H
#define ZINK_PUBLIC_H

#include <stdint.h>

struct pipe_screen;
struct renderonly;
struct sw_winsys;
struct pipe_screen_config;

struct pipe_screen *
zink_create_screen(struct sw_winsys *winsys, const struct pipe_screen_config *config);

struct pipe_screen *
zink_drm_create_screen(int fd, const struct pipe_screen_config *config, struct renderonly *ro);
struct pipe_screen *
zink_win32_create_screen(uint64_t adapter_luid);

/* Optional creation failure detail for frontends that have no visible stderr.
 * result is a Win32 error for loader stages, VkResult for Vulkan calls, and
 * zero when the stage only has a boolean failure. line identifies the call
 * site in the matching source revision. This is not an ICD or kernel ABI.
 */
enum zink_screen_create_stage {
   ZINK_SCREEN_CREATE_OK,
   ZINK_SCREEN_CREATE_ALLOCATE,
   ZINK_SCREEN_CREATE_LOADER,
   ZINK_SCREEN_CREATE_ENTRYPOINTS,
   ZINK_SCREEN_CREATE_INSTANCE,
   ZINK_SCREEN_CREATE_ENUMERATE_DEVICES,
   ZINK_SCREEN_CREATE_MATCH_DEVICE,
   ZINK_SCREEN_CREATE_FEATURES,
   ZINK_SCREEN_CREATE_DEVICE,
   ZINK_SCREEN_CREATE_INITIALIZE,
};

enum zink_instance_call {
   ZINK_INSTANCE_VERSION,
   ZINK_INSTANCE_EXTENSION_COUNT,
   ZINK_INSTANCE_EXTENSIONS,
   ZINK_INSTANCE_LAYER_COUNT,
   ZINK_INSTANCE_LAYERS,
   ZINK_INSTANCE_CREATE,
   ZINK_INSTANCE_CALL_COUNT,
};

enum zink_instance_diagnostic_flags {
   ZINK_INSTANCE_DEBUG_AVAILABLE = 1 << 0,
   ZINK_INSTANCE_DEBUG_CHAINED = 1 << 1,
   ZINK_INSTANCE_CACHED = 1 << 2,
   ZINK_INSTANCE_EXTENSION_ALLOC_FAILED = 1 << 3,
   ZINK_INSTANCE_LAYER_ALLOC_FAILED = 1 << 4,
};

struct zink_instance_diagnostics {
   uint32_t called_mask, flags, enabled_extensions, enabled_layers;
   struct {
      int32_t result;
      uint32_t value; /* Version/count only when the API returned non-error. */
   } calls[ZINK_INSTANCE_CALL_COUNT];
};

static inline void
zink_record_instance_call(struct zink_instance_diagnostics *info,
                          enum zink_instance_call call, int32_t result, uint32_t value)
{
   info->called_mask |= 1u << call;
   info->calls[call].result = result;
   info->calls[call].value = result >= 0 ? value : 0;
}

struct zink_screen_create_error {
   enum zink_screen_create_stage stage;
   int32_t result;
   uint32_t detail, line;
   char message[1024]; /* First instance debug-utils error, when available. */
   /* Valid on success too; a cached snapshot is not a new set of API calls. */
   struct zink_instance_diagnostics instance;
};

struct pipe_screen *
zink_win32_create_screen_with_error(uint64_t adapter_luid,
                                   struct zink_screen_create_error *error);
#endif
