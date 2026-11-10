/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 *
 * Which path queue_submit_empty() takes for a submit with no control
 * streams (hk_queue_empty.h). On Windows an empty submit with waits must
 * wait on the CPU before anything is signalled; the signal-only path and
 * every non-Windows path stay as they were.
 *
 *    cc -std=c11 -Wall -Wextra -Werror -I.. test-hk-empty-submit.c
 */
#include "hk_queue_empty.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void
expect(bool windows, uint32_t waits, uint32_t signals,
       enum hk_empty_submit_path want)
{
   const enum hk_empty_submit_path got =
      hk_empty_submit_path(windows, waits, signals);
   if (got != want) {
      fprintf(stderr, "FAIL windows=%d waits=%u signals=%u: path %d, want %d\n",
              windows, waits, signals, got, want);
      failures++;
   }
}

int
main(void)
{
   const uint32_t counts[] = {0, 1, 2, 7, UINT32_MAX};
   unsigned checks = 0;

   for (unsigned w = 0; w < sizeof(counts) / sizeof(counts[0]); ++w) {
      for (unsigned s = 0; s < sizeof(counts) / sizeof(counts[0]); ++s) {
         const uint32_t waits = counts[w], signals = counts[s];

         /* Not Windows: the waits go into the queue timeline as before. */
         expect(false, waits, signals, HK_EMPTY_SUBMIT_TIMELINE);

         /* Windows. */
         enum hk_empty_submit_path want;
         if (waits == 0 && signals == 0)
            want = HK_EMPTY_SUBMIT_TIMELINE; /* nothing to do, as before */
         else if (waits == 0)
            want = HK_EMPTY_SUBMIT_CPU_SIGNAL; /* unchanged branch */
         else if (signals == 0)
            want = HK_EMPTY_SUBMIT_CPU_WAIT;
         else
            want = HK_EMPTY_SUBMIT_CPU_WAIT_SIGNAL;
         expect(true, waits, signals, want);
         checks += 2;
      }
   }

   if (failures) {
      fprintf(stderr, "hk_empty_submit: %d/%u checks FAILED\n", failures,
              checks);
      return EXIT_FAILURE;
   }
   printf("hk_empty_submit: %u checks passed\n", checks);
   return EXIT_SUCCESS;
}
