#!/usr/bin/env python3
"""Exercise the real parser generator in an isolated tree using initialized deps."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ParserGenerationCacheTest(unittest.TestCase):
    def test_cold_warm_incomplete_and_stale_cache(self):
        repo = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory(prefix="seekdb-parser-cache-") as directory:
            root = Path(directory)
            parser = root / "src/sql/parser"
            parser.mkdir(parents=True)
            for name in ("gen_parser.sh", "gen_type_name.sh", "sql_parser_mysql_mode.y",
                         "sql_parser_mysql_mode.l", "ftsparser.y", "ftsblex.l"):
                shutil.copy2(repo / "src/sql/parser" / name, parser / name)
            header = Path("src/query/api/query/parser/ob_item_type.h")
            (root / header).parent.mkdir(parents=True)
            shutil.copy2(repo / header, root / header)
            (root / "deps").symlink_to(repo / "deps", target_is_directory=True)
            env = dict(os.environ, NEED_PARSER_CACHE="ON")

            def generate():
                run = subprocess.run(["bash", "gen_parser.sh"], cwd=parser, env=env,
                                     capture_output=True, text=True, timeout=120)
                self.assertEqual(0, run.returncode, run.stdout + run.stderr)
                return run.stdout

            stamp = parser / "_MD5"
            output = parser / "sql_parser_mysql_mode_tab.c"
            self.assertFalse(stamp.exists())
            self.assertNotIn("hit the md5 cache", generate())
            self.assertEqual(32, len(stamp.read_text().strip()))
            self.assertGreater(output.stat().st_size, 0)

            before = output.stat().st_mtime_ns
            self.assertIn("hit the md5 cache", generate())
            self.assertEqual(before, output.stat().st_mtime_ns)

            (parser / "ftsparser_tab.h").unlink()
            self.assertNotIn("hit the md5 cache", generate())
            self.assertGreater((parser / "ftsparser_tab.h").stat().st_size, 0)

            expected = stamp.read_text()
            stamp.write_text("stale\n")
            self.assertNotIn("hit the md5 cache", generate())
            self.assertEqual(expected, stamp.read_text())


if __name__ == "__main__":
    unittest.main()
