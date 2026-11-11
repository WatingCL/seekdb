#!/usr/bin/env python3
"""Run the five original AI COLUMN mysqltests against a disposable instance."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

CASES = (
    'ai_column_ddl', 'ai_column_t1_dml', 'ai_column_worker',
    'ai_column_vector_index', 'ai_column_retry_strict',
)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqltest', default=shutil.which('mysqltest') or shutil.which('mariadb-test'))
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2885)
    parser.add_argument('--user', default='root')
    parser.add_argument('--logdir', required=True, type=Path)
    args = parser.parse_args()
    candidate = Path(args.mysqltest).resolve() if args.mysqltest else root / 'deps/3rd/u01/obclient/bin/mysqltest'
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        parser.error('Pass --mysqltest with the path to an executable mysqltest client')
    logs = args.logdir.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    deploy = root / 'tools/deploy'
    suite = deploy / 'mysql_test/test_suite/ai_column'
    passed = []
    try:
        for case in CASES:
            print(f'Running {case}', flush=True)
            command = [str(candidate), '--no-defaults', '--protocol=tcp',
                       f'--host={args.host}', f'--port={args.port}', f'--user={args.user}',
                       '--database=test', f'--test-file={suite / "t" / (case + ".test")}',
                       f'--result-file={suite / "r/mysql" / (case + ".result")}',
                       f'--logdir={logs}']
            with (logs / (case + '.out')).open('w') as output:
                result = subprocess.run(command, cwd=deploy, stdout=output, stderr=subprocess.STDOUT)
            print((logs / (case + '.out')).read_text(), end='', flush=True)
            if result.returncode:
                return result.returncode
            passed.append(case)
        print(f'AI COLUMN: {len(passed)}/{len(CASES)} passed (original SQL and expected results)')
        return 0
    finally:
        for port in range(19121, 19125):
            subprocess.run([sys.executable, str(suite / 'include/ai_column_mock.py'), 'stop', str(port)],
                           cwd=deploy, check=False)


if __name__ == '__main__':
    sys.exit(main())
