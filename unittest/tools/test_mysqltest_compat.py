#!/usr/bin/env python3
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/deploy'))
from mysqltest_compat import normalize_regex_flags


class MysqltestCompat(unittest.TestCase):
    def test_only_global_modifiers_change(self):
        self.assertEqual('--replace_regex /g/gg/i /x/y/\n',
                         normalize_regex_flags('--replace_regex /g/gg/gi /x/y/g\n'))
        self.assertEqual('--replace_regex /a\\/b/c\\/d/i\n',
                         normalize_regex_flags('--replace_regex /a\\/b/c\\/d/ig\n'))

    def test_sql_and_invalid_directives_are_untouched(self):
        for text in ["SELECT '/a/b/g';\n", '--replace_regex /x/y/gx\n',
                     '--replace_regex /unterminated/g\n', '--replace_regex $variable\n']:
            self.assertEqual(text, normalize_regex_flags(text))

    def test_replacements_and_line_endings_are_preserved(self):
        self.assertEqual('  --replace_regex /__vid_[0-9]+(_[0-9]+)?/__vid_0/ /__type_[0-9]+_[0-9]+/__type_0_0/\r\n',
                         normalize_regex_flags('  --replace_regex /__vid_[0-9]+(_[0-9]+)?/__vid_0/g /__type_[0-9]+_[0-9]+/__type_0_0/g\r\n'))


if __name__ == '__main__':
    unittest.main()
