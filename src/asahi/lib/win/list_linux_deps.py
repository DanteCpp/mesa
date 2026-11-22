#!/usr/bin/env python3
# Copyright 2026 The Mesa Authors
# SPDX-License-Identifier: MIT
"""
List what keeps the asahi drivers (hk, gallium asahi, the shared lib and
winsys) from building for a Windows target (aarch64-w64-mingw32).

Static scan only: it reads the sources, it does not compile them. Each hit is
classified so the Windows port can be planned per call site:

  include   a header that does not exist on a mingw-w64 or MSVC target, or
            that needs a shim (drm-uapi/*.h pull <sys/ioccom.h> off Linux)
  sync      DRM syncobj / sync_file / dma-buf fence sites; these move to a
            Windows sync provider backed by the render node's timelines
  drm       other libdrm or DRM ioctl use (version, PRIME, GEM close, ...)
  posix     fd and mmap semantics with no direct Win32 equivalent

Run from the top of the Mesa tree:
  python3 src/asahi/lib/win/list_linux_deps.py > src/asahi/lib/win/LINUX-DEPS.txt
"""

import os
import re
import sys
from collections import defaultdict

SCAN_DIRS = [
    "src/asahi/lib",
    "src/asahi/vulkan",
    "src/asahi/layout",
    "src/asahi/compiler",
    "src/asahi/libagx",
    "src/asahi/genxml",
    "src/asahi/clc",
    "src/asahi/isa",
    "src/poly",
    "src/gallium/drivers/asahi",
    "src/gallium/winsys/asahi",
]

# Not built for the Windows targets at all.
SKIP_PARTS = ("/tests/", "/test/", "/drm-shim/", "/tools/", "/win/",
              "/agx_device_win.", "/agx_device_d3dkmt.", "/agx_sync_",
              "/agx_d3dkmt", "/agx_bridge_vma", "/agxw_")

# Built for the build machine or only as Linux debug tools, never for the
# Windows target; listed but marked.
HOST_ONLY = {
    "src/asahi/clc/asahi_clc.c": "host build tool (native mesa_clc/asahi_clc)",
    "src/asahi/lib/wrap.c": "LD_PRELOAD trace wrapper, tools only",
}

# Call sites the Windows sync provider must replace (subset of "sync").
SYNC_CALL_RE = re.compile(
    r"\b(drmSyncobj\w+|DRM_IOCTL_SYNCOBJ\w*|DRM_IOCTL_TIMELINE_SYNCOBJ\w*|"
    r"vk_drm_syncobj\w*|vk_sync_type_is_drm_syncobj|"
    r"sync_(wait|merge|accumulate)|agx_(import|export)_sync_file|"
    r"DMA_BUF_IOCTL_(IMPORT|EXPORT)_SYNC_FILE|DRM_ASAHI_SYNC_\w+)\b")

# Header -> (verdict, note).  "missing" = no such header on mingw-w64;
# "partial" = exists on mingw-w64 but lacks what Linux code uses from it;
# "shim" = portable once a small Windows compat header is supplied.
INCLUDES = [
    (r"xf86drm(Mode)?\.h", "missing", "libdrm; not built for Windows"),
    (r"linux/.*", "missing", "Linux kernel uAPI"),
    (r"sys/mman\.h", "missing", "mmap/munmap; use os_mman/VirtualAlloc or KMD map ioctl"),
    (r"sys/ioctl\.h", "missing", "ioctl(); use DeviceIoControl via agx_win_ioctl"),
    (r"sys/wait\.h", "missing", "waitpid/fork"),
    (r"sys/sysmacros\.h", "missing", "major/minor of a DRM node"),
    (r"sys/(select|socket|eventfd|syscall|epoll|inotify|prctl|sysinfo)\.h",
     "missing", "Linux/POSIX only"),
    (r"poll\.h", "missing", "poll() on fences/fds"),
    (r"dlfcn\.h", "missing", "use util/u_dl.h (LoadLibrary on Windows)"),
    (r"util/libsync\.h", "missing", "sync_file fds (Linux only)"),
    (r"util/u_drm\.h", "missing", "libdrm helpers"),
    (r"vdrm\.h", "missing", "virtio native context; Linux guest only"),
    (r"asahi_proto\.h", "missing", "virtio native-context protocol"),
    (r"drm-uapi/virtgpu_drm\.h", "missing", "virtio-gpu uAPI"),
    (r"drm-uapi/dma-buf\.h", "missing", "dma-buf sync_file ioctls"),
    (r"drm-uapi/drm_fourcc\.h", "shim", "pure defines; needs drm.h types"),
    (r"drm-uapi/(asahi_)?drm\.h", "shim",
     "drm.h includes <sys/ioccom.h> when !__linux__; supply a Windows ioccom shim"),
    (r"vk_drm_syncobj\.h", "missing", "vk_sync type over DRM syncobj; built only with libdrm"),
    (r"wsi_common_drm\.h", "missing", "DRM WSI; Windows uses wsi_common_win32"),
    (r"unistd\.h", "partial", "mingw has close/read/lseek but no sysconf/fork/getpagesize; absent on MSVC"),
    (r"fcntl\.h", "partial", "no O_CLOEXEC / F_DUPFD_CLOEXEC semantics"),
    (r"pthread\.h", "partial", "winpthreads on mingw only; prefer c11/threads.h"),
    (r"signal\.h", "partial", "no SIGBUS/sigaction"),
    (r"sys/stat\.h", "partial", "no S_ISCHR/st_rdev semantics for DRM nodes"),
    (r"dirent\.h", "partial", "mingw only; absent on MSVC"),
]

