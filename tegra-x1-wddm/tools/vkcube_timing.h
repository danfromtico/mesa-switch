/* SPDX-License-Identifier: MIT
 * Optional instrumentation for the upstream Win32 cube. Records API-return
 * cadence, not guaranteed displayed frames. No Vulkan calls are replaced.
 */
#ifndef VKCUBE_TIMING_H
#define VKCUBE_TIMING_H
#include <share.h>

static FILE *cube_timing_file;
static uint64_t cube_timing_frequency, cube_timing_base, cube_timing_frames;
static char cube_timing_buffer[65536];

static uint64_t cube_timing_now(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static void cube_timing_close(void)
{
    if (cube_timing_file) {
        fprintf(cube_timing_file, "exit,%llu,0,0,0,0\n",
                (unsigned long long)cube_timing_now());
        fclose(cube_timing_file);
        cube_timing_file = NULL;
    }
}

static void cube_timing_open(void)
{
    if (cube_timing_file)
        return;
    const char *path = getenv("VKCUBE_TIMING_FILE");
    if (!path || !*path)
        path = "vkcube-timing.csv";
    cube_timing_file = _fsopen(path, "wb", _SH_DENYNO);
    if (!cube_timing_file) {
        fprintf(stderr, "Cannot open cube timing file: %s\n", path);
        exit(EXIT_FAILURE);
    }
    setvbuf(cube_timing_file, cube_timing_buffer, _IOFBF,
            sizeof(cube_timing_buffer));
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    cube_timing_frequency = (uint64_t)frequency.QuadPart;
    fprintf(cube_timing_file, "kind,qpc,begin,result,a,b\nfrequency,%llu,0,0,0,0\n",
            (unsigned long long)cube_timing_frequency);
    fflush(cube_timing_file);
    atexit(cube_timing_close);
}

static void cube_timing_event(const char *kind, uint64_t begin, VkResult result,
                              uint32_t a, uint32_t b)
{
    cube_timing_open();
    fprintf(cube_timing_file, "%s,%llu,%llu,%d,%u,%u\n", kind,
            (unsigned long long)cube_timing_now(), (unsigned long long)begin,
            result, a, b);
    if (strcmp(kind, "present") || result < 0)
        fflush(cube_timing_file);
}

static void cube_timing_present(HWND window, uint64_t begin, VkResult result)
{
    cube_timing_event("present", begin, result, 0, 0);
    const uint64_t now = cube_timing_now();
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        if (!cube_timing_base)
            cube_timing_base = now;
        else
            cube_timing_frames++;
    }
    if (cube_timing_base && now - cube_timing_base >= cube_timing_frequency) {
        const double fps = (double)cube_timing_frames * cube_timing_frequency /
                           (double)(now - cube_timing_base);
        char title[96];
        snprintf(title, sizeof(title), "Vulkan Cube - %.1f FPS (Present)", fps);
        SetWindowTextA(window, title);
        fflush(cube_timing_file);
        cube_timing_base = now;
        cube_timing_frames = 0;
    }
}
#endif
