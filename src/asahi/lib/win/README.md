<!-- SPDX-License-Identifier: MIT -->
# asahi on Windows (hk, headless compute)

An `agx_device_ops_t` transport for the Windows render node of the g18win
kernel driver (the G18 backend), next to native DRM (`agx_device.c`), virtio
native context (`agx_device_virtio.c`) and the D3DKMT and AGXKMD Windows
backends (`agx_device_d3dkmt.c`, `agx_device_win.c`; those two build only
when the cross file names their drivers tree, see `../meson.build`). hk (Vulkan) builds and links for
aarch64-w64-mingw32 as `vulkan_asahi.dll`; it has run in WinPE under QEMU on
the stub node (below), not yet on hardware.

| File | What |
|---|---|
| `g18_uapi.h` | Verbatim copy of g18win's `uapi/g18_uapi.h`: the only definition of the render-node ABI. Refreshed by `sync-g18-uapi.sh <g18win>` (`--check` compares); provenance in `g18_uapi.h.src`. Never edit it here |
| `agx_win_kmd.h` | Transport API on top of it, plus static checks that the g18 mirrors equal Mesa's `drm_asahi_*` structs |
| `agx_win_kmd.c` | Transport: pseudo-fds, frame marshalling (pointer fields become frame offsets), Linux-errno to CRT-errno mapping, `DeviceIoControl` on `\\.\G18Win0`, `G18_IOCTL_LOG` |
| `agx_win_kmd_stub.c` | In-process stub render node (`AGX_WIN_KMD_STUB=fake`): the kernel's frame rules and G4 semantics, GET_PARAMS as the kernel builds it, SUBMIT checked but not executed, every call recorded as G18R |
| `agx_win_g18r.c/.h` | G18R v1 writer for the stub's record (`AGX_WIN_KMD_STUB_LOG=<file>`) |
| `agx_device_g18.c/.h` | The backend: ops table, `agx_g18_open_device()`, host-pointer import, user-mode `util_sync_provider` |
| `agx_win_no_d3dkmt.c`, `agx_win_no_agxkmd.c` | Entry points of the other two Windows backends for builds without them |
| `check-dll.sh` | Link gates on the built DLL: imports on the WinPE list only, no libc++/libunwind/winpthread DLL, exactly the three ICD exports |
| `include/win32_stub/sys/ioccom.h` | Stand-in for the BSD header `drm-uapi/drm.h` includes off Linux (`_IO*` encoders and `_IOC_*` accessors) |
| `cross/aarch64-w64-mingw32.ini`, `cross/build-machine.ini` | Meson cross and native files |
| `list_linux_deps.py`, `LINUX-DEPS.txt` | Spike-era static scan of Linux-only code (ignores `#ifdef`s; kept for reference) |
| `../tests/test-win-kmd-stub.c` | Host test of the transport against the stub, any OS (meson test `asahi-win-kmd-stub`) |

## Render node

One device, opened twice by hk (physical device, logical device), each open
one DRM file. Every call is one METHOD_BUFFERED `DeviceIoControl` with code
`G18_RN_CTL(cmd)` on a frame: `g18_rn_hdr` (32 bytes), the asahi argument
struct at +32, inline data after it. The Linux "pointer" fields travel as
frame offsets: GET_PARAMS `pointer`/`size` (in/out), VM_BIND `userptr`
(ops), SUBMIT `cmdbuf`. The kernel copies `min(caller size, its size)` of an
argument, so older, shorter asahi structs keep working (QUEUE_CREATE goes out
at 24 bytes). `hdr.result` is a Linux errno; the transport returns -1 and
sets the CRT errno of the same name, like `drmIoctl`.

Every command but VERSION answers ENODEV until the driver is LIVE (GO_LIVE);
`agx_g18_open_device()` checks VERSION's state first and says so. No
render-node call touches the GPU before then.

What Windows does differently from Linux (G4-PLAN 3):
- **Syncs are user mode.** The kernel's SUBMIT is synchronous and carries no
  syncs. `agx_device_g18.c` keeps a process-wide syncobj table with libdrm
  semantics (binary and timeline payloads, waits, transfers that fire later),
  serves it as `dev->vk.sync`, and its SUBMIT op waits for the in-syncs,
  submits, then signals the out-syncs. `vk_drm_syncobj.c` builds without
  libdrm on Windows. No sync_file, no opaque fds.
