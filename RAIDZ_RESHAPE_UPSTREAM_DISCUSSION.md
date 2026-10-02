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
> feature. **Liveness status up front:** parity reshape and capacity grow are
> fully online (guests stay live) — including reshaping **the pool the host
> itself boots from** (validated on a ZFS-root rig; requires the fully-live
> no-suspend sweep, since suspending the root deadlocks against the host's own
> writeback). Snapshot/clone-preserving reshape and codec changes run live via a
> validated **procedure** (the engine's persistent epoch + fail-closed EAGAIN +
> in-pool `send|recv` + re-run → commit) rather than a single command. What
> still needs export: **width contraction**, and the *single-command in-place*
> form of the shared-graph/codec rewrite — we have empirical data (six
> characterized failure modes) that the latter needs a new txg-integrated
> deadlist-relocation primitive, a key question for maintainers. See §1.1 and
> the per-feature "Live?" column.

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
| **A. Parity reshape** | raidz1 ↔ raidz2 ↔ raidz3 (promote / demote) | CORE/NEW, NATIVE | ✅ plain pool (`--online`), **incl. the pool the host itself boots from** |
| **B. Width contraction** | remove a disk from a raidz in place (N → N-1) | CORE/NEW, NATIVE | ❌ needs export today |
| **C. Online operation** | do A while imported and mounted | NATIVE | ✅ (chunked-suspend default, or fully-live `nosuspend` sweep) |
| **D. Shared-graph reshape** | preserve snapshots/clones across A/B | EXPERIMENTAL | ✅ **live procedure** (epoch + in-pool `send\|recv`); single-command in-place = open (§1.1) |
| **E. Feature coverage** | enc / multi-vdev / gang / DDT / BRT under A/B | NATIVE (dedup opt-in) | ⚠️ enc/multi-vdev live; in-place DDT/BRT re-key offline (dedup survives the live `send\|recv` route) |
| **F. Perf & observability** | background sweep, throttle, status, cancel | NATIVE | ✅ |
| **G. Codec normalization** | re-checksum / re-compress the existing corpus | EXPERIMENTAL | ✅ **live procedure** (recv-side codec); in-place tool offline |
| **Capacity grow** | replace disks with larger / grow leaves | EXISTING-PRIMITIVES | ✅ `zpool replace` + autoexpand (stock) |

With the validated **live procedure** (§1.1), every row except width contraction
(B) now runs without stopping the pool's consumers — including the case where the
*host itself* is booted from the pool being reshaped. What remains open is (a)
contraction without export and (b) a literal *single-command in-place* variant of
D/G, which needs a new in-kernel rewrite primitive (§1.1, question 3).

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
  `zpool reparity --online` (datasets stay mounted), capacity grow via
  `zpool replace` + autoexpand (stock), and the async/throttle/status controls.
  Encrypted and multi-vdev pools reshape live too.
- **Live via a validated procedure (no export, no new kernel code):** the
  shared-graph paths (snapshot/clone-preserving reshape) and codec
  normalization. The engine's fail-closed design makes this composable:
  `reparity --online` establishes the **persistent** parity epoch up front and,
  when snapshots pin old-geometry blocks, returns EAGAIN *without* committing —
  the epoch stays active, so every new write already lands at the target
  geometry. The operator then relocates the snapshotted data **inside the same
  pool** (`zfs send -R … | zfs recv …`, optionally with recv-side
  `-o compression/-o checksum` for codec changes — snapshots preserved on the
  copy), destroys the old dataset, and re-runs `reparity` → census clean →
  commit. *Validated:* raidz1→2 preserving snapshots commits and genuinely
  survives 2-disk loss (a parity-1 control fails the same test); codec
  off→zstd / fletcher4→sha256 with byte-identical data; a concurrent writer
  never lost its mount. For VM disks the switch to the copy is a live storage
  migration (qemu drive-mirror) — guests never stop.
- **The self-hosting bar — the pool the host *boots from* — also holds.** On a
  system whose root *is* the raidz pool being reshaped (validated on a real
  ZFS-root boot rig): the default chunked-suspend online sweep **deadlocks**
  (suspending the root takes its teardown lock as writer against the host's own
  writeback/reclaim), but the fully-live `nosuspend` sweep (pure
  `dmu_buf_will_rewrite` CoW, a one-tunable opt-in in the series) reshapes the
  booted-from pool with the OS running: commit at parity 2, data intact, a
  heartbeat writing to the root throughout saw **zero** failures, and the
  root-BE's own snapshots are handled live by relocating them to an in-pool
  sibling dataset before the commit — no reboot at any point.
- **Offline today (require `zpool export`):** width contraction, and the
  *single-command in-place* form of the shared-graph/codec rewrite (`zhack …`
  on the exported pool).

