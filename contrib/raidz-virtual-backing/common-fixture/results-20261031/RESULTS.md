# Virtual/backing on the common RAID-Z fixture

Author-run userspace Linux libzpool comparison: 48 conversions,
eight inputs, three repeats per input in debug and true release.
All conversions and post-snapshot-deletion cold checks passed.
Each run includes four readonly checks preserving all five member
hashes. Values below are medians with [minimum, maximum].

Prototype: `169e96520909f8650a39d4bdf14ace2e755311f1`.
Generator: `6319a29f86a9771c62a4c59b029c9c18a839998b`, unchanged.
Five 256 MiB file vdevs, ashift 12, compression off, copies=1,
12.5 MiB logical payload; physical_rewrite enabled before moving.
Retained fractions describe logical user payload, not metadata.

## Release

| block KiB | retained % | cold current mappings | cold retired owners | after-drop current mappings | whole mover s | sweep + final fence s |
|---|---|---|---|---|---|---|
| 128 | 0 | 269 [269, 269] | 84 [81, 84] | 278 [278, 278] | 1.95 [1.69, 2.28] | 1.46 [1.30, 1.83] |
| 128 | 10 | 291 [291, 291] | 84 [84, 84] | 277 [277, 277] | 1.79 [1.37, 1.92] | 1.33 [1.03, 1.45] |
| 128 | 30 | 291 [291, 291] | 84 [81, 84] | 257 [257, 257] | 1.73 [1.51, 1.81] | 1.34 [1.12, 1.36] |
| 128 | 100 | 289 [289, 289] | 84 [81, 84] | 185 [185, 185] | 1.47 [1.45, 1.80] | 1.10 [1.08, 1.33] |
| 16 | 0 | 969 [969, 969] | 84 [84, 84] | 978 [978, 978] | 2.87 [2.31, 3.19] | 2.41 [1.89, 2.44] |
| 16 | 10 | 991 [991, 991] | 84 [84, 84] | 907 [907, 907] | 2.74 [2.44, 2.90] | 2.29 [2.11, 2.39] |
| 16 | 30 | 991 [991, 991] | 84 [84, 84] | 747 [747, 747] | 2.90 [2.36, 3.12] | 2.40 [2.00, 2.62] |
| 16 | 100 | 989 [989, 989] | 84 [84, 84] | 185 [185, 185] | 2.73 [2.30, 2.84] | 2.31 [1.96, 2.34] |

| block KiB | retained % | raw catalog reserved MiB | journal MiB | maximum observed heap MiB | converter writes MiB |
|---|---|---|---|---|---|
| 128 | 0 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 13.40 [13.18, 14.11] | 39.52 [39.48, 39.56] |
| 128 | 10 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 13.85 [13.17, 14.46] | 40.48 [40.44, 40.54] |
| 128 | 30 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 15.16 [14.22, 15.18] | 40.50 [40.44, 40.54] |
| 128 | 100 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 14.43 [13.80, 14.76] | 40.31 [40.27, 40.39] |
| 16 | 0 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 10.50 [9.62, 12.10] | 65.54 [65.42, 65.55] |
| 16 | 10 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 9.38 [8.99, 10.03] | 66.59 [66.55, 66.59] |
| 16 | 30 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 10.53 [9.58, 10.76] | 66.63 [66.61, 66.64] |
| 16 | 100 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 9.54 [8.85, 10.16] | 66.47 [66.42, 66.52] |

Environment and exact metric vectors are retained in
`report-release.json`.

## Debug

| block KiB | retained % | cold current mappings | cold retired owners | after-drop current mappings | whole mover s | sweep + final fence s |
|---|---|---|---|---|---|---|
| 128 | 0 | 269 [269, 269] | 84 [81, 84] | 278 [278, 278] | 1.92 [1.85, 1.94] | 1.44 [1.43, 1.45] |
| 128 | 10 | 291 [291, 291] | 84 [81, 84] | 277 [277, 277] | 2.02 [1.67, 2.08] | 1.57 [1.27, 1.59] |
| 128 | 30 | 291 [291, 291] | 84 [84, 84] | 257 [257, 257] | 1.97 [1.72, 2.08] | 1.38 [1.34, 1.41] |
| 128 | 100 | 289 [289, 289] | 84 [84, 84] | 185 [185, 185] | 1.93 [1.86, 2.22] | 1.46 [1.25, 1.71] |
| 16 | 0 | 969 [969, 969] | 84 [84, 84] | 978 [978, 978] | 2.77 [2.71, 2.81] | 2.30 [2.28, 2.36] |
| 16 | 10 | 991 [991, 991] | 84 [81, 84] | 907 [907, 907] | 2.57 [2.57, 3.07] | 2.18 [2.15, 2.60] |
| 16 | 30 | 991 [991, 991] | 84 [81, 84] | 747 [747, 747] | 2.96 [2.60, 3.00] | 2.51 [2.21, 2.60] |
| 16 | 100 | 989 [989, 989] | 84 [81, 84] | 185 [185, 185] | 3.12 [3.09, 3.33] | 2.62 [2.48, 2.76] |

