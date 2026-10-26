#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""Run libzpool conversion and cold readonly import on fresh file vdevs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import time


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def records(text, prefix):
    return [dict(token.split('=', 1) for token in line.split()[1:]
                 if '=' in token)
            for line in text.splitlines() if line.startswith(prefix + ' ')]


def require(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument('--manifest', type=Path,
                        default=Path(__file__).with_name(
                            'source46-sha256.json'))
    parser.add_argument('--output', type=Path, required=True,
                        help='new directory; images and logs are retained')
    parser.add_argument('--timeout', type=int, default=300,
                        help='seconds per native command')
    args = parser.parse_args()
    source, out = args.source.resolve(), args.output.resolve()
    require(1 <= args.timeout <= 1800,
            'timeout must be between 1 and 1800 seconds')
    out.mkdir(parents=True, exist_ok=False)
    report = dict(passed=False, source=str(source), userspace_only=True,
                  commands=[])
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    def save():
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(label, settings):
        env = {name: value for name, value in os.environ.items()
               if not name.startswith('PR19262_')}
        env.update(settings)
        row = dict(label=label, argv=[str(source / 'ztest')], env=settings,
                   cwd=str(source))
        report['commands'].append(row)
        begin = time.monotonic()
        try:
            with (out / (label + '.stdout')).open('xb') as stdout, \
                    (out / (label + '.stderr')).open('xb') as stderr:
                child = subprocess.Popen(row['argv'], cwd=source, env=env,
                                         stdout=stdout, stderr=stderr,
                                         start_new_session=True)
                try:
                    row['exit'] = child.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    row['timeout'] = True
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=15)
                    raise
        finally:
            row['seconds'] = time.monotonic() - begin
            save()
        require(row['exit'] == 0,
                label + ' failed; retain output for inspection')
        return (out / (label + '.stdout')).read_text()

    try:
        manifest = json.loads(args.manifest.read_text())
        require(len(manifest) == 46 and all(
            sha(source / name) == digest for name, digest in manifest.items()),
            'tested source46 differs')
        report['source_manifest_sha256'] = sha(args.manifest)
        report['binaries'] = {name: sha(source / name) for name in
                              ('.libs/ztest', '.libs/libzpool.so.7.0.0')}
        images = out / 'members'
        images.mkdir()
        for i in range(6):
            with (images / ('v' + str(i))).open('xb') as stream:
                stream.truncate(256 << 20)
        text = run('convert', dict(
            PR19262_RUNTIME_ENGINE=str(images), PR19262_ENGINE_DURABLE='1',
            PR19262_ENGINE_CONVERT='1', PR19262_ENGINE_CONVERT_BATCH='8',
            PR19262_ENGINE_CONVERT_LIVE_BLOCKS='16',
            PR19262_ENGINE_CONVERT_COLD_BLOCKS='8',
            PR19262_ENGINE_CHILDREN='6',
            PR19262_ENGINE_PARITY='1', PR19262_ENGINE_ASHIFT='12',
            PR19262_ARC_MAX_MIB='128'))
        require('PR19262_CONVERT_PASS' in text and
                'PR19262_RUNTIME_PASS' in text,
                'conversion completion markers')
        census = {row['label']: row
                  for row in records(text, 'PR19262_CENSUS')}
        for label in ('swept', 'fenced', 'certified', 'final'):
            require(census[label]['legacy_bytes'] ==
                    census[label]['weak_current'] == '0',
                    label + ': allocated old-layout blocks remain')
        require(census['certified']['certified'] == '2',
                'strong parity certificate missing')
        scrub = records(text, 'PR19262_SCRUB')
        require(scrub and scrub[0]['errors'] == scrub[0]['errlog'] == '0',
                'native scrub errors')
        before = {p.name: dict(size=p.stat().st_size, sha256=sha(p))
                  for p in sorted(images.glob('v[0-5]'))}
        cold = run('cold-readonly', dict(
            PR19262_ENGINE_COLD=str(images), PR19262_ENGINE_COLD_VERIFY='1',
            PR19262_ENGINE_COLD_EXPECT_STRONG='1', PR19262_ARC_MAX_MIB='128'))
        require('PR19262_STORE_COLD_PASS' in cold and
                records(cold, 'PR19262_LIVE_VERIFY'),
                'cold catalog or live payload missing')
        verify = records(cold, 'PR19262_VERIFY')
        require(len(verify) == 1 and verify[0]['errors'] ==
                verify[0]['traverse_error'] == '0' and
                verify[0]['missing_children'] == '0',
                'cold traversal/payload errors')
        after = {p.name: dict(size=p.stat().st_size, sha256=sha(p))
                 for p in sorted(images.glob('v[0-5]'))}
        require(len(before) == 6 and before == after,
                'readonly import changed raw members')
        require(all(sha(source / name) == digest
                    for name, digest in manifest.items()), 'source changed')
        report.update(passed=True, raw_members=after,
                      readonly_raw_unchanged=True,
                      certified_parity=2, whole_engine_qualified=False)
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        save()
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
