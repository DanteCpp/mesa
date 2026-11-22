/*
 * Copyright 2026 LiamRay11
 * SPDX-License-Identifier: MIT
 *
 * Run a call on a thread with a large stack when the calling thread has little left. The
 * compiler has frames of 100 KiB (slot assignment) and 72 KiB (nir_opt_varyings) that nest;
 * on Windows a host process's threads get its EXE's stack reserve (DWM's is small), so a D3D
 * device on zink over hk overflowed in DWM. Elsewhere the call runs inline.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void agx_call_with_stack(void (*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif
