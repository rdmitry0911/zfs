# Private online parity-conversion engine for PR #19262: validation results, 5-7 November 2026

**Status.** The engine is a private prototype. It is not proposed for merge, and nothing here is a
qualification. The validation graph is **not closed**: open items and pending decisions are listed in
sections 6 and 7. All numbers come from lab records. Most cells ran once, on file vdevs, on a shared
and often loaded host.

## 1. Scope and method

- **Subject.** A private engine that raises RAID-Z parity online (raidz1 to 2, 2 to 3) on top of the
  parity-epoch table of PR #19262. The pool is not exported. Relocated blocks are tracked in an
  engine catalog (journal plus page store), so block pointers are not rewritten.
- **Trees.** Fixes were built as separate layers (L9-L22) and assembled into one integrated tree.
  The final kernel round ran on that tree with L9-L21 and two small follow-ups. L22 was written
  afterwards and has userspace evidence only.
- **Lab.** Four disposable lab VMs (4 to 32 vCPU) on Linux 6.8 kernels, plus host userspace runs
  (ztest and libzpool). Vdevs are files on tmpfs or on the VM disk. A power cut is a hard VM kill.
- **Builds.** Each kernel cell names its module by hash. Two flavours:
  - debug: ZFS_DEBUG on, `-Werror` with the 1,024-byte frame limit;
  - true release: NDEBUG proven on every object. Debug-only VERIFY strings and engine ASSERT
    strings are absent from the binary. `zfs_flags` verifiers are set and read back, and kprobes
    confirm that they ran.
- **Cells.** Userspace suites (floor, journal roll, growth deferral, reseed matrix, the W2/W4/W6/W8
  regression set). Kernel cells: ZTS-based tests, fixture cells, fio, and power cuts. Interleaved A/B
  runs (unfixed against fixed) where a cause had to be attributed.
- **Oracles.** Pool suspension, growth ENOSPC, journal reason codes, birth drift, scrub errors, payload
  hashes, export/import, read-only import with members hidden, and a leak check with a patched zdb
  that knows the engine's deferred frees.
- **Reviews.** Every layer had fresh independent reviews until a round found 0 BLOCKER and 0 MAJOR.
  The assembled trees had cross-reviews. Where a last revision was not re-reviewed, this is noted.

## 2. What was found and fixed

| layer | defect class | evidence | key numbers |
|---|---|---|---|
| L9 | A conversion rebinds a never-written ZIL tail block with a provisional birth. Every import after a later ZIL commit is refused: the pool cannot be imported. | userspace repro; kernel ZTS online, degraded and rewind tests, debug and release | unfixed control refuses import; fixed: 0 refusals |
| L10 | A journal roll ENOSPC was sticky. ZIL fell back to txg sync and the converter stopped until export. | userspace hook cells 4/4; kernel: one natural roll ENOSPC seen | recovery in the kernel witnessed only by injection |
| L11 | **Permanent pool suspension on checkpoint discard** at low catalog headroom: the whole checkpoint space map was retired in one txg. A writable import resumed the discard and suspended again. | kernel: three kept suspended images, a fresh fixture, a power cut mid-discard; true release | 65-66 paced steps, 18-20 s, 0 growth ENOSPC |
| L12 | Twice the uberblocks: empty catalog stores were forced while frees were deferred. | userspace A/B, 4 interleaved pairs; integrated-stack rerun (userspace ztest) | 481 stores vs 641-649; conversion ~13% faster on a quiet host; same 481 on the integrated stack; kernel cells run clean with fewer empty stores (not an A/B) |
| L13 | **Async destroy, snapshot destroy, rollback, long frees and clone livelist destroy at low headroom**: unbounded catalog rows per txg, then suspension. | userspace repro 2/2; kernel A/B | unfixed suspends 1.6 s after the destroy (44,863 blocks in one txg); fixed ≤ 3.5k rows / ≤ 359 pages per txg; power cut mid-destroy resumes, scrub 0 |
| L14 | The checkpoint discard starved under steady client writes. After a crash mid-drain, writers blocked for the whole drain. | kernel, 3 runs per cell | wait 0.9-90.9 s → one step in 12/12; writer blocked 125.6 s → 1,213 writes during a 61 s drain |
| L15 | **Spurious ENOSPC with gigabytes free** after an empty fence (a fence-ordering race). | deterministic userspace repro; kernel A/B | 64/64 ENOSPC → 64/64 restart. Kernel: race on the unfixed tree in 2 of 5 runs, 0 of 7 fixed (consistent, not proof) |
| L15b | Counted arena runs taken by data allocations before the catalog store, then growth ENOSPC and suspension. | userspace knob | 13 of 14 runs bitten: suspends; fixed: 0 growth ENOSPC in 36 runs. No kernel witness yet |
| L16 | A stale `floor=` status field; skipped readmits were not retried. | kernel debug and release (status); userspace (retry) | 0 mismatches in 363 + 234 samples |
| L17 | **Release-only accounting difference**: the conversion reserve shared the native slop and then used it. | userspace bisect; kernel release and debug | after a conversion, fixed +41/+34 MB above the slop, unfixed 4 MB below |
| L18 | **A conversion on a nearly full pool ate the native slop** and ended stuck. The converter starved under 4 writers. | kernel | refused before the epoch, AVAIL unchanged; 4 writers: 6,023 copies on L18 alone (on the integrated tree see A32, section 6) |
| L19 | Catalog churn under a checkpoint could take the catalog down to its last arena run. | userspace; kernel | free pages 2,911 → 17 unfixed vs min 650 fixed, no growth; kernel reserve 1,093 pages, released after the discard |
| L20 | **Journal bind serialization**: every ZIL block's bind took a pool-wide lock and two write+flush rounds, inside the fsync path. | kernel, tmpfs vdevs | zvol 128K, 4 writers: 126 → 400 fsync/s on one VM, 256 on the final tree (2.03x); stock 1,201; power cut 602/602 acknowledged blocks intact |
| L21 | A checkpoint under guest load caused a fence storm and guest EIO: bytes pinned by the checkpoint were treated as releasable. | userspace; kernel fio on a zvol | 8/8 ENOSPC → 10/10 admitted; kernel 0 errors, 79 fences in 61 s (guest record: 476 in 31 s) |
| L22 | A destroy under a checkpoint drew the catalog down to the reserve line, then writers were refused (zvol EIO). | userspace only | 232 writes, 0 ENOSPC (kernel control: EIO on 1,494 of 1,500) |