| block KiB | retained % | raw catalog reserved MiB | journal MiB | maximum observed heap MiB | converter writes MiB |
|---|---|---|---|---|---|
| 128 | 0 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 13.80 [13.65, 14.51] | 39.52 [39.50, 39.54] |
| 128 | 10 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 13.95 [13.93, 14.44] | 40.49 [40.46, 40.55] |
| 128 | 30 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 14.45 [14.14, 15.98] | 40.54 [40.50, 40.55] |
| 128 | 100 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 14.79 [13.89, 14.84] | 40.27 [40.25, 40.39] |
| 16 | 0 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 9.18 [9.15, 9.45] | 65.55 [65.44, 65.57] |
| 16 | 10 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 9.82 [8.41, 10.04] | 66.54 [66.54, 66.63] |
| 16 | 30 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 9.50 [9.07, 10.25] | 66.62 [66.59, 66.72] |
| 16 | 100 | 20.00 [20.00, 20.00] | 5.00 [5.00, 5.00] | 9.40 [9.17, 9.70] | 66.48 [66.32, 66.52] |

Environment and exact metric vectors are retained in
`report-debug.json`.

## Interpretation

Whole mover time covers activation, epoch creation, conversion,
fencing, certification and census; it excludes payload checking.
The converter I/O window begins after activation/epoch and ends
after final census. Raw reports retain its separate counters and
the inclusive mover I/O window. Current mappings include metadata
and new allocations paired by this prototype. The total owners
counter also includes retired owners; all three counters and hot
and cold snapshots are recorded. Snapshot deletion includes admin
metadata updates, ordinary txgs and export, so its net row delta
does not isolate the deleted payload population.
Retired bytes include pending strong metadata versions; they are
not necessarily the cost of snapshot-retained old data. Native
allocation fields are vdev_stat observations: readonly imports may
leave these native counters unloaded and report zero. Use the
writable before-mover snapshot for the initial native inventory.
Native allocation accounts for the virtual domain; physical copy
space is described by the engine census. These are not disjoint
charges that can be added together.

Catalog/journal bytes are raw reserved extent accounting across
the members, not live MOS table bytes or an in-memory charge.
Heap is the largest observed glibc uordblks sample during the mover
(nominal 1 ms sampling), including libzpool ARC/dbufs. It excludes
mmap-backed allocations and is not a complete allocator or kernel
RAM peak. Process-lifetime ru_maxrss is reported separately.

The peer results were obtained on another VM. Shared inputs make
the input workload reproducible. Counting coverage also differs:
virtual/backing includes MOS and subsequent paired allocations,
whereas the redirect table excludes MOS. Row differences therefore
include those policy and population differences. These times do not
establish a three-way speedup on one machine. Host page cache was
not flushed; native processes ran sequentially with niceness +10.
Our ARC cap is 256 MiB; the 29 October peer report used its default
ARC cap of 5,164,007,424 bytes and a nominal 10 ms heap sampler.
Its physical_rewrite-alignment rerun is pending separately.
[Peer report](https://github.com/tigerblue77/zfs/blob/98dc0fa/contrib/raidz-redirect-spike/fixture-results-20261029.txt).

Clones, dedup/BRT, keyless ZIL, gang/multi-DVA, crash/full-pool and
replacement/kernel qualification remain outside this core matrix.
Raw images remain in the author VM. Process logs, library build
logs and macro dumps are published in the compressed archive.

## Artifacts

[Release raw JSON](report-release.json), [debug raw JSON](report-debug.json),
[process/build logs](raw-logs-20261031.tar.gz),
[separate audit](independent-audit-20261031.json),
[validation record](VALIDATION.md).
