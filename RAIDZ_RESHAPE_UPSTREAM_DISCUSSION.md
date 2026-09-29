# Discussion / RFC: In-place reshape for RAIDZ and dataset geometry — the full package

> **TL;DR** — The north star is **reconfiguring the storage of a live
> virtualization host (Proxmox, etc.) without taking it down** — no pool export,
> no host reboot, so the guests (VMs/containers on zvols and datasets) keep
> running. Concretely: change RAIDZ parity (raidz1 ↔ raidz2 ↔ raidz3, both
> directions), grow capacity, and remove a disk (N → N-1) **in place, online**,
> using only the pool's own free space, with snapshots and clones preserved. One
> new incompatible feature flag underlies it: a per-vdev **layout-epoch table**
> (`{start_txg, width, parity}`, the same shape as RAIDZ-expansion's
> `ZPOOL_CONFIG_RAIDZ_EXPAND_TXGS`); each block is reconstructed at the geometry
> it was written with, and a **bounded, fail-closed, crash-resumable CoW sweep**
> re-encodes the corpus, raising effective redundancy only after a census proves
> zero residual. No change to the RAIDZ row encoding, BP format, ASIZE, ashift,
> or vdev type. **Validated prototype** (device-loss matrices, crash cutpoints,
> KASAN, soak, online, snapshots/clones/encryption/multi-vdev) — not a finished
> feature. **Honest gap up front:** the plain parity-reshape and capacity-grow
> paths are already fully online (guests stay live); the shared-graph
> (snapshot/clone), codec, and contraction paths are currently **offline
> (require `zpool export`, which stops guests)** — closing that gap (in-kernel
> online for those paths) is the main remaining work and a key question for
> maintainers. See §1.1 and the per-feature "Live?" column.

## 0. How to read this

This document gives (1) a **bird's-eye view** of a coherent body of work —
making RAIDZ and dataset geometry *reshapeable in place* — and then (2)
**per-feature detail** for each proposed change, with an honest maturity/scope
label on every one. The goal is early maintainer feedback on direction, on the
one new on-disk format, and on where the line should sit between "in-kernel" and
"offline tooling."

Status legend used throughout:
- **[CORE/NEW]** genuinely new on-disk format + kernel work we propose to upstream.
- **[NATIVE]** works on a real pool in a lab VM (prototype, validated).
- **[EXPERIMENTAL]** works but least mature; we want direction before hardening.
- **[EXISTING-PRIMITIVES]** achievable today with current ZFS mechanisms — listed
  for completeness of the "reshape" story, **not** an ask for new code.
- **[IMPOSSIBLE-INLINE]** cannot be done in place by construction; only
  `send`/`recv` migration achieves it. Listed so scope is honest.

Not claiming production readiness, a finalized format, or a full ZTS-matrix run.
This is a validated prototype seeking design review before anything is locked.

---

## 1. Bird's-eye view

**The motivating use case is the live virtualization host.** On a Proxmox/ESXi-
style box, the pool backs running VMs and containers (zvols + datasets). Today,
changing RAIDZ redundancy or removing a disk means evacuating/migrating the
guests, exporting the pool or rebooting the host, reshaping, and bringing
everything back — a maintenance window with downtime. RAIDZ *expansion* (2.3)
added online width growth but nothing else; parity change, disk removal, and
in-place geometry changes still require that window. The goal here is to do
those **on the live host with the pool imported and the guests still running** —
using only the pool's own free space, no second pool and no `send | recv`.

This package delivers that across three genuinely-new capabilities plus a family
of supporting/adjacent ones. The **"Live?"** column is the operational bar that
matters for the host use case — can it run without exporting the pool (i.e.
without stopping guests):

| Capability | What it does | Label | Live? (no export) |
|---|---|---|---|
| **A. Parity reshape** | raidz1 ↔ raidz2 ↔ raidz3 (promote / demote) | CORE/NEW, NATIVE | ✅ plain pool (`--online`) |
| **B. Width contraction** | remove a disk from a raidz in place (N → N-1) | CORE/NEW, NATIVE | ❌ needs export today |
| **C. Online operation** | do A while imported and mounted | NATIVE | ✅ (brief per-dataset suspend) |
| **D. Shared-graph reshape** | preserve snapshots/clones across A/B | EXPERIMENTAL | ❌ offline tool (export) |
| **E. Feature coverage** | enc / multi-vdev / gang / DDT / BRT under A/B | NATIVE (dedup opt-in) | ⚠️ enc/multi-vdev live; dedup/BRT offline |
| **F. Perf & observability** | background sweep, throttle, status, cancel | NATIVE | ✅ |
| **G. Codec normalization** | re-checksum / re-compress the existing corpus | EXPERIMENTAL | ⚠️ unshared live (`zfs rewrite`); shared offline |
| **Capacity grow** | replace disks with larger / grow leaves | EXISTING-PRIMITIVES | ✅ `zpool replace` + autoexpand (stock) |

