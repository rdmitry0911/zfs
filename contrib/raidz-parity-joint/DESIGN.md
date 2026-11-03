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

Draft by tigerblue77, 26 October. The goal and the four constraints are
those of #2, which rdmitry0911 signed "as written" on 21 October; the
second now names the references it preserves, as his review of 27
October asked. The requirements after them collect what each author has
since asked of either mechanism; section 3 holds the choices of how to
meet them.

**Problem.** The parity-epoch table of openzfs/zfs#19262 writes the new
blocks of a RAID-Z top-level vdev at a higher parity, but the blocks
written before keep the old one, and the vdev reports the lowest parity
of any layout it has held (`vdev_raidz_min_nparity()` in that series).
Its tolerance therefore does not change. amotin's 16 October review of
openzfs/zfs#19262 asked to "first present/discuss what and how you are
going to implement, then implement it"; this document is meant as that
presentation.

**Goal.** Raise the parity of an existing RAID-Z top-level vdev online,
at the same width, and report the new tolerance only once no DVA on it
is left at the old parity.

**Constraints.**

- No narrowing, no lowering. Narrowing in place is impossible
  (openzfs/zfs#9013). A parity change combined with an added disk is out
  of scope.
- No block pointer rewrite. Snapshots are immutable: no block pointer,
  checksum, MAC or IV changes. That covers every reference a snapshot
  or clone origin holds, every one shared through dedup or BRT, and
  every one in a dataset whose key is not loaded. A head with its key
  loaded may still rewrite a block it owns alone through the normal
  write path, which replaces its block pointer as any write does
  (point 2 of section 3) and rewrites no snapshot metadata.
- Rewrite alone is not enough. It cannot reach blocks that only
  snapshots hold, or encrypted data whose key is not loaded, and one
  block left behind voids the guarantee (openzfs/zfs#19233). That is why
  openzfs/zfs#19419, which proposes to rewrite every block of a vdev,
  refuses when snapshots or a checkpoint hold blocks.
- Padding can grow a block from 4 sectors to 6 (openzfs/zfs#16677), so
  data may not fit even with enough raw space.

**Requirements on either mechanism.**

1. Completeness: the new tolerance is reported, and the old epoch
   retired, only from a durable record written after a census of zero
   that the allocator can answer, pending frees and asynchronous
   destruction included (point 4 of section 3).
2. Sharing: dedup and block-cloning references survive the move, and a
   block that several references share is copied once.
3. Lifetime: an old extent is reused only once no reader can still be
   reading it, ordinary reads included; a defer interval alone does not
   prove that (point 8).
4. Repair: each copy is checked against the DTLs at the txg it was
   written in, and a resilver visits every copy written inside its
   window, whatever birth its block pointer carries.
5. Space: dataset dsize never changes between a block's birth and its
   free; what the move adds is charged where the allocator refuses on
   it, and a move once started can finish (points 6 and 12).
6. Recovery: an import after a crash at any point finds the state before
   or after each step of the move.
7. Size: the mapping's memory is estimated before a move starts and held
   to a budget (point 3).

## 2. The two mechanisms

Writes: rdmitry0911; tigerblue77 corrects the column describing the
redirect prototype. Each reviews the other's column.

Draft contribution by rdmitry0911, updated 3 November. The redirect column
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
| Table population | One entry per redirected DVA, keyed by offset and physical birth, until its last reference is freed, then an in-memory tombstone until its extents are reusable. Dedup under the hybrid not witnessed; BRT exclusion not built | Materialized data and metadata allocation identities, new allocations included, with retired ownership accounted separately; untouched legacy extents use an identity range tree |
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

Draft by tigerblue77, 26 October. It merges the six questions of #2, the
points of the 25 October reply (rewind, readers in flight, dedup, BRT,
expansion) and the open decisions rdmitry0911 has named on #2, in #4 and
in #5 and consolidated as D1 to D8 in his review of 27 October; the
table at the end shows where each went. Each point gives the question,
the options, where each prototype stands and what would settle it.
"Redirect" is the prototype of #3 and "virtual/backing" the one of #4;
their positions describe the code as published, not the converged
design.

References: `spike:` is the [redirect snapshot at `094d2a05f`](https://github.com/tigerblue77/zfs/tree/094d2a05f9d568996163718155dad4b47a3e547c)
and `vb:` the [virtual/backing snapshot at `46d04a1af`](https://github.com/tigerblue77/zfs/tree/46d04a1afab04c4e9e12baa96fbc6677ac9a36e9).
A path without a directory is under
`module/zfs/`, except `README.md` and redirect's `*_probe.c`, which are
under `contrib/raidz-redirect-spike/` (`spike:`) or
`contrib/raidz-virtual-backing/` (`vb:`), and virtual/backing's
`pr19262_*.ksh`, under `tests/zfs-tests/tests/functional/pr19262/`. A
bare `file:line` is the [upstream snapshot at `ea111ca50f`](https://github.com/openzfs/zfs/tree/ea111ca50fedd4008e8622c6a7f5b70dc2d43f59).
These source references describe the pinned implementations; section 4
separately identifies the sources used for the measurement adapters.

### Proposed by both authors, for the maintainers to confirm

**1. A mapping beneath unchanged block pointers.** Is a per-block
mapping from a block's old extent to a copy at the new parity acceptable
for the blocks a rewrite cannot reach? It is device removal's principle
("Since we cannot rewrite block pointers of snapshots, etc., we keep a
mapping", `vdev_indirect.c:37-39`), per block because RAID-Z needs block
boundaries. Both prototypes rely on one; rdmitry0911 accepted it on 21
October on the condition of point 4.

**2. Which blocks get a row.** Proposed default, subject to section 4:
blocks that a head whose key is loaded owns alone are rewritten through
the normal write path, logical birth kept; snapshot- and clone-held,
dedup- and BRT-shared and keyless blocks are mapped; new and untouched
blocks get no row. rdmitry0911 is "open to the hybrid being the
default" (26 October) if section 4 shows fewer retained rows and lower
full memory on head-heavy cases, without losing sharing or failing the
lifetime, recovery and space checks, and with acceptable copy work,
latency and fragmentation (#5). The alternative, his position of 21
October, makes the mapping the general mechanism and the rewrite an
optional drain; as the 25 October reply on #2 argued, unmodified head
data then keeps its rows, and the table is bounded by capacity rather
than by retention. Neither prototype is that default yet:

- Redirect: the hybrid is a probe (`spike:hybrid_probe.c:557-562`,
  `774-818`). On master a rewrite of a dedup block is a no-op
  (`zio.c:4364-4387`), so the census round after it redirects the block;
  not witnessed under the hybrid. A clone's block pointer takes the
  clone's txg as logical birth (`dmu.c:3033-3035`), so the ownership
  test rewrites, and un-shares, BRT-shared head blocks until what
  `brt_maybe_exists()` flags (`brt.c:905`) is redirected instead; not
  built. Where the checksum and compression allow nopwrite
  (`dmu.c:2706-2708`), a rewrite of unchanged data keeps its old block
  (`zio.c:3751-3778`), so the rewrite needs an exemption; not built, and
  the hybrid fixture has compression off (from reading, not tested).
- Virtual/backing: new allocations are paired too
  (`vb:README.md:9-11`); identity mapping without those rows "remains a
  change to build and measure" (26 October).

**3. A table bounded by retention, with an estimate and a budget.** An
entry leaves when its block is freed; nothing drains the table by
rewriting snapshot metadata, which both authors agree is block pointer
rewrite. Before a move starts, an estimate, as `zpool remove -n` prints
one for removal, and a refusal above a tunable budget (Q2 of #2, agreed
21 October). The budget is to be agreed from section 4 (26 October).
For reference: about 50-100 MB per TB for device removal's mapping
(ahrens on openzfs/openzfs#627, as quoted in #2); 50.0-52.8 B per entry
in RAM for the redirect table at 1.7e7 synthetic entries
(`spike:README.md:566-568`), whose per-entry block-pointer ZAP for the
scan walk (1482 B allocated at ashift 12, `spike:README.md:583-588`)
must go; no complete figure yet for virtual/backing
(`vb:README.md:117-121`). Snapshots kept for years keep their rows; an
in-pool `zfs send -R | zfs recv` can empty them.

**4. The completeness gate.** The new tolerance is reported, and the old
epoch retired, only from a durable record written after a census of zero
(Q3 of #2). The census must be one the allocator can answer rather than
a walk's own report (rdmitry0911's condition of 21 October, accepted on
25 October), pending frees and asynchronous destruction included.
Options:

- Reconcile the mapping with the native space maps. Virtual/backing
  certifies only with no legacy bytes, no current or retired version
  below the target parity, a recorded floor and no checkpoint
  (`vb:vdev_reloc_convert.c:437-460`); the certificate then raises the
  parity the vdev reports (`vb:vdev_raidz.c:2247`).
- A per-vdev count of old-epoch allocated bytes kept by the allocator,
  decreased by every free and move that releases them; built in neither
  prototype.
- A traversal, which is what redirect has today, in test code
  (`spike:crypt_probe.c:23-27`, `1123-1142`). One probe counts the
  deferred and free bpobjs beside it and only reports an async-destroy
  bptree; a walk is not enough alone.

The guard proposed with it in #2, not built, as rdmitry0911 corrected
it on 27 October: once an epoch is retired, refuse a dispatch, free or
claim that would use its obsolete physical layout without a valid
mapping. An unchanged block pointer may still carry a birth in that
epoch, so its mapping is validated and its copy resolved before the
guard applies (redirect: the entry keyed by offset and physical birth,
`spike:vdev_raidz.c:2911-2921`; virtual/backing: the allocation
identity, whose dispatch keeps the block pointer for verification and
takes the copy's offset, size and birth,
`vb:vdev_reloc_engine.c:1923-1937`). Read, repair, free and claim of
that block remain valid. An absent or stale entry must never fall back
to the old layout at the old extent, nor reach another block that has
reused the address.

The durable record needs validity rules of its own. A change that could
make any reachable block's backing weaker than the record states is
refused, or durably withdraws the record before that state is
published; only a new record, after another census of zero, restores
the new tolerance. Keeping the feature active (point 5) is a separate
obligation.

**5. One feature.** Epochs, mapping and census form one pool feature,
not read-only compatible, since older code would read the old extent;
it is registered only in the last step of section 5 (Q3 of #2, agreed
21 October). The epochs of openzfs/zfs#19262 lose their own feature and
land with this one, as rdmitry0911, their author, agreed on 26 October;
the experimental GUID of #19262 is not reused with a meaning that an
epochs-only reader would accept. The feature is active before the first
incompatible state becomes durable: an epoch table, a mapping row, or
bootstrap or journal state if the design has them (points 7 and 11).
Its guard is visible before any translated MOS read, at every crash
and import point, so that older code refuses read-only imports as well
as read-write ones. It stays active while any durable state on an
affected vdev, or in a retained or recoverable root, still needs it:
an empty mapping is not enough, since epoch layouts, a checkpoint,
retained copies, a journal and a durable floor or completion record can
outlive the last row. A first implementation may stay active for good;
deactivation needs its own proof that every such root is fully readable
by older code. Activation is separate from the durable record of
point 4, which alone lets the new tolerance be reported. Neither
prototype registers a feature for its mapping (redirect: none;
virtual/backing: a private label entry that older code refuses,
`vb:spa.c:5017-5020`, and "no supported administration interface or
agreed integrated feature/format", `vb:README.md:15-17`).

**6. Space, the parts agreed.** Dataset dsize never changes between a
block's birth and its free. What a move adds is charged at pool level,
as the number the allocator refuses on rather than a display
subtraction, and it holds for the whole move (Q4 of #2, with
rdmitry0911's two additions of 21 October, accepted on 25 October).
Redirect charges it in `spa_update_dspace()`
(`spike:spa_misc.c:2117-2127`), which the DMU's space check reads
(`dsl_pool.c:950-951`); virtual/backing admits each transaction against
physical credits and a standing reserve (`vb:vdev_reloc_admit.c:1-12`).
The open parts are point 12.

### Open between the authors

**7. Where the mapping lives.**

- In the MOS (redirect): a log object and a block-pointer ZAP per vdev,
  loaded right after the epoch table and before any block outside the MOS
  is read (`spike:vdev_raidz.c:8924-8927`). MOS blocks are then never
  mapped (`spike:vdev_raidz.c:8343-8352`) and must be rewritten through
  the normal write path in syncing context, as one probe does
  (`spike:crypt_probe.c:38-41`).
- In raw replicated regions on the vdev's own children, found from an
  uberblock extension before the MOS is read (virtual/backing; rdmitry0911
  on #2, 21 October). MOS blocks are mapped like any other, at the cost of
  new fields in `struct uberblock`
  (`vb:include/sys/uberblock_impl.h:175-176`) and of metadata outside the
  MOS.

Is an uberblock extension with raw per-child metadata acceptable, or
must the mapping live in the MOS?

**8. Old extents and readers in flight.** Requirement 3 of section 1
holds for both; the options differ in when an old extent is reused.

- Free most of it at the move (redirect). One sync task writes the copy
  and waits, adds the entry, then frees the old extent's tail as an
  ordinary deferred free (`spike:vdev_raidz.c:8492-8510`); scan and
  resilver I/O are waited for within their txg (`dsl_scan.c:4739-4758`,
  `4790-4794`). Ordinary reads rely on the defer window alone
  (`spike:vdev_raidz.c:2875-2882`), and a txg that defers no frees (free
  space at or below the slop, `metaslab.c:4632-4637`) makes the tail
  reusable at once; the move does not pause there. Proposed, not built:
  expansion's `vre_rangelock`, which RAID-Z I/O takes as reader only
  during a reflow today, taken as reader by every RAID-Z I/O while a move
  runs and as writer by the move over the old extent before its tail is
  freed (`vdev_raidz.c:2844-2848`, `4609-4610`), and a pause while frees
  are not deferred. Cost: tails that cannot hold a block of the same size
  at the new parity, for as long as the block lives
  (`spike:README.md:935-941`).
- Retire the old extent whole, and reuse it only once its version
  leases have drained and a durable fence permits it (virtual/backing).
  A read or repair already bound keeps its selected backing across
  publication and final-reference deletion. A fresh reference to a
  freed or reused allocation is refused with ESTALE and cannot select a
  different allocation at the same virtual address
  (`vb:vdev_reloc_engine.c:1669-1679`); a converter that meets a stale
  traversal item can revisit it from a fresh root (point 13). Cost:
  retained bytes, which depend on held operations and fence progress
  (its 21 October fixture used a 64 MiB batch); the floor policy
  remains point 9.

Evidence required by section 4, item 5: an ordinary read held across the
reuse boundary, the no-deferral case included, in both prototypes.

**9. Rewind.** `-F` reaches back at most `TXG_DEFER_SIZE` txgs
(`spa.c:6583-6585`), which an ordinary deferred free covers only in a
txg that defers its frees. In one that does not (point 8), redirect's
freed tail is reusable at once, and `-F` can meet what `-X` meets
below, unless the move pauses there as point 8 proposes. `-X`, and
`-T`, which implies it (`cmd/zpool/zpool_main.c:4470-4472`), guarantee
no consistency (`zpool-import.8`) and can reach an old extent whose
freed part another block now holds: a moved block then fails its
checksum, as after any free, except that a move frees part of every
block it moves. Options: stock behaviour, or a durable floor that
refuses, in every import mode, roots older than released backing
(virtual/backing). Both authors keep the floor policy open (26 October).

**10. Checkpoint.**

- Refuse a move while a checkpoint exists, as attach and removal refuse
  one (`spa.c:8500-8504`, `vdev_removal.c:2487-2489`; redirect:
  `spike:vdev_raidz.c:8329-8330`). A free after a later checkpoint is
  checkpointed by each extent's own birth
  (`spike:metaslab.c:5901-5906`), computed but never exercised
  (`spike:README.md:435-436`).
- Allow a move, with reclaim pinned while the checkpoint exists and a
  writable rewind to it refused once the engine is persistent
  (virtual/backing: `vb:README.md:108`, `130-131`;
  `vb:spa.c:6151-6154`). Its README leaves "the initial product policy"
  to a joint decision.

**11. Acknowledged writes and the keyless ZIL.**

- Redirect never moves a ZIL block (`spike:vdev_raidz.c:8343-8357`).
  Ordinary churn retires them, except those of a dataset whose key is
  not loaded, which it does not handle (#2, open problems).
- Virtual/backing treats every ZIL block as an allocation like any
  other: the pair is bound in a journal before the ZIL acknowledges and
  replayed before claim, log devices included, with no key (21 October;
  `vb:README.md:54`).

Is a journal beside the ZIL acceptable, or does a pool whose keyless
datasets hold a ZIL stay unconverted until their keys are loaded?

**12. Space, the parts open.**

- Before the move: refuse up front unless the end state fits, the new
  extents pack first fit decreasing into the free segments as they
  stand, and what is left covers the slop, the table and the rewrites'
  growth (redirect, `spike:vdev_raidz.c:8573-8752`; a model of the
  allocator, not the allocator); or admit each transaction against a
  standing reserve for the growth still to come, so that a full pool
  still converts (virtual/backing: "793 MiB of weak data inside a
  304 MiB reserve", 21 October).
- During the move: a reservation for the whole migration
  (virtual/backing: the standing reserve; redirect: not built).
- What users see: AVAIL, quotas and reservations during and after a
  move; #5 leaves "public quota/reservation/AVAIL promises" to
  agreement.
- Under the epochs, new-parity writes are still charged at the deflate
  ratio of txg 0; both authors leave it open (#2, 21 October).

**13. The mover and its traversal.** Should the mover use `dsl_scan`'s
sorted, resumable traversal (Q5 of #2)? rdmitry0911 (21 October): good
for sorted I/O and resumption; after holding a txg the mover must wait
for the txg below it to sync, or that txg's frees remove blocks the
walk's root still references; ESTALE and busy are answers the pass
counts, not failures; the gate stays the census. Redirect's mover is
test code that re-checks in syncing context that the old extent is
still allocated and that the bookmark still names the block, and skips
it otherwise (`spike:vdev_raidz.c:8434-8443`). Virtual/backing's
converter walks the MOS, then each dataset with
`traverse_dataset_resume()`, in slices that each start from a fresh root
(`vb:vdev_reloc_convert.c:903-955`). The first evidence asked for (Q6 of
#2), the traversal timed against a scrub, is about the same for
virtual/backing (4 s against 4-5 s, 22 October). Redirect has it only
at the 12.5 MiB scale of the common fixture (section 4): a census walk
alone, run just after an inventory walk of the same pool, takes
0.004-0.007 s against 0.10-0.15 s for a scrub, release medians of the
1 November rerun
(`contrib/raidz-redirect-spike/fixture-results-20261101.txt` on
`spike/bypass-fixture-adapter`, `55420b5de`), too small a pool to tell
what the traversal costs.

**14. Scope, expansion and removal.** At first, a move converts one
top-level vdev, selected explicitly, per pool at a time, and no move
runs while an expansion or a device removal does, nor does either
start while a move runs (rdmitry0911, 27 October). These exclusions
are pool-wide, a log device's removal included, and they remain an
open policy and qualification condition of D8, which neither
prototype has selected or proved; a later design narrows them only
with the matching proof (rdmitry0911, 29 October). Virtual/backing
retains "one translated top-level vdev" per pool for an engine's
lifetime (`vb:README.md:132-133`); redirect's move names one vdev
(`spike:vdev_raidz.c:8335-8337`) but enforces neither one move per
pool nor these exclusions, and several top-level vdevs are untested
(#2). Master already keeps the removal of a data vdev apart: it
refuses to remove a top-level vdev other than a log from a pool that
has a RAID-Z vdev (`vdev_removal.c:2340-2363`), and to add a RAID-Z
vdev during a removal or beside indirect vdevs (`spa.c:8277-8293`), so
on a pool with a RAID-Z vdev what is left to exclude is a log device's
removal. Refuse, at first, to expand a vdev that holds a mapping and to
move on an expanded one (both authors); the epoch series
already refuses to expand an epoch-bearing vdev
(`spike:spa.c:8543-8551`). The combination is first to be defined for
the epochs (openzfs/zfs#19413, question 2). From reading, not tested:
redirect then needs one rule, no move during a reflow, since a reflow
defers no frees (`metaslab.c:4632-4637`); its keys, kept sectors and
copies are offsets a reflow preserves. Virtual/backing's catalog arenas
are "raw per-child regions a reflow would relay out" (21 October), and
its README puts expansion outside the comparison
(`vb:README.md:133-134`).

**15. Interface and observation.** How a move is started, estimated,
watched, paused and cancelled, what `zpool status` shows, and how the
change of tolerance is reported (point 4). A zdb report of DVAs per
parity comes first (section 5). Neither prototype has an interface:
redirect's mover is driven by probes, and virtual/backing has "no
supported administration interface" (`vb:README.md:15-16`).

**16. Degraded operation and metadata repair.** What permits a
read-only or writable import, and a durable mapping, bootstrap, journal
or completion update, while children are missing or stale? Point 7
places the mapping and requirement 4 of section 1 covers the copies'
DTLs; neither answers it. With raw replicas, read and write quorums
need defining against the tolerance established at that stage,
including before completion; with MOS storage, the native rules the
mapping inherits, and the restrictions it adds, need naming. For
returning or replacement members: how data and every required mapping,
bootstrap, journal, floor and completion record are reconciled and
repaired, and how stale copies are kept from authority. Redirect keeps
the mapping in the MOS and adds no rule of its own; a move with a child
offline, then the child's return, its resilver and a scrub that repairs
nothing, is witnessed (`spike:README.md:403-406`, `418`).
Virtual/backing, from reading, confirmed by its author for a private
candidate and its kernel fixture, not as a qualification of
replacement or of other topologies (29 October): a raw write or
required read tolerates at most as many absent or failing members as
the certified parity, and certification, initial or renewed, is
refused until every direct child is readable and writable
(`vb:vdev_reloc_engine.c:140-145`, `vb:vdev_reloc_store.c:576-580`); a
writable import gives members that missed writes the live pages
(`vb:vdev_reloc_store.c:1726-1746`); a kernel test imports a pool
certified at parity 2 writable with two of six members hidden, reads
and writes, is refused with three hidden, and resilvers and scrubs
the members on return (`vb:pr19262_degraded_pos.ksh:5-8`). These
rules remain to be selected and demonstrated for the joint design;
section 4, item 7, asks for the publication and recovery cuts of an
update made while a member is missing or stale.

### Relation to AnyRAID

Draft by tigerblue77, 1 November, with rdmitry0911's 2 November
source-review qualifications incorporated for review. This is source
inspection, not a combined build or runtime compatibility result.
AnyRAID is the tiled top-level vdev of openzfs/zfs#17567 (phase 1,
`anymirror`) and openzfs/zfs#18406 (phase 2: `anyraidz`, rebalance,
contraction), both open as of 3 November. `ar1:` is the reviewed
[phase 1 snapshot at `5ffeee877`](https://github.com/openzfs/zfs/tree/5ffeee877ebb67add36ca2b5d19830fbffbb52e9);
`ar2:` is [phase 2 at `18d064529`](https://github.com/openzfs/zfs/tree/18d064529962a4f45cb0b551f820b32a680ee863),
cited only for what phase 2 adds. Phase 1 has since advanced to
`3d7176b2a49872cfd64dfda7e52432d4818dd4a3`; the line references and
conclusions below apply to the pinned reviewed snapshot, not that newer
head.

**Scope.** An AnyRAID vdev is a top-level type of its own
(`ar1:vdev.c:890-891`), its parity and width fixed at creation
(`ar2:vdev_anyraid.c:342-350`): our epochs and mapping do not reach it.
Its format plans "new entry types ... to store information like parity
changes" (`ar1:include/sys/vdev_anyraid.h:112-113`); that is AnyRAID's.

**Block mapping and tile mapping.** Both keep block pointers intact.

- AnyRAID maps a logical tile, 16 GiB or more by default
  (`ar1:vdev_anyraid.c:112-117`), to one physical tile on each child it
  uses (`ar1:include/sys/vdev_anyraid.h:58-69`). No metaslab spans a
  tile (`ar1:vdev_anyraid.c:1028-1033`), an I/O is translated by its
  offset alone (`1079-1080`), and a block keeps its layout in the tile.
- This design maps blocks, because a parity change gives each block a
  new layout (point 1). The current tile copier does not implement
  RAID-Z parity conversion: phase 2 copies coalesced physical ranges
  after subtracting allocatable space, without preserving the block
  boundaries needed for a per-block relayout
  (`ar2:vdev_anyraid.c:3146-3175`). This describes the present
  implementation, not a proof that tile-based parity conversion is
  impossible.
- So an `anymirror` parity change would be a tile operation, one more
  copy and entry per tile, while an `anyraidz` tile is a RAID-Z map of
  `vd_width` columns (`ar2:vdev_anyraid.c:1391-1392`), whose parity
  change is this design's problem again. Neither phase does either.

**A map read before the MOS (points 7 and 16).** Each child holds four
copies of the whole map, two after the front labels and two before the
back ones (`ar1:vdev_anyraid.c:119-128`): a header nvlist (version, txg,
pool guid, tile size, child sizes) and up to 64 MiB of 4-byte entries
(`ar1:include/sys/vdev_anyraid.h:71-118`, `245-263`). At every txg the
next copy is rewritten in full on every writable child, after the even
labels and before the uberblocks (`ar1:vdev_anyraid.c:1433-1440`;
`ar1:vdev_label.c:1993-2011`, `2312-2345`). At open, the copy whose txg
is nearest `spa_load_max_txg()` is used, falling back to the next copy
or child (`ar1:vdev_anyraid.c:391`, `501-519`, `557-614`), as it also
does past a copy whose version or entry type it does not know
(`437-443`, `672-676`, `687-693`). What keeps older code out is the
feature, `ZFEATURE_FLAG_MOS`
(`ar1:module/zcommon/zfeature_common.c:815-817`): older code refuses
the pool from its label, as point 5 asks of ours.

- The regions are reserved at creation, every child offset shifted past
  them (`ar1:vdev_anyraid.c:1079-1080`, `1620-1622`). An existing
  `raidz` vdev's children have none, so a region added later is found
  through a pointer, as virtual/backing's arenas are through the
  uberblock. Selection and fallback are implementation examples to
  evaluate for point 16; the placement does not carry over.
- A full rewrite each txg works because the map is bounded by its tiles
  (2^24 at most); a block mapping, 1.7e7 entries in redirect's scale
  test, needs the log or journal both prototypes have.
  Its header-selection and fallback code is an implementation example
  for point 16, not a resolution of that point. The comparator uses
  absolute distance from the target txg (`ar1:vdev_anyraid.c:501-518`);
  the open path logs an older-than-minimum header rather than rejecting
  it at that check (`557-571`). This alone does not establish writable
  authority, missing/stale-member publication safety, or member repair.
- Phase 2 keeps the current relocation task in the pre-MOS header,
  alongside the complete pre-MOS tile map
  (`ar2:vdev_anyraid.c:742-843`, `1931-1970`); the rest of the plan is in
  a MOS object loaded with the vdev (`2256-2323`,
  `ar2:vdev.c:4125-4129`). A small pre-MOS record with a MOS-resident
  redirect table remains an unbuilt option for point 7. It needs its
  own bootstrap proof: pre-MOS state must resolve every block needed
  to read the mapping, including completed relocations and retained
  or recoverable roots. The task-record split alone does not prove
  that closure.

**Acknowledged writes (point 11).** A tile is created by the first write
that reaches it (`ar1:vdev_anyraid.c:1144-1192`); ZIL and `dmu_sync()`
writes use it only once all four copies hold it (`1150`, `1474-1475`,
`1633-1645`; `ar1:metaslab.c:3555-3557`, `5560-5562`; `ar1:dmu.c:2253`,
`2466`). No acknowledged block depends on map state not yet durable, and
no journal is needed; point 2's default gives our ZIL the same (no row,
new parity), while virtual/backing journals its new pairs.

This source-level four-copy durability gate is not a demonstrated
protocol for bringing every returning or replacement member up to date;
the joint authority and repair requirements remain open under point 16.

**Rebalance as a mover (points 8, 10 and 13).** Phase 2 moves a tile by
copying, chunk by chunk at the physical level, every range of the
metaslabs over it that is not allocatable
(`ar2:vdev_anyraid.c:3124-3160`, `3027-3097`), records progress each txg
(`2484-2530`) and switches the tile's entry when done (`3304-3329`).
Every I/O takes a range lock as reader (as raidz expansion's I/O does
during a reflow), the copy takes it as writer, and an I/O below the
progress goes to the destination (`1489-1510`, `3050-3051`). The copy
checks no checksum, so the source is freed only after a scrub
(`2606-2659`, `2532-2560`). One relocation runs per pool, none starts
while a checkpoint exists, and no checkpoint is taken during one
(`ar2:spa.c:12078-12095`, `12160-12178`; `ar2:spa_checkpoint.c:472-473`).

- That is the shape point 8 proposes for redirect, except that phase 2
  locks on every I/O, not only while a move runs. A copy behind an
  offset also misses writes behind it (a MOS block rewritten in a later
  sync pass, a ZIL block written after its allocation); tigerblue77's
  fixes for phase 2 write both slots until the switch is on disk
  (`3cd048adde` on this fork). Freeing the source only after a scrub is
  the opposite of redirect's tail freed at the move, close to
  virtual/backing's retire-whole. Neither mover replaces the other:
  this one gives no block a new parity, and ours moves no tile.
- If a scan is running when a relocation ends, phase 2 frees the source
  without the scrub (`ar2:vdev_anyraid.c:2652-2657`,
  `ar2:dsl_scan.c:973-975`). A mover run as a `dsl_scan` pass (point 13)
  would make every relocation ending during a move skip its scrub.

**On one pool.** No kernel code of either phase refuses a pool with a
`raidz` vdev and an AnyRAID vdev. Each mechanism keeps to its vdev, a
block with a DVA on each being handled by each for its DVA. Constraints:

- A proposed extension of point 14's pool-wide exclusions would cover
  AnyRAID rebalance or contraction through its verification phase,
  in both directions. Phase 2 checks only its
  own relocation and the checkpoint (above), and an attach, raidz
  expansion included, checks removal and expansion, not a relocation
  (`ar2:spa.c:8337-8360`). Reasons: the scan above, and the move's
  pre-flight and reservation (point 12), which a contraction undoes by
  removing a child (`ar2:spa.c:9571-9590`). These exclusions remain open
  policy and qualification conditions under D8.
- Section 5, step 2: `anyraidz` builds RAID-Z maps with its own width
  and parity and starts them through `vdev_raidz_io_start_impl()`, which
  phase 2 factors out of `vdev_raidz_io_start()`
  (`ar2:vdev_anyraid.c:1391-1398`, `ar2:vdev_raidz.c:2696-2710`): the
  tail the epoch series changes to take the layout from the birth
  (`spike:vdev_raidz.c:4796`, `4904-4926`). Step 2 must keep an
  explicit-geometry entry for `anyraidz`, and whichever lands second
  rebases on the other.
- Point 5: phase 2 adds its relocation state to the version-0 header
  under the same feature (`ar2:vdev_anyraid.c:1914`, `1931-1970`), and
  the phase 1 reader ignores keys it does not know
  (`ar1:vdev_anyraid.c:425-490`). AnyRAID faces point 5's rule: an
  incompatible addition lands with its feature, or under a new one.

### Where the earlier questions went

| Raised | Point |
| --- | --- |
| #2, Q1: a per-block redirect, in principle | 1, 2 |
| #2, Q2: a retention-bounded table, estimate and budget | 3 |
| #2, Q3: one feature, gated on a census of zero | 4, 5 |
| #2, Q4: pool-level charge, dataset dsize unchanged | 6, 12 |
| #2, Q5: a mover on `dsl_scan`'s traversal | 13 |
| #2, Q6: traversal timed against a scrub | 13, section 4 |
| 25 October: rewind | 9 |
| 25 October: readers in flight | 8 |
| 25 October: dedup, BRT | 2 |
| 25 October: expansion | 14 |
| rdmitry0911, D1: population, rewrite choice | 2, 3 |
| rdmitry0911, D2: discovery, bootstrap | 7, 5 |
| rdmitry0911, D3: ACK durability | 11 |
| rdmitry0911, D4: old-copy lifetime, older roots | 8, 9, 10 |
| rdmitry0911, D5: stronger-tolerance certificate | 4 |
| rdmitry0911, D6: degraded authority, member catch-up | 16, section 4 |
| rdmitry0911, D7: allocation credits, space promises | 6, 12 |
| rdmitry0911, D8: initial scope, administration | 5, 14, 15 |

## 4. Measurements on one fixture

Each author for their own prototype; both review.

Draft measurement contribution by rdmitry0911, updated 3 November. Each
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
columns are the redirect author's 1 November feature-aligned rerun
(three true-release runs per case); their entry counts match the
29 October report in every case. The third is the virtual/backing
author's 31 October true-release result (three runs per case). The shared
generator is unchanged at `6319a29f86a9771c62a4c59b029c9c18a839998b`:
five 256 MiB file vdevs, 12.5 MiB logical payload, compression off, copies=1.

Virtual/backing completed 48/48 conversions: all eight cases, three repeats
in debug and three in true release. All 384 native commands succeeded;
192 readonly operations preserved all five member hashes. A separate
author-side audit rehashed and parsed 768 saved native process streams,
regenerated the expected payload hashes, and compared 792 saved VERIFY
records against them. It did not re-execute the native commands or
reread the raw members. Every post-conversion/deletion state had
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

The counts have different coverage: virtual/backing includes data, MOS
and other metadata, including new allocations paired by this prototype.
The redirect table counts only redirected DVAs; it excludes MOS and head
blocks that the hybrid rewrote through the normal write path.
Virtual/backing's
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
the mover. The redirect author's 1 November rerun enables
`physical_rewrite` and its `extensible_dataset` dependency before either
mover on a fresh copy of an input from the unchanged common generator;
the hybrid adapter enables nothing. All 96 debug/true-release runs passed
the reported payload, census and cold-table checks. The move, cold-import
and post-deletion entry counts match 29 October across all 32
tree/case/mover cells. Three kept-sector cells vary by two 4 KiB sectors;
allocated table bytes vary by a roughly 28.5-29 KiB step within the
reported ranges. The per-copy raw member hashes around readonly imports
were not collected: the unchanged before/after hashes cover the 16
original input sets, not the readonly operations on their run copies.

VM hardware, ARC limits, mover boundaries and sampler intervals differ
between authors; the times in the reports do not prove a three-way speedup.
Redirect reports standalone census-walk/scrub timing on the 12.5 MiB
fixture and 48 debug post-deletion fills to ENOSPC, without pool suspension
and with zero gang blocks. Those fills use the five 256 MiB members after
the retained snapshot has been dropped; they do not
establish initially-too-full or snapshot-retained admission safety.
Virtual/backing has neither a standalone walk/scrub measurement nor an
ENOSPC/gang fill in this core publication. Flush counts, post-move churn,
complete resource charge and the extended failure matrices above remain
separate obligations. The redirect true-release build still lacks zdb;
that report's successful library/adapter runs do not qualify release zdb.

Sources: [feature-aligned redirect rerun at `55420b5de`](https://github.com/tigerblue77/zfs/blob/55420b5dea97a59365dab285625941c9eece760c/contrib/raidz-redirect-spike/fixture-results-20261101.txt),
[historical redirect report at `98dc0fa7f`](https://github.com/tigerblue77/zfs/blob/98dc0fa7f4d6a73c598f8b37b4d59e1b81f5b30d/contrib/raidz-redirect-spike/fixture-results-20261029.txt),
[virtual/backing source and reproduction commands at `c3e59b5d3`](https://github.com/rdmitry0911/zfs/tree/c3e59b5d3cb1a9656274aa6c12976b56e85ad343/contrib/raidz-virtual-backing/common-fixture).

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
[the common generator at `6319a29f8`](https://github.com/rdmitry0911/zfs/tree/6319a29f86a9771c62a4c59b029c9c18a839998b/contrib/raidz-parity-fixtures).
It creates equivalent logical inputs without invoking either mover; its
baseline creation/read verification does not establish a migration result.
The core adapter and measurement reports are published on the separate
[virtual/backing comparison source at `c3e59b5d3`](https://github.com/rdmitry0911/zfs/tree/c3e59b5d3cb1a9656274aa6c12976b56e85ad343/contrib/raidz-virtual-backing/common-fixture).
The mixed shared extensions and complete resource/failure collectors
remain open. Large author-local raw fixtures are not embedded in this
document.

## 5. Landing order

Writes: tigerblue77. Reviews: rdmitry0911.

Draft by tigerblue77, 26 October, from the order proposed in #2, which
rdmitry0911 took into the outline of this document on 21 October, with a
step added for lifetime and space. No on-disk format lands without the
mechanism that completes it: the feature of point 5 of section 3 is
registered only in the last step, or the series merges together.
Every step builds on the parity-epoch series of openzfs/zfs#19262, whose
format lands with the feature of the last step. Steps 1 and 2 do not
depend on the choice between the mechanisms; steps 3 to 6 take their
shape from points 2, 7, 8, 11 and 16.

0. Evidence first: the measurements of section 4 on one fixture and the
   decisions of section 3, presented to the maintainers before any code
   is proposed.
1. Observation: a zdb report of the DVAs on each RAID-Z vdev by parity
   epoch (ahrens on openzfs/zfs#12225: "it would be important to have a
   mechanism to at least observe the amount of old blocks"). Read-only,
   no format, and the reference the census of point 4 is tested against.
2. A refactor of layout selection with no functional change: every
   RAID-Z read, write, repair and resilver check takes its layout from
   one place, so that a later step can give a block a layout other than
   the one its block pointer's birth implies.
   Where `anyraidz` shares the RAID-Z implementation, preserve its
   explicit width/parity entry point rather than deriving its geometry
   from this design's parity epochs. Whichever series lands second must
   account for the other.
3. The mapping and its read paths: format, load and validation; the
   rules of point 16 for the mapping when children are missing or
   stale, and its repair on returning and replacement members; reads
   and repairs at the copy, with the copy's own birth for DTL checks;
   scrub and resilver following the mapping, copies written inside a
   resilver's window included; blocks with several DVAs; frees at the
   last reference; zdb's claims. Entries come only from test code until
   step 6.
4. Lifetime and space: the reader exclusion and the no-deferral rule of
   point 8; the pool-level charge and the move's reservation (points 6
   and 12); the estimate and the budget (point 3).
5. The mover: the rewrite of what a head owns alone, with its BRT and
   nopwrite exceptions (point 2); the mapping of the rest; the traversal
   (point 13); the keyless ZIL (point 11), with a journal's rules
   under point 16; the checkpoint rule (point 10); the scope and
   exclusions of point 14. Until step 6 it runs only from test code, so
   that no supported path writes a mapping before the feature exists.
6. The feature, registered last, with the interface of point 15 that
   starts a move: active before the first incompatible state is durable
   and while any durable state needs it (point 5); the census of point
   4, its durable record, the record's rules under point 16 and the
   rules that withdraw it, the new tolerance and the retirement of the
   old epoch; the guard on retired physical layouts, applied after a
   block's mapping is validated and resolved, so that an unchanged block
   pointer with a birth in the retired epoch is still read, repaired,
   freed and claimed (point 4); `zpool-features.7` and the man page of
   the command that starts a move.

Each step comes with its tests: ZTS for the kernel paths, `ztest` on
RAID-Z, zdb's checks on the pools it leaves, and, from step 3 on, the
fault cases of section 4 and the crash points of requirement 6 of
section 1.
