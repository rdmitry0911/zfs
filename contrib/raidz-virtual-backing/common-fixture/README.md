# Virtual/backing column on the common fixture

This userspace adapter runs the virtual/backing prototype on the unchanged
neutral generator at `6319a29f86a9771c62a4c59b029c9c18a839998b`. It creates
five 256 MiB sparse file vdevs and the generator's default 12.5 MiB logical
payload, at 0/10/30/100% snapshot retention and 128/16 KiB records. It uses
the object IDs from `objects.txt`; it does not substitute a mounted workload.

The adapter is experimental test code. It does not add a supported product
interface or change the production source manifest of the public prototype.

## Reproduce

Fetch the fixture branch separately. Build fresh matching debug and release
userspace trees of this prototype. The true-release build must override
`LIBZPOOL_CPPFLAGS='-UDEBUG -DNDEBUG -UZFS_DEBUG'` for every library and
consumer and retain verbose compiler commands. `--disable-debug` alone does
not establish a release libzpool ABI. The companion `build.py` creates both
native drivers and records their source, macro, library and binary identities.

For the library build used here, in a fresh debug checkout:

```sh
./autogen.sh
./configure --with-config=user --enable-debug --prefix=/usr/local
make -j2 V=1 LIBZPOOL_CPPFLAGS='-DDEBUG -UNDEBUG -DZFS_DEBUG' \
    libzpool.la libzfs_core.la libnvpair.la librange_tree.la \
    > /absolute/path/to/debug-build.log 2>&1
```

In a separate fresh release checkout, use `--disable-debug` and
`LIBZPOOL_CPPFLAGS='-UDEBUG -DNDEBUG -UZFS_DEBUG'` with the same targets.
These builds create the required userspace libraries; no installation or
kernel/module replacement is needed. Then build the matching drivers:

```sh
python3 contrib/raidz-virtual-backing/common-fixture/build.py \
    --tree /absolute/path/to/debug-tree --profile debug \
    --fixtures /absolute/path/to/common-fixtures/contrib/raidz-parity-fixtures \
    --library-build-log /absolute/path/to/debug-build.log \
    --output /absolute/path/to/debug-drivers
python3 contrib/raidz-virtual-backing/common-fixture/run.py \
    --tree /absolute/path/to/debug-tree --profile debug \
    --build /absolute/path/to/debug-drivers \
    --fixtures /absolute/path/to/common-fixtures/contrib/raidz-parity-fixtures \
    --output /absolute/path/to/new-debug-results --repeats 3
```

Repeat with a fresh release tree and driver directory and its actual clean
release `--library-build-log`. Both profiles require their build logs. A default
campaign runs 24 conversions per profile, three repetitions of all eight
cases. `--repeats 1 --only-case r128-retained0` selects a bounded smoke run;
its report marks it as a partial matrix. The output path must be new. Logs,
descriptors and all five raw members remain after success or failure; the
runner never overwrites or deletes a partial attempt. Preserve their absolute
paths when opening these images again.

Native commands run with a finite watchdog and, by default, a nice increment
of 10. All `PR19262_` environment settings and `LD_PRELOAD` are excluded;
the selected tree supplies `LD_LIBRARY_PATH`. Benchmark while builds and
other test workloads are idle. A fresh process is used for each command;
there is no claim of a cold host page cache. Different VM/hardware/load
settings preclude direct absolute-time conclusions between authors.

## Checks and report

Each fresh case has the following lifecycle:

1. Create with the unchanged generator and cold-read its payload.
2. Enable `physical_rewrite`, record it before the move, and cold-read again.
3. Activate the persistent virtual/backing engine, add the target parity-2
   epoch, convert, fence and certify; retain the complete before/after census.
4. Cold readonly import and verify every head/snapshot payload and head hole.
5. Drop the retained snapshot where present, export, then cold-read again.

The Python verifier checks C read hashes against the generator's independent
payload specification, including complete coverage and duplicate refusal.
Every readonly command is bracketed by streaming SHA-256 hashes of all five
raw members, which must remain unchanged. A successful converted census
requires no legacy or weak backing and a parity-2 certificate. Missing
records, command failures, timeouts and mismatches keep the report failed.

`report.json` retains every command, log hash, per-phase adapter record,
readonly hash pair and raw repetition. Its grouped metrics contain medians,
minima, maxima and original values. `report.txt` is a short timing index.
Mapping owners, versions, retained backing, raw catalog/journal storage and
native allocation remain distinct fields. Primary `mover_total_s` spans
activation, epoch setup, conversion, fencing, certification and the final
allocation census, ending before byte verification. `mover_io` is a delta of
frozen leaf-vdev counters over that whole interval. `elapsed_s` and
`converter_io` retain the narrower sweep/fence/certification scope as
diagnostics. The runner also records native process duration from launch
through wait return, including import, activation, validation and export;
Python parsing and validation are outside that timer and separately recorded
as `orchestration_s`.

The process heap is glibc `uordblks`; a 1 ms sampler observes it during the
mover, activation and epoch setup included. Its overhead is not independently
measured. It includes libzpool/ARC/dbuf and allocator
behavior; it is neither table-only memory nor a complete simultaneous kernel
resident/allocator peak. `ru_maxrss` has a separate process-RSS meaning.
The shared matrix still does not qualify clones, dedup/BRT, keyless
encryption/ZIL, gang/multi-DVA, crash cuts, full pools, live readers across
reuse, replacement, or kernel behavior.

Parser acceptance tests need Python only:

```sh
python3 contrib/raidz-virtual-backing/common-fixture/test_report.py
python3 -O contrib/raidz-virtual-backing/common-fixture/test_report.py
```

After both full profiles pass, generate the combined report:

```sh
python3 contrib/raidz-virtual-backing/common-fixture/summarize.py \
    --debug /absolute/path/to/debug-results/report.json \
    --release /absolute/path/to/release-results/report.json \
    --output /absolute/path/to/new-combined-results
```

The summary rejects incomplete or duplicate case grids, mismatched build
macros, missing readonly phases and inconsistent owner/space accounting.
The [31 October results](results-20261031/RESULTS.md) include both raw JSON
reports and the finite author-run validation record.
