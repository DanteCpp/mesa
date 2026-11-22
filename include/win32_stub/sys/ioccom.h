/*
 * Copyright 2026 The Mesa Authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Windows stand-in for the BSD <sys/ioccom.h>.
 *
 * include/drm-uapi/drm.h includes <sys/ioccom.h> on every system that is not
 * Linux. Windows has no such header and no ioctl(2); the asahi Windows
 * backend never issues these command words to an OS ioctl. It only uses them
 * as tags: the G18 render-node backend (src/asahi/lib/win/agx_device_g18.c)
 * pulls the asahi command number (bits 0-7) and the
 * argument size (bits 16-28) back out and frames the request for
 * DeviceIoControl (see agx_win_kmd.h). Both the BSD and the Linux encodings
 * keep those two fields in the same place, so code that only reads them works
 * with either.
 *
 * include/win32_stub is added to inc_include for Windows builds only.
 */

#ifndef AGX_WIN_SYS_IOCCOM_H
#define AGX_WIN_SYS_IOCCOM_H

#ifndef _WIN32
#error "only for Windows builds; other systems have their own <sys/ioccom.h>"
#endif

#define IOCPARM_SHIFT 13
#define IOCPARM_MASK  ((1u << IOCPARM_SHIFT) - 1)
#define IOCPARM_LEN(x) (((x) >> 16) & IOCPARM_MASK)
#define IOCBASECMD(x)  ((x) & ~(IOCPARM_MASK << 16))
#define IOCGROUP(x)    (((x) >> 8) & 0xff)

#define IOC_VOID  0x20000000u
#define IOC_OUT   0x40000000u
#define IOC_IN    0x80000000u
#define IOC_INOUT (IOC_IN | IOC_OUT)
#define IOC_DIRMASK (IOC_VOID | IOC_OUT | IOC_IN)

#define _IOC(inout, group, num, len)                                           \
   ((unsigned long)((inout) | (((len) & IOCPARM_MASK) << 16) |                 \
                    ((group) << 8) | (num)))
#define _IO(g, n)        _IOC(IOC_VOID, (g), (n), 0)
#define _IOR(g, n, t)    _IOC(IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t)    _IOC(IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t)   _IOC(IOC_INOUT, (g), (n), sizeof(t))

/* Linux-style field accessors, so code that re-encodes a command word with a
 * different size (_IOC(_IOC_DIR(c), _IOC_TYPE(c), _IOC_NR(c), size)) works
 * with this encoding too. */
#define _IOC_DIR(x)  ((x) & IOC_DIRMASK)
#define _IOC_TYPE(x) IOCGROUP(x)
#define _IOC_NR(x)   ((x) & 0xff)
#define _IOC_SIZE(x) IOCPARM_LEN(x)

#endif /* AGX_WIN_SYS_IOCCOM_H */
