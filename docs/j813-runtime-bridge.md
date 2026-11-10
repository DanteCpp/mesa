# J813 runtime bridge candidate

This is an offline source/build candidate for the single T8142/G17G/M5 adapter.
It does not register a UMD, change the kernel/INF, access hardware, or qualify
DWM, cold compute, resize, shared/primary allocation, presentation or smoothness.

The current Mesa port uses ABI4 with exact Win64 size64, distinct runtime adapter
and device identities, generation plus callback leases, owned publisher module
references, callback-backed device operations and typed device op4 Escape.
Adapter-only raw operations remain raw. The runtime adapter is independently
queried and matched to a complete, uniquely qualifying raw-adapter enumeration;
unknown/ambiguous hardware render adapters refuse. Each distinct handle from a successful bounded enumeration is
closed once; failed-call outputs are not assumed owned. Raw resource open/export fallback cannot consume a bridge sentinel;
real runtime-owned imports use the already opened allocation and trusted query.

Shared process-root main/USC/replay allocation retains current G17 firmware
holes, fixed USC reservation, low-allocation direction and unbind-before-reuse.
Fixed content is initialized before alias publication. Alias handover callbacks
run outside the arena lock while explicit transaction participants stay alive.
Uncertain retirement quarantines later submissions and admissions. Reentrant
claimant teardown deliberately preserves its object/stack until process exit;
it cannot be mistaken for successful cleanup. See the preimplementation and
reviewed failure model in gpu/honeykrisp-port/bridge/FAILURE-MODES-g17-runtime-port.md.


The paired Aurora source and repeatable offline replays live in its
`integration/agx-runtime-bridge` directory. This Mesa worktree ports the bridge
onto the current G17 layout; do not apply the historical G13/G14 overlay script
on top. `HONEYKRISP_RT_DEVICE=1` explicitly selects the bridge. The default
nonbridge D3DKMT path retains its existing kernel device ownership.
