# Author-run validation, 31 October 2026

The tested production source is the published virtual/backing prototype
`169e96520909f8650a39d4bdf14ace2e755311f1`. All 46 production/test files
match its manifest, SHA-256
`9d0ba115675bffc9e23c8f9a28d6c6ef54aa32d7622d1293b2f964bb84f59e4f`.
The adapter, builder and runner used for the final matrix are committed at
`63202c07a5e044875d9a12217e802e27930e4266`. Their exact hashes are in both
raw reports. The final publication additionally contains the report generator,
measurement interpretations, reproduction instructions and result artifacts.

## Runtime and build evidence

- Eight default inputs, three repeats, two profiles: 48/48 conversions pass.
- All 384 native CLI commands exit zero.
- All 192 readonly operations preserve the recorded hashes of all five
  members: 960 before/after member pairs.
- All 792 C payload-verification records match a separate recomputation
  from the payload specification, independent of the generator helper.
- Conversion and post-deletion cold states have no legacy/weak backing,
  certified parity 2, matching owner populations and a balanced physical
  allocation partition. The summary checks these against the raw counters.
- Both profiles use newly built matching libraries and consumers. Each has
  183 inspected libzpool compile recipes. Actual library-source macro dumps
  for `kernel.c`, `spa.c` and `zio.c` confirm the intended profile. Release
  has `DEBUG=false`, `ZFS_DEBUG=false`, `NDEBUG=true`.
- Every raw stdout/stderr hash (768 files), phase record, binary/library
  identity, timer and I/O delta is rechecked in the separate local audit.
  This is an author-side verification, not independent peer reproduction.
  The audit checks the recorded member-hash pairs; it does not perform an
  additional complete reread of the final raw images.

The authoritative VM outputs are
`/home/dima/vb-common-20261031/matrix-{release,debug}-02`, built with
`adapter-{release,debug}-final03`. Images and earlier attempts remain there.
Preparatory build-log matcher failures and the interrupted preliminary
`matrix-release-01` are excluded from the final 48-run result. That first
matrix was stopped to finish licensing headers and rebuild the exact final
driver sources; its five completed cases are not selected into this report.

The VM remains on Linux `6.8.0-147-generic`, ZFS srcversion
`D62EED3EB76A1F039ED90D7`, SPL `416A9EC5515380D6BDC3382`, with no imported
kernel pools. All comparison pools ran inside userspace libzpool on new files.

## Tool checks and limits

C-style, executable test metadata, tracked-file checks, commit checks and
flake8 pass for the added work. Seven parser acceptance tests pass normally
and with `python3 -O`. Six deliberately corrupted copies of the actual
release report are rejected in both modes: partial status, duplicate case,
wrong readonly phase, wrong macro profile, inconsistent physical partition
and weak backing.

The whole-tree `make checkstyle` is not green on the frozen baseline: its
SPDX checker reports 20 pre-existing incomplete CDDL boilerplates, exactly
the same set on the unchanged publication source and on the final branch.
No new file is in that failure set. The host style-only build also has no
standalone zstd objects for the symbol check. These limitations are retained;
no global CI or full upstream qualification is claimed.

## Published artifacts

- [Combined results](RESULTS.md), with medians, ranges and metric boundaries.
- [Release report](report-release.json) and [debug report](report-debug.json).
- [Source/report inputs](inputs.json).
- [Separate audit result](independent-audit-20261031.json).
- [Compressed process/build logs and macro dumps](raw-logs-20261031.tar.gz).
  Raw vdev files and pool-cache files are excluded from this archive.

Archive SHA-256:
`5c676f3d4967483b5e9c03778db66391d3b6a19f49a2f3405c93d1089276e1c0`.
The raw report hashes are recorded in `inputs.json` and the separate audit.

Standalone BP-walk/scrub timing, flush counts, full incremental allocator
attribution, ENOSPC/churn and the extended sharing/failure/kernel matrices
remain open. The finite core result does not select the joint product design.
