/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_atomic.h"
#include "util/u_queue.h"
#include "util/u_thread.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define CHECK(condition) do { \
   if (!(condition)) { \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      exit(1); \
   } \
} while (0)

enum { WORKERS = 4, GENERATIONS = 8 };

struct barrier_test {
   util_barrier barrier;
   unsigned serial[GENERATIONS];
};

static int
barrier_worker(void *data)
{
   struct barrier_test *test = data;
   for (unsigned i = 0; i < GENERATIONS; ++i) {
      if (util_barrier_wait(&test->barrier))
         p_atomic_inc(&test->serial[i]);
   }
   return 0;
}

static void
test_serial_return(void)
{
   struct barrier_test test = {0};
   thrd_t workers[WORKERS];
   util_barrier_init(&test.barrier, WORKERS);
   for (unsigned i = 0; i < WORKERS; ++i)
      CHECK(u_thread_create(&workers[i], barrier_worker, &test) == thrd_success);
   for (unsigned i = 0; i < WORKERS; ++i)
      CHECK(thrd_join(workers[i], NULL) == thrd_success);
   util_barrier_destroy(&test.barrier);
   for (unsigned i = 0; i < GENERATIONS; ++i) {
      if (test.serial[i] != 1)
         fprintf(stderr, "Generation %u: %u serial returns, expected 1\n", i, test.serial[i]);
      CHECK(test.serial[i] == 1);
   }
   puts("One serial return per barrier generation passed");
}

static void
count_job(void *job, void *global_data, int thread_index)
{
   (void)global_data;
   (void)thread_index;
   p_atomic_inc((unsigned *)job);
}

static void
test_queue_finish(void)
{
   unsigned completed = 0;
   for (unsigned cycle = 0; cycle < 16; ++cycle) {
      struct util_queue queue;
      CHECK(util_queue_init(&queue, "barrier", 16, WORKERS, 0, NULL));
      util_queue_adjust_num_threads(&queue, WORKERS, false);
      CHECK(queue.num_threads == WORKERS);
      for (unsigned round = 0; round < 32; ++round) {
         for (unsigned i = 0; i < WORKERS; ++i)
            util_queue_add_job(&queue, &completed, NULL, count_job, NULL, 0);
         /* All workers must leave the finish barrier before it is destroyed. */
         util_queue_finish(&queue);
         CHECK(p_atomic_read(&completed) == (cycle * 32 + round + 1) * WORKERS);
      }
      util_queue_destroy(&queue);
   }
   puts("512 four-worker queue finishes and 2048 jobs passed");
}

int
main(int argc, char **argv)
{
#ifdef _WIN32
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
   if (argc != 2)
      return 2;
   if (!strcmp(argv[1], "serial"))
      test_serial_return();
   else if (!strcmp(argv[1], "queue"))
      test_queue_finish();
   else
      return 2;
   return 0;
}
