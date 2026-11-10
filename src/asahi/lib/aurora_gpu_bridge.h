/* SPDX-License-Identifier: MIT
 *
 * Aurora GPU in-process bridge -- the exported handoff by which Honeykrisp
 * (the Vulkan ICD, loaded beneath DXVK beneath this D3D11 UMD, in the SAME
 * process) obtains the D3D runtime's kernel callback table and runtime device
 * handle, so it can render on the RUNTIME's D3DKMT device instead of creating
 * its own.  That collapses the two-D3DKMT-device split documented in
 * umd/compat/tritonResource.c to one VM and one ASID. Ordinary nested resources
 * use the callback directly. Direct scanout is different: Triton atomically
 * creates a donor+adopter pair through its genuine outer callback, then asks
 * Honeykrisp to borrow the returned donor allocation on this same device.
 *
 * WHY AN EXPORTED C ABI, WDK-FREE:
 * Honeykrisp cross-builds with llvm-mingw and vendors only the D3DKMT *thunk*
 * ABI (asahi/lib/agx_d3dkmt.h); it has no <d3dumddi.h> and therefore cannot
 * name D3DDDI_DEVICECALLBACKS.  So this header hands the callback table over
 * as an opaque `const void *callbacks` plus the runtime device handle.
 * Honeykrisp vendors the ~30 D3DDDICB_* structs it needs (exactly as it
 * already vendors the thunk subset) and casts `callbacks` to its own
 * definition.  This mirrors how agx_d3dkmt_load() fills a function table from
 * gdi32.dll today -- only the source of the entry points changes.
 *
 * ACQUISITION MODEL:
 * Honeykrisp's device is brought up by vkCreateDevice nested inside this UMD's
 * tritonCreateDevice (through the renamed-DXVK factory). The common case stays
 * on the publishing thread and uses AuroraGpuBridgeAcquireCurrentThread().
 * DXVK may instead run Vulkan creation on a factory worker; a process-wide
 * registry lets AuroraGpuBridgeAcquireSingleActive() retrieve the sole
 * reserved/live device in that case. Multiple entries are ambiguous and fail
 * closed.
 *
 * This header is the ONLY contract between the two modules; keep it ABI-stable
 * and version every change through abi_version.
 */

