/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <inttypes.h>
#include <vulkan/vulkan_core.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "triangle-shaders.h"

enum {
    WIDTH = 32, HEIGHT = 32, GUARD = 64,
    PIXELS = WIDTH * HEIGHT, IMAGE_BYTES = PIXELS * 4,
    BUFFER_BYTES = GUARD + IMAGE_BYTES + GUARD,
};

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return UINT32_MAX;
}

static VkPhysicalDevice pick_device(VkInstance instance, uint64_t luid)
{
    uint32_t count = 0;
    VkPhysicalDevice devices[16];
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS ||
        !count || count > sizeof(devices) / sizeof(*devices))
        return VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceIDProperties identity = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceDriverProperties driver = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
            .pNext = &identity,
        };
        VkPhysicalDeviceProperties2 properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver,
        };
        vkGetPhysicalDeviceProperties2(devices[i], &properties);
        uint64_t actual = 0;
        memcpy(&actual, identity.deviceLUID, sizeof(actual));
        if (driver.driverID == VK_DRIVER_ID_MESA_NVK &&
            properties.properties.vendorID == 0x10de &&
            identity.deviceLUIDValid && actual == luid)
            return devices[i];
    }
    return VK_NULL_HANDLE;
}

static uint32_t graphics_family(VkPhysicalDevice physical)
{
    VkQueueFamilyProperties queues[16];
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count || count > sizeof(queues) / sizeof(*queues))
        return UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
    for (uint32_t i = 0; i < count; i++)
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            return i;
    return UINT32_MAX;
}

static int check_pixels(const uint8_t *mapped)
{
    const uint8_t red[] = {255, 0, 0, 255};
    const uint8_t blue[] = {0, 0, 255, 255};
    for (unsigned i = 0; i < GUARD; i++) {
        if (mapped[i] != 0xa5 || mapped[GUARD + IMAGE_BYTES + i] != 0xa5) {
            fprintf(stderr, "Copy guard changed at %u\n", i);
            return 1;
        }
    }
    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            const uint8_t *pixel = mapped + GUARD + 4 * (y * WIDTH + x);
            const uint8_t *expected = x >= 4 && y >= 4 && x + y <= 31 ? red : blue;
            if (memcmp(pixel, expected, 4)) {
                fprintf(stderr, "Pixel %u,%u: %02x %02x %02x %02x\n",
                        x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
                return 1;
            }
        }
    }
    return 0;
}

#define CHECK(operation) do { \
    VkResult result = (operation); \
    if (result != VK_SUCCESS) { \
        fprintf(stderr, "%s returned %d\n", #operation, result); \
        goto done; \
    } \
} while (0)

static uint32_t preview_bgra[PIXELS];
static bool preview_painted;

