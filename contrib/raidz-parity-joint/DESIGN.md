# Raising RAID-Z parity in place: joint design

Working document for one design, written jointly by @rdmitry0911 and
@tigerblue77, before anything is proposed upstream. Discussion:
https://github.com/tigerblue77/zfs/issues/2

How this branch works:

- This file is changed only through pull requests into `parity-joint`,
  reviewed by the other author.
- Each prototype comes in as its own pull request, for side-by-side
  review. Prototypes are not merged here; only this document is.
- Anything stated as "mine does X" is "mine does X in that fixture"
  until the other side has run it.

## 1. Problem and constraints

Writes: tigerblue77. Reviews: rdmitry0911.

## 2. The two mechanisms

Writes: rdmitry0911; tigerblue77 corrects the column describing the
redirect prototype. Each reviews the other's column.

Draft contribution by rdmitry0911, updated 31 October. The redirect column
follows [tigerblue77's source-qualified review of 28 October](https://github.com/tigerblue77/zfs/pull/5#discussion_r4404987395), at
`spike/bypass-free` `094d2a05f9` with upstream references at `ea111ca50f`.
These are comparison baselines, not a jointly selected implementation.
Neither author has independently reproduced the other's results yet.

Both mechanisms must preserve snapshot block pointers and support copying
keyless encrypted blocks as stored. A rewrite of exclusively owned head
data may allocate a new DVA through the normal write path; that does not
permit rewriting snapshot metadata or treating the rewrite's completion as
proof that every allocated old-layout extent has been covered.

| Concern | Rewrite heads plus redirects | Virtual identity and physical backing |
| --- | --- | --- |
| Current policy | Hybrid probe: rewrite the one head's plain-file level-0 blocks (logical birth after its last snapshot, key loaded), logical birth kept; redirect every other old DVA outside the MOS and the ZIL, except gang DVAs (counted, not moved); MOS rewritten in one probe only | Relocate backing beneath unchanged BPs for all populations; new allocations also get a V/P pair |
| Table population | One entry per redirected DVA, keyed by offset and physical birth, until its last reference is freed, then an in-memory tombstone until its extents are reusable. Dedup under the hybrid not witnessed; BRT exclusion not built | Rows for materialized owners, including rewritten/new allocations and retained retirement state; untouched legacy extents use an identity range tree |
| Lookup | Offset and physical birth map to N, its birth and sizes. Every raidz I/O, the free, zdb's claims, scan routing, the resilver check and the root mirror look it up, lock-free, skipped when no entry can match; not on allocation | Logical V names an allocation incarnation; dispatch selects the current physical version and its own birth |
| Sharing | A dedup or BRT-shared block is redirected once, its BPs and DDT/BRT entries unchanged, and freed at the last reference (witnessed). Hybrid: dedup goes to the census round (not witnessed); BRT-shared head blocks would be rewritten and un-shared, as the `brt_maybe_exists()` exclusion is not built | Original BPs and DDT/BRT references remain unchanged when backing moves |
| Free and reuse | At the move, keep 1 + ((s - 1) mod (p + 1)) of O's s sectors, p the new parity, and free the tail as a deferred free; at the last reference, free N and the kept sectors. Cost: a tail cannot hold a same-size block at the new parity while the block lives | Retire the old physical version and keep V/P ownership until an ordinary durable floor and all applicable holds/leases/copies have drained |
| In-flight I/O | The move writes N, adds the entry, frees the tail; a read that missed the entry reads O, intact for `TXG_DEFER_SIZE` txgs. Every I/O looks up at issue; a DVA already moved bypasses the sorted queues; scan and resilver I/O end within their txg; tombstones route late I/O to N. Unprotected: reads past the defer window, txgs that defer no frees | Logical holds retain the allocation incarnation; dispatch leases retain the selected physical version; fresh requests to retired identities fail |
| Durability/bootstrap | A MOS log of 40-byte records, loaded right after the epoch table, and a per-entry block-pointer ZAP that only the scan reads (to be replaced); a move and its entry commit in one txg. MOS and ZIL never redirected; the hybrid census omits the MOS; MOS rewrite built in one probe; keyless ACK-bearing encrypted ZIL not handled | Raw replicated catalog and journal found through experimental uberblock/label metadata before translated MOS reads; ACK bindings precede log acknowledgement |
| Rewind | No floor: -F stays inside the defer window; -X and -T may find a tail reused, and the block then fails its checksum, as after any free. Gap: txgs that defer no frees. A move is refused while a checkpoint exists | Durable floor refuses roots older than released backing; current prototype also retains native checkpoint roots and pins reclaim |
| Space | Pre-flight built: ENOSPC unless the end state and the packed new extents fit above the slop. Pool-level charge from the table, subtracted in `spa_update_dspace()`, so the DMU refuses on it; dsize unchanged. No reservation during a move | Physical credits and standing growth reserve drive admission; public quota/reservation/AVAIL promises still need agreement and qualification |
| Completeness | Walk census is test code and does not change reported tolerance; an allocation-based inventory must cover pending frees and async destruction | Owner/identity inventory reconciled with native allocation state; durable zero-weak census gates the stronger certificate |
| Implementation status | Userspace libzpool on file vdevs, debug build, scale runs also with a release libzpool (`LIBZPOOL_CPPFLAGS` overridden); never kernel-built; no ZTS, no real disks; mover driven by probes; no feature flag; vdev_raidz.c +2,866 lines net, 823 more in 12 other files, on 94407d4003. Hybrid fixture: one unencrypted head plus one snapshot, plain-file objects; under the hybrid, no gang, dedup or clone case (the all-redirect probes move gang headers and children) | Userspace and a Linux kernel experiment; finite fixtures and exact-source checks, with remaining activation/repair/resource gaps |

Proposed convergence to measure: normal rewrite for exclusively owned,
key-loaded heads, redirects for snapshot/clone-held, dedup/BRT-shared and
keyless populations, and identity mapping without persistent rows for new
or untouched blocks. The virtual/backing prototype published for comparison
does not implement this row-sparse policy yet.

Choosing rewrite as the default does not require a preceding relocate-all
pass. The shared question is whether the chosen paths satisfy the same
lifetime, recovery, reservation and allocation-census requirements. Leases
versus a range lock, floor policy, journal/bootstrap and checkpoint policy
remain open. No acceptance of one author's complete format follows from
agreement on the hybrid's row-count advantage.

Proposed integrated-feature contract (review of section 3 point 5 and
section 5 step 6): epochs, mapping and the allocation-backed completion
certificate should land under **one new, non-read-only-compatible pool
feature**. The separate experimental epochs feature in the unmerged
#19262 would be superseded in that joint series. This is a proposed
upstream format decision, not a change to #19262 or a reuse of its GUID
with a meaning that an older epochs-only reader would mistakenly accept.

One feature gives maintainers and administrators one import/refusal and
qualification contract and avoids deploying the incomplete epochs-only
format as a supported intermediate state. The costs are coupled delivery
and compatibility, no independent supported epochs-only deployment, and a
more demanding drain/deactivation proof. Independent observation and
no-format-change refactors can still land separately. Future incompatible
format changes still need a version/feature contract that older readers
will reject; the single flag is not permission to extend the format freely.

Activation must be durable before the **first incompatible epoch table,
mapping, bootstrap or journal state**, with the guard visible before any
translated MOS access, including crash/import cuts. Use the MOS/label
feature guard so unsupported readers refuse both read-write and read-only
imports. Keep the feature active while *any* durable state needs it across
all affected vdevs and roots: empty mapping alone is insufficient. Epoch
layouts, checkpoints, retained/retired copies, recovery journal and durable
floor/certificate state can outlive the last mapping row. The initial
design may conservatively stay active; deactivation requires a separately
proved transition to a fully legacy-readable layout, including all retained
and recoverable roots. Feature activation and the zero-weak certificate
that permits reporting stronger parity are separate events.

## 3. Decision points

Writes: tigerblue77. Reviews: rdmitry0911.

## 4. Measurements on one fixture

Each author for their own prototype; both review.

Draft measurement contribution by rdmitry0911, updated 31 October. Each
author has now run their prototype on the core common head/snapshot
generator. The results below retain the different counting domains and
measurement environments. The complete qualification protocol that follows
has not been met by these core runs.

Proposed common fixture and reporting contract:

1. Use the same VM, source prerequisites, effective library/driver debug macros, file-vdev
   geometry, ashift, block sizes, dataset mix, cache limits and background
   load. Start from equivalent data and retention state; retain each
   prototype's exact source manifest and actual command lines.
2. Begin with common userspace libzpool/file-vdev cases at 0%, 10%, 30%
   and 100% snapshot-retained user payload, each at 128 KiB and 16 KiB.
   Keep total logical payload constant; metadata/physical overhead is
   measured separately. Extend with a snapshot clone, dedup- and BRT-shared
   data, and an encrypted dataset whose key is unloaded. Count physical
   allocations and sharing explicitly. Missing capabilities are pending or
   refused cases, never passes. Keep kernel evidence separate.
3. Report walk-only time against scrub, conversion wall time and bytes
   read/written/flushed at stated batch/group/cache settings. Alternate
   runs and retain variability. Separate queueing/I/O-style effects from
   the choice to rewrite or redirect.
4. Report table rows, retained sectors, catalog/journal bytes and complete
   incremental allocator charge at idle, during conversion and after a
   cold import. Record the simultaneous peak and the matching allocation
   denominator. Separate retained/shared/native costs, cache limits,
   transient holds/copies and allocator slack. Include post-move churn,
   final-reference deletion and a subsequent ENOSPC/gang check.
5. Extend both mechanisms with the same offline-child move, return and
   resilver, then replacement case, checking data and copy-birth DTL
   selection. Record which subset has actually run. Hold an ordinary read
   across the reuse boundary, including the no-deferral case; a passing
   scrub alone does not establish ordinary-read lifetime safety.
6. Include gang and multi-DVA cases, keyless ACK-bearing ZIL, crash cuts
   through activation/move/free/acknowledgement, and an initially too-full
   pool. Check refusal before mutation and the ability to complete an
   admitted move; do not substitute preflight for ongoing reservation.
7. With one child missing, then as many as the tolerance in force
   allows, cut each durable mapping, bootstrap, journal, floor and
   completion update before and after it is published, both before and
   after completion, and repeat each cut with a returned member still
   stale. At each cut, import read-only and writable, return the missing
   member stale, and separately replace it. Record which imports and
   updates are allowed or refused, against the quorum (raw replicas) or
   the native rules (MOS) selected for that stage (section 3, point 16).
   Check that no stale copy becomes authoritative and that the returning
   or replacement member ends with every required record repaired: zdb
   clean, a scrub that repairs nothing, a per-member check that it holds
   a current copy of every raw record, and a cold import with as many
   other members hidden as the tolerance allows. Record which subset has
   actually run.

The measurements that would justify choosing the hybrid default are fewer
retained rows and lower full retained/peak memory on head-heavy archives,
without losing sharing or violating the lifetime/recovery/space checks,
and with acceptable copy work, latency and fragmentation. The other cases
determine its limitations and budgets. A smaller table alone cannot prove
the missing safety paths; a larger table alone cannot justify retaining
rows that the common design can avoid.

Use device removal's cited 50-100 MB/TB only as a provisional budget
reference. Select the joint budget from the complete measured charge,
record-size distribution and retained fraction, including peaks and slack.

Build qualification: `config/Rules.am` forcibly sets
`LIBZPOOL_CPPFLAGS=-DDEBUG -UNDEBUG -DZFS_DEBUG` for libzpool and its users.
`configure --disable-debug` alone therefore does not produce a true release
libzpool. Rebuild libraries and every libzpool consumer with matching
`LIBZPOOL_CPPFLAGS='-UDEBUG -DNDEBUG -UZFS_DEBUG'`, and retain actual compile
commands and effective macro dumps. Our historical native "release" runs
without that override remain correctness observations for a configure
flavour with libzpool debug enabled, not true-release performance evidence.

### Core head/snapshot results on the common generator

Each entry below is a cold readonly count after the move, then a cold
readonly count after the snapshot-deletion lifecycle. The first two
columns are the redirect author's 29 October
report (three true-release runs per case); the third is the virtual/backing
author's 31 October true-release result (three runs per case). The shared
generator is unchanged at `6319a29f86a9771c62a4c59b029c9c18a839998b`:
five 256 MiB file vdevs, 12.5 MiB logical payload, compression off, copies=1.

Virtual/backing completed 48/48 conversions: all eight cases, three repeats
in debug and three in true release. All 384 native commands succeeded;
192 readonly operations preserved all five member hashes, and a separate
author-side audit replayed 768 process logs and independently checked 792
payload-verification records. Every post-conversion/deletion state had
certified parity 2 and zero legacy/weak backing. The
[full report, raw JSON and logs](https://github.com/rdmitry0911/zfs/blob/c3e59b5d3cb1a9656274aa6c12976b56e85ad343/contrib/raidz-virtual-backing/common-fixture/results-20261031/RESULTS.md)
give medians, ranges, source/build identities and verification limits.

| Input | Hybrid redirect entries | All-redirect entries | Virtual/backing current bindings |
| --- | --- | --- | --- |
| 128 KiB, 0% retained | 0 → 0 | 116 → 116 | 269 → 278 |
| 128 KiB, 10% retained | 26 → 0 | 132 → 106 | 291 → 277 |
| 128 KiB, 30% retained | 46 → 0 | 132 → 86 | 291 → 257 |
| 128 KiB, 100% retained | 130 → 14 | 130 → 14 | 289 → 185 |
| 16 KiB, 0% retained | 0 → 0 | 816 → 816 | 969 → 978 |
| 16 KiB, 10% retained | 96 → 0 | 832 → 736 | 991 → 907 |
| 16 KiB, 30% retained | 256 → 0 | 832 → 576 | 991 → 747 |
| 16 KiB, 100% retained | 830 → 14 | 830 → 14 | 989 → 185 |

The counts have different coverage: virtual/backing includes MOS and new
paired metadata allocations, while the redirect table excludes MOS. Its
`current_versions` counts active bindings; `owners` additionally includes
retired owners and is not used as the live count here. A writable no-op
drop at 0% retention increases the virtual/backing current count by nine
through normal metadata/fence/export work, so the before/after delta does
not isolate the deleted snapshot population. Raw reports retain hot/cold
current, retired and total owners and the physical-space census.

Virtual/backing reserves 20 MiB for raw catalog arenas and 5 MiB for the
initial replicated journal on these five children. These are reserved
on-disk extents, not live table payload or RAM. The reported heap maximum
is an observed 1 ms glibc `uordblks` sample including ARC/dbufs, with RSS
separate; complete incremental allocator/kernel charge remains open.

Starting virtual/backing inputs have `physical_rewrite` enabled before
the mover. The redirect author's feature-state-alignment rerun is pending.
VM hardware, ARC limits, mover boundaries and sampler intervals differ
between authors; the times in the reports do not prove a three-way speedup.
Standalone BP-walk/scrub timing, flush counts, ENOSPC/churn and the full
resource and extended failure matrices above remain separate obligations.

Sources: [redirect report at 98dc0fa](https://github.com/tigerblue77/zfs/blob/98dc0fa/contrib/raidz-redirect-spike/fixture-results-20261029.txt),
[virtual/backing source and reproduction commands](https://github.com/rdmitry0911/zfs/tree/comparison/vb-common-fixtures-20261031/contrib/raidz-virtual-backing/common-fixture).

Historical author-run observations, on different fixtures:

| Observation | Qualification |
| --- | --- |
| Redirect author's 70/30 hybrid versus all-redirect, 5 x 320 MiB raidz1 to raidz2 at ashift 12, compression off: at 128 KiB, 788 versus 2608 entries, 3.2-3.9 versus 4.6-5.6 seconds, later 128 KiB writes 0 versus 27 % gang; at 16 KiB, 6170 versus 20548 entries, 13 versus 36 seconds | Prototype README and the message of 094d2a05f9 (issue #2 gave single runs within these ranges); debug libzpool; the hybrid probe's census leaves the MOS out; some timing difference comes from I/O style (the redirect reads uncached, the rewrite prefetches). Not a comparison with the virtual/backing implementation |
| Redirect table at 1.7e7 synthetic entries on a sparse 20 TiB raidz1: 50.0-52.8 B per entry in RAM, load 2.44-3.24 s, lookup p99 0.55-0.77 µs; reads it does not redirect within 1.5 % of the base with one thread, 1.3-3.1 % slower with four | Prototype README; synthetic entries without block pointers; table memory only (malloc's in-use bytes), not a complete allocator charge; userspace file vdevs, ashift 12; the read costs are from a debug build, where only the full table's +3.1 % is resolved |
| Virtual/backing walk: approximately 4 seconds against a 4–5 second scrub; conversion 97–121 seconds on 2.6 GiB | Earlier stock Linux 6.8 experiment and source/cache settings; not rerun on the common fixture or claimed for this publication head |
| Virtual/backing public 369 B per allocation at leaf cap 512 | Historical partial source/fixture/cache measurement. It is not the complete current allocator charge or peak bound. Subsequent linked-record counters also leave transient/native/shared costs open |
| Virtual/backing current activation tests | Exact-source Linux 6.8.12 build, named native/kernel activation and startup-admission cases, checkpoint and native snapshot/clone turnover. These are finite correctness observations, not a common performance run or full repair qualification |

The comparison source includes a small native conversion/cold-import
script, `contrib/raidz-virtual-backing/smoke.py`, and its 46-file source
manifest, published for review in [prototype PR #4](https://github.com/tigerblue77/zfs/pull/4).
The neutral head/snapshot generator is published separately on
[the common-fixtures branch](https://github.com/rdmitry0911/zfs/tree/fixtures/raidz-parity-common-20261026/contrib/raidz-parity-fixtures).
It creates equivalent logical inputs without invoking either mover; its
baseline creation/read verification does not establish a migration result.
The core adapter and measurement reports are published on the separate
[virtual/backing comparison branch](https://github.com/rdmitry0911/zfs/tree/comparison/vb-common-fixtures-20261031/contrib/raidz-virtual-backing/common-fixture).
The mixed shared extensions and complete resource/failure collectors
remain open. Large author-local raw fixtures are not embedded in this
document.

## 5. Landing order

Writes: tigerblue77. Reviews: rdmitry0911.
