/* SPDX-License-Identifier: MIT
 * Optional Windows telemetry for an actually created Mesa device/screen.
 * Kind 1 is native Vulkan, 2 is Zink, 3 is the tgd3d D3D10/11 frontend.
 * A local read-only observer can distinguish these without DLL injection
 * or inferring Zink from the presence of a multi-driver opengl32.dll.
 * No rendering, synchronization, clock or device policy depends on this marker.
 */
#ifndef U_GRAPHICS_API_MARKER_H
#define U_GRAPHICS_API_MARKER_H
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
struct util_graphics_api_marker { HANDLE mapping; volatile LONG *count; };
static inline void util_graphics_api_marker_open(struct util_graphics_api_marker *m, unsigned kind)
{
   char name[96];
   snprintf(name, sizeof(name), "Local\\Mesa.GraphicsApi.v1.%lu.%u", (unsigned long)GetCurrentProcessId(), kind);
   m->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LONG), name);
   if (!m->mapping) return;
   m->count = (volatile LONG *)MapViewOfFile(m->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LONG));
   if (!m->count) { CloseHandle(m->mapping); m->mapping = NULL; return; }
   InterlockedIncrement(m->count);
}
static inline void util_graphics_api_marker_close(struct util_graphics_api_marker *m)
{
   if (m->count) { InterlockedDecrement(m->count); UnmapViewOfFile((const void *)m->count); m->count = NULL; }
   if (m->mapping) { CloseHandle(m->mapping); m->mapping = NULL; }
}
#else
struct util_graphics_api_marker { unsigned unused; };
static inline void util_graphics_api_marker_open(struct util_graphics_api_marker *m, unsigned kind)
{ (void)m; (void)kind; }
static inline void util_graphics_api_marker_close(struct util_graphics_api_marker *m) { (void)m; }
#endif
#endif
