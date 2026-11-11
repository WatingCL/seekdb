#!/usr/bin/env python3
"""Compare the original FTS SQL regressions without changing expected output."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

CASES = ('ik_character_classification', 'beng_ascii_normalization')


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqltest', default=shutil.which('mysqltest') or shutil.which('mariadb-test'))
    parser.add_argument('--port', type=int, default=2885)
    parser.add_argument('--logdir', required=True, type=Path)
    args = parser.parse_args()
    client = Path(args.mysqltest).resolve() if args.mysqltest else root / 'deps/3rd/u01/obclient/bin/mysqltest'
    if not client.is_file() or not os.access(client, os.X_OK):
        parser.error('Pass --mysqltest with the path to an executable mysqltest client')
    logs = args.logdir.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    suite = root / 'tools/deploy/mysql_test/test_suite/fts_index'
    for case in CASES:
        command = [str(client), '--no-defaults', '--result-format-version=1',
                   f'--prologue={root / "tools/deploy/mysql_test/compat/quiet_connections.inc"}', '--protocol=tcp',
                   '--host=127.0.0.1', f'--port={args.port}', '--user=root', '--database=test',
                   f'--test-file={suite / "t" / (case + ".test")}',
                   f'--result-file={suite / "r/mysql" / (case + ".result")}', f'--logdir={logs}']
        with (logs / (case + '.out')).open('w') as output:
            result = subprocess.run(command, cwd=root / 'tools/deploy', stdout=output, stderr=subprocess.STDOUT)
        print(case + ': ' + (logs / (case + '.out')).read_text(), end='', flush=True)
        if result.returncode:
            return result.returncode
    return 0


if __name__ == '__main__':
    sys.exit(main())
