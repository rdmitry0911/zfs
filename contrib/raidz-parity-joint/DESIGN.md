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

Draft contribution by rdmitry0911, 26 October. The redirect column follows
[tigerblue77's 25 October reply](https://github.com/tigerblue77/zfs/issues/2#issuecomment-6399029897)
and awaits his corrections. These are comparison baselines, not a jointly
selected implementation. Neither author has independently reproduced the
other's results yet.

Both mechanisms must preserve snapshot block pointers and support copying
keyless encrypted blocks as stored. A rewrite of exclusively owned head
data may allocate a new DVA through the normal write path; that does not
permit rewriting snapshot metadata or treating the rewrite's completion as
proof that every allocated old-layout extent has been covered.

| Concern | Rewrite heads plus redirects | Virtual identity and physical backing |
| --- | --- | --- |
| Current policy | Rewrite eligible head-owned blocks; redirect blocks the rewrite cannot reach | Relocate backing beneath unchanged BPs for all populations; new allocations also get a V/P pair |
| Table population | Entries for redirected blocks until their final free; hybrid dedup coverage and BRT exclusion still need the stated fixes/tests | Rows for materialized owners, including rewritten/new allocations and retained retirement state; untouched legacy extents use an identity range tree |
| Lookup | Offset and physical birth identify the redirect; read, free and claim paths must agree | Logical V names an allocation incarnation; dispatch selects the current physical version and its own birth |
| Sharing | Dedup original is redirected. BRT-shared heads need exclusion from ordinary rewrite, as proposed by tigerblue77 | Original BPs and DDT/BRT references remain unchanged when backing moves |
| Free and reuse | Keep the required sectors of O, free its tail, free N and remove the entry at final reference; fragmentation remains a cost | Retire the old physical version and keep V/P ownership until an ordinary durable floor and all applicable holds/leases/copies have drained |
| In-flight I/O | Sorted scans look up at issue; ordinary reads need the proposed range-lock protection before the tail is reusable | Logical holds retain the allocation incarnation; dispatch leases retain the selected physical version; fresh requests to retired identities fail |
| Durability/bootstrap | Per-vdev MOS log; MOS rewrite/bootstrap and keyless encrypted ZIL remain decision/work items | Raw replicated catalog and journal found through experimental uberblock/label metadata before translated MOS reads; ACK bindings precede log acknowledgement |
| Rewind | Ordinary defer window is the present protection for bounded rewind; low-space/reflow gaps and extreme rewind need an agreed rule | Durable floor refuses roots older than released backing; current prototype also retains native checkpoint roots and pins reclaim |
| Space | Pool-level charge, unchanged dataset dsize; migration reservation is still to be built | Physical credits and standing growth reserve drive admission; public quota/reservation/AVAIL promises still need agreement and qualification |
| Completeness | Current walk census must become an allocation-based inventory covering pending frees and async destruction | Owner/identity inventory reconciled with native allocation state; durable zero-weak census gates the stronger certificate |
| Implementation status | Userspace libzpool/file-vdev prototype; no kernel or ZTS qualification claimed by its author | Userspace and a Linux kernel experiment; finite fixtures and exact-source checks, with remaining activation/repair/resource gaps |

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

## 3. Decision points

Writes: tigerblue77. Reviews: rdmitry0911.

## 4. Measurements on one fixture

Each author for their own prototype; both review.

Draft measurement contribution by rdmitry0911, 26 October. The common
comparison has not been run. Existing numbers below establish what needs
to be compared, not a winner.

Proposed common fixture and reporting contract:

1. Use the same VM, source prerequisites, release/debug choices, file-vdev
   geometry, ashift, block sizes, dataset mix, cache limits and background
   load. Start from equivalent data and retention state; retain each
   prototype's exact source manifest and actual command lines.
2. Include head-only archives, retained snapshots, a snapshot clone,
   dedup-shared and BRT-shared data, and an encrypted dataset whose key is
   unloaded. Start with the existing 70/30 mix for continuity; vary the
   retained/shared fraction and use both 128 KiB and 16 KiB records. Count
   physical allocations and sharing explicitly, not just logical files.
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

The measurements that would justify choosing the hybrid default are fewer
retained rows and lower full retained/peak memory on head-heavy archives,
without losing sharing or violating the lifetime/recovery/space checks,
and with acceptable copy work, latency and fragmentation. The other cases
determine its limitations and budgets. A smaller table alone cannot prove
the missing safety paths; a larger table alone cannot justify retaining
rows that the common design can avoid.

Historical author-run observations, on different fixtures:

| Observation | Qualification |
| --- | --- |
| Redirect author's 70/30, 128 KiB hybrid versus all-redirect: 788 versus 2608 entries, 3.58 versus 5.19 seconds | Reported in issue #2; some timing difference comes from I/O style. Not a comparison with the virtual/backing implementation |
| Virtual/backing walk: approximately 4 seconds against a 4–5 second scrub; conversion 97–121 seconds on 2.6 GiB | Earlier stock Linux 6.8 release experiment and source/cache settings; not rerun on the common fixture or claimed for this publication head |
| Virtual/backing public 369 B per allocation at leaf cap 512 | Historical partial source/fixture/cache measurement. It is not the complete current allocator charge or peak bound. Subsequent linked-record counters also leave transient/native/shared costs open |
| Virtual/backing current activation tests | Exact-source Linux 6.8.12 build, named native/kernel activation and startup-admission cases, checkpoint and native snapshot/clone turnover. These are finite correctness observations, not a common performance run or full repair qualification |

The comparison source includes a small native conversion/cold-import
script, `contrib/raidz-virtual-backing/smoke.py`, and its 46-file source
manifest, published for review in [prototype PR #4](https://github.com/tigerblue77/zfs/pull/4).
That smoke run is a starting point for reproduction; the mixed
shared fixture and both prototypes' performance/resource collectors must
still be agreed and run. Large author-local raw fixtures and detailed
failure logs are not embedded in this document.

## 5. Landing order

Writes: tigerblue77. Reviews: rdmitry0911.