Closing the ❌/⚠️ rows to ✅ — making the shared-graph, contraction, and codec
paths run **in-kernel online** rather than via an offline exported-pool tool — is
the main remaining work; see §1.1.

**One shared foundation underlies A, B, C, D.** A small on-disk *layout-epoch
table* per top-level RAIDZ vdev records `{start_txg, logical_width, parity}`,
and a single choke point selects a block's `{width, parity}` from its physical
birth txg. That is the same shape as the existing
`ZPOOL_CONFIG_RAIDZ_EXPAND_TXGS` machinery — expansion already keys the mapper
on physical birth; we add parity to the selected tuple and make reconstruction
honor a block's own parity. Everything else is a bounded, crash-resumable,
fail-closed sweep that re-encodes the existing corpus through ordinary
copy-on-write and commits the new effective geometry only after a census proves
zero residual.

The design deliberately does **not** touch the RAIDZ row encoding, P/Q/R bytes,
ASIZE math, BP format, ashift, recordsize, or vdev type. Only *which* geometry a
block uses changes, and only via a normal CoW rewrite of that block.

### 1.1 Guest liveness — the operational bar (and the honest gap)

For the live-host use case, "in place" is necessary but not sufficient: the
operation must not **export the pool**, because export unmounts everything and
stops the guests. By that bar the package splits cleanly today:

- **Already live (guests keep running):** plain parity promote/demote via
  `zpool reparity --online` (datasets stay mounted; only a brief, transparent
  per-dataset I/O suspend, on the order of a txg — not a guest outage), capacity
  grow via `zpool replace` + autoexpand (stock), and the async/throttle/status
  controls. Encrypted and multi-vdev pools reshape live too.
- **Offline today (require `zpool export` → guests stop):** the shared-graph
  paths (snapshot/clone-preserving reshape), codec normalization of shared
  blocks, dedup/BRT re-key, and width contraction. These run as an **offline
  libzpool tool** (`zhack …` on the exported pool) because the block-pointer
  rewrite touches snapshot deadlists / clone livelists / DDT / BRT accounting
  that the online sweep (`dmu_buf_will_rewrite` on live datasets) cannot reach —
  so the online engine currently *fail-closed refuses* when snapshots pin
  old-geometry blocks.

**Why this is closeable, and the plan.** The offline passes are already ordinary
`dsl_sync_task`s; they are "offline" only because the tool opens the pool in
userspace rather than driving the kernel-imported pool. Moving them behind the
same ioctl path `zpool reparity --online` already uses makes them run on the live
pool. The key enabling observation: **snapshots are immutable**, so a
snapshot-pinned block has no concurrent writer and can be safely rewritten by an
in-kernel sync task without export; live head/clone blocks get the existing
online-envelope (CoW + compare-and-swap on the BP) treatment; the
deadlist/livelist/DDT/BRT/`ds_unique`/`DD_USED_SNAP` reconciliation runs as sync
tasks exactly as it does offline. This is the "restricted in-kernel rewrite
primitive" in the questions below — and for the Proxmox use case the answer
needs to be in-kernel/online, not an offline tool. Contraction additionally
needs the reflow + metaslab-shrink to run without the current export/import
cycle; that is the hardest of the online conversions and is sequenced last.

---

## 2. The shared foundation  **[CORE/NEW]**

### 2.1 Layout-epoch table (the one new on-disk format)
Append-only `{start_txg, logical_width, parity}` records in each top-level RAIDZ
vdev's ZAP, emitted in the label config alongside the expansion txgs, read at
`vdev_raidz_init()` and re-emitted by `vdev_raidz_config_generate()` from the
ZAP directly (the expansion pattern). Gated behind a new incompatible feature
flag (working name `com.openzfs:raidz_parity_epochs`); inert and byte-neutral
until the first reshape.

### 2.2 Per-block layout selection
Centralized: `(physical_birth_txg, epoch_table) → {logical_width, parity}`.
Blocks decode/reconstruct at the geometry they were written with.

### 2.3 Per-block reconstruction & resilver
`vdev_raidz_combrec()` / `vdev_raidz_need_resilver()` today cap at the vdev's
base `vd_nparity`; that is wrong for a block written at a higher parity on a
still-transitioning vdev. We read the block's own parity
(`rr_firstdatacol` / epoch-aware `vdev_raidz_honored_parity()`).
**This path is unreachable in stock ZFS** (nothing can produce a block with
parity > `vd_nparity`), so it is a no-op there and ships as part of the feature,
not as a standalone "bugfix."

