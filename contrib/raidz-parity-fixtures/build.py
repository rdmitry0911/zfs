#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""Build the neutral driver against a prebuilt Linux libzpool tree."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--profile', choices=['debug', 'release'], required=True)
    p.add_argument('--library-build-log', type=Path)
    a = p.parse_args()
    tree, out = a.tree.resolve(), a.output.resolve()
    source = Path(__file__).with_name('fixture.c').resolve()
    if a.profile == 'release' and not a.library_build_log:
        p.error('release requires the actual clean library rebuild log')
    out.mkdir(parents=True, exist_ok=False)
    flags = ['-DHAVE_CONFIG_H', '-I' + str(tree), '-include',
             str(tree / 'zfs_config.h'), '-include',
             str(tree / 'include/sys/simd_config.h')]
    flags += ['-I' + str(tree / name) for name in (
        'include', 'lib/libspl/include', 'lib/libspl/include/os/linux',
        'lib/libzpool/include')]
    flags += ['-D_GNU_SOURCE', '-D_REENTRANT', '-D_FILE_OFFSET_BITS=64',
              '-D_LARGEFILE64_SOURCE', '-DTEXT_DOMAIN="zfs-linux-user"']
    flags += (['-DDEBUG', '-UNDEBUG', '-DZFS_DEBUG']
              if a.profile == 'debug' else
              ['-UDEBUG', '-DNDEBUG', '-UZFS_DEBUG'])
    flags += ['-std=gnu99', '-Wall', '-Wextra', '-Wstrict-prototypes',
              '-Wmissing-prototypes', '-Wwrite-strings', '-Wno-sign-compare',
              '-Wno-missing-field-initializers', '-fno-strict-aliasing',
              '-fno-omit-frame-pointer', '-Wimplicit-fallthrough', '-Werror',
              '-pthread', '-Wno-format-zero-length', '-Wno-format-truncation',
              '-g', '-O2']
    report = dict(passed=False, profile=a.profile, tree=str(tree), commands=[],
                  source_sha256=sha(source),
                  library_abi_requires_matching_build=True,
                  kernel_runtime_qualified=False)

    def run(label, argv):
        report['commands'].append(argv)
        with (out / (label + '.log')).open('xb') as f:
            subprocess.run(argv, stdout=f, stderr=subprocess.STDOUT,
                           check=True, timeout=180)

    try:
        libs = ['libzpool.so.7.0.0', 'libzfs_core.so', 'libnvpair.so',
                'librange_tree.a']
        report['libraries'] = {n: sha(tree / '.libs' / n) for n in libs}
        report['config_sha256'] = sha(tree / 'zfs_config.h')
        # A current Makefile cannot prove how an existing library was built.
        if a.library_build_log:
            log = a.library_build_log.resolve()
            report['library_build_log_sha256'] = sha(log)
            text = log.read_text(errors='replace')
            if a.profile == 'release':
                for flag in ['-UDEBUG', '-DNDEBUG', '-UZFS_DEBUG']:
                    if flag not in text:
                        raise RuntimeError('missing release flag: ' + flag)
            report['library_build_provenance'] = (
                'supplied log; review required')
        else:
            report['library_build_provenance'] = (
                'prebuilt debug library; retain its source/build provenance')
        run('macros', ['gcc'] + flags + ['-dM', '-E', '-x', 'c', '/dev/null'])
        macros = (out / 'macros.log').read_text()
        names = {line.split()[1] for line in macros.splitlines()
                 if line.startswith('#define ')}
        expected = (a.profile == 'debug')
        if (('DEBUG' in names) != expected or
                ('ZFS_DEBUG' in names) != expected or
                ('NDEBUG' in names) == expected):
            raise RuntimeError(
                'effective driver macros differ from profile')
        report['driver_macros'] = {n: n in names for n in
                                   ('DEBUG', 'ZFS_DEBUG', 'NDEBUG')}
        obj = out / 'fixture.o'
        run('compile', ['gcc'] + flags + ['-c', str(source), '-o', str(obj)])
        run('link', ['gcc', '-g', '-O2', '-pthread', '-o',
                     str(out / 'fixture'),
                     str(obj), '-L' + str(tree / '.libs'), '-lzpool',
                     '-lzfs_core', '-lnvpair',
                     str(tree / '.libs/librange_tree.a'), '-lm'])
        report['binary_sha256'] = sha(out / 'fixture')
        report['passed'] = True
    finally:
        (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
