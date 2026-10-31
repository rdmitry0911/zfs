#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""Run the unchanged common inputs through the virtual/backing adapter."""

import argparse
from collections import defaultdict
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import resource
import signal
import statistics
import subprocess
import sys
import time

GENERATOR_COMMIT = '6319a29f86a9771c62a4c59b029c9c18a839998b'
GENERATOR_HASHES = {
    'fixture.c':
        'dc655a0c6c9dfcd9583a6f7dc3f2fe3deb67d303fc2575209707e84488589398',
    'run.py':
        'b21c647a11101a3da5ba535829577cc493571b3b1f0ba10de97936447dd0452d',
    'build.py':
        'c8c470871b2a2d50eb677b0c49917cbaffc3b50f0224eb383525065c545aa648',
}
RECORDS = (128, 16)
FRACTIONS = (0, 10, 30, 100)


def sha(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 << 20), b''):
            value.update(block)
    return value.hexdigest()


def system_observation():
    value = dict(observed_unix_s=time.time(),
                 host_page_cache='not flushed; cache state uncontrolled')
    for name, path in (('loadavg', '/proc/loadavg'),
                       ('meminfo', '/proc/meminfo')):
        location = Path(path)
        if location.is_file():
            value[name] = location.read_text().strip()
    if hasattr(os, 'sched_getaffinity'):
        value['cpu_affinity'] = sorted(os.sched_getaffinity(0))
    return value


def fail_if(condition, message):
    if condition:
        raise RuntimeError(message)


