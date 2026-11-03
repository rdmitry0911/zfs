# RAID-Z parity conversion: engineering progress, 3 November 2026

Research update by Dmitry R (`rdmitry0911`), with tigerblue77 on the
comparison and joint design.

Changing the parity of future writes does not strengthen blocks already
allocated on a RAID-Z vdev. Completing an online parity increase requires
a mechanism that also covers snapshot-held, shared, keyless and metadata
allocations, with safe lifetime, recovery and space rules.

The work now has two independent fixes merged into OpenZFS, public
prototype and reproduction sources, a completed finite userspace fixture
matrix, and a jointly reviewed design draft. This report indexes the
existing evidence at fixed source revisions. The integrated format,
supported interface and remaining qualification are still open work.

## Changes accepted upstream

| Change | Result | Evidence and boundary |
| --- | --- | --- |
| Atomic checkpoint-space accounting during parallel vdev load/teardown | OpenZFS #19511 merged on 22 October | [PR #19511](https://github.com/openzfs/zfs/pull/19511), [merge commit](https://github.com/openzfs/zfs/commit/1ad9b782c3c1513204ad32dab628611708bc08b9). The change addresses a race identified in the code; the original intermittent CI failure was not reproduced locally. The targeted 300-cycle witness passed both before and after the change. |
| Keep the simulated MMP host alive through a long sequence of import probes | OpenZFS #19686 merged on 3 November | [PR #19686](https://github.com/openzfs/zfs/pull/19686), [merge commit](https://github.com/openzfs/zfs/commit/cc604603a11f636bbe6d9d7eee0d25814884a3ec). The test uses 600 seconds instead of the helper's default 120; other callers keep their default. The reported local VM runs did not reproduce the old helper-expiry failure. |

Both changes originated in the work around the parity-epoch series. The
MMP patch was submitted upstream by tigerblue77 with Dmitry R's authorship
and sign-off preserved. These accepted changes are independent of a future
decision on the parity-conversion format.

## Public prototype and common inputs

The virtual/backing prototype relocates physical backing beneath unchanged
block pointers. Its current implementation also pairs new allocations;
keeping persistent rows only for relocated allocations is a proposed
convergence direction, not a property of this published snapshot.

The public comparison contains the source map, a 46-file production/test
manifest and a small native smoke driver. The separate common generator
creates equivalent logical inputs without invoking either mover. The
common-fixture adapter then runs the virtual/backing implementation on
those inputs.

