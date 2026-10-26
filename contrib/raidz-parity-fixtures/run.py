#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""Create and cold-verify the eight neutral head/snapshot fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import struct
import subprocess
import time

SEED = 0x726169647a323032
MASK = (1 << 64) - 1


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(8 << 20), b''):
            h.update(block)
    return h.hexdigest()


def expected_sha(size, tag):
    h = hashlib.sha256()
    block = bytearray()
    for word in range(size // 8):
        x = ((SEED ^ tag) + word * 0x9e3779b97f4a7c15) & MASK
        x = ((x ^ (x >> 30)) * 0xbf58476d1ce4e5b9) & MASK
        x = ((x ^ (x >> 27)) * 0x94d049bb133111eb) & MASK
        x ^= x >> 31
        block.extend(struct.pack('<Q', x))
        if len(block) == 131072:
            h.update(block)
            block.clear()
    h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--units', type=int, default=100,
                   help='128 KiB units per case, positive multiple of ten')
    p.add_argument('--timeout', type=int, default=180)
    p.add_argument('--drop-retained', action='store_true',
                   help='exercise final snapshot deletion after cold verify')
    a = p.parse_args()
    if a.units <= 0 or a.units > 1020 or a.units % 10:
        p.error('units must be a multiple of ten between 10 and 1020')
    if not 1 <= a.timeout <= 1800:
        p.error('timeout must be 1..1800 seconds')
    tree, build, out = a.tree.resolve(), a.build.resolve(), a.output.resolve()
    provenance = json.loads((build / 'build.json').read_text())
    if not provenance['passed'] or sha(build / 'fixture') != \
            provenance['binary_sha256']:
        p.error('driver build provenance mismatch')
    for name, digest in provenance['libraries'].items():
        if sha(tree / '.libs' / name) != digest:
            p.error('library changed after driver build: ' + name)
    out.mkdir(parents=True, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    report = dict(passed=False, userspace_only=True, migration_run=False,
                  units=a.units, build=provenance, cases=[],
                  runner_sha256=sha(Path(__file__).resolve()),
                  extended_cases_qualified=False,
                  true_release_qualified=False)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('PR19262_')}
    env['LD_LIBRARY_PATH'] = str(tree / '.libs')

    def save():
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def members(case):
        return {p.name: dict(size=p.stat().st_size, sha256=sha(p))
                for p in sorted(case.glob('v[0-4]'))}

    def run(case, row, command, extra=()):
        entry = dict(argv=[str(build / 'fixture'), command, str(case)] +
                     list(map(str, extra)), cwd=str(case),
                     library_path=env['LD_LIBRARY_PATH'])
        row['commands'].append(entry)
        begin = time.monotonic()
        label = str(len(row['commands'])) + '-' + command
        try:
            with (case / (label + '.stdout')).open('xb') as stdout, \
                    (case / (label + '.stderr')).open('xb') as stderr:
                child = subprocess.Popen(entry['argv'], cwd=case, env=env,
                                         stdout=stdout, stderr=stderr,
                                         start_new_session=True)
                try:
                    entry['exit'] = child.wait(timeout=a.timeout)
                except subprocess.TimeoutExpired:
                    entry['timeout'] = True
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=15)
                    raise
            if entry['exit'] != 0:
                raise RuntimeError('fixture command failed: ' + label)
            lines = (case / (label + '.stdout')).read_text().splitlines()
            entry['result'] = [line for line in lines
                               if line.startswith(('FIXTURE ', 'VERIFY '))]
            verified = [dict(token.split('=', 1) for token in line.split()[1:])
                        for line in entry['result']
                        if line.startswith('VERIFY ')]
            wanted = {}
            if row['head_bytes']:
                wanted['jointfixture'] = row['expected_head_sha256']
            if (row['retained_bytes'] and command != 'drop-retained' and
                    not row.get('dropped', False)):
                wanted['jointfixture@retained'] = (
                    row['expected_retained_sha256'])
            actual = {r['dataset']: r['sha256'] for r in verified
                      if r['hole'] == '0'}
            if actual != wanted:
                raise RuntimeError(
                    'C read SHA differs from Python payload SHA')
            if not any(line.startswith('FIXTURE ') and line.endswith(' PASS')
                       for line in lines):
                raise RuntimeError('missing fixture success marker')
        finally:
            entry['seconds'] = time.monotonic() - begin
            save()

    try:
        total = a.units * 131072
        hashes = {(pct, tag): expected_sha(total * pct // 100, tag)
                  for pct in (0, 10, 30, 70, 90, 100) for tag in (1, 2)}
        for record in (128, 16):
            for retained in (0, 10, 30, 100):
                case = out / ('r%d-retained%d' % (record, retained))
                case.mkdir()
                row = dict(record_kib=record, retained_pct=retained,
                           retained_bytes=total * retained // 100,
                           head_bytes=total * (100 - retained) // 100,
                           expected_retained_sha256=hashes[retained, 1],
                           expected_head_sha256=hashes[100 - retained, 2],
                           fraction_denominator='logical user payload only',
                           commands=[], passed=False)
                report['cases'].append(row)
                run(case, row, 'create', (record, retained, a.units))
                row['objects'] = (case / 'objects.txt').read_text().strip()
                before = members(case)
                if len(before) != 5:
                    raise RuntimeError('missing raw members')
                run(case, row, 'verify')
                after = members(case)
                row['before_readonly'] = before
                row['after_readonly'] = after
                if before != after:
                    raise RuntimeError(
                        'readonly import changed raw members')
                if a.drop_retained:
                    run(case, row, 'drop-retained')
                    row['dropped'] = True
                    row['after_drop'] = members(case)
                    run(case, row, 'verify')
                    row['after_drop_readonly'] = members(case)
                    if row['after_drop'] != row['after_drop_readonly']:
                        raise RuntimeError(
                            'post-drop readonly changed members')
                row['passed'] = True
                save()
        report['passed'] = True
    finally:
        save()


if __name__ == '__main__':
    main()
