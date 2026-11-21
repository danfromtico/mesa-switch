/* SPDX-License-Identifier: MIT */
/* Real Windows timer/CPU-time control; this does not execute GPU work. */
#include "../wsi_win32_pacing.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint64_t file_ticks(FILETIME time)
{
   return ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

static uint64_t cpu_ticks(void)
{
   FILETIME created, exited, kernel, user;
   CHECK(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user));
   return file_ticks(kernel) + file_ticks(user);
}

int main(void)
{
   uint32_t min_images = 0, max_images = 0;
   wsi_win32_get_surface_image_count_bounds(false, false, &min_images, &max_images);
   CHECK(min_images == 2 && max_images == 4);
   wsi_win32_get_surface_image_count_bounds(false, true, &min_images, &max_images);
   CHECK(min_images == 2 && max_images == 2);
   wsi_win32_get_surface_image_count_bounds(true, false, &min_images, &max_images);
   CHECK(min_images == 1 && max_images == 0);

   struct wsi_win32_pacer pacer;
   CHECK(wsi_win32_pacer_init(&pacer, 0) && !pacer.timer);
   CHECK(wsi_win32_pacer_wait(&pacer, false));
   CHECK(wsi_win32_pacer_wait(&pacer, true));
   wsi_win32_pacer_finish(&pacer);

   CHECK(wsi_win32_pacer_init(&pacer, 60));
   LARGE_INTEGER start, end;
   CHECK(QueryPerformanceCounter(&start));
   const uint64_t cpu_start = cpu_ticks();
   for (unsigned i = 0; i < 60; ++i)
      CHECK(wsi_win32_pacer_wait(&pacer, false));
   CHECK(QueryPerformanceCounter(&end));
   const uint64_t cpu_elapsed = cpu_ticks() - cpu_start;
   CHECK(end.QuadPart - start.QuadPart >= 59 * pacer.period);
   const double wall_seconds = (double)(end.QuadPart - start.QuadPart) / pacer.frequency;
   CHECK(cpu_elapsed / 10000000.0 < wall_seconds / 2.0 + 0.02);

   /* A compositor-paced frame neither waits nor moves the deadline: the
    * VBlank wait already provided the full backpressure for that frame. */
   LARGE_INTEGER composed;
   CHECK(QueryPerformanceCounter(&composed));
   wsi_win32_pacer_sync_compositor(&pacer, composed.QuadPart);
   CHECK(pacer.next == composed.QuadPart + pacer.period);
   CHECK(QueryPerformanceCounter(&start));
   CHECK(wsi_win32_pacer_wait(&pacer, true));
   CHECK(QueryPerformanceCounter(&end));
   CHECK(end.QuadPart - start.QuadPart < pacer.period / 4);
   CHECK(pacer.next == composed.QuadPart + pacer.period);

   /* The fallback timer stays phase-locked to the compositor: after a paced
    * frame it waits out only the remainder of the display period instead of
    * stacking a full timer period on top of the VBlank wait. */
   CHECK(wsi_win32_pacer_wait(&pacer, false));
   CHECK(QueryPerformanceCounter(&end));
   CHECK(end.QuadPart >= composed.QuadPart + pacer.period);
   CHECK(end.QuadPart < composed.QuadPart + 2 * pacer.period);

   /* Classification boundary: blocking at least half a period in the
    * compositor counts as paced; anything shorter leaves the timer armed. */
   CHECK(wsi_win32_pacer_compositor_paced(&pacer, pacer.period));
   CHECK(wsi_win32_pacer_compositor_paced(&pacer, pacer.period / 2));
   CHECK(!wsi_win32_pacer_compositor_paced(&pacer, pacer.period / 2 - 1));
   CHECK(!wsi_win32_pacer_compositor_paced(&pacer, 0));

   /* A late producer gets one slot, never a burst of accumulated slots. */
   Sleep(50);
   CHECK(wsi_win32_pacer_wait(&pacer, false));
   const int64_t first_slot = pacer.next;
   CHECK(wsi_win32_pacer_wait(&pacer, false));
   CHECK(pacer.next >= first_slot + pacer.period);

   CHECK(CloseHandle(pacer.timer));
   CHECK(QueryPerformanceCounter(&end));
   pacer.next = end.QuadPart + pacer.frequency;
   CHECK(!wsi_win32_pacer_wait(&pacer, false)); /* Timer failure is not success. */
   CHECK(wsi_win32_pacer_wait(&pacer, true));   /* A paced frame cannot fail. */
   pacer.timer = NULL;
   wsi_win32_pacer_finish(&pacer);
   printf("PASS: 60-Hz cap, blocking wait, late-frame pacing, compositor phase-lock, "
          "and timer errors; wall=%.6fs CPU=%.6fs (Windows timer control, no GPU execution)\n",
          wall_seconds, cpu_elapsed / 10000000.0);
   return 0;
}