def load_generator(directory):
    for name, wanted in GENERATOR_HASHES.items():
        fail_if(sha(directory / name) != wanted,
                'common generator changed: ' + name)
    spec = importlib.util.spec_from_file_location(
        'common_raidz_generator', directory / 'run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_json_records(text):
    rows = []
    phases = set()
    for line in text.splitlines():
        if not line.startswith('VB_JSON '):
            continue
        row = json.loads(line[len('VB_JSON '):])
        fail_if(not isinstance(row, dict), 'VB_JSON is not an object')
        phase = row.get('phase')
        fail_if(not isinstance(phase, str) or phase in phases,
                'missing or duplicate VB_JSON phase')
        phases.add(phase)
        rows.append(row)
    return rows


def phase_record(records, phase):
    matches = [row for row in records if row.get('phase') == phase]
    fail_if(len(matches) != 1, 'expected one adapter phase: ' + phase)
    return matches[0]


def check_complete(row):
    fail_if(not row.get('engine_active'), 'engine is inactive')
    census = row.get('census')
    fail_if(not isinstance(census, dict), 'missing census')
    for key in ('legacy_bytes', 'weak_current', 'weak_retired'):
        fail_if(census.get(key) != 0, 'nonzero/missing census ' + key)
    fail_if(census.get('certified') != 2, 'parity-2 certificate missing')
    fail_if(census.get('min_parity', 0) < 2,
            'census minimum parity is below target')


def check_payload(text, spec, expected):
    actual = []
    for line in text.splitlines():
        if not line.startswith('VERIFY '):
            continue
        fields = dict(token.split('=', 1) for token in line.split()[1:])
        actual.append((fields['dataset'], int(fields['object']),
                       int(fields['bytes']), int(fields['hole']),
                       fields['sha256']))
    wanted = []
    if spec['head_bytes']:
        wanted.append(('jointfixture', spec['hobj'], spec['head_bytes'],
                       0, expected['head']))
    if spec['retained_bytes']:
        wanted.append(('jointfixture', spec['robj'],
                       spec['retained_bytes'], 1, expected['holes']))
        if not spec['dropped']:
            wanted.append(('jointfixture@retained', spec['robj'],
                           spec['retained_bytes'], 0,
                           expected['retained']))
    fail_if(sorted(actual) != sorted(wanted),
            'C read coverage or SHA differs from independent Python payload')


def descriptor(directory):
    fields = (directory / 'objects.txt').read_text().split()
    fail_if(len(fields) != 7 or fields[0] != '1',
            'invalid objects.txt descriptor')
    values = list(map(int, fields))
    return dict(record=values[1], retained_bytes=values[2],
                head_bytes=values[3], robj=values[4], hobj=values[5],
                dropped=values[6])


def member_hashes(directory):
    members = {}
    for index in range(5):
        path = directory / ('v%d' % index)
        fail_if(not path.is_file() or path.is_symlink(),
                'missing or nonregular raw member: ' + str(path))
        fail_if(path.stat().st_size != 256 << 20,
                'raw member size differs from common fixture')
        members[path.name] = dict(size=path.stat().st_size,
                                  sha256=sha(path))
    return members


def numeric_metrics(value, prefix=''):
    result = {}
    if not isinstance(value, dict):
        return result
    for key, item in value.items():
        label = prefix + key
        if isinstance(item, dict):
            result.update(numeric_metrics(item, label + '.'))
        elif isinstance(item, (int, float)) and not isinstance(item, bool):
            fail_if(not math.isfinite(item), 'nonfinite metric: ' + label)
            result[label] = item
    return result


def summarize(cases):
    groups = defaultdict(list)
    for row in cases:
        if row['passed']:
            groups[(row['record_kib'], row['retained_pct'])].append(row)
    result = []
    for (record, retained), rows in sorted(groups.items(), reverse=True):
        values = defaultdict(list)
        for row in rows:
            values['convert_process_s'].append(row['convert_process_s'])
            for phase in ('before-mover', 'before-convert', 'convert',
                          'converted-readonly', 'drop-retained',
                          'dropped-readonly'):
                snapshot = row['metrics'][phase]
                for name, value in numeric_metrics(snapshot).items():
                    values[phase + '.' + name].append(value)
            for name, value in row['converter_io'].items():
                values['converter_io.' + name].append(value)
            for name, value in row['mover_io'].items():
                values['mover_io.' + name].append(value)
        result.append(dict(record_kib=record, retained_pct=retained,
                           runs=len(rows), metrics={
                               name: dict(median=statistics.median(items),
                                          minimum=min(items),
                                          maximum=max(items), values=items)
                               for name, items in sorted(values.items())}))
    return result


def validate_build(tree, build, profile):
    provenance = json.loads((build / 'build.json').read_text())
    fail_if(provenance.get('passed') is not True,
            'adapter/fixture build did not pass')
    fail_if(provenance.get('profile') != profile,
            'build profile does not match requested profile')
    fail_if(not provenance.get('libraries'), 'missing library identities')
    for name, wanted in provenance['libraries'].items():
        fail_if(sha(tree / '.libs' / name) != wanted,
                'library changed after build: ' + name)
    for name in ('adapter', 'fixture'):
        fail_if(sha(build / name) != provenance['binaries'][name],
                'binary changed after build: ' + name)
    return provenance


def write_report(out, report):
    report['summary'] = summarize(report['cases'])
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    lines = [
        'Virtual/backing common-fixture report',
        'profile=%s passed=%s completed=%d expected=%d' % (
            report['profile'], report['passed'],
            sum(row['passed'] for row in report['cases']),
            report['expected_cases']),
        '',
        'Heap figures are process observations, not full kernel RAM charge.',
        'Mover I/O includes activation/epoch/conversion/fence/certification.',
        'Mover time excludes import and payload verification. Process time',
        'includes import, activation, epoch, verification and export.',
        '',
        '| block KiB | retained % | runs | mover s median [min,max] |',
        '|---|---|---|---|',
    ]
    for row in report['summary']:
        elapsed = row['metrics'].get('convert.mover_total_s')
        text = ('%.6f [%.6f,%.6f]' % (elapsed['median'],
                elapsed['minimum'], elapsed['maximum'])
                if elapsed else 'missing')
        lines.append('| %d | %d | %d | %s |' % (
            row['record_kib'], row['retained_pct'], row['runs'], text))
    (out / 'report.txt').write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('tree', 'build', 'fixtures', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--profile', choices=('debug', 'release'),
                        required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--nice', type=int, default=10)
    parser.add_argument('--only-case', choices=[
        'r%d-retained%d' % (r, p) for r in RECORDS for p in FRACTIONS])
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10 or not 1 <= args.timeout <= 1800:
        parser.error('repeats must be 1..10; timeout must be 1..1800')
    if not 0 <= args.nice <= 19:
        parser.error('nice increment must be 0..19')
    tree, build = args.tree.resolve(), args.build.resolve()
    fixtures, out = args.fixtures.resolve(), args.output.resolve()
    generator = load_generator(fixtures)
    provenance = validate_build(tree, build, args.profile)
    out.mkdir(parents=True, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    report = dict(passed=False, userspace_only=True, migration_run=True,
                  profile=args.profile, units=100, repeats=args.repeats,
                  generator_commit=GENERATOR_COMMIT,
                  generator_hashes=GENERATOR_HASHES, build=provenance,
                  runner_sha256=sha(Path(__file__).resolve()),
                  uname=platform.uname()._asdict(),
                  inherited_nice=os.getpriority(os.PRIO_PROCESS, 0),
                  nice_increment=args.nice, command_line=sys.argv,
                  cpu_count=os.cpu_count(), cases=[],
                  system_before=system_observation(),
                  expected_cases=args.repeats * (1 if args.only_case else 8),
                  full_matrix=not bool(args.only_case),
                  kernel_ram_qualified=False,
                  extended_cases_qualified=False,
                  timing_scopes={
                      'mover_total_s': 'activation through final census',
                      'elapsed_s': 'sweep, fence and certification only',
                      'process_seconds': 'native launch through wait return',
                      'orchestration_s': 'command wrapper plus validation'},
                  io_scopes={
                      'mover_io': 'before-mover to frozen convert counters',
                      'converter_io':
                          'before-convert to frozen convert counters'})
    env = {name: value for name, value in os.environ.items()
           if not name.startswith('PR19262_') and name != 'LD_PRELOAD'}
    env['LD_LIBRARY_PATH'] = str(tree / '.libs')
    report['library_path'] = env['LD_LIBRARY_PATH']
    expected_cache = {}

    def save():
        write_report(out, report)

    def expected(retained):
        if retained not in expected_cache:
            rbytes = (100 * 131072) * retained // 100
            hbytes = (100 * 131072) - rbytes
            expected_cache[retained] = dict(
                retained=generator.expected_sha(rbytes, 1),
                head=generator.expected_sha(hbytes, 2),
                holes=hashlib.sha256(bytes(rbytes)).hexdigest())
        return expected_cache[retained]

    def run(case, row, executable, command, extra=()):
        entry = dict(argv=['nice', '-n', str(args.nice),
                           str(build / executable), command, str(case)] +
                     list(map(str, extra)), cwd=str(case),
                     library_path=env['LD_LIBRARY_PATH'],
                     timeout_seconds=args.timeout,
                     system_before=system_observation())
        row['commands'].append(entry)
        label = '%02d-%s-%s' % (len(row['commands']), executable, command)
        stdout_path = case / (label + '.stdout')
        stderr_path = case / (label + '.stderr')
        entry['stdout'] = str(stdout_path.relative_to(out))
        entry['stderr'] = str(stderr_path.relative_to(out))
        begin = time.monotonic()
        try:
            with stdout_path.open('xb') as stdout, \
                    stderr_path.open('xb') as stderr:
                process_begin = time.monotonic()
                child = subprocess.Popen(entry['argv'], cwd=case, env=env,
                                         stdout=stdout, stderr=stderr,
                                         start_new_session=True)
                entry['pid'] = child.pid
                entry['running'] = True
                save()
                try:
                    entry['exit'] = child.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    entry['timeout'] = True
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=15)
                    raise
                finally:
                    entry['process_seconds'] = (
                        time.monotonic() - process_begin)
                    entry['seconds'] = entry['process_seconds']
            fail_if(entry['exit'] != 0, 'command failed: ' + label)
            text = stdout_path.read_text()
            entry['records'] = parse_json_records(text)
            if executable == 'fixture':
                fail_if(not any(line.startswith('FIXTURE ') and
                                line.endswith(' PASS')
                                for line in text.splitlines()),
                        'fixture success marker missing')
            else:
                for record in entry['records']:
                    numeric_metrics(record)
            check_payload(text, descriptor(case),
                          expected(row['retained_pct']))
            return entry
        finally:
            entry['running'] = False
            entry['orchestration_s'] = time.monotonic() - begin
            entry['stdout_sha256'] = sha(stdout_path)
            entry['stderr_sha256'] = sha(stderr_path)
            save()

    def readonly(case, row, label, converted):
        before = member_hashes(case)
        executable = 'adapter' if converted else 'fixture'
        command = 'inspect' if converted else 'verify'
        entry = run(case, row, executable, command)
        after = member_hashes(case)
        row['readonly_checks'].append(dict(
            label=label, before=before, after=after,
            stdout=entry['stdout'], unchanged=before == after))
        fail_if(before != after, label + ' changed raw member images')
        if converted:
            snapshot = phase_record(entry['records'], 'inspect')
            fail_if(not snapshot.get('readonly'), 'inspect not readonly')
            if label in ('converted-readonly', 'dropped-readonly'):
                check_complete(snapshot)
            row['metrics'][label] = snapshot
        return after

    save()
    try:
        for repeat in range(args.repeats):
            for record in RECORDS:
                for retained in FRACTIONS:
                    name = 'r%d-retained%d' % (record, retained)
                    if args.only_case and name != args.only_case:
                        continue
                    case = out / ('repeat%d' % repeat) / name
                    case.mkdir(parents=True, exist_ok=False)
                    row = dict(record_kib=record, retained_pct=retained,
                               repeat=repeat, directory=str(case),
                               commands=[], readonly_checks=[], metrics={},
                               passed=False)
                    report['cases'].append(row)
                    run(case, row, 'fixture', 'create',
                        (record, retained, 100))
                    row['initial_objects'] = descriptor(case)
                    readonly(case, row, 'input-readonly', False)
                    enabled = run(case, row, 'adapter', 'enable')
                    snapshot = phase_record(enabled['records'], 'enable')
                    fail_if(not snapshot.get('physical_rewrite_enabled'),
                            'physical_rewrite is not enabled before move')
                    row['metrics']['enable'] = snapshot
                    readonly(case, row, 'enabled-readonly', True)
                    converted = run(case, row, 'adapter', 'convert')
                    before_mover = phase_record(converted['records'],
                                                'before-mover')
                    before = phase_record(converted['records'],
                                          'before-convert')
                    after = phase_record(converted['records'], 'convert')
                    check_complete(after)
                    fail_if(not before.get('physical_rewrite_enabled') or
                            not after.get('physical_rewrite_enabled'),
                            'physical_rewrite state changed during move')
                    fail_if(after.get('elapsed_s', 0) <= 0,
                            'missing positive converter duration')
                    fail_if(after.get('mover_total_s', 0) <= 0,
                            'missing positive whole-mover duration')
                    row['metrics']['before-mover'] = before_mover
                    row['metrics']['before-convert'] = before
                    row['metrics']['convert'] = after
                    row['convert_process_s'] = converted['seconds']
                    row['converter_io'] = {}
                    row['mover_io'] = {}
                    for counter in ('vdev_read_bytes', 'vdev_write_bytes',
                                    'vdev_read_ops', 'vdev_write_ops'):
                        delta = after[counter] - before[counter]
                        fail_if(delta < 0, 'vdev counter went backwards')
                        row['converter_io'][counter] = delta
                        whole_delta = after[counter] - before_mover[counter]
                        fail_if(whole_delta < 0,
                                'whole-mover vdev counter went backwards')
                        row['mover_io'][counter] = whole_delta
                    readonly(case, row, 'converted-readonly', True)
                    dropped = run(case, row, 'adapter', 'drop-retained')
                    snapshot = phase_record(dropped['records'],
                                            'drop-retained')
                    check_complete(snapshot)
                    row['metrics']['drop-retained'] = snapshot
                    row['final_objects'] = descriptor(case)
                    fail_if(retained != 0 and
                            row['final_objects']['dropped'] != 1,
                            'snapshot descriptor was not advanced')
                    row['final_members'] = readonly(
                        case, row, 'dropped-readonly', True)
                    row['passed'] = True
                    save()
                    print(json.dumps(dict(profile=args.profile,
                                          case=name, repeat=repeat,
                                          elapsed_s=after['elapsed_s'],
                                          mover_total_s=after['mover_total_s'],
                                          passed=True)), flush=True)
        fail_if(sum(row['passed'] for row in report['cases']) !=
                report['expected_cases'], 'incomplete case matrix')
        report['passed'] = True
    except Exception as error:
        report['error'] = '%s: %s' % (type(error).__name__, error)
        raise
    finally:
        report['system_after'] = system_observation()
        save()


if __name__ == '__main__':
    main()
