# Common RAID-Z parity fixtures

Neutral input generator for the comparison in
[tigerblue77/zfs#2](https://github.com/tigerblue77/zfs/issues/2).
Published on a separate fork branch; this directory is not proposed for
merging into `parity-joint`. It invokes neither prototype's mover and does
not write an epoch table, mapping or completion certificate.

## Input contract

- Userspace Linux `libzpool`, five 256 MiB sparse file vdevs, RAID-Z1,
  ashift 12, compression off, copies 1, no dedup. The only explicitly
  enabled feature is the common `raidz_parity_epochs` prerequisite;
  feature dependencies are handled by pool creation.
- Eight cases: retained payload fractions **0%, 10%, 30%, 100%**, each
  with **128 KiB and 16 KiB** data blocks. Default payload is always
  100 x 128 KiB (12.5 MiB), independently of block size. Use `--units`
  to scale; multiples of ten avoid rounding fractions.
- These are DMU objects typed `DMU_OT_PLAIN_FILE_CONTENTS`, not pathname
  files or a mounted ZPL filesystem. This matches the current hybrid
  probe's level-0 data type. The pool is `jointfixture`, snapshot
  `jointfixture@retained`; `objects.txt` records object IDs and sizes.
- Write retained object R, snapshot, free R's data from the head, then
  write head-only object H. Thus R/(R+H) is exactly the stated retained
  **logical user payload** fraction. At 0% no snapshot is created; at
  100% the head has no live user payload. The emptied R object remains
  in the head, where its data must read as holes. Metadata, padding,
  indirect blocks and pending frees are outside this fraction and must
  be counted separately by migration collectors. The reported native
  allocation is an observation, not a prediction of mapping rows.
- Fixed SplitMix64-derived, little-endian payload depends on stream tag
  and absolute byte offset, not block size. Independent Python expected
  SHA-256 values and C byte comparisons check the same specification.
  Pool GUIDs, txgs and raw images are not promised byte-identical across
  independently created inputs. Compare logical state and actual allocation
  inventories, not just percentages or raw-image equality between authors.

## Build and run

Use an already built userspace tree of either prototype. Nothing is copied
into or installed from that tree. The helper writes a standalone executable,
actual commands, effective driver macro dump, library/config/source/binary
hashes and `build.json` into a fresh output directory.

```sh
python3 contrib/raidz-parity-fixtures/build.py \
    --tree /absolute/path/to/built-prototype --profile debug \
    --output /var/tmp/joint-fixture-build-01
python3 contrib/raidz-parity-fixtures/run.py \
    --tree /absolute/path/to/built-prototype \
    --build /var/tmp/joint-fixture-build-01 \
    --output /var/tmp/joint-fixture-inputs-01
```

Default input generation preserves the snapshot for the mover. On a separate
fresh validation matrix, `--drop-retained` also exercises final snapshot
removal and another cold read. Do not use that option when preparing the
retained inputs for migration. Each native process has a finite watchdog;
all images and logs remain after success or failure. Every readonly import
must preserve all five raw-member hashes. No kernel module or pool is used.
Keep the absolute member paths stable, or explicitly update an adapter's
import configuration before using copied images.

The C driver also supports `create DIR RECORD_KIB RETAINED_PCT UNITS_128K`,
`verify DIR` (readonly) and `drop-retained DIR` (writable). DIR must be a fresh
absolute directory for creation. It refuses existing member paths. It does
not repair or delete partial failures. The sidecar descriptor is test input,
not transactional pool metadata; retain a pre-operation copy at crash cuts.

### Effective debug/release ABI

`configure --disable-debug` alone is insufficient: `config/Rules.am` forces
`LIBZPOOL_CPPFLAGS=-DDEBUG -UNDEBUG -DZFS_DEBUG` for the library and its
consumers, and ZFS_DEBUG can change shared structures. For a true release
experiment use a **fresh dedicated tree**, configure with
`--with-config=user --disable-debug`, then rebuild all libraries and users
with the override, retaining verbose commands, for example:

```sh
make -j8 V=1 LIBZPOOL_CPPFLAGS='-UDEBUG -DNDEBUG -UZFS_DEBUG' \
    > /var/tmp/joint-release-build.log 2>&1
python3 contrib/raidz-parity-fixtures/build.py \
    --tree /absolute/path/to/fresh-release-tree --profile release \
    --library-build-log /var/tmp/joint-release-build.log \
    --output /var/tmp/joint-fixture-release-01
```

Do not reuse old objects compiled with different flags. The helper checks
its own effective macros and requires a release log containing the override;
this is a provenance check, not an independent proof that every library
object matches that log. Review actual compile rows and retain library-side
macro evidence too. A current Makefile or a profile name cannot establish
how an existing binary was compiled. The published baseline validation is
**debug only**; no true-release qualification is claimed.

## What the two mover adapters must add

Import the exported input, record physical allocation/sharing counts, add
the same target parity-2 epoch, invoke the selected mover, then run the
same byte/cold verification. Use `objects.txt` instead of hardcoded probe
object IDs. Capture source identity and effective macros, rows and complete
retained/peak memory charge, all catalog/journal/allocator costs, copy I/O,
wall time, fragmentation, churn and final-reference deletion. Alternate
runs on matching cache and load settings. This generator alone supplies no
migration or performance result and does not qualify either mechanism.

The initial core deliberately matches the current hybrid's unencrypted
head/snapshot capability. Required extensions remain **pending**, not passed:
clones, dedup, BRT sharing, unloaded encryption keys including ACK-bearing
ZIL, gang and multi-DVA blocks, crash cuts, too-full pools, readers held
across reuse including no-deferral, and offline/return/replacement resilver.
Kernel qualification is a separate campaign. Missing support must be reported
as unsupported/refused, with the reason. The provisional device-removal
50-100 MB/TB reference is not a measured budget for either prototype.

## Author-run baseline validation

[`validation-summary.json`](validation-summary.json) records the finite
Linux VM debug result: all eight retained-input cases (16 native commands)
and eight separate deletion cases (32 commands) passed. C hashes of data
actually read match independent Python expectations; every cold readonly
check preserves all raw members. A deliberately wrong Python expected SHA
is rejected even though native creation/data comparison succeeds. Inputs,
original iterations, failed pre-build style attempts, logs and complete raw
images remain in the author-local archive/VM. The final tested generator
source and driver/library hashes are recorded. The production source46 map
is unchanged and the kernel has no imported pools. This establishes usable
neutral inputs, not migration, memory budgets, release performance, the
extended shared mix or independent reproduction of the peer prototype.
