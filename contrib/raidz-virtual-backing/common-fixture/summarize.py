#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0

# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
"""Validate both complete profiles and publish the common-fixture summary."""

import argparse
import hashlib
import json
from pathlib import Path
import statistics


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def value(row, phase, key):
    result = row['metrics'][phase]
    for part in key.split('.'):
        result = result[part]
    return result


def span(values, scale=1, decimals=2):
    data = [v / scale for v in values]
    fmt = '%.' + str(decimals) + 'f'
    return ('%s [%s, %s]' % (fmt % statistics.median(data),
            fmt % min(data), fmt % max(data)))


def validate(report):
    require(report.get('passed') is True, 'profile did not pass')
    require(report.get('full_matrix') is True, 'partial fixture selection')
    require(report.get('repeats') == 3, 'three repetitions required')
    require(report.get('units') == 100, 'unexpected total payload')
    require(report.get('generator_commit') ==
            '6319a29f86a9771c62a4c59b029c9c18a839998b',
            'wrong common generator revision')
    require(report['build'].get('source46_matches') is True,
            'source46 binding missing')
    debug = report['profile'] == 'debug'
    require(report['build'].get('profile') == report['profile'] and
            report['build'].get('driver_macros') == {
                'DEBUG': debug, 'ZFS_DEBUG': debug, 'NDEBUG': not debug},
            'effective build macros do not match profile')
    cases = report['cases']
    wanted = {(r, p, n) for r in (128, 16) for p in (0, 10, 30, 100)
              for n in range(3)}
    actual = [(r['record_kib'], r['retained_pct'], r['repeat'])
              for r in cases]
    require(len(actual) == 24 and len(set(actual)) == 24 and
            set(actual) == wanted, 'missing or duplicate matrix case')
    for row in cases:
        require(row.get('passed') is True, 'failed matrix case')
        checks = row['readonly_checks']
        require(len(checks) == 4 and all(c['unchanged'] and
                c['before'] == c['after'] for c in checks),
                'readonly proof missing or changed member')
        require({c['label'] for c in checks} == {
            'input-readonly', 'enabled-readonly', 'converted-readonly',
            'dropped-readonly'}, 'readonly phases missing or duplicated')
        for check in checks:
            require(set(check['before']) == {'v0', 'v1', 'v2', 'v3', 'v4'},
                    'readonly member set differs from common fixture')
        for phase in ('convert', 'converted-readonly', 'drop-retained',
                      'dropped-readonly'):
            census = row['metrics'][phase]['census']
            require(census['certified'] == 2 and
                    census['legacy_bytes'] == 0 and
                    census['weak_current'] == 0 and
                    census['weak_retired'] == 0,
                    'incomplete conversion at ' + phase)
        require(value(row, 'convert', 'heap_sample_count') > 0,
                'heap sampler did not observe mover')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--debug', required=True, type=Path)
    parser.add_argument('--release', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    reports = {}
    inputs = {}
    for profile, path in (('debug', args.debug), ('release', args.release)):
        report = json.loads(path.read_text())
        require(report.get('profile') == profile, 'profile mismatch')
        validate(report)
        reports[profile] = report
        inputs[profile] = dict(report_sha256=sha(path),
                               source46_sha256=report['build'][
                                   'source46_sha256'])
    require(inputs['debug']['source46_sha256'] ==
            inputs['release']['source46_sha256'], 'source profiles differ')
    args.output.mkdir(parents=True, exist_ok=False)
    lines = [
        '# Virtual/backing on the common RAID-Z fixture', '',
        'Author-run userspace Linux libzpool comparison: 48 conversions,',
        'eight inputs, three repeats per input in debug and true release.',
        'All conversions and post-snapshot-deletion cold checks passed.',
        'Each run includes four readonly checks preserving all five member',
        'hashes. Values below are medians with [minimum, maximum].', '',
        'Prototype: `169e96520909f8650a39d4bdf14ace2e755311f1`.',
        'Generator: `6319a29f86a9771c62a4c59b029c9c18a839998b`, unchanged.',
        'Five 256 MiB file vdevs, ashift 12, compression off, copies=1,',
        '12.5 MiB logical payload; physical_rewrite enabled before moving.',
        'Retained fractions describe logical user payload, not metadata.', '',
    ]
    for profile in ('release', 'debug'):
        report = reports[profile]
        lines += [
            '## ' + profile.capitalize(), '',
            '| block KiB | retained % | cold current mappings | '
            'cold retired owners | after-drop current mappings | '
            'whole mover s | sweep + final fence s |',
            '|---|---|---|---|---|---|---|',
        ]
        for record in (128, 16):
            for retained in (0, 10, 30, 100):
                rows = [r for r in report['cases']
                        if r['record_kib'] == record and
                        r['retained_pct'] == retained]

                def data(phase, key):
                    return [value(r, phase, key) for r in rows]

                lines.append('| %d | %d | %s | %s | %s | %s | %s |' % (
                    record, retained,
                    span(data('converted-readonly', 'census.current_versions'),
                         decimals=0),
                    span(data('converted-readonly', 'census.retired_owners'),
                         decimals=0),
                    span(data('dropped-readonly', 'census.current_versions'),
                         decimals=0),
                    span(data('convert', 'mover_total_s')),
                    span(data('convert', 'elapsed_s'))))
        lines += ['', '| block KiB | retained % | raw catalog reserved MiB | '
                  'journal MiB | maximum observed heap MiB | '
                  'converter writes MiB |', '|---|---|---|---|---|---|']
        for record in (128, 16):
            for retained in (0, 10, 30, 100):
                rows = [r for r in report['cases']
                        if r['record_kib'] == record and
                        r['retained_pct'] == retained]
                lines.append('| %d | %d | %s | %s | %s | %s |' % (
                    record, retained,
                    span([value(r, 'converted-readonly',
                                'census.store_bytes') for r in rows], 2**20),
                    span([value(r, 'converted-readonly',
                                'census.journal_bytes') for r in rows], 2**20),
                    span([value(r, 'convert',
                                'heap_max_observed_uordblks_bytes')
                          for r in rows], 2**20),
                    span([r['converter_io']['vdev_write_bytes']
                          for r in rows], 2**20)))
        lines += ['', 'Environment and exact metric vectors are retained in',
                  '`report-%s.json`.' % profile, '']
    lines += [
        '## Interpretation', '',
        'Whole mover time covers activation, epoch creation, conversion,',
        'fencing, certification and census; it excludes payload checking.',
        'The converter I/O window begins after activation/epoch and ends',
        'after final census. Raw reports retain its separate counters and',
        'the inclusive mover I/O window. Current mappings include metadata',
        'and new allocations paired by this prototype. The total owners',
        'counter also includes retired owners; all three counters and hot',
        'and cold snapshots are recorded. Snapshot deletion includes admin',
        'metadata updates, ordinary txgs and export, so its net row delta',
        'does not isolate the deleted payload population.',
        '',
        'Catalog/journal bytes are raw reserved extent accounting across',
        'the members, not live MOS table bytes or an in-memory charge.',
        'Heap is the largest observed glibc uordblks sample during the mover',
        '(nominal 1 ms sampling), including libzpool ARC/dbufs. It excludes',
        'mmap-backed allocations and is not a complete allocator or kernel',
        'RAM peak. Process-lifetime ru_maxrss is reported separately.', '',
        'The peer results were obtained on another VM. Shared inputs make',
        'the behavior and row populations comparable; these times do not',
        'establish a three-way speedup on one machine. Host page cache was',
        'not flushed; native processes ran sequentially with niceness +10.',
        '',
        'Clones, dedup/BRT, keyless ZIL, gang/multi-DVA, crash/full-pool and',
        'replacement/kernel qualification remain outside this core matrix.',
        'Raw images and complete logs remain in the author VM; the published',
        'reports retain all member hashes and process-output hashes.', '',
    ]
    (args.output / 'RESULTS.md').write_text('\n'.join(lines))
    (args.output / 'inputs.json').write_text(
        json.dumps(inputs, indent=2) + '\n')
    for profile, report in reports.items():
        (args.output / ('report-' + profile + '.json')).write_text(
            json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
