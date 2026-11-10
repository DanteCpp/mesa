/* Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How queue_submit_empty() carries out a submit with no control streams. */
enum hk_empty_submit_path {
   /* Transfer the waits into the queue timeline and signal the outputs from
    * it. Every non-Windows empty submit, and a Windows one with neither
    * waits nor signals. */
   HK_EMPTY_SUBMIT_TIMELINE,
   /* Windows, signals only: wait for the queue point on the CPU, then signal
    * the outputs on the CPU. */
   HK_EMPTY_SUBMIT_CPU_SIGNAL,
   /* Windows, waits only: wait for them on the CPU. */
   HK_EMPTY_SUBMIT_CPU_WAIT,
   /* Windows, waits and signals: wait for the waits on the CPU, then as
    * HK_EMPTY_SUBMIT_CPU_SIGNAL. */
   HK_EMPTY_SUBMIT_CPU_WAIT_SIGNAL,
};

/*
 * On Windows a WDDM GPU wait/signal pair on a context with no DMA buffer is
 * not an execution guarantee: the callbacks succeed while the output fence
 * may never advance. An empty submit there does its waiting and signalling
 * on the CPU instead.
 */
static inline enum hk_empty_submit_path
hk_empty_submit_path(bool windows, uint32_t wait_count, uint32_t signal_count)
{
   if (!windows)
      return HK_EMPTY_SUBMIT_TIMELINE;
   if (wait_count != 0)
      return signal_count != 0 ? HK_EMPTY_SUBMIT_CPU_WAIT_SIGNAL
                               : HK_EMPTY_SUBMIT_CPU_WAIT;
   return signal_count != 0 ? HK_EMPTY_SUBMIT_CPU_SIGNAL
                            : HK_EMPTY_SUBMIT_TIMELINE;
}
