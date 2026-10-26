# RAID-Z virtual/backing prototype

Comparison source for [tigerblue77/zfs#2](https://github.com/tigerblue77/zfs/issues/2)
and the joint `parity-joint` design discussion. This branch is an experimental
review snapshot. Its PR is not intended to be merged.

The prototype raises parity by copying physical backing beneath unchanged
block pointers. It keeps a virtual allocation identity V and one or more
physical versions P. Current new allocations are paired too; keeping new
writes identity-mapped without rows is a proposed design change, not a property
of this snapshot. The shared document must decide whether exclusively owned
heads are normally rewritten and only the remaining populations relocated.

Use disposable file vdevs in a VM. The bootstrap, label extension, module
parameters and test hooks are experimental. There is no supported
administration interface or agreed integrated feature/format. The existing
private-label refusal guard does not establish compatibility qualification.

## Source and review boundary

The prerequisite snapshot is
`b0de7972fbfd318d3e320345b786b776fe68b9ff`. It carries the parity-epoch
foundation and its then-current upstream merge. The subsequent prototype
history preserves authorship and commit trailers. The final activation
snapshot includes the checkpoint, native-free and startup-admission fixes.

To isolate the implementation from the prerequisite series and upstream
changes in the comparison PR:

```sh
git diff b0de7972fbfd318d3e320345b786b776fe68b9ff..HEAD -- \
    cmd include module tests
```

All 46 changed source/test files are listed in
[`source46-sha256.json`](source46-sha256.json). The SHA-256 of that file is
`9d0ba115675bffc9e23c8f9a28d6c6ef54aa32d7622d1293b2f964bb84f59e4f`.
This is the exact production-side source manifest used for the latest
candidate05 experiments. The public packaging adds this directory; it does
not change those 46 files. Some focused experiments add userspace observers
and drivers; removing those additions restores this manifest exactly.

This snapshot is based on its tested prerequisite, rather than rebased onto
the newer `parity-joint` base. A mergeable upstream series needs a separate
preparation and validation pass. `openzfs/zfs#19262` is unchanged.

## Where to read

| Concern | Source |
| --- | --- |
| Allocation identity, physical versions, holds and dispatch leases | `module/zfs/vdev_reloc_engine.c`; native hooks in `metaslab.c`, `zio.c`, `vdev_raidz.c` |
| Physical credits and transaction admission | `vdev_reloc_admit.c`, `vdev_reloc_space.c`, `vdev_reloc_tx.c`, `dmu_tx.c` |
| Raw bootstrap, catalog and bounded page cache | `vdev_reloc_store.c`, `vdev_reloc_pages.c`, `vdev_label.c`, `spa.c` |
| Acknowledged-write bindings and journal recovery | `vdev_reloc_journal.c`, `zil.c`, `zio.c` |
| Bounded traversal, copies, census, durable floor and release | `vdev_reloc_convert.c`, `dmu_traverse.c` |
| Kernel experimental controls and partial RAM snapshot | `vdev_reloc_kernel.c` |
| Native fixture drivers and cold-import checks | `cmd/ztest.c`, `cmd/zdb/zdb.c` |
| Kernel file-vdev tests | `tests/zfs-tests/tests/functional/pr19262/` |

Several implementation units are included by the enclosing source file;
they are not all independent compilation units. Diagnostic paths and
failure assertions are retained for review. The small change to
`module/zstd/zfs_zstd.c` is a KASAN build compatibility workaround used by
the test VM, not part of the relocation design or a proposed upstream fix.

## Small native run

Build in a disposable Linux VM with the normal OpenZFS build dependencies:

```sh
./autogen.sh
./configure --with-config=user --enable-debug --prefix=/usr/local
make -j8
make unit
make checkstyle
python3 contrib/raidz-virtual-backing/smoke.py \
    --output /var/tmp/raidz-virtual-backing-smoke-01
```

The output directory must not exist. The script creates six new sparse
256 MiB files, calls the native `ztest` conversion fixture, checks a clean
scrub and a parity-2 census/certificate, then starts a separate readonly
import to verify payloads. It checks that the readonly operation changes
none of the six raw members. It records actual binary hashes, commands,
source identity and exits in `report.json`, with separate stdout/stderr.
The default watchdog is 300 seconds per command. No module is installed or
loaded; this uses `libzpool`. Images and logs are retained on success and
failure. This is a reproduction smoke test, not the common performance
fixture or the full qualification campaign.

For kernel work, the registered `pr19262_online_pos`,
`pr19262_rewind_pos`, `pr19262_degraded_pos` and `pr19262_classes_pos`
tests exercise the experimental driver in a disposable Linux ZTS VM.
Module replacement and ZTS setup need the usual dedicated test environment;
they are not steps performed by the native smoke script.

## Evidence and limits

Author-run local evidence for this source, not independent verification:

| Experiment | Observed boundary |
| --- | --- |
| Linux 6.8.12 VM build | Kernel build, unit tests and full style pass for candidate05 |
| Native regression matrices | Debug/release, 36 conversion/recovery commands plus 13 torn-write commands per flavour; original bounded outcome predicates retained |
| Live activation | Kernel run with 64 ordered fsync acknowledgements spanning activation; cold head and two snapshot payloads, journal rollover and certificate |
| Startup space admission | Successful activation, missing catalog reserve, missing journal reserve after catalog selection; native release and actual kernel controls |
| In-flight activation | Eight named native schedules per debug/release; three named kernel schedules for READY-delayed logical I/O, native SCL drain and real sorted scan queue |
| Preactivation checkpoint | Cold native import/enable, readonly preview, refused writable rewind, fence refusal until discard, subsequent retirement/release and cold reads |
| Snapshot/clone turnover | Two native deletion orders per flavour; held read, final-reference retirement, exact V/P exclusion, fresh ESTALE, ordinary floor/drain/release and cold head |

`smoke.py` exercises a smaller subset. Large raw fixtures, detailed observer
campaigns and original failure logs remain in the local evidence archive;
their complete matrices are not bundled by this comparison branch. Local
passes do not imply CI for the public PR head, FreeBSD qualification, real
device or power-loss qualification, or arbitrary repair/activation coverage.

The whole-engine RAM census is open. The earlier public **369 B per
allocation** was a historical partial measurement on a particular
source/fixture/cache setting, not a complete current allocator charge or
peak bound. Current linked-record counters also exclude some transient,
native/shared and allocator costs. No newer lower total is claimed here.

Other open items include native no-deferral turnover, a genuine
journal-touched rollback witness, remaining delayed repair and activation
intersections, the passive ARC identity investigation, product controls,
integrated feature/format and space-accounting promises, realistic scale and
independent review. A native API control for journal-bind-then-free is
labelled as such in the source; it is not evidence of a natural rollback.

Checkpoint support is a prototype mechanism with reclaim pinned while a
checkpoint exists. The initial product policy remains a joint decision.
The current per-SPA implementation retains one translated top-level vdev
for that engine lifetime; several SPAs have separate state. Expansion,
narrowing and parity reduction are outside this comparison's agreed goal.
