/* SPDX-License-Identifier: MIT */
/* Real Windows timer/CPU-time control; this does not execute GPU work. */
#include "../wsi_win32_pacing.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

/* Drive whole present beats against a virtual compositor and verify the
 * producer never waits more than the remaining deadline after a successful
 * DwmFlush(). work[] is the virtual GPU/GDI time before DwmFlush; the
 * compositor completes on the vblank grid (or immediately, when idle[]
 * simulates an occluded/idle desktop). The release advance mirrors
 * wsi_win32_pacer_wait()'s exit semantics — sleep only until the deadline,
 * never a full extra period — which the real-time loops in main()
 * exercise against the live timer. */
static void check_beats(const char *name, int64_t vblank,
                        const int64_t *work, const bool *idle,
                        const bool *expect_paced, unsigned n)
{
   struct wsi_win32_pacer sim;
   CHECK(wsi_win32_pacer_init(&sim, 60));
   const int64_t period = sim.period;
   /* Start on a virtual vblank so every scenario is deterministic. */
   int64_t t = (INT64_C(1000000000) / period + 1) * period;
   int64_t prev_release = 0;
   for (unsigned i = 0; i < n; i++) {
      t += work[i];
      int64_t completed = t;
      if (!idle[i]) {
         /* DwmFlush() blocks until the next strictly later grid point. */
         completed = (t / vblank) * vblank;
         if (completed <= t)
            completed += vblank;
      }
      const int64_t dwm_elapsed = completed - t;
      const bool resumed =
         wsi_win32_pacer_compositor_resumed(&sim, dwm_elapsed);
      const bool paced =
         wsi_win32_pacer_compositor_paced(&sim, dwm_elapsed, completed);
      CHECK(paced == expect_paced[i]);
      wsi_win32_pacer_presented(&sim, dwm_elapsed, completed);
      int64_t release = completed;
      if (paced) {
         /* Display-locked: re-anchor the deadline to this completion, as
          * queue_present() does, so the remaining wait is exactly zero
          * and the skip below is equivalent to waiting it. */
         wsi_win32_pacer_sync_compositor(&sim, completed);
         CHECK(sim.next == completed + period);
      } else if (!resumed) {
         /* Producer cap: wait out only the time still owed to the
          * deadline (mirrors wsi_win32_pacer_wait()). */
         if (sim.next > release)
            release = sim.next;
         sim.next = release + period;
         sim.last_present = release;
      }
      /* The cap never holds the producer past the deadline for a full
       * extra period, and a paced or resumed frame is released at the
       * composition it already waited for. */
      CHECK(release - completed < period);
      if (paced || resumed)
         CHECK(release == completed);
      if (i > 0)
         CHECK(release > prev_release);
      prev_release = release;
      t = release;
   }
   printf("%s: %u beats, deadline never exceeded by a period\n", name, n);
   wsi_win32_pacer_finish(&sim);
}

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
   CHECK(wsi_win32_pacer_wait(&pacer));
   /* Without a timer nothing is ever classified as compositor-paced. */
   CHECK(!wsi_win32_pacer_compositor_paced(&pacer, 1000, 1000));
   wsi_win32_pacer_finish(&pacer);

   CHECK(wsi_win32_pacer_init(&pacer, 60));
   LARGE_INTEGER start, end;
   CHECK(QueryPerformanceCounter(&start));
   const uint64_t cpu_start = cpu_ticks();
   for (unsigned i = 0; i < 60; ++i)
      CHECK(wsi_win32_pacer_wait(&pacer));
   CHECK(QueryPerformanceCounter(&end));
   const uint64_t cpu_elapsed = cpu_ticks() - cpu_start;
   CHECK(end.QuadPart - start.QuadPart >= 59 * pacer.period);
   const double wall_seconds = (double)(end.QuadPart - start.QuadPart) / pacer.frequency;
   CHECK(cpu_elapsed / 10000000.0 < wall_seconds / 2.0 + 0.02);

   /* Compositor classification. A present is display-locked when DwmFlush()
    * blocked at all and it completed a full display period after the
    * previous present was released; this holds no matter how much of the
    * period the GPU/GDI work consumed before DwmFlush() ran. */
   LARGE_INTEGER now;
   CHECK(QueryPerformanceCounter(&now));
   const int64_t period = pacer.period;
   const int64_t min_block = wsi_win32_pacer_min_block(&pacer);
   wsi_win32_pacer_presented(&pacer, period, now.QuadPart - period);
   CHECK(wsi_win32_pacer_compositor_paced(&pacer, period, now.QuadPart));
   CHECK(wsi_win32_pacer_compositor_paced(&pacer, period / 2, now.QuadPart));
   CHECK(wsi_win32_pacer_compositor_paced(&pacer, min_block, now.QuadPart));
   /* An occluded or idle desktop returns from DwmFlush() within
    * microseconds, so the producer cap must stay armed. */
   CHECK(!wsi_win32_pacer_compositor_paced(&pacer, min_block - 1, now.QuadPart));
   CHECK(!wsi_win32_pacer_compositor_paced(&pacer, 0, now.QuadPart));
   /* A desktop refreshing twice as fast as the cap completes presents only
    * half a period after the previous release; the timer keeps limiting
    * the producer. */
   CHECK(!wsi_win32_pacer_compositor_paced(&pacer, period, now.QuadPart - period / 2));
   /* Interval boundary: just short of a full period releases the producer,
    * a full period locks to the display. */
   CHECK(!wsi_win32_pacer_compositor_paced(&pacer, period, now.QuadPart - period / 8 - 1));
   CHECK(wsi_win32_pacer_compositor_paced(&pacer, period, now.QuadPart - period / 8));
   /* Nothing recorded yet: any first present is unpaced by default. */
   struct wsi_win32_pacer fresh;
   CHECK(wsi_win32_pacer_init(&fresh, 60));
   CHECK(!wsi_win32_pacer_compositor_paced(&fresh, period, now.QuadPart));
   wsi_win32_pacer_finish(&fresh);

   /* Back on screen after uncomposited frames: the first blocked present
    * re-anchors the free-running deadline once, so the cadence locks to
    * the next display period instead of keeping the free-running phase;
    * it is a resumed frame and must not wait out a full period either. */
   struct wsi_win32_pacer back;
   CHECK(wsi_win32_pacer_init(&back, 60));
   wsi_win32_pacer_presented(&back, 0, now.QuadPart - back.period / 2);
   CHECK(!back.compositor_blocked);
   CHECK(wsi_win32_pacer_compositor_resumed(&back, back.period / 2));
   wsi_win32_pacer_presented(&back, back.period / 2, now.QuadPart);
   CHECK(back.compositor_blocked);
   CHECK(back.next == now.QuadPart + back.period);
   /* A desktop that keeps blocking is neither resumed nor re-anchored. */
   CHECK(!wsi_win32_pacer_compositor_resumed(&back, back.period / 2));
   wsi_win32_pacer_presented(&back, back.period / 2, now.QuadPart + back.period);
   CHECK(back.next == now.QuadPart + back.period);
   wsi_win32_pacer_finish(&back);

   /* 120 Hz desktop closed loop: the compositor completes half a period
    * after each release. Every frame must stay capped; recording the
    * release at the producer-cap wait exit (not at the pre-wait
    * DwmFlush() completion) is what prevents an unpaced/paced frequency
    * doubling. The 120 Hz waitable timer paces the simulation so the test
    * does not depend on the system clock tick. */
   struct wsi_win32_pacer fast, vblank;
   CHECK(wsi_win32_pacer_init(&fast, 60));
   CHECK(wsi_win32_pacer_init(&vblank, 120));
   for (unsigned i = 0; i < 4; i++) {
      if (i > 0) {
         CHECK(QueryPerformanceCounter(&now));
         wsi_win32_pacer_sync_compositor(&vblank, now.QuadPart);
         CHECK(wsi_win32_pacer_wait(&vblank));
      }
      CHECK(QueryPerformanceCounter(&now));
      const bool paced =
         wsi_win32_pacer_compositor_paced(&fast, fast.period / 4, now.QuadPart);
      CHECK(!paced);
      wsi_win32_pacer_presented(&fast, fast.period / 4, now.QuadPart);
      CHECK(wsi_win32_pacer_wait(&fast));
   }
   wsi_win32_pacer_finish(&vblank);
   wsi_win32_pacer_finish(&fast);

   /* Virtual compositor scenarios. Heavy frames leave DwmFlush() only a
    * few milliseconds of the period, which must still count as display
    * locked; an occluded desktop returns immediately and must stay capped;
    * a late frame must not burst; a desktop faster than the cap must stay
    * capped every frame. In none of them may the producer wait more than
    * the remaining deadline after a successful DwmFlush(). */
   {
      struct wsi_win32_pacer gauge;
      CHECK(wsi_win32_pacer_init(&gauge, 60));
      const int64_t gv = gauge.period;
      wsi_win32_pacer_finish(&gauge);

      int64_t heavy_work[6];
      bool heavy_idle[] = { false, false, false, false, false, false };
      bool heavy_paced[] = { false, true, true, true, true, true };
      /* First frame has no history and is capped; every later frame is
       * display-locked even though DwmFlush() itself only waits 3-4 ms
       * (60 Hz period is 16.67 ms and the GPU/GDI beat eats the rest). */
      for (unsigned i = 0; i < 6; i++)
         heavy_work[i] = (i % 2 == 0 ? 12 : 2) * gv / 16 + (i == 2 ? gv / 16 : 0);
      check_beats("60 Hz heavy/light alternation", gv, heavy_work,
                  heavy_idle, heavy_paced, 6);

      int64_t occl_work[] = { gv / 8, gv / 8, gv / 8, gv / 8 };
      bool occl_idle[] = { true, true, true, true };
      bool occl_paced[] = { false, false, false, false };
      check_beats("occluded immediate return", gv, occl_work,
                  occl_idle, occl_paced, 4);

      int64_t late_work[] = { gv / 16, gv / 16, 5 * gv / 2, gv / 16, gv / 16 };
      bool late_idle[] = { false, false, false, false, false };
      bool late_paced[] = { false, true, true, true, true };
      check_beats("missed deadline after a late frame", gv, late_work,
                  late_idle, late_paced, 5);

      int64_t fast_work[] = { gv / 16, gv / 16, gv / 16, gv / 16 };
      bool fast_idle[] = { false, false, false, false };
      bool fast_paced[] = { false, false, false, false };
      check_beats("120 Hz desktop under the 60 Hz cap", gv / 2, fast_work,
                  fast_idle, fast_paced, 4);

      int64_t back_work[] = { gv / 4, gv / 4, gv / 4, gv / 4, gv / 4, gv / 4 };
      bool back_idle[] = { true, true, false, false, false, false };
      bool back_paced[] = { false, false, false, true, true, true };
      /* Back on screen after occlusion: the first composited frame is not
       * display-locked yet, but as a resumed frame it is released at the
       * composition it waited for instead of waiting out a full period;
       * the following frames lock immediately. */
      check_beats("occluded then back on screen", gv, back_work,
                  back_idle, back_paced, 6);
   }

   /* The fallback timer stays phase-locked to the compositor: after a paced
    * present it waits out only the remainder of the display period instead
    * of stacking a full timer period on top of the VBlank wait. */
   wsi_win32_pacer_sync_compositor(&pacer, now.QuadPart);
   CHECK(pacer.next == now.QuadPart + period);
   CHECK(wsi_win32_pacer_wait(&pacer));
   CHECK(QueryPerformanceCounter(&end));
   CHECK(end.QuadPart >= now.QuadPart + period);
   CHECK(end.QuadPart < now.QuadPart + 2 * period);

   /* A late producer gets one slot, never a burst of accumulated slots. */
   Sleep(50);
   CHECK(wsi_win32_pacer_wait(&pacer));
   const int64_t first_slot = pacer.next;
   CHECK(wsi_win32_pacer_wait(&pacer));
   CHECK(pacer.next >= first_slot + period);

   CHECK(CloseHandle(pacer.timer));
   CHECK(QueryPerformanceCounter(&end));
   pacer.next = end.QuadPart + pacer.frequency;
   CHECK(!wsi_win32_pacer_wait(&pacer)); /* Timer failure is not success. */
   pacer.timer = NULL;
   wsi_win32_pacer_finish(&pacer);
   printf("PASS: 60-Hz cap, blocking wait, late-frame pacing, compositor phase-lock, "
          "and timer errors; wall=%.6fs CPU=%.6fs (Windows timer control, no GPU execution)\n",
          wall_seconds, cpu_elapsed / 10000000.0);
   return 0;
}
