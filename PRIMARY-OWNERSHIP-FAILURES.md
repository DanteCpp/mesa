# J813 primary producer ownership, before implementation

This extends the closed resource milestone, not frozen hardware candidates.
The selected producer is the Honeykrisp D3DKMT backend at ad6e0e2. Its existing
v3 read set is not a write set: it includes every CPU-mapped/shared BO, including
an idle scanned-out front buffer. Treating that vector as writes would prevent
back-buffer rendering. Current hk_queue.c emits flags=0, so the dormant
WRITE_SET_COMPLETE flag cannot be taken as existing evidence.

## Proposed narrow real contract

Use the existing adapter identity submit_private_max_version negotiation.
A new v6 record carries the existing complete read vector plus a bounded,
canonical primary-write vector. It names actual GPU virtual address ranges,
never PFNs/private handles. The per-command producer declaration travels beside
the unchanged single render/compute command; batching must preserve the exact
write vector for each split command. No new firmware command or invented
attachment payload is sent. Old v1/v3/v5 operation stays unchanged unless the
new primary ownership mode is explicitly selected by the driver.

Record destinations while the command is recorded, before mutable render state
changes. Whole image allocation spans are conservative and legal. Ordinary
render targets, API image clears and copies have known destinations. A shader
storage write, transform feedback, descriptor-address store, unknown meta
operation, ambiguous alias/sparse image or incomplete destination set refuses
the new mode before submission; it never silently drops work or declares an
unknown write complete. Internal scratch writes are distinct from writes to
runtime primary backing. Scope must be tied to owning entry paths, not inferred
from a command's type or a later successful image.

KMD resolves each declared range through the admitted VM's actual live UAT and
retains allocation-owned backing generation before queue admission. Ordinary
non-primary direct pages are not invented primary objects. Primary references
remain attached to queued/in-flight submissions, including DestroyDevice
handoff. Mark write ownership only at publication, clear it only after genuine
queue10/11/installed12 retirement, and preserve uncertain ownership on loss.
Display acquisition refuses outstanding primary writers and GPU write admission
refuses a DCP lease. Paging/free cannot replace a referenced backing generation.
D590 old-surface retirement releases only DCP ownership, not GPU completion.

## Failure cases that must drive the implementation/E2E

1. Old adapter version: byte-exact old submit contract; no v6 optimistic fallback.
2. Advertised v6 but missing/malformed declaration, unknown flags, oversized,
   wrapped/noncanonical ranges or truncated vector: refuse before any doorbell.
3. Two command buffers with changing render targets; split submits and repeated
   command buffers: each GPU command keeps its own captured destination ranges.
4. Clear/copy implemented as compute, graphics or several internal streams:
   every affected stream inherits its actual API destination. A mixed unknown
   dispatch cannot upgrade to complete because a later known copy adds a range.
5. Shader storage/BDA/transform-feedback/unknown meta and unsupported sparse or
   alias destination: explicitly incomplete/refused, not a zero-write success.
6. Destination range straddles allocations/pages, is unmapped/read-only, changes
   mapping generation, names the wrong VM or stale allocation: refuse all before
   commit; undo only unpublished references, preserve any published ownership.
7. A page belongs to a primary retained by DCP: refuse GPU writer admission.
   A primary with queued or in-flight writes cannot be mapped/presented to DCP.
8. Two queued writes to one primary retain separate references; retiring the
   first does not permit scanout/free while the later command can still write.
9. Failed publish before doorbell unwinds the unpublished set; an uncertain
   doorbell, queue failure, timeout or changed backing quarantines references.
10. Allocation/device/process destroy and retained-job handoff cannot discard
    write references. Actual completion terminalizes once before reclamation.
11. CPU-visible/cached/mutable primary aliases cannot be silently accepted.
    Initial primary shape remains GPU-only, stable allocation-owned noncached
    backing; no escape allocator or VidMm-cookie-as-PFN shortcut.
12. Native panel1040 pages, multiple surfaces and bounded set exhaustion: refuse
    before partial publication; no small-buffer-only qualification claim.
13. FULL remains closed until producer, standard allocation/paging, single-owner
    routing and cadence are all complete. Successful offline write tracking is
    not primary/scanout/DWM hardware qualification.
14. A primary used only as a texture/copy source still owns backing through GPU
    completion. Retain primary entries from the complete read vector separately
    from writes. Read leases may coexist with DCP readers; only write-reference
    counts exclude scanout and advance completed-write history. A range appearing
    in both vectors holds two references and releases each exactly once.
15. Bound both read and write page walks (16,384 native pages per vector). A queued
    ordinary mapping becoming a primary before publication must refuse, including
    same-PA reclassification; validate every declared page again before publish.

## Evidence

Use actual command-recording/queue splitting and KMD admission/retirement
bodies with modeled OS/firmware boundaries, then compile both Windows ARM64
components. Reuse the existing source-extraction and G17 lifecycle workflows.
Save input/source hashes, complete per-case traces and mutated omission failures.
Do not add a parallel generic test framework or weaken the existing image,
fence, allocation or scanout gates.