Closed as non-defects, each with its own record:
- zdb "leaked space" on engine pools is the native space of retired owners that wait for a fence
  (57/57 states). The patched zdb reports "No leaks" and still catches an injected leak.
- A soak's 209 ENOSPC were the L15 race, not the tree under test.
- A failing built-in runtime test was stale after two earlier design changes.
- The release-only full-pool ENOSPC was the slop sharing that L17 fixes.

## 3. Final integrated tree: kernel round

Builds (lab record final-kernel-20261107):
- debug: the `-Werror` 1,024-byte frame limit holds; the engine's conversion routine has a
  1,016-byte frame, 8 bytes under the limit.
- true release: NDEBUG in 9/9 checked units and 306/306 objects; 0 of 4,133 debug-only VERIFY strings
  and 0 of 21 engine ASSERT strings in the binary.

| cell | flavour | result |
|---|---|---|
| ZTS online conversion | debug | PASS |
| ZTS online conversion + rewind | release | PASS; verifier kprobe hits equal entries, 0 missed |
| R-01 hold with the phase-robust check | release | PASS: bounded, one P reclaim, certified parity 3, payload unchanged |
| L13: rm of a file a snapshot holds | debug | PASS, 1.0 s (an earlier revision: 37.7 s) |
| L13: sparse rm at low headroom | debug | PASS, 5.2 s |
| L13: clone livelist destroy | debug | PASS, paced ≤ 364 rows per txg, no growth |
| L19 reserve under a checkpoint, writer first | debug | PASS: armed and released; fio 0 errors |
| L18 conversion on a full pool | debug | PASS: refused before the epoch |
| Power cut during a paced destroy | debug | PASS: resumes, scrub 0 |
| Destroy under a checkpoint, then a writer | debug | **FAIL** (A31): writer refused, zvol EIO 1,494/1,500. Fail-safe: no suspension, everything returned at the discard. Fixed in userspace by L22 |
| Conversion under 4 writers, nearly full pool | debug | **FAIL** (A32): ENOSPC after 2,099 copies. See section 6 |
| L20 group commit | debug | PARTIAL: 2.03x the earlier tree (about 3x was the target) |

## 4. Branches of the original project graph

**Snapshot tree under an online conversion** (lab record snaptree-20261106; debug; one run).
- A raidz1 pool with a snapshot tree was converted online to certified parity 2: 415 s,
  15,383 blocks, error 0.
- The tree had snapshots, a clone, a clone of a clone, a zvol and a zvol clone.
- During the conversion: 4 writers (0 errors), 4 new snapshots and a `zfs promote`.
- Content matched 16/16 during and 20/20 after. Rollback, `send -i` (3/3) and hard-order destroys
  (13/13) passed. Read-only import with 2 of 6 members hidden: DEGRADED, 13/13 matched.
- Limits: the promote and the snapshots landed before any block was copied. No crash cut. Not
  covered: holds, bookmarks, `send -R`, raw send, encryption, dedup, block cloning.

**A nested KVM guest through an online conversion** (lab record guest-20261106; release
module; one complete run).
- ext4 on 16K and 64K zvols with O_DIRECT/O_DSYNC fio load. Online raidz1 to 2 conversion, 53 s,
  17,197 blocks. Export only at the very end.
