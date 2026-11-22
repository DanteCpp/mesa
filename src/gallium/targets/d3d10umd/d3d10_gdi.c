/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "util/u_debug.h"
#include "target-helpers/inline_debug_helper.h"
#include "llvmpipe/lp_public.h"
#include "softpipe/sp_public.h"
#include "sw/gdi/gdi_sw_winsys.h"
#ifdef GALLIUM_ZINK
#include "zink/zink_public.h"
#endif

#include "winddk_compat.h"
#include <d3dkmthk.h>

extern struct pipe_screen *
d3d10_create_screen(void);

static HDC
d3d10_gdi_acquire_hdc(void *winsys_drawable_handle) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   return GetDC(hWnd);
}

static void
d3d10_gdi_release_hdc(void *winsys_drawable_handle, HDC hDC) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   ReleaseDC(hWnd, hDC);
}

struct pipe_screen *
d3d10_create_screen(void)
{
   const char *default_driver;
   const char *driver;
   struct pipe_screen *screen = NULL;
   struct sw_winsys *winsys;

   winsys = gdi_create_sw_winsys(d3d10_gdi_acquire_hdc, d3d10_gdi_release_hdc);
   if(!winsys)
      goto no_winsys;

#ifdef GALLIUM_LLVMPIPE
   default_driver = "llvmpipe";
#else
   default_driver = "softpipe";
#endif

   driver = debug_get_option("GALLIUM_DRIVER", NULL);
#ifdef GALLIUM_ZINK
   /* M6 (track H): with no GALLIUM_DRIVER, C:\g6w\zink.on (written once the G18 render node is
    * live) selects zink on hk, the ICD next to it in C:\g6x\g18 unless ZINK_VULKAN_ICD names one.
    * A process started before the file exists (DWM at logon) stays on softpipe. */
   if (driver == NULL && GetFileAttributesA("C:\\g6w\\zink.on") != INVALID_FILE_ATTRIBUTES) {
      if (!getenv("ZINK_VULKAN_ICD"))
         _putenv("ZINK_VULKAN_ICD=C:\\g6x\\g18\\vulkan_asahi.dll");
      driver = "zink";
      fprintf(stderr, "g18d3d10: C:\\g6w\\zink.on: zink on hk\n");
      /* G6 r7: C:\g6w\callstats.on: hk's render-node call times (AGX_WIN_CALLSTATS) every 600 SUBMITs
       * on stderr, for processes that cannot be given the variable (DWM) */
      if (!getenv("AGX_WIN_CALLSTATS") &&
          GetFileAttributesA("C:\\g6w\\callstats.on") != INVALID_FILE_ATTRIBUTES) {
         _putenv("AGX_WIN_CALLSTATS=600");
         fprintf(stderr, "g18d3d10: C:\\g6w\\callstats.on: AGX_WIN_CALLSTATS=600\n");
      }
      /* G6 r7: C:\g6w\hkanat.on: hk's per-submit job anatomy (every 300th) to C:\g6u\out\hkanat-<pid>.txt */
      if (!getenv("HK_FRAME_ANATOMY") &&
          GetFileAttributesA("C:\\g6w\\hkanat.on") != INVALID_FILE_ATTRIBUTES) {
         char v[96];
         snprintf(v, sizeof v, "HK_FRAME_ANATOMY=C:\\g6u\\out\\hkanat-%lu.txt", GetCurrentProcessId());
         _putenv(v);
         _putenv("HK_FRAME_ANATOMY_TS=none");
         _putenv("HK_FRAME_ANATOMY_EVERY=300");
         fprintf(stderr, "g18d3d10: C:\\g6w\\hkanat.on: %s\n", v);
      }
   }
   /* M6 (track H): C:\g6w\zink.off (first line: a G18_ZINK_OFF list) for processes that cannot
    * be given the variable (DWM); an explicit G18_ZINK_OFF wins. */
   if (driver != NULL && strcmp(driver, "zink") == 0 && !getenv("G18_ZINK_OFF")) {
      FILE *zf = fopen("C:\\g6w\\zink.off", "r");
      if (zf) {
         char line[200] = "G18_ZINK_OFF=";
         size_t n = strlen(line);
         if (fgets(line + n, (int)(sizeof(line) - n), zf)) {
            size_t e = strlen(line);
            while (e > n && (line[e - 1] == '\n' || line[e - 1] == '\r' || line[e - 1] == ' '))
               line[--e] = 0;
            if (e > n) {
               _putenv(line);
               fprintf(stderr, "g18d3d10: C:\\g6w\\zink.off: %s\n", line);
            }
         }
         fclose(zf);
      }
   }
#endif
   if (driver == NULL)
      driver = default_driver;

#ifdef GALLIUM_LLVMPIPE
   if (strcmp(driver, "llvmpipe") == 0) {
      screen = llvmpipe_create_screen( winsys );
   }
#endif

#ifdef GALLIUM_ZINK
   /* G6 r7 (H): the UMD's async flips wait for zink's submit thread through this */
   {
      extern bool zink_fence_wait_flushed(struct pipe_fence_handle *pfence, uint64_t timeout_ns);
      extern void G18SetFenceWaitFlushed(bool (*fn)(struct pipe_fence_handle *, uint64_t));
      G18SetFenceWaitFlushed(zink_fence_wait_flushed);
      extern void zink_flush_nosync(struct pipe_context *pctx, struct pipe_fence_handle **pfence);
      extern void G18SetFlushNoSync(void (*fn)(struct pipe_context *, struct pipe_fence_handle **));
      G18SetFlushNoSync(zink_flush_nosync);
   }
   /* GALLIUM_DRIVER=zink: Gallium over Vulkan (hk on the G18). ZINK_VULKAN_ICD may name a
    * Vulkan ICD DLL to load directly when there is no Vulkan loader. */
   if (screen == NULL && strcmp(driver, "zink") == 0) {
      screen = zink_create_screen( winsys, NULL );
      fprintf(stderr, "g18d3d10: GALLIUM_DRIVER=zink: zink_create_screen -> %p\n", (void *)screen);
   }
#endif

   if (screen == NULL) {
      screen = softpipe_create_screen( winsys );
   }

   if (screen == NULL)
      goto no_screen;

   return debug_screen_wrap( screen );

no_screen:
   winsys->destroy(winsys);
no_winsys:
   return NULL;
}
