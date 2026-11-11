#!/usr/bin/env python3
"""Guard FTS A/B aggregation against biased scoring and sample selection."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/benchmark'))
from fts_compare_binaries import compare_runs

CATEGORIES = {'build': ['b1', 'b2', 'b3'], 'tokenize': ['t1', 't2'],
              'query': ['q1', 'q2', 'q3', 'q4']}


def run(variant, build, tokenize, query):
    values = {'build': build, 'tokenize': tokenize, 'query': query}
    return {'variant': variant, 'metrics': {key: values[category]
            for category, keys in CATEGORIES.items() for key in keys}}


class FtsComparison(unittest.TestCase):
    def test_categories_are_equal_and_regressions_are_not_clipped(self):
        runs = [run('before', 100, 100, 100), run('after', 50, 100, 130)] * 2
        result = compare_runs(runs, CATEGORIES)
        self.assertAlmostEqual(result['categories']['build']['improvement'], .5)
        self.assertAlmostEqual(result['categories']['query']['improvement'], -.3)
        self.assertAlmostEqual(result['mean_improvement'], (.5 + 0 - .3) / 3)

    def test_all_runs_are_used_instead_of_the_fastest(self):
        runs = [run('before', 100, 100, 100), run('after', 40, 40, 40),
                run('after', 160, 160, 160), run('before', 100, 100, 100)]
        result = compare_runs(runs, CATEGORIES)
        self.assertEqual(result['mean_improvement'], 0)
        metric = result['categories']['build']['metrics'][0]
        self.assertEqual(metric['after_runs'], [40, 160])
        self.assertGreater(metric['after_stdev'], 80)

    def test_incomplete_or_nonpositive_measurements_fail(self):
        with self.assertRaises(ValueError):
            compare_runs([run('before', 100, 100, 100), run('after', 50, 50, 50)], CATEGORIES)
        with self.assertRaises(ValueError):
            compare_runs([run('before', 100, 100, 100), run('after', 0, 50, 50)] * 2, CATEGORIES)
        with self.assertRaises(ValueError):
            compare_runs([run('before', 100, 100, 100), run('after', float('nan'), 50, 50)] * 2, CATEGORIES)
        with self.assertRaises(ValueError):
            compare_runs([run('before', 100, 100, 100), run('after', 50, 50, 50)] * 2 + [run('after', 50, 50, 50)], CATEGORIES)


if __name__ == '__main__':
    unittest.main()
