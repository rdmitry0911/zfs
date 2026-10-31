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
"""Build the neutral driver and VB adapter against matching Linux libzpool."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

GENERATOR_SHA256 = (
    'dc655a0c6c9dfcd9583a6f7dc3f2fe3deb67d303fc2575209707e84488589398')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', required=True, type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--profile', required=True,
                        choices=('debug', 'release'))
    parser.add_argument('--library-build-log', required=True, type=Path)
    args = parser.parse_args()
    tree, fixtures = args.tree.resolve(), args.fixtures.resolve()
    output = args.output.resolve()
    source = Path(__file__).resolve().with_name('adapter.c')
    generator = fixtures / 'fixture.c'
    if digest(generator) != GENERATOR_SHA256:
        raise RuntimeError('generator fixture.c differs from 6319a29f86')
    manifest = tree / 'contrib/raidz-virtual-backing/source46-sha256.json'
    source46 = json.loads(manifest.read_text())
    for path, expected in source46.items():
        if digest(tree / path) != expected:
            raise RuntimeError('prototype source manifest mismatch: ' + path)
    output.mkdir(parents=True, exist_ok=False)
    log = args.library_build_log.resolve()
    report = {
        'passed': False, 'profile': args.profile, 'tree': str(tree),
        'generator_commit': '6319a29f86a9771c62a4c59b029c9c18a839998b',
        'generator_source_sha256': digest(generator),
        'source_sha256': digest(source), 'build_helper_sha256':
        digest(Path(__file__).resolve()), 'source46_sha256': digest(manifest),
        'source46_matches': True, 'library_build_log_sha256': digest(log),
        'commands': [], 'binaries': {}, 'libraries': {},
        'kernel_runtime_qualified': False,
        'library_abi_requires_matching_build': True,
    }

    def run(label, command):
        report['commands'].append(command)
        with (output / (label + '.log')).open('xb') as stream:
            subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                           timeout=180, check=True)

    flags = ['-DHAVE_CONFIG_H', '-I' + str(tree), '-include',
             str(tree / 'zfs_config.h'), '-include',
             str(tree / 'include/sys/simd_config.h')]
    flags += ['-I' + str(tree / name) for name in (
        'include', 'lib/libspl/include', 'lib/libspl/include/os/linux',
        'lib/libzpool/include')]
    flags += ['-I' + str(fixtures), '-D_GNU_SOURCE', '-D_REENTRANT',
              '-D_FILE_OFFSET_BITS=64', '-D_LARGEFILE64_SOURCE',
              '-DTEXT_DOMAIN="zfs-linux-user"']
    profile = (['-DDEBUG', '-UNDEBUG', '-DZFS_DEBUG']
               if args.profile == 'debug' else
               ['-UDEBUG', '-DNDEBUG', '-UZFS_DEBUG'])
    flags += profile
    flags += ['-std=gnu99', '-Wall', '-Wextra', '-Wstrict-prototypes',
              '-Wmissing-prototypes', '-Wwrite-strings', '-Wno-sign-compare',
              '-Wno-missing-field-initializers', '-fno-strict-aliasing',
              '-fno-omit-frame-pointer', '-Wimplicit-fallthrough', '-Werror',
              '-pthread', '-Wno-format-zero-length', '-Wno-format-truncation',
              '-g', '-O2']
    try:
        # Inspect emitted compiler rows, not only a Makefile or profile name.
        rows = []
        for line in log.read_text(errors='replace').splitlines():
            compiler_row = ('libtool: compile:' in line or
                            ('./libtool ' in line and
                             '--mode=compile ' in line))
            if not compiler_row or ' -c ' not in line:
                continue
            if 'libzpool_la-' in line or '-DLIB_ZPOOL_BUILD' in line:
                rows.append(line)
        if not rows:
            raise RuntimeError(
                'no libzpool compiler rows in library build log')
        for line in rows:
            words = shlex.split(line)
            for flag in profile:
                if flag not in words:
                    raise RuntimeError('library compile row lacks ' + flag)
            if args.profile == 'release' and (
                    '-DZFS_DEBUG' in words or '-DDEBUG' in words):
                raise RuntimeError('release library contains debug compile')
        report['reviewed_library_compile_rows'] = len(rows)
        report['library_compile_profile_flags'] = profile
        for name in ('libzpool.so.7.0.0', 'libzfs_core.so', 'libnvpair.so',
                     'librange_tree.a'):
            report['libraries'][name] = digest(tree / '.libs' / name)
        report['config_sha256'] = digest(tree / 'zfs_config.h')
        run('macros', ['gcc'] + flags + ['-dM', '-E', '-x', 'c', '/dev/null'])
        macros = {line.split()[1]
                  for line in (output / 'macros.log').read_text().splitlines()
                  if line.startswith('#define ')}
        debug = args.profile == 'debug'
        if (('DEBUG' in macros) != debug or
                ('ZFS_DEBUG' in macros) != debug or
                ('NDEBUG' in macros) == debug):
            raise RuntimeError('effective driver macros differ from profile')
        report['driver_macros'] = {name: name in macros for name in
                                   ('DEBUG', 'ZFS_DEBUG', 'NDEBUG')}
        for name, c_source in (('fixture', generator), ('adapter', source)):
            obj = output / (name + '.o')
            run(name + '-compile', ['gcc'] + flags +
                ['-c', str(c_source), '-o', str(obj)])
            run(name + '-link', ['gcc', '-g', '-O2', '-pthread', '-o',
                str(output / name), str(obj), '-L' + str(tree / '.libs'),
                '-lzpool', '-lzfs_core', '-lnvpair',
                str(tree / '.libs/librange_tree.a'), '-lm'])
            report['binaries'][name] = digest(output / name)
        report['passed'] = True
    finally:
        (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