#ifndef AURORA_GPU_BRIDGE_H
#define AURORA_GPU_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* v2: adds generation-based revocation. v1 published `&TRITON_DEVICE::
 * KTCallbacks` -- a pointer into the RUNTIME-OWNED private device block, which
 * the runtime recycles across Create/Destroy. A Honeykrisp winsys that
 * outlived its D3D device (measured: LogonUI 0xc0000005 in vulkan_asahi.dll's
 * allocation flow during abnormal XAML teardown, 0.1.340) then called through
 * freed, recycled memory. v2 publishes a pointer to a STABLE process-lifetime
 * copy inside the bridge registry, plus an alive/generation cell the consumer
 * must check before every callback: tritonBridgeUnpublish bumps it, so a
 * stale provider degrades to a clean device-removed error instead of a wild
 * indirect call. */
#ifndef AURORA_GPU_BRIDGE_API
#define AURORA_GPU_BRIDGE_API __declspec(dllexport)
#endif
#define AURORA_GPU_BRIDGE_ABI_VERSION 4u
#define AURORA_GPU_RESOURCE_BRIDGE_ABI_VERSION 2u

/* The name Honeykrisp passes to GetModuleHandleExA to retain this UMD, then
 * GetProcAddress for the acquire entry points below. */
#define AURORA_GPU_BRIDGE_MODULE "AuroraD3D11Triton.dll"

typedef struct AuroraGpuBridge {
    unsigned int struct_size;  /* = sizeof(AuroraGpuBridge) */
    unsigned int abi_version;  /* = AURORA_GPU_BRIDGE_ABI_VERSION */

    /* Opaque `const D3DDDI_DEVICECALLBACKS *`. Most callbacks take hRTDevice;
     * pfnEscapeCb instead takes hRTAdapter and names hRTDevice in its payload. */
    const void *callbacks;

    /* Opaque runtime device handle (D3D10DDI_HRTDEVICE::handle == the runtime
     * hDevice the callbacks require as their first argument). */
    void *hRTDevice;

    /* v2 revocation cell: points at stable process-lifetime storage owned by
     * the publisher. The consumer captures alive_value at acquire and must
     * treat any callback attempt where *alive != alive_value as
     * device-removed. The publisher bumps *alive on unpublish and on slot
     * reuse, so values are never reissued. */
    const volatile long *alive;
    unsigned long alive_value;

    /* v3 in-flight drain. Points at publisher-owned storage alongside `alive`.
     *
     * `alive` alone cannot make a callback safe: the consumer tests it and
     * THEN calls through hRTDevice, and the runtime device can be freed in
     * between. That is not theoretical -- it is the measured dwm.exe crash,
     * a read of freed memory inside d3d11.dll called from the bridge's
     * destroy-allocation path, because DXVK destroys resources on worker
     * threads while the main thread tears the device down.
     *
     * Protocol: the consumer increments this BEFORE testing `alive` and
     * decrements it after the callback returns; the publisher bumps `alive`
     * first and then waits for this to reach zero before letting the runtime
     * device go. A call that got past the test is therefore waited for, and a
     * call that starts afterwards sees the bump and degrades. */
    volatile long *in_flight;

    /* v4: D3D10DDI_HRTADAPTER::handle captured at OpenAdapter. Escape's first
     * callback argument is this runtime adapter, never a D3DKMT integer or
     * hRTDevice. Consumers require this field and hold a module reference for
     * the entire lifetime of pointers into the publisher's static storage. */
    void *hRTAdapter;
    /* PFND3DDDI_QUERYADAPTERINFOCB: independently identifies this runtime
     * adapter before a consumer accepts an enumerated raw adapter/LUID. */
    const void *query_adapter_info;
} AuroraGpuBridge;

/* Synchronous CreateResource handoff.  Triton publishes the real runtime
 * resource on TLS around the nested DXVK/Honeykrisp texture creation.  The
 * bridge asks for it only when Honeykrisp requests CreateResource, then
 * reports the callback's allocation back to the same TLS scope. */
typedef struct AuroraGpuResourceBridge {
    unsigned int struct_size;
    unsigned int abi_version;
    void *hRTDevice;   /* in: expected owner; out: confirmed owner */
    void *hRTResource; /* out: callback resource handle */
    unsigned int allocation_flags_set;
    unsigned int allocation_flags_clear;
    unsigned long long allocation_reserved;
    unsigned int allocation_info_flags;
    unsigned int vidpn_source_id;
    /* v2 compatibility fields. Do not enable these in production. 0.1.622
     * proved that expanding Honeykrisp's nested one-allocation callback makes
     * the KMD create both records but fails to return slot 1's UMD allocation
     * handle (`bad_output=1`). Production direct scanout instead issues a real
     * two-allocation callback in Triton and imports slot 0 into Honeykrisp.
     * Retained only so old bridge binaries fail/diagnose without an ABI break. */
    unsigned int secondary_enabled;
    unsigned int secondary_flags_set;
    unsigned int secondary_flags_clear;
    unsigned int secondary_info_flags;
    unsigned int secondary_vidpn_source_id;
} AuroraGpuResourceBridge;

typedef struct AuroraGpuResourceBridgeResult {
    unsigned int struct_size;
    unsigned int abi_version;
    void *hRTDevice;
    void *hRTResource;
    unsigned int status;
    unsigned int hKMResource;
    unsigned int hAllocation;
    unsigned int secondary_hAllocation;
    unsigned long long allocation_size;
} AuroraGpuResourceBridgeResult;

/* Retrieve the bridge published for the device currently being created on
 * THIS thread (the synchronous nested-create handoff).  Returns 1 on success
 * and fills *out; returns 0 if no bridge is active on this thread or the
 * output ABI does not match.  Non-owning: no release call is required for the
 * current-thread form. */
typedef int(__cdecl *PFN_AuroraGpuBridgeAcquireCurrentThread)(
    AuroraGpuBridge *out);
AURORA_GPU_BRIDGE_API int __cdecl AuroraGpuBridgeAcquireCurrentThread(
    AuroraGpuBridge *out);

/* Retrieve the bridge when EXACTLY ONE device's bridge is reserved or live
 * process-wide. A reservation covers the nested factory interval before the
 * outer D3D CreateDevice can commit, including a Vulkan factory worker thread.
 * Returns 1 and fills *out in that case; returns 0 if zero or more than one
 * are present (ambiguous), or on ABI mismatch. */
typedef int(__cdecl *PFN_AuroraGpuBridgeAcquireSingleActive)(
    AuroraGpuBridge *out);
AURORA_GPU_BRIDGE_API int __cdecl AuroraGpuBridgeAcquireSingleActive(
    AuroraGpuBridge *out);

/* Number of live bridges (diagnostic; lets a consumer distinguish
 * "none published yet" from "ambiguous"). */
typedef unsigned int(__cdecl *PFN_AuroraGpuBridgeCount)(void);
AURORA_GPU_BRIDGE_API unsigned int __cdecl AuroraGpuBridgeCount(void);

typedef int(__cdecl *PFN_AuroraGpuResourceBridgeAcquireCurrentThread)(
    AuroraGpuResourceBridge *out);
AURORA_GPU_BRIDGE_API int __cdecl
AuroraGpuResourceBridgeAcquireCurrentThread(AuroraGpuResourceBridge *out);

typedef void(__cdecl *PFN_AuroraGpuResourceBridgeCompleteCurrentThread)(
    const AuroraGpuResourceBridgeResult *result);
AURORA_GPU_BRIDGE_API void __cdecl
AuroraGpuResourceBridgeCompleteCurrentThread(
    const AuroraGpuResourceBridgeResult *result);

#ifdef __cplusplus
}
#endif

#endif /* AURORA_GPU_BRIDGE_H */