SYNC_PATTERNS = [
    r"\bdrmSyncobj\w+",
    r"\bDRM_IOCTL_SYNCOBJ\w*",
    r"\bvk_drm_syncobj\w*",
    r"\bvk_sync_type_is_drm_syncobj\b",
    r"\bDMA_BUF_IOCTL_(IMPORT|EXPORT)_SYNC_FILE\b",
    r"\bsync_(wait|merge|accumulate|valid_fd)\s*\(",
    r"\bagx_(import|export)_sync_file\s*\(",
    r"\bsyncobj\w*\b",
    r"\bsync_file\w*\b",
    r"\bDRM_ASAHI_SYNC_\w+",
]

DRM_PATTERNS = [
    r"\bdrm(Ioctl|GetVersion|FreeVersion|Prime\w+|GetDevice\w*|FreeDevice\w*|"
    r"GetRenderDeviceNameFromFd|GetPrimaryDeviceNameFromFd|Open\w*|Close|"
    r"CommandWrite\w*|GetCap|SetClientCap|GetNodeTypeFromFd|Auth\w*)\b",
    r"\bDRM_IOCTL_(?!SYNCOBJ)\w+",
    r"\bDRM_(CLOEXEC|RDWR|NODE_\w+|BUS_\w+)\b",
    r"\bvdrm_\w+\s*\(",
]

POSIX_PATTERNS = [
    r"\b(os_)?mmap\s*\(",
    r"\b(os_)?munmap\s*\(",
    r"\bMAP_(FAILED|FIXED|SHARED|PRIVATE|ANONYMOUS)\b",
    r"\bmadvise\s*\(",
    r"\bioctl\s*\(",
    r"\b(os_dupfd_cloexec|dup|dup2|fcntl|lseek|poll)\s*\(",
    r"(?<![\w.>])close\s*\(",
    r"\bprime_fd\b",
    r"\bsysconf\s*\(",
    r"\b(fork|waitpid|sigaction)\s*\(",
    r"%m\b",
]

INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
PP_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")
# Conditions that already exclude a Windows build (HAVE_LIBDRM is never
# defined there, nor __linux__).
GUARD_RE = re.compile(r"HAVE_LIBDRM|__linux__|DETECT_OS_LINUX|HAVE_VDRM|"
                      r"DETECT_OS_POSIX|HAVE_DLFCN_H")


def scan_file(path):
    hits = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return hits
    in_comment = False
    stack = []
    for no, raw in enumerate(lines, 1):
        line = raw
        # Strip block and line comments (good enough for a static inventory).
        if in_comment:
            end = line.find("*/")
            if end < 0:
                continue
            line = line[end + 2:]
            in_comment = False
        line = re.sub(r"/\*.*?\*/", "", line)
        start = line.find("/*")
        if start >= 0:
            line = line[:start]
            in_comment = True
        line = re.sub(r"//.*", "", line)
        if not line.strip():
            continue

        # Track #if nesting so includes that a platform guard already keeps
        # out of non-Linux builds are marked rather than counted as work.
        pp = PP_RE.match(line)
        if pp:
            kw, cond = pp.group(1), pp.group(2).strip()
            if kw in ("if", "ifdef", "ifndef"):
                stack.append(cond if kw != "ifndef" else "!" + cond)
            elif kw in ("elif", "else") and stack:
                stack[-1] = "else-of(" + stack[-1] + ")"
            elif kw == "endif" and stack:
                stack.pop()
            continue

        m = INC_RE.match(line)
        if m:
            hdr = m.group(1)
            guard = next((c for c in reversed(stack)
                          if GUARD_RE.search(c) and not c.startswith("else-of(")
                          and not c.startswith("!")), None)
            for pat, verdict, note in INCLUDES:
                if re.fullmatch(pat, hdr):
                    if guard:
                        verdict = "guarded: #if " + guard
                    hits.append(("include", no, f"<{hdr}> [{verdict}] {note}"))
                    break
            continue

        for cat, pats in (("sync", SYNC_PATTERNS), ("drm", DRM_PATTERNS),
                          ("posix", POSIX_PATTERNS)):
            found = []
            for pat in pats:
                for mm in re.finditer(pat, line):
                    tok = mm.group(0).rstrip("( \t")
                    if tok not in found:
                        found.append(tok)
            if found:
                hits.append((cat, no, ", ".join(found) + "  | " +
                             line.strip()[:110]))
                if cat == "sync":
                    break  # a sync line is not double-counted as drm/posix
    return hits


