#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""Negative checks for report acceptance, without a libzpool installation."""

import importlib.util
import json
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    'vb_common_runner', Path(__file__).with_name('run.py'))
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


class AcceptanceTests(unittest.TestCase):
    def test_duplicate_phase_rejected(self):
        row = 'VB_JSON ' + json.dumps(dict(phase='convert')) + '\n'
        with self.assertRaises(RuntimeError):
            RUN.parse_json_records(row + row)

    def test_missing_phase_rejected(self):
        with self.assertRaises(RuntimeError):
            RUN.phase_record([], 'inspect')

    def test_weak_and_missing_census_rejected(self):
        for census in (None, dict(certified=2, legacy_bytes=0,
                                  weak_current=1, weak_retired=0),
                       dict(certified=1, legacy_bytes=0,
                            weak_current=0, weak_retired=0),
                       dict(certified=2, legacy_bytes=0,
                            weak_current=0, weak_retired=0,
                            min_parity=1)):
            with self.assertRaises(RuntimeError):
                RUN.check_complete(dict(engine_active=True, census=census))

    def test_nonfinite_metric_rejected(self):
        with self.assertRaises(RuntimeError):
            RUN.numeric_metrics(dict(census=dict(bytes=float('nan'))))

    def test_duplicate_payload_identity_rejected(self):
        spec = dict(head_bytes=16, retained_bytes=0, hobj=1, dropped=0)
        expected = dict(head='abc')
        line = ('VERIFY dataset=jointfixture object=1 bytes=16 '
                'hole=0 sha256=abc\n')
        RUN.check_payload(line, spec, expected)
        with self.assertRaises(RuntimeError):
            RUN.check_payload(line + line, spec, expected)
        with self.assertRaises(RuntimeError):
            RUN.check_payload(line.replace('abc', 'bad'), spec, expected)

    def test_snapshot_coverage_required_until_drop(self):
        spec = dict(head_bytes=0, retained_bytes=16, robj=7, dropped=0)
        expected = dict(retained='data', holes='zeros')
        hole = ('VERIFY dataset=jointfixture object=7 bytes=16 '
                'hole=1 sha256=zeros\n')
        snapshot = ('VERIFY dataset=jointfixture@retained object=7 '
                    'bytes=16 hole=0 sha256=data\n')
        with self.assertRaises(RuntimeError):
            RUN.check_payload(hole, spec, expected)
        RUN.check_payload(hole + snapshot, spec, expected)
        spec['dropped'] = 1
        RUN.check_payload(hole, spec, expected)

    def test_summary_preserves_mover_and_converter_scopes(self):
        metrics = {name: {} for name in (
            'before-mover', 'before-convert', 'convert',
            'converted-readonly', 'drop-retained', 'dropped-readonly')}
        metrics['convert'] = dict(mover_total_s=3, elapsed_s=1)
        row = dict(passed=True, record_kib=128, retained_pct=0,
                   convert_process_s=8, metrics=metrics,
                   mover_io=dict(vdev_write_bytes=500),
                   converter_io=dict(vdev_write_bytes=300))
        summary = RUN.summarize([row])[0]['metrics']
        self.assertEqual(summary['convert.mover_total_s']['median'], 3)
        self.assertEqual(summary['convert.elapsed_s']['median'], 1)
        self.assertEqual(summary['mover_io.vdev_write_bytes']['median'], 500)
        self.assertEqual(summary['converter_io.vdev_write_bytes']['median'],
                         300)


if __name__ == '__main__':
    unittest.main()