- Guest I/O errors: 0. e2fsck clean. Hashes equal in the guest, on the VM and after import.
- p99 latency 7-20 ms before vs 120-217 ms during the conversion. Worst stall 5.6 s, equal to one
  converter slice hold.
- This run exposed the checkpoint fence storm that L21 fixes (above). It also measured the
  journal-bind cost that L20 reduces.
- Limits: the VM disk ran with `zfs_nocacheflush=1`; file vdevs on a loaded VM disk; one guest.
  Not run: near-full pools, thin zvols, guest TRIM, several guests, device loss, a power cut.

## 5. What has not been witnessed

- The L21 fix inside a nested guest (it was witnessed with fio on a zvol on the VM).
- L22 in the kernel, and L22's last revision has not been re-reviewed.
- L15b in the kernel.
- The full userspace regression set on the final tree in both flavours (it ran on the earlier
  stack and after L17).
- L13 in a release module; a recovery from a natural journal-roll ENOSPC in the kernel.
- A crash-cut hook on the L20 group path (one power cut fell between groups).
- Holds, bookmarks, raw send, encryption, dedup and block cloning together with a conversion.
- Scale and long soaks: no pool above 18 GiB, no real disks, no soak above 3,600 s.

## 6. Limits and open items

| item | state |
|---|---|
| A32: conversion under 4 writers on a nearly full pool | Documented as a limitation. The outcome is fail-safe and the conversion can be resumed. Probable mechanism, by reasoning, not isolated: worst-fit data allocations take the largest free piece, so they consume whole 24 MiB catalog runs, and the catalog has no run left to grow into. A run-preserving data allocator or fallback arenas would remove this dependence; that is a later, separate step. |
| Fragmented pools | A pool fragmented below one free 24 MiB run, or with a full catalog, refuses client writes with ENOSPC while native space remains. |
| Catalog reserve under a checkpoint (L19) | The reserve buys time; it does not bound catalog churn. While the checkpoint exists, its discard is the only bound. |
| L20 single-dataset case | With one ZIL issuer there is nothing to group: 4K fsync in one dataset is unchanged (1,490 vs 1,398 fsync/s). The remaining group-write cost is parked; L20 is accepted at 2x. |
| L22 writer throttle | With a destroy waiting under a checkpoint, writers are admitted with 0 ENOSPC but run at about 4 writes/s of 100 asked (userspace). The cause is not isolated. |
| L13 minor findings | Blocks from the last unsynced txgs are not paced. With ≤ 2 free runs under write load, a destroy waits for writes to pause. A direct fence at a near-zero budget can stop early, and certification then returns EBUSY (fail-closed). A small clone-livelist bookkeeping residual is unpaced. |
| Under a checkpoint | Certification is refused, the converter ends EBUSY, and P is reclaimed after the discard. Writable rewind to the checkpoint is refused for engine pools. |
| Scope | Linux only; RAID-Z only; one translated vdev per pool; parity 1 → 2 → 3. |

## 7. Decisions still pending

These are open on the project side. Each has a written recommendation, and none is decided here:
- whether to enable exact-leaf reseed for disk replacement by default, and its scope;
- the RAM accounting rules, and an exclusion class for two census sites whose kernel precondition
  cannot be met;
- whether to adopt the R-01 behaviour under a checkpoint (above) as the contract;
- the legacy catalog codec 5 (read-only today) and lab pools written before the L9 fix (refused);
- the product surface (CLI, feature flag, persisted requests): waits for the maintainer
  questions in the joint design;
- the platform and topology scope (above);
- keeping userspace-only test hooks in the tree (they are not built into the kernel module);
- an upstream report about a base `zinject -r` debug assertion (not engine code).

Already decided: the run dependence (A32) is documented as a limitation; L20 is accepted at 2x.

Open external items: the joint design review, Discussion #19714, the translation-scope (D1)
question, and the PR #19262 review.

## 8. Lab records cited

final-kernel-20261107, stack-v8-20261107, l19-ckres-20261107, l22-ckdestroy-20261107,
snaptree-20261106, guest-20261106, jbind-20261106, l20-jbind-20261106, l21-ckguest-20261106,
l11-ckpt-discard-20261106, l12-store-force-20261106, l13-bulk-retire-20261106, l13-kernel-20261106,
l14-kernel-20261106, l15-fence-stall-20261106, l15-kernel-20261106, l15b-runs-20261106,
l16-kernel-20261106, l17-relj-20261106, l17-kernel-20261106, l18-convres-20261106,
zdb-leak-20261106, w8-enospc-ab-20261106, sfhold-robust-20261106, stack-relckpt-20261106,
rebind-birth-fix-20261105, w10-zilfix-20261105.
The records are private lab records. They hold the commands, module hashes, logs and limits of
each cell.