| Material | Fixed revision and entry point |
| --- | --- |
| Production-side virtual/backing snapshot | [`169e96520909f8650a39d4bdf14ace2e755311f1`](https://github.com/rdmitry0911/zfs/blob/169e96520909f8650a39d4bdf14ace2e755311f1/contrib/raidz-virtual-backing/README.md) |
| Exact 46-file source manifest | [`source46-sha256.json`](https://github.com/rdmitry0911/zfs/blob/169e96520909f8650a39d4bdf14ace2e755311f1/contrib/raidz-virtual-backing/source46-sha256.json), SHA-256 `9d0ba115675bffc9e23c8f9a28d6c6ef54aa32d7622d1293b2f964bb84f59e4f` |
| Unchanged neutral generator | [`6319a29f86a9771c62a4c59b029c9c18a839998b`](https://github.com/rdmitry0911/zfs/blob/6319a29f86a9771c62a4c59b029c9c18a839998b/contrib/raidz-parity-fixtures/README.md) |
| Tested virtual/backing adapter, builder and runner | [`63202c07a5e044875d9a12217e802e27930e4266`](https://github.com/rdmitry0911/zfs/tree/63202c07a5e044875d9a12217e802e27930e4266/contrib/raidz-virtual-backing/common-fixture) |
| Published commands, reports, logs and interpretations | [`c3e59b5d3cb1a9656274aa6c12976b56e85ad343`](https://github.com/rdmitry0911/zfs/tree/c3e59b5d3cb1a9656274aa6c12976b56e85ad343/contrib/raidz-virtual-backing/common-fixture) |

The prototypes are available for side-by-side review in
[tigerblue77/zfs#3](https://github.com/tigerblue77/zfs/pull/3) (redirect)
and [#4](https://github.com/tigerblue77/zfs/pull/4) (virtual/backing).
Those experimental code PRs are comparison artifacts.

## Completed virtual/backing core matrix

The 31 October campaign uses five 256 MiB sparse file vdevs, ashift 12,
12.5 MiB of logical user payload, compression off and copies=1. Eight
inputs combine 0%, 10%, 30% and 100% snapshot-retained user payload with
128 KiB and 16 KiB records. Each input runs three times in debug and
three times in true release: **48 of 48 conversions passed**.

The retained percentages describe logical user payload, not the fraction
of all metadata or physical allocations. The published campaign runs in
userspace Linux libzpool; it does not install or exercise a kernel module.

| Recorded check | Result |
| --- | --- |
| Native CLI commands | 384 completed with exit status zero |
| Readonly operations | 192, each preserving the hashes of all five raw members: 960 before/after member pairs |
| Payload checks | 792 saved C verification records match hashes independently recomputed from the payload specification |
| Converted and post-deletion cold states | Certified parity 2, zero legacy/weak backing, checked owner populations and physical allocation partition |
| Build profiles | Matching libraries and consumers; 183 actual libzpool compile recipes inspected per profile, with effective macro dumps |

True release explicitly overrides
`LIBZPOOL_CPPFLAGS='-UDEBUG -DNDEBUG -UZFS_DEBUG'` for libraries and their
consumers. `--disable-debug` alone does not establish a release libzpool:
the build rules otherwise force its debug macros.

A separate author-side audit rehashes and parses 768 saved stdout/stderr
streams, regenerates expected payload hashes and checks the saved records
and readonly hash pairs. It does not re-execute the commands or perform an
additional complete reread of the final raw images. This remains author-run
evidence, not independent peer reproduction.

The [results](../common-fixture/results-20261031/RESULTS.md) and
[validation record](../common-fixture/results-20261031/VALIDATION.md)
contain the raw JSON, medians/ranges, build identities, failures excluded
from the final matrix, and verification limits.

## Shared-generator comparison

tigerblue77's
[1 November feature-aligned rerun](https://github.com/tigerblue77/zfs/blob/55420b5dea97a59365dab285625941c9eece760c/contrib/raidz-redirect-spike/fixture-results-20261101.txt)
reports 96 successful debug/true-release runs across hybrid and all-redirect
modes. `physical_rewrite` and its `extensible_dataset` dependency are enabled
before either mover on every run copy; the hybrid adapter enables nothing.
Its entry counts equal the
[29 October report](https://github.com/tigerblue77/zfs/blob/98dc0fa7f4d6a73c598f8b37b4d59e1b81f5b30d/contrib/raidz-redirect-spike/fixture-results-20261029.txt)
in every tree/case/mover cell. These are tigerblue77's author-run results.
The 31 October virtual/backing report's statement that this rerun was
pending describes its publication date; the rerun is now complete.

The table shows cold readonly counts after conversion, then after the
snapshot-deletion lifecycle. Each cell represents three true-release runs.

| Record size | Retained user payload | Hybrid redirect entries | All-redirect entries | Virtual/backing current bindings |
| --- | --- | --- | --- | --- |
| 128 KiB | 0% | 0 → 0 | 116 → 116 | 269 → 278 |
| 128 KiB | 10% | 26 → 0 | 132 → 106 | 291 → 277 |
| 128 KiB | 30% | 46 → 0 | 132 → 86 | 291 → 257 |
| 128 KiB | 100% | 130 → 14 | 130 → 14 | 289 → 185 |
| 16 KiB | 0% | 0 → 0 | 816 → 816 | 969 → 978 |
| 16 KiB | 10% | 96 → 0 | 832 → 736 | 991 → 907 |
| 16 KiB | 30% | 256 → 0 | 832 → 576 | 991 → 747 |
| 16 KiB | 100% | 830 → 14 | 830 → 14 | 989 → 185 |

These columns count different populations. Virtual/backing includes data,
MOS and other metadata, including new paired allocations. The redirect
table counts redirected DVAs, excludes MOS and, under the hybrid, excludes
exclusive-head blocks successfully rewritten. Retired owners are recorded
separately from virtual/backing's current bindings. Administrative metadata,
txgs, fencing and export also change the counts: the 0%-retention no-op drop
adds nine current bindings, so the net delta does not isolate deleted
snapshot payload.

The authors used different VMs, ARC limits, measurement boundaries and
sampler intervals. These reports do not establish a three-way speedup or
a complete memory-cost ranking. Virtual/backing's 20 MiB catalog and 5 MiB
initial journal figures are raw reservations across the five members,
not live table payload or RAM. Its maximum observed glibc `uordblks` sample
during the mover, with nominal 1 ms sampling, includes ARC/dbufs and excludes
mmap-backed allocations; process-lifetime
`ru_maxrss` is separate. Full incremental allocator/kernel charge and the
simultaneous peak remain open.

The readonly hash guarantees also differ: virtual/backing brackets each
readonly operation with five member hashes; the redirect rerun checks its
16 original input sets before and after the campaign, without collecting
those pairs around each working copy's readonly imports. Redirect reports
separate census-walk/scrub timing and 48 debug fills to ENOSPC after snapshot
deletion, without suspension and with zero gang blocks. Those fills do not
establish initially-too-full or snapshot-retained admission safety. The
published virtual/backing core includes neither experiment.

## Reproduce and inspect

Use the pinned production/publication source and fetch the neutral generator
separately. Follow the
[common-fixture build and run instructions](https://github.com/rdmitry0911/zfs/blob/c3e59b5d3cb1a9656274aa6c12976b56e85ad343/contrib/raidz-virtual-backing/common-fixture/README.md)
in fresh matching debug and release userspace trees, retaining the actual
library build logs. The default run is 24 conversions per profile. Use new
output directories and disposable file-vdev fixtures.

The published evidence includes
[release JSON](../common-fixture/results-20261031/report-release.json),
[debug JSON](../common-fixture/results-20261031/report-debug.json),
[input/build identities](../common-fixture/results-20261031/inputs.json),
[the saved-stream audit](../common-fixture/results-20261031/independent-audit-20261031.json)
and [compressed logs/macro dumps](../common-fixture/results-20261031/raw-logs-20261031.tar.gz).
The archive SHA-256 is
`5c676f3d4967483b5e9c03778db66391d3b6a19f49a2f3405c93d1089276e1c0`.
Raw vdev images and pool-cache files are not embedded in that archive.

The older prototype smoke summary and configure-labelled release observations
have their own dates and limits; the 31 October common-fixture validation is
the authority for the 48-run and true-release claims above. Its validation
record also retains whole-tree style limitations on the frozen baseline;
this summary adds no new runtime or upstream-CI result.

## Design status and remaining work

The jointly reviewed five-part
[design draft at `dfb1047732e9c130198c146873919f3a75577e67`](https://github.com/tigerblue77/zfs/blob/dfb1047732e9c130198c146873919f3a75577e67/contrib/raidz-parity-joint/DESIGN.md)
is public: document PRs [#5](https://github.com/tigerblue77/zfs/pull/5)
and [#6](https://github.com/tigerblue77/zfs/pull/6) merged on 3 November.
It compares rewrite-plus-redirect and virtual/backing, records the common
evidence, lists the open decisions and proposes a landing order.

The current goal remains an online, same-width parity increase. D1-D8
retain open choices about population, bootstrap, acknowledged writes,
lifetime/rewind/checkpoints, completion, degraded authority/member repair,
space and administration. Broader relocation applications are under
separate [document review in #7](https://github.com/tigerblue77/zfs/pull/7).
An agreed discussion draft is not maintainer acceptance of a public format.

The common matrix does not qualify clones, dedup/BRT, keyless ACK-bearing
ZIL, gang/multi-DVA, held readers across reuse, crash cuts, initially-full
pools, return/replacement or kernel behavior. Full allocator/shared/transient
RAM attribution, remaining activation/repair intersections, real-device and
power-loss qualification, a supported interface and independent review remain
open. Older kernel observations have separate source and fixture boundaries;
they are not relabelled as this common userspace campaign.

As agreed in the
[coordination thread](https://github.com/tigerblue77/zfs/issues/2#issuecomment-6600377941),
the joint OpenZFS Discussion will follow the proposed hybrid kernel report
and review of the additional framing text. This dated author report makes
the already public results and reproduction material easier to inspect
while that work continues.
