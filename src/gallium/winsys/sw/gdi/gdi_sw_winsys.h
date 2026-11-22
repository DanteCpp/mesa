#ifndef GDI_SW_WINSYS_H
#define GDI_SW_WINSYS_H

#include <windows.h>

#include "util/compiler.h"
#include "frontend/sw_winsys.h"

/*
 * resource_from_handle over memory the caller owns: winsys_handle.handle is the CPU
 * pointer, winsys_handle.stride the row pitch (single level, single layer). Used by the
 * D3D10 UMD for WDDM allocations it keeps locked.
 */
#define GDI_SW_HANDLE_TYPE_USER_MEMORY 0x6731

void gdi_sw_display( struct sw_winsys *winsys,
                     struct sw_displaytarget *dt,
                     HDC hDC );

struct sw_winsys *
gdi_create_sw_winsys(
    /* Following functions are used to acquire HDC to draw on
     * from winsys_drawable_handle argument of screen->flush_frontbuffer
     */
    HDC (*acquire_hdc)(void *winsys_drawable_handle),
    void (*release_hdc)(void *winsys_drawable_handle, HDC hdc)
);

/* Used when winsys_drawable_handle is HDC itself */
HDC gdi_sw_acquire_hdc_by_value(void *context_private);
void gdi_sw_release_hdc_by_value(void *context_private, HDC hdc);
#endif