### 2.4 Redundancy realization & the tolerance gate
Effective device-loss tolerance must not rise until the pool is *actually* at
the target geometry. A per-vdev marker gated on a persisted `completion_txg`
governs `vdev_raidz_open()`/`_state_change()`, and is **rewind-safe**: an older
selected uberblock that predates completion still sees the lower tolerance, so
importing into the past can't call a device healthy that the newest state needs.
A pre-commit scrub gate is mandatory before parity rises.

---

## 3. Proposed changes, per feature

### A. RAIDZ parity reshape (raidz1 ↔ 2 ↔ 3)  **[CORE/NEW, NATIVE]**
`zpool reparity [--target N]` appends an epoch, then a bounded per-txg sweep
re-encodes every block to the target parity: file data via `zfs rewrite`-style
CoW re-emission, genesis MOS/dataset metadata via a `dsl_sync_task` that dirties
what `zfs rewrite` can't reach, and a force-condense of every metaslab spacemap
so no untouched spacemap is left behind. **Fail-closed**: commits the new
effective parity only after a census proves zero residual old-parity blocks,
else EAGAIN + resume. Promotion increases redundancy; demotion (raidz3→2→1)
reclaims a parity column as usable space (validated ~25% reclaim, durable across
cold import).
*Validated:* survives exactly `target` disk losses / fails at `target+1`, widths
4–9, {1→2, 2→3} and back, ashift 9/12/13; round-trip parity intact + data intact.

### B. RAIDZ width contraction / single-disk removal  **[CORE/NEW, NATIVE]**
The most-requested "opposite of expansion": remove a disk from a raidz in place.
`zhack raidz_contract` re-encodes every MOS+dataset block of an N-wide raidz to
(N-1)-wide, placed collision-free by a physical-gap allocator (occupancy bitmap +
gap scan + on-device ceiling); then a coordinated vdev-asize + metaslab
**shrink-without-rebuild** (fini only the empty high metaslabs, keep the valid
low ones — sidestepping the grow-only `vdev_metaslab_init` wall) detaches the
emptied child.
*Validated:* 5-disk raidz1 → 4-disk raidz1 with **no tunables**, on a default
`log_spacemap`-enabled pool; vdev size 2.3G→1.75G; data intact; survives all
C(4,1) single losses and fails all C(4,2); fault-injected child + scrub repairs
from parity. *Remaining:* large-pool scale.

### C. Online operation  **[NATIVE]**
`zpool reparity --online` reshapes each mounted dataset in place via ZFS's own
suspend/resume (chunked `zfs_suspend_fs`/resume): mount never dropped, concurrent
reads/writes preserved. A concurrent-write redundancy edge (a write racing the
epoch-establishment txg) is closed by making the epoch effective at
`append_txg + 1`.
*Validated:* concurrent fsync'd writer during promotion, no lost writes, mount
preserved, survives 2-disk loss. *Remaining:* a literal zero-I/O-pause lock-free
CoW/CAS path that removes the brief per-dataset suspend windows entirely.

### D. Shared-graph reshape — snapshots & clones  **[EXPERIMENTAL]**
Reshaping a pool with snapshots/clones means re-encoding **shared** blocks
without breaking deadlists, livelists, bpobj, `ds_unique`, or DDT/BRT refcounts.
Done today by an **offline** libzpool tool (`zhack snap_bpr`) that rewrites each
shared block once, re-points every reference, and reconciles the accounting.
*Validated:* origin + clone survive 2-disk loss, destroys are leak-free
(`freeing` settles to 0, `zdb -bb` "No leaks"). **This is the least mature piece
and the main thing we want direction on** — specifically whether upstream would
prefer a restricted in-kernel rewrite primitive over an offline tool.

### E. Feature-class coverage  **[NATIVE, dedup opt-in]**
- **Encryption:** key-oblivious — RAIDZ parity is below encryption (over
  ciphertext), so reshape is redundancy-only; validated on aes-256-gcm.
- **Multiple top-level RAIDZ vdevs:** per-vdev epoch, shared boundary txg;
  census classifies each block by its DVA's vdev.
- **Gang blocks:** handled by the existing read-reassemble-then-rewrite path.
- **DDT / BRT:** per-refcount re-key / DVA-remap (opt-in); otherwise fail-closed.
All non-handled cases are **fail-closed rejected**, never silently proceeded.

### F. Performance & observability  **[NATIVE]**
`zpool reparity --async | --status | --cancel`: non-blocking background kernel
thread with queryable progress and cancel; bounded per-txg MOS/condense budget;
foreground-load feedback (adaptive backoff so a saturated pool defers the sweep,
never starves foreground I/O). Cancel leaves the op uncommitted/revertible.

