/*
 * Copyright 2026 LiamRay11
 * SPDX-License-Identifier: MIT
 */
#include "agx_bigstack.h"

#ifdef _WIN32
#include <stdint.h>
#include <windows.h>

#define AGX_BIGSTACK_NEED (4u << 20)    /* run inline with at least this much stack left */
#define AGX_BIGSTACK_RESERVE (32u << 20) /* the helper thread's stack reserve */

struct agx_bigstack_call {
   void (*fn)(void *);
   void *arg;
};

static DWORD WINAPI
agx_bigstack_thunk(LPVOID p)
{
   struct agx_bigstack_call *c = p;
   c->fn(c->arg);
   return 0;
}

void
agx_call_with_stack(void (*fn)(void *), void *arg)
{
   ULONG_PTR lo = 0, hi = 0;
   GetCurrentThreadStackLimits(&lo, &hi);
   uintptr_t sp = (uintptr_t)&lo;
   if (lo && sp > lo && sp - lo >= AGX_BIGSTACK_NEED) {
      fn(arg);
      return;
   }
   struct agx_bigstack_call c = {fn, arg};
   HANDLE t = CreateThread(NULL, AGX_BIGSTACK_RESERVE, agx_bigstack_thunk, &c,
                           STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
   if (!t) {
      fn(arg);
      return;
   }
   WaitForSingleObject(t, INFINITE);
   CloseHandle(t);
}
#else
void
agx_call_with_stack(void (*fn)(void *), void *arg)
{
   fn(arg);
}
#endif