static LRESULT CALLBACK preview_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        const BITMAPINFO info = {.bmiHeader = {
            .biSize = sizeof(BITMAPINFOHEADER), .biWidth = WIDTH, .biHeight = -HEIGHT,
            .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB,
        }};
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT area;
        GetClientRect(window, &area);
        const int side = area.right < area.bottom ? area.right : area.bottom;
        const int rows = StretchDIBits(dc, (area.right - side) / 2, (area.bottom - side) / 2,
                                      side, side, 0, 0, WIDTH, HEIGHT, preview_bgra,
                                      &info, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(window, &paint);
        preview_painted |= rows > 0;
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

static int show_preview(void)
{
    const WNDCLASSW cls = {
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = preview_window, .hInstance = GetModuleHandleW(NULL),
        .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
        .lpszClassName = L"SwitchLiteNvkReadback",
    };
    if (!RegisterClassW(&cls))
        return 1;
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Switch Lite - NVK Vulkan on GM20B",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  530, 540, NULL, NULL, cls.hInstance, NULL);
    if (!window)
        return 1;
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    if (!preview_painted || !SetTimer(window, 1, 5 * 60 * 1000, NULL)) {
        DestroyWindow(window);
        return 1;
    }
    puts("{\"win32_readback_preview\":true}");
    fflush(stdout);
    MSG message;
    BOOL next;
    while ((next = GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return next == -1;
}

enum { PERF_SUBMISSIONS = 24 };

static uint64_t ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

static uint64_t cpu_clock(void)
{
    FILETIME creation, exit_time, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
        return 0;
    return ((uint64_t)kernel.dwHighDateTime << 32) + kernel.dwLowDateTime +
           ((uint64_t)user.dwHighDateTime << 32) + user.dwLowDateTime;
}

int main(int argc, char **argv)
{
    const bool preview = argc == 4 && strcmp(argv[3], "--preview") == 0;
    const bool timings = argc == 4 && strcmp(argv[3], "--timings") == 0;
    const bool directory_kind = argc == 4 && strcmp(argv[3], "--directory-kind") == 0;
    if (argc != 3 && !preview && !timings && !directory_kind) {
        fprintf(stderr, "usage: vk-triangle.exe <absolute-NVK-DLL> <hex-KMT-LUID> [--preview | --timings | --directory-kind]\n");
        return 2;
    }
    if (strlen(argv[2]) != 16 || strspn(argv[2], "0123456789abcdefABCDEF") != 16)
        return 2;
    const uint64_t luid = strtoull(argv[2], NULL, 16);
    if (!luid)
        return 2;
    LARGE_INTEGER frequency = {0};
    if (timings && !QueryPerformanceFrequency(&frequency))
        return 2;
    const uint64_t start = timings ? ticks() : 0;
    const uint64_t cpu_start = timings ? cpu_clock() : 0;
    /* Stage boundaries are load, instance, adapter selection, device, image,
     * staging, pipeline, recording, submission batch and destruction. */
    uint64_t stage_ticks[10] = {0};
    uint64_t submit_ticks[PERF_SUBMISSIONS] = {0};
    uint64_t wait_ticks[PERF_SUBMISSIONS] = {0};
    uint64_t read_ticks[PERF_SUBMISSIONS] = {0};
    uint64_t pipeline_start = 0, pipeline_cpu = 0, pipeline_only = 0;
    HMODULE icd = LoadLibraryExA(argv[1], NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!icd) {
        fprintf(stderr, "Loading NVK failed: %lu\n", GetLastError());
        return 2;
    }

    if (timings) stage_ticks[0] = ticks();
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory image_mem = VK_NULL_HANDLE, buffer_mem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *mapped = NULL;
    int exit_code = 1;

    const VkDirectDriverLoadingInfoLUNARG direct = {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG,
        .pfnGetInstanceProcAddr = (PFN_vkGetInstanceProcAddrLUNARG)
            (void *)GetProcAddress(icd, "vk_icdGetInstanceProcAddr"),
    };
    if (!direct.pfnGetInstanceProcAddr)
        goto done;
    const VkDirectDriverLoadingListLUNARG drivers = {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
        .mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG,
        .driverCount = 1, .pDrivers = &direct,
    };
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Switch Lite NVK offscreen triangle",
        .apiVersion = VK_API_VERSION_1_2,
    };
    const char *extension = VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME;
    const VkInstanceCreateInfo create_instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = &drivers, .pApplicationInfo = &app,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension,
    };
    CHECK(vkCreateInstance(&create_instance, NULL, &instance));
    if (timings) stage_ticks[1] = ticks();
    VkPhysicalDevice physical = pick_device(instance, luid);
    if (!physical) {
        fprintf(stderr, "NVK did not expose the requested KMT LUID %016llx\n",
                (unsigned long long)luid);
        exit_code = 3;
        goto done;
    }

    uint32_t family = graphics_family(physical);
    if (family == UINT32_MAX) {
        fprintf(stderr, "No graphics queue for GM20B\n");
        goto done;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo create_queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo create_device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &create_queue,
    };
    if (timings) stage_ticks[2] = ticks();
    CHECK(vkCreateDevice(physical, &create_device, NULL, &device));
    if (timings) stage_ticks[3] = ticks();
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    const VkImageCreateInfo create_image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        /* More than 64 MiB of optimal image backing spans a GM20B leaf
         * table. Keep the rendered/read-back region at the original 32x32. */
        .extent = {directory_kind ? 4096 : WIDTH, directory_kind ? 4097 : HEIGHT, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    CHECK(vkCreateImage(device, &create_image, NULL, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    const VkDeviceSize image_allocation_bytes = requirements.size;
    if (directory_kind && image_allocation_bytes <= UINT64_C(64) * 1024 * 1024) {
        fprintf(stderr, "Directory-kind control needs more than one leaf table of image backing\n");
        goto done;
    }
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    if (allocate.memoryTypeIndex == UINT32_MAX)
        goto done;
    CHECK(vkAllocateMemory(device, &allocate, NULL, &image_mem));
    CHECK(vkBindImageMemory(device, image, image_mem, 0));
    if (timings) stage_ticks[4] = ticks();

    const VkImageViewCreateInfo create_view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = create_image.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    CHECK(vkCreateImageView(device, &create_view, NULL, &view));
    const VkBufferCreateInfo create_buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUFFER_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    CHECK(vkCreateBuffer(device, &create_buffer, NULL, &buffer));
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX)
        goto done;
    CHECK(vkAllocateMemory(device, &allocate, NULL, &buffer_mem));
    CHECK(vkBindBufferMemory(device, buffer, buffer_mem, 0));
    CHECK(vkMapMemory(device, buffer_mem, 0, VK_WHOLE_SIZE, 0, &mapped));
    memset(mapped, 0xa5, BUFFER_BYTES);
    const VkMappedMemoryRange staging_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = buffer_mem, .size = VK_WHOLE_SIZE,
    };
    CHECK(vkFlushMappedMemoryRanges(device, 1, &staging_range));
    if (timings) stage_ticks[5] = ticks();

    const VkAttachmentDescription color_attachment = {
        .format = create_image.format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    const VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color_reference,
    };
    const VkSubpassDependency after_draw = {
        .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    const VkRenderPassCreateInfo create_render_pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &color_attachment,
        .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 1, .pDependencies = &after_draw,
    };
    CHECK(vkCreateRenderPass(device, &create_render_pass, NULL, &render_pass));
    const VkFramebufferCreateInfo create_framebuffer = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass, .attachmentCount = 1, .pAttachments = &view,
        .width = WIDTH, .height = HEIGHT, .layers = 1,
    };
    CHECK(vkCreateFramebuffer(device, &create_framebuffer, NULL, &framebuffer));
    const VkPipelineLayoutCreateInfo create_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    CHECK(vkCreatePipelineLayout(device, &create_layout, NULL, &layout));
    const VkShaderModuleCreateInfo create_vert = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(vert_spirv), .pCode = vert_spirv,
    };
    const VkShaderModuleCreateInfo create_frag = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(frag_spirv), .pCode = frag_spirv,
    };
    CHECK(vkCreateShaderModule(device, &create_vert, NULL, &vert));
    CHECK(vkCreateShaderModule(device, &create_frag, NULL, &frag));
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"},
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkViewport viewport = {0, 0, WIDTH, HEIGHT, 0, 1};
    const VkRect2D area = {{0, 0}, {WIDTH, HEIGHT}};
    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &area,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState rgba = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &rgba,
    };
    const VkGraphicsPipelineCreateInfo create_pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = layout, .renderPass = render_pass,
    };
    if (timings) {
        pipeline_cpu = cpu_clock();
        pipeline_start = ticks();
    }
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create_pipeline, NULL, &pipeline));
    if (timings) {
        stage_ticks[6] = ticks();
        pipeline_only = stage_ticks[6] - pipeline_start;
        pipeline_cpu = cpu_clock() - pipeline_cpu;
    }

    const VkCommandPoolCreateInfo create_pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
    };
    CHECK(vkCreateCommandPool(device, &create_pool, NULL, &pool));
    VkCommandBuffer commands;
    const VkCommandBufferAllocateInfo allocate_commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    CHECK(vkAllocateCommandBuffers(device, &allocate_commands, &commands));
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = timings ? 0 : VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(commands, &begin));
    const VkClearValue clear = {.color.float32 = {0, 0, 1, 1}};
    const VkRenderPassBeginInfo render = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass, .framebuffer = framebuffer, .renderArea = area,
        .clearValueCount = 1, .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(commands, &render, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    const VkBufferImageCopy copy = {
        .bufferOffset = GUARD, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1},
    };
    vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    const VkBufferMemoryBarrier readback = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .offset = GUARD, .size = IMAGE_BYTES,
    };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &readback, 0, NULL);
    CHECK(vkEndCommandBuffer(commands));
    const VkFenceCreateInfo create_fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &create_fence, NULL, &fence));
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands,
    };
    if (timings) stage_ticks[7] = ticks();
    const unsigned submissions = timings ? PERF_SUBMISSIONS : 1;
    for (unsigned sample = 0; sample < submissions; sample++) {
        if (sample) {
            CHECK(vkResetFences(device, 1, &fence));
            memset((uint8_t *)mapped + GUARD, 0x5a, IMAGE_BYTES);
            CHECK(vkFlushMappedMemoryRanges(device, 1, &staging_range));
        }
        const uint64_t queued = timings ? ticks() : 0;
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        const uint64_t submitted = timings ? ticks() : 0;
        VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000));
        const uint64_t completed = timings ? ticks() : 0;
        if (waited != VK_SUCCESS) {
            fprintf(stderr, "GPU fence wait returned %d\n", waited);
            return 4;
        }
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &staging_range));
        if (check_pixels(mapped))
            goto done;
        if (timings) {
            submit_ticks[sample] = submitted - queued;
            wait_ticks[sample] = completed - submitted;
            read_ticks[sample] = ticks() - completed;
        }
    }
    if (timings) stage_ticks[8] = ticks();
    exit_code = 0;
    printf("{\"vulkan_draw\":true,\"luid\":\"%016llx\",\"pixels\":1024,"
           "\"red\":300,\"blue\":724,\"guard_bytes\":128,"
           "\"directory_kind\":%s,\"image_allocation_bytes\":%" PRIu64 "}\n",
           (unsigned long long)luid, directory_kind ? "true" : "false",
           (uint64_t)image_allocation_bytes);
    if (preview) {
        const uint8_t *rgba = (const uint8_t *)mapped + GUARD;
        for (unsigned i = 0; i < PIXELS; i++, rgba += 4)
            preview_bgra[i] = (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
    }

done:
    if (device) {
        if (mapped)
            vkUnmapMemory(device, buffer_mem);
        vkDestroyFence(device, fence, NULL);
        vkDestroyCommandPool(device, pool, NULL);
        vkDestroyPipeline(device, pipeline, NULL);
        vkDestroyShaderModule(device, frag, NULL);
        vkDestroyShaderModule(device, vert, NULL);
        vkDestroyPipelineLayout(device, layout, NULL);
        vkDestroyFramebuffer(device, framebuffer, NULL);
        vkDestroyRenderPass(device, render_pass, NULL);
        vkDestroyBuffer(device, buffer, NULL);
        vkFreeMemory(device, buffer_mem, NULL);
        vkDestroyImageView(device, view, NULL);
        vkDestroyImage(device, image, NULL);
        vkFreeMemory(device, image_mem, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance)
        vkDestroyInstance(instance, NULL);
    FreeLibrary(icd);
    if (timings) stage_ticks[9] = ticks();
    if (timings && !exit_code) {
        PROCESS_MEMORY_COUNTERS memory = {0};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
            return 5;
        printf("MEASURE %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
               (uint64_t)frequency.QuadPart, cpu_clock() - cpu_start, (uint64_t)memory.PeakWorkingSetSize,
               pipeline_only, pipeline_cpu);
        uint64_t previous = start;
        for (unsigned i = 0; i < 10; i++) {
            printf("STAGE %u %" PRIu64 "\n", i, stage_ticks[i] - previous);
            previous = stage_ticks[i];
        }
        for (unsigned i = 0; i < PERF_SUBMISSIONS; i++)
            printf("SUBMIT %u %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                   i, submit_ticks[i], wait_ticks[i], read_ticks[i]);
    }
    if (!exit_code && preview && show_preview()) {
        fprintf(stderr, "Could not display the verified readback\n");
        return 1;
    }
    return exit_code;
}