### G. Codec normalization  **[EXPERIMENTAL]**
`zhack normalize_bpr` / `normalize_verify`: re-checksum and/or re-compress the
existing corpus (incl. snapshot/clone-pinned blocks) to a target
checksum+compressor, with DDT **re-key** (dedup preserved), BRT DVA-remap,
encryption refused (offline, key-oblivious), and a fail-closed psize-growth
guard. Offline libzpool tool; same maturity caveat as D.

---

## 4. Honest scope: the rest of the "reshape" story

For completeness — several adjacent "reshape" capabilities were investigated.
Most need **no new upstream code**, and some are **impossible in place** by
construction. We list them so the scope is not overstated:

**[EXISTING-PRIMITIVES]** achievable today, no new format:
- recordsize change → CoW data rewrite under the new recordsize;
- volblocksize change → new zvol at target volblocksize + copy (same pool);
- passphrase / wrapping-key rotation → `zfs change-key`;
- capacity shrink / special-class evacuation → `zpool remove`;
- mirror split/merge → `zpool split` / `zpool attach`.

**[IMPOSSIBLE-INLINE]** (vdev-level immutables — only `send`/`recv` migration):
- `ashift` change; RAIDZ ↔ mirror conversion; encryption on/off & cipher change;
  new master key.

So the actual **new-code ask** is Sections 2–3 (A, B, C, D, E, F, G), centered on
the one shared on-disk format in Section 2.

---

## 5. On-disk format & compatibility (please review before we lock it)

- One new incompatible feature flag; inert until first reshape.
- Layout-epoch table in the top-level vdev ZAP; emitted in label config
  alongside expansion txgs; read at `vdev_raidz_init`.
- No change to block layout, BP format, RAIDZ row encoding, or ASIZE math.
- Rewind retains enough epoch history to decode later-written blocks; effective
  geometry for an older selected uber comes from its committed epoch prefix.

Specific questions: ZAP key naming/layout; epoch table in vdev ZAP vs. MOS;
boundary-txg semantics vs. the expansion precedent; feature dependency /
incompatibility class; whether contraction should share this table or carry its
own.

---

## 6. Validation status (consolidated)

Prototype validated in a disposable Ubuntu QEMU/KVM lab VM against
`zfs-2.4.4`-based and current-`master`-based builds:
- per-block column-fault injection (parity-2 block recovers from 2 losses on a
  `vd_nparity=1` vdev; parity-1 control fails);
- device-loss matrices (survive `target` / fail `target+1`) across widths 4–9,
  {1→2, 2→3}, ashift 9/12/13; demotion round-trips;
- multi-cutpoint crash + host power-cut, resumable, no false commit;
- hundreds of soak cycles `fail=0`; clean under KASAN;
- mixed-parity persistence round-trip (import→export→reimport, 0 read mismatches);
- online concurrent-writer, no lost writes; snapshots/clones/enc/multi-vdev/gang
  survive `target` losses with leak-free destroys;
- contraction: N→N-1 tunable-free on default `log_spacemap`;
- ZTS functional cases added (`raidz_parity_epochs_pos`, `raidz_recon_pos`,
  `reparity_*`, `normalize_bpr_pos`).

Explicit gaps: large-pool scale + long-duration soak, a full ZTS matrix under
upstream CI, and hardening of the offline shared-graph/codec tooling (D, G).

---

## 7. Delivery plan

A stacked, `master`-based PR series is prepared, mapping to the sections above:
1. **Foundation** — layout-epoch table + per-block selection + reconstruction
   (Section 2). The reconstruction changes ride here (no-op in stock, so not a
   standalone bugfix PR).
2. **`zpool reparity`** — promotion / demotion / online / async (A, C, F).
3. **Offline `zhack` toolkit** — shared-graph reshape + codec normalization
   (D, G), the experimental tier.

(Width contraction, Section B, is a further slice built on the same foundation;
we can sequence it as a 4th PR or fold it in per review preference.)

Happy to split/reorder/reshape however review prefers.

---

## 8. Questions for maintainers

1. Is in-place RAIDZ reshape (parity change **and** disk removal) something
   OpenZFS wants upstream, and is the shared layout-epoch-table approach the
   right direction (vs. an expansion-style dedicated reflow per operation)?
2. On-disk format review (Section 5) — before we treat it as stable.
3. For the corpus re-encode, and especially the shared snapshot/clone case:
   the CoW + metadata-dirtying sweep as implemented, vs. a dedicated restricted
   in-kernel rewrite primitive? (Sections D, G are offline today.)
4. What validation bar (ZTS/ztest additions, fault campaigns, scale/soak) would
   you want before considering the series?
5. Should width contraction (B) and parity reshape (A) land as one feature or
   two?
