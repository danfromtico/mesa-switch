/* SPDX-License-Identifier: MIT */
#ifndef WSI_WIN32_PACING_H
#define WSI_WIN32_PACING_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline void
wsi_win32_get_surface_image_count_bounds(bool sw,
                                         bool has_d3d12_queue,
                                         uint32_t *min_images,
                                         uint32_t *max_images)
{
   if (!sw && has_d3d12_queue) {
      /* DXGI requires in-order presentation, so cap at double buffering. */
      *min_images = 2;
      *max_images = 2;
   } else if (!sw) {
      /* Hardware GDI/CPU presentation allocates a device-local image, a
       * linear host-visible blit buffer, and a 32-bpp DIB per swapchain
       * slot. Require at least double buffering and cap in-flight images.
       */
      *min_images = 2;
      *max_images = 4;
   } else {
      *min_images = 1;
      *max_images = 0;
   }
}

/* CPU presentation has no swapchain wait object. Use a private one-shot
 * timer, without changing the system timer resolution or polling the CPU.
 * A late frame resets the deadline instead of accumulating catch-up frames.
 *
 * The deadline is kept phase-locked to the compositor. Every present
 * measures its own DwmFlush() wait; a present that blocked in the
 * compositor and completed one display period after the previous one is
 * display-locked, and the timer stays idle for it. The timer only caps the
 * producer when the compositor returns immediately (occluded or idle
 * desktop) or the desktop refreshes faster than the cap, so the timer wait
 * and the VBlank wait never chain into a fixed double period and queueing
 * stays bounded either way. */
struct wsi_win32_pacer {
   HANDLE timer;
   int64_t frequency;
   int64_t period;
   int64_t next;
   /* QPC of the previous present's compositor completion, 0 if none. */
   int64_t last_present;
};

static inline BOOL
wsi_win32_pacer_init(struct wsi_win32_pacer *pacer, uint32_t fps)
{
   memset(pacer, 0, sizeof(*pacer));
   if (!fps)
      return TRUE;
   LARGE_INTEGER frequency;
   if (!QueryPerformanceFrequency(&frequency))
      return FALSE;
   pacer->frequency = frequency.QuadPart;
   pacer->period = (frequency.QuadPart + fps - 1) / fps;
   pacer->timer = CreateWaitableTimerExW(NULL, NULL,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
   return pacer->timer != NULL;
}

/* Remember when this present completed in the compositor. */
static inline void
wsi_win32_pacer_presented(struct wsi_win32_pacer *pacer, int64_t completed_at)
{
   pacer->last_present = completed_at;
}

/* Anchor the fallback deadline to a completed composition so the timer
 * stays phase-locked to the display cadence instead of free-running. */
static inline void
wsi_win32_pacer_sync_compositor(struct wsi_win32_pacer *pacer,
                                int64_t composed_at)
{
   pacer->next = composed_at + pacer->period;
}

/* Classify a measured DwmFlush() wait. The compositor must have blocked at
 * all (an occluded or idle desktop returns within microseconds), a previous
 * present must exist to measure the cadence against, and the completed
 * frame must sit a full display period after the previous one, so on a
 * desktop that refreshes faster than the cap the timer still limits the
 * producer instead of the fast compositor cadence. */
static inline bool
wsi_win32_pacer_compositor_paced(const struct wsi_win32_pacer *pacer,
                                 int64_t dwm_elapsed, int64_t completed_at)
{
   return pacer->timer != NULL && pacer->period > 1 &&
          pacer->last_present > 0 &&
          dwm_elapsed >= pacer->period / 16 &&
          completed_at - pacer->last_present >=
             pacer->period - pacer->period / 8;
}

static inline BOOL
wsi_win32_pacer_wait(struct wsi_win32_pacer *pacer)
{
   if (!pacer->timer)
      return TRUE;
   LARGE_INTEGER now;
   if (!QueryPerformanceCounter(&now))
      return FALSE;
   while (now.QuadPart < pacer->next) {
      LARGE_INTEGER due;
      due.QuadPart = -((pacer->next - now.QuadPart) * INT64_C(10000000) +
                      pacer->frequency - 1) / pacer->frequency;
      if (!SetWaitableTimer(pacer->timer, &due, 0, NULL, NULL, FALSE) ||
          WaitForSingleObject(pacer->timer, INFINITE) != WAIT_OBJECT_0 ||
          !QueryPerformanceCounter(&now))
         return FALSE;
   }
   pacer->next = now.QuadPart + pacer->period;
   return TRUE;
}

static inline void
wsi_win32_pacer_finish(struct wsi_win32_pacer *pacer)
{
   if (pacer->timer)
      CloseHandle(pacer->timer);
   pacer->timer = NULL;
}

#endif