- **No placed or cached maps.** GEM_MAP returns a write-combined mapping the
  kernel places; there is no HOST_CACHED memory type and no
  `VK_EXT_map_memory_placed`.
- **No DRM device.** hk enumerates through
  `vk_instance::physical_devices.enumerate`; `EXT_physical_device_drm`,
  dma-buf, DRM format modifiers and external fds are off.
- **No GNU build-id.** The driver UUID hashes the version and the module's
  identity (`disk_cache_get_function_identifier`, as dzn does).
- `AGX_WIN_FEATURE_CLEAR=<hex>` clears feature bits the kernel reports, for
  variants in one image without a rebuild.

## Stub node and G18R

`AGX_WIN_KMD_STUB=fake` replaces the device with an in-process node that
answers like the G4 kernel driver: same IOCTL codes and frame checks,
handles from 1 per file, GET_PARAMS group 0 field by field as the kernel's
K4 builds it (`AGX_WIN_KMD_STUB_FEATURES=<hex>` overrides the feature mask),
group 1 VM status, groups 2/3 refused, the G4 SUBMIT admission (compute only,
one command, no samplers, stream at most 64 KiB), EOPNOTSUPP for what G4
does not serve. SUBMIT is not executed. With `AGX_WIN_KMD_STUB_LOG=<file>`
the node writes a G18R v1 record of everything Mesa sent, including the
PAGE/BLOB snapshot of every bound page at each SUBMIT, in the format of the
M6 project's Linux recorder, so the two runs can be compared offline.

## Build

Toolchain: llvm-mingw (clang + lld in MinGW mode, UCRT). Build-machine tools
come from a native Linux Mesa build, as Mesa CI does for cross builds:

```
meson setup build-host --prefix=<tools> -Dlibdir=lib -Dbuild-tests=false \
  -Denable-glcpp-tests=false -Dgallium-drivers= -Dvulkan-drivers= \
  -Dvideo-codecs= -Dglx=disabled -Dplatforms= -Dmesa-clc=enabled \
  -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled \
  -Dinstall-precomp-compiler=true -Dtools=asahi
ninja -C build-host install    # mesa_clc, vtn_bindgen2, asahi_clc
```

Then the Windows build (headless, hk only, no WSI). `buildtype=release`
gives NDEBUG, as the release builds of the Linux driver: a debug build hits an
assertion in `nir_lower_explicit_io` on the G18G storage-buffer path (the
descriptor lowering builds 5-component map addresses while the SPIR-V options
and the explicit-I/O pass use the 4-component format), on any OS.

```
meson setup build-win --wrap-mode=nodownload \
  --cross-file src/asahi/lib/win/cross/aarch64-w64-mingw32.ini \
  --native-file tools-path.ini \
  --native-file src/asahi/lib/win/cross/build-machine.ini \
  -Dbuildtype=release -Ddebug=true -Dgallium-drivers= -Dvulkan-drivers=asahi \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled \
  -Dgbm=disabled -Dglx=disabled -Dplatforms= -Dllvm=disabled \
  -Dbuild-tests=false -Dmesa-clc=system -Dprecomp-compiler=system \
  -Dxmlconfig=disabled -Dexpat=disabled -Dzlib=disabled -Dzstd=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dmin-windows-version=10
ninja -C build-win src/asahi/vulkan/vulkan_asahi.dll
llvm-strip --strip-debug -o vulkan_asahi.dll build-win/src/asahi/vulkan/vulkan_asahi.dll
src/asahi/lib/win/check-dll.sh vulkan_asahi.dll
```

`tools-path.ini` defines `[constants] native_tools = '<tools>/bin'`.

The DLL exports `vk_icdNegotiateLoaderICDInterfaceVersion`,
`vk_icdGetInstanceProcAddr` and `vk_icdGetPhysicalDeviceProcAddr`
(`vulkan_api.def`); a program loads it directly, with no Khronos loader.