def main():
    root = os.getcwd()
    per_file = {}
    for d in SCAN_DIRS:
        for dirpath, _, files in os.walk(os.path.join(root, d)):
            for fn in sorted(files):
                if not fn.endswith((".c", ".h", ".cpp", ".cc")):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, fn), root)
                if any(p in "/" + rel for p in SKIP_PARTS):
                    continue
                hits = scan_file(os.path.join(dirpath, fn))
                if hits:
                    per_file[rel] = hits

    totals = defaultdict(int)
    files_by_cat = defaultdict(set)
    guarded = 0
    for rel, hits in per_file.items():
        for cat, _, text in hits:
            if rel in HOST_ONLY:
                continue
            if cat == "include" and "[guarded:" in text:
                guarded += 1
                continue
            totals[cat] += 1
            files_by_cat[cat].add(rel)

    # Section B: the sync lines that hold an actual call site (SYNC_CALL_RE),
    # a subset of the "sync" lines, which also count types and comments.
    call_lines = []
    for rel in sorted(per_file):
        if rel in HOST_ONLY:
            continue
        for cat, no, text in per_file[rel]:
            if cat != "sync":
                continue
            toks = text.split("  | ")[0]
            calls = [t.strip() for t in toks.split(",")
                     if SYNC_CALL_RE.fullmatch(t.strip())]
            if calls:
                call_lines.append((rel, no, calls, text.split("  | ", 1)[1]))
    call_files = {rel for rel, _, _, _ in call_lines}

    out = sys.stdout
    out.write("# SPDX-License-Identifier: MIT\n")
    out.write("# Linux-only dependencies of the asahi drivers (static scan)\n")
    out.write("# generated by src/asahi/lib/win/list_linux_deps.py\n#\n")
    out.write("# Counts are source lines (a line with several hits counts once).\n")
    for cat in ("include", "sync", "drm", "posix"):
        out.write(f"# {cat:8s} {totals[cat]:5d} lines in "
                  f"{len(files_by_cat[cat]):3d} files\n")
    out.write(f"#   of the sync lines, {len(call_lines)} in {len(call_files)} files are "
              "call sites (section B);\n#   the rest name sync types, fields or "
              "headers\n")
    out.write(f"# (plus {guarded} includes already behind a non-Windows guard)\n")
    out.write("# Host-only files are listed in section C but not counted in A/B.\n")
    out.write("\n")
    out.write("## A. Linux-only / non-Windows includes (target code)\n\n")
    for rel in sorted(per_file):
        if rel in HOST_ONLY:
            continue
        for cat, no, text in per_file[rel]:
            if cat == "include" and "[guarded:" not in text:
                out.write(f"{rel}:{no}: {text}\n")
    out.write("\n## B. syncobj / sync_file call sites (target code)\n\n")
    for rel, no, calls, src in call_lines:
        out.write(f"{rel}:{no}: {', '.join(calls)}  | {src}\n")
    out.write(f"\n# {len(call_lines)} syncobj/sync_file call-site lines in "
              f"{len(call_files)} files\n")
    out.write("\n## C. Per-file detail (all categories)\n\n")
    for rel in sorted(per_file):
        tag = f"   [{HOST_ONLY[rel]}]" if rel in HOST_ONLY else ""
        out.write(f"== {rel}{tag}\n")
        for cat, no, text in per_file[rel]:
            out.write(f"  {cat:7s} {no:5d}  {text}\n")
        out.write("\n")


if __name__ == "__main__":
    main()