**What we learned about making D/G a single in-kernel command.** We implemented
and tested the obvious plan — relocating the offline BPR passes behind the same
ioctl the online engine uses (the engine itself relocates cleanly into a module
shared by libzpool and the kernel). The snapshot-tree half works: snapshots are
immutable, and a sync-task rewrite of snapshot trees on the live pool commits
and survives 2-disk loss. But every variant that touches the **live head's
deadlist** — full tree rewrite, deadlist-only remap, with or without
`zfs_suspend_fs` — hits fatal races against the host's own writers (six
distinct kernel-panic modes, fully characterized). The offline machinery
fundamentally assumes exported-pool quiescence that a live imported pool cannot
provide; per-fs suspend is not enough. A correct single-command in-place variant
therefore needs a **new, txg-integrated relocation primitive** for
deadlist-referenced blocks (hooked into ZFS's own deadlist maintenance), which
is genuine new infrastructure — that is exactly question 3 below. Meanwhile the
live procedure above covers the operational need without it. Contraction
additionally needs the reflow + metaslab-shrink to run without the current
export/import cycle; that is the hardest conversion and is sequenced last.

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
`zpool reparity --online` reshapes each mounted dataset in place. Two sweep
modes: the default chunked `zfs_suspend_fs`/resume (mount never dropped, brief
per-batch pauses), and an opt-in fully-live sweep
(`reparity_online_nosuspend=1`) that never suspends — the rewrite is a plain
`dmu_buf_will_rewrite` CoW tx serialized against concurrent writes by the DMU.
A concurrent-write redundancy edge (a write racing the epoch-establishment txg)
is closed by making the epoch effective at `append_txg + 1`.
*Validated:* concurrent fsync'd writer during promotion, no lost writes, mount
preserved, survives 2-disk loss. The `nosuspend` mode is additionally proven on
the **self-hosting** case — reshaping the pool the OS itself is booted from —
where it is *required*: suspending the root deadlocks against the host's own
writeback/reclaim (teardown-lock cycle, reproduced and root-caused), while the
fully-live sweep commits with a zero-failure heartbeat on the running root.
*Remaining:* heavier concurrent-load soak of `nosuspend` before making it the
default.

### D. Shared-graph reshape — snapshots & clones  **[EXPERIMENTAL]**
Reshaping a pool with snapshots/clones means re-encoding **shared** blocks
without breaking deadlists, livelists, bpobj, `ds_unique`, or DDT/BRT refcounts.
Two routes today:
- **Live procedure (no export, no new code — validated):** epoch active after
  the fail-closed EAGAIN → in-pool `zfs send -R | zfs recv` writes the dataset
  incl. all snapshots at the target geometry → destroy old → re-run → commit
  (§1.1). Covers the operational need, at the cost of a transient second copy
  and a dataset rename/switch.
- **Single-command in-place:** the **offline** libzpool tool (`zhack snap_bpr`)
  rewrites each shared block once, re-points every reference, and reconciles
  the accounting. *Validated:* origin + clone survive 2-disk loss, destroys are
  leak-free (`freeing` settles to 0, `zdb -bb` "No leaks").
**This remains the least mature piece and the main thing we want direction
on.** We have empirical data now (§1.1): running the same BPR passes in-kernel
on the *live* pool works for immutable snapshot trees but is fundamentally
unsafe for the live head's deadlist (six characterized panic modes; per-fs
suspend insufficient) — so a true in-place online variant needs a new
txg-integrated deadlist-relocation primitive, not an adaptation of the offline
tool (question 3).

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
guard. Offline libzpool tool; same maturity caveat as D. The **live route** is
the same procedure as D with recv-side properties — *validated:*
`zfs recv -o compression=zstd -o checksum=sha256` rewrote an off/fletcher4
corpus live (ratio 1.00→205x on compressible data), byte-identical content,
snapshots preserved.

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
  `reparity_*`, `normalize_bpr_pos`);
- the **live procedure** for snapshots/codec (§1.1): control-tested 2-disk
  survival (parity-1 control fails import with 2 missing; the reshaped pool
  imports DEGRADED with snapshot checksums intact), codec change byte-identical,
  concurrent writer's mount never dropped;
- the **self-hosting case**: a nested rig direct-kernel-boots from a raidz ZFS
  root and reshapes it from within — default suspend sweep deadlock reproduced
  and root-caused (root teardown-lock vs the host's own writeback/reclaim),
  `nosuspend` sweep commits parity 2 with a zero-failure heartbeat on the
  running root, data intact, 2-disk survival; the root's own snapshots handled
  live via the in-pool sibling relocation (no reboot).

Explicit gaps: large-pool scale + long-duration soak (incl. `nosuspend` under
heavy load before default-on), a full ZTS matrix under upstream CI, and
hardening of the offline shared-graph/codec tooling (D, G).

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
3. For the corpus re-encode, and especially the shared snapshot/clone case: we
   now have empirical data that the offline BPR passes cannot be safely driven
   on a live imported pool (the live head's deadlist races the host's own
   writers — six characterized failure modes; per-fs suspend insufficient),
   while the *operational* need is covered by the epoch + in-pool `send|recv`
   procedure (§1.1). Is a dedicated **txg-integrated deadlist-relocation
   primitive** (so D/G become a single in-place online command) something
   upstream would want pursued, or is the offline tool + live procedure the
   right long-term shape?
4. What validation bar (ZTS/ztest additions, fault campaigns, scale/soak) would
   you want before considering the series?
5. Should width contraction (B) and parity reshape (A) land as one feature or
   two?
