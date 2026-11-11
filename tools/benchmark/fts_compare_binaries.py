#!/usr/bin/env python3
"""Run an alternating full FTS A/B comparison using the untouched benchmark.

Both binaries must already be built with the same toolchain and flags. Each run
uses a fresh disposable database directory on the same port, cpu_count=2 and
memory_limit=2G. No data, SQL, timings or fixed scoring baseline are rewritten.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import socket
from statistics import mean, stdev
import subprocess
import sys
import time

from fts_large_bench_score import check_config, load_baseline, load_report, calculate_score


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def compare_runs(runs, score_categories):
    """Include every completed run, retain regressions, weight categories equally."""
    categories = {}
    for category, names in score_categories.items():
        results = []
        for name in names:
            values = {v: [r['metrics'][name] for r in runs if r['variant'] == v]
                      for v in ('before', 'after')}
            if (len(values['before']) != len(values['after'])
                    or any(len(samples) < 2 or any(not math.isfinite(x) or x <= 0 for x in samples)
                           for samples in values.values())):
                raise ValueError(f'{name}: require equal counts of >=2 finite positive timings for each variant')
            before, after = mean(values['before']), mean(values['after'])
            results.append({'metric': name, 'before_mean': before, 'after_mean': after,
                            'before_runs': values['before'], 'after_runs': values['after'],
                            'before_stdev': stdev(values['before']), 'after_stdev': stdev(values['after']),
                            'improvement': (before - after) / before})
        categories[category] = {'improvement': mean(r['improvement'] for r in results), 'metrics': results}
    return {'categories': categories, 'mean_improvement': mean(c['improvement'] for c in categories.values())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', required=True, type=Path)
    parser.add_argument('--after', required=True, type=Path)
    parser.add_argument('--client', required=True, type=Path)
    parser.add_argument('--mysqltest', required=True, type=Path)
    parser.add_argument('--bash', default='bash')
    parser.add_argument('--port', type=int, default=28935)
    parser.add_argument('--pairs', type=int, default=2, help='even number >=2; order AB BA AB BA ...')
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--source-manifest', type=Path)
    args = parser.parse_args()
    if args.pairs < 2 or args.pairs % 2:
        parser.error('--pairs must be even and >=2 to balance run order')
    root = Path(__file__).resolve().parents[2]
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('--output-dir must be empty (never append to old timing reports)')
    output.mkdir(parents=True, exist_ok=True)
    binaries = {'before': args.before.resolve(), 'after': args.after.resolve()}
    for binary in binaries.values():
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f'not an executable binary: {binary}')
    hashes = {key: sha256(path) for key, path in binaries.items()}
    if hashes['before'] == hashes['after']:
        parser.error('the two binaries are identical')
    baseline_path = root / 'tools/benchmark/fts_large_bench_baseline.json'
    baseline = load_baseline(baseline_path)
    protected = [root / 'tools/benchmark' / name for name in (
        'fts_large_bench.sh', 'fts_large_bench_gen.py', 'fts_large_bench_setup.sql',
        'fts_large_bench_score.py', 'fts_large_bench_baseline.json')]
    original_hashes = {str(path.relative_to(root)): sha256(path) for path in protected}
    client = [str(args.client.resolve()), '--no-defaults', '--protocol=tcp', '-h127.0.0.1',
              f'-P{args.port}', '-uroot', '-N', '-s', '-A', '--default-character-set=utf8mb4']
    def sql(query):
        return subprocess.check_output(client + ['-e', query], text=True, stderr=subprocess.PIPE).strip()
    def result_digest(query):
        # Hash every result row, not just counts; stream to bound memory use.
        digest = hashlib.sha256()
        with subprocess.Popen(client + ['--quick', '--raw', '-e', query], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) as process:
            for block in iter(lambda: process.stdout.read(65536), b''):
                digest.update(block)
            error = process.stderr.read().decode()
            if process.wait():
                raise RuntimeError(f"{query}: {error}")
        return digest.hexdigest()
    manifest = {
        'platform': platform.platform(), 'machine': platform.machine(),
        'binary_sha256': hashes, 'binaries': {k: str(v) for k, v in binaries.items()},
        'benchmark_files_sha256': original_hashes,
        'config': baseline['benchmark_config'], 'server_config': {'cpu_count': 2, 'memory_limit': '2G'},
        'source_manifest': json.loads(args.source_manifest.read_text()) if args.source_manifest else None,
        'order': [], 'runs': [],
    }
    if platform.system() == 'Darwin':
        manifest['hardware'] = subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string', 'hw.memsize'], text=True).splitlines()
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    checks = {
        'corpus_tokens': 'SELECT id,TOKENIZE(title,\'ik\'),TOKENIZE(CAST(content AS CHAR CHARACTER SET utf8mb4),\'ik\'),TOKENIZE(title_en,\'beng\'),TOKENIZE(CAST(content_en AS CHAR CHARACTER SET utf8mb4),\'beng\') FROM fts_large_bench.docs ORDER BY id',
    }
    for name, columns, terms in (
        ('cn', 'title,content', '全文索引 分词器'),
        ('beng', 'title_en,content_en', 'database performance audit'),
        ('mixed', 'title,content', 'OceanBase 稳定 database'),
        ('content', 'content', '倒排索引 tokenizer'),
    ):
        match = f"MATCH({columns}) AGAINST('{terms}')"
        checks['match_' + name] = f'SELECT id,{match} AS score FROM fts_large_bench.docs WHERE {match} ORDER BY id'
        if name == 'content':
            checks['match_limit'] = checks['match_' + name] + ' LIMIT 20'
    manifest["correctness_queries"] = checks
    reference_checks = None
    for pair in range(args.pairs):
        order = ('before', 'after') if pair % 2 == 0 else ('after', 'before')
        for variant in order:
            label = f'{len(manifest["runs"]) + 1:02d}-{variant}'
            run_dir = output / label
            run_dir.mkdir()
            data_dir = run_dir / 'data'
            data_dir.mkdir()
            # Fail instead of accidentally measuring an existing user service.
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', args.port))
            command = [str(binaries[variant]), '--nodaemon', f'--port={args.port}',
                       f'--base-dir={data_dir}', '--parameter', 'cpu_count=2',
                       '--parameter', 'memory_limit=2G']
            print(f'Starting {label}', flush=True)
            load_before = os.getloadavg()
            with (run_dir / 'server.out').open('w') as server_log:
                server = subprocess.Popen(command, stdout=server_log, stderr=subprocess.STDOUT)
                try:
                    for _ in range(120):
                        if server.poll() is not None:
                            raise RuntimeError(f'{label}: server exited {server.returncode}')
                        try:
                            version = sql('SELECT @@version_comment')
                            break
                        except subprocess.CalledProcessError:
                            time.sleep(.5)
                    else:
                        raise RuntimeError(f'{label}: server did not start')
                    sql('CREATE DATABASE IF NOT EXISTS test')
                    environment = os.environ.copy()
                    environment.update({key.upper(): str(value) for key, value in baseline['benchmark_config'].items()})
                    environment.update(MYSQL=shlex.join(client), MYSQL_VERBOSE=shlex.join(client),
                                       LABEL=label, OUTPUT=str(run_dir / 'report.txt'))
                    with (run_dir / 'benchmark.out').open('w') as benchmark_log:
                        subprocess.run([args.bash, str(root / 'tools/benchmark/fts_large_bench.sh')],
                                       cwd=root, env=environment, stdout=benchmark_log,
                                       stderr=subprocess.STDOUT, check=True)
                    metrics = load_report(run_dir / 'report.txt')
                    check_config(metrics, baseline, False)
                    fixed_score = calculate_score(metrics, baseline)
                    (run_dir / 'fixed-baseline-score.json').write_text(json.dumps(fixed_score, indent=2) + '\n')
                    with (run_dir / 'regressions.out').open('w') as regressions:
                        subprocess.run([sys.executable, str(root / 'tools/deploy/run_fts_tests.py'),
                                        '--mysqltest', str(args.mysqltest.resolve()), '--port', str(args.port),
                                        '--logdir', str(run_dir / 'regressions')], cwd=root,
                                       stdout=regressions, stderr=subprocess.STDOUT, check=True)
                    digests = {name: result_digest(query) for name, query in checks.items()}
                    (run_dir / 'correctness.json').write_text(json.dumps(digests, indent=2) + '\n')
                    if reference_checks is None:
                        reference_checks = digests
                    if digests != reference_checks:
                        raise RuntimeError(f'{label}: full TOKENIZE/MATCH results differ from first run')
                    manifest['order'].append(variant)
                    manifest['runs'].append({'label': label, 'variant': variant, 'version': version,
                                             'metrics': metrics, 'correctness': digests,
                                             'load_before': load_before, 'load_after': os.getloadavg()})
                    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
                    print(f'{label}: all metrics and complete result digests verified', flush=True)
                finally:
                    if server.poll() is None:
                        server.terminate()
                        try:
                            server.wait(timeout=45)
                        except subprocess.TimeoutExpired:
                            server.kill()
                            server.wait()
    comparison = compare_runs(manifest['runs'], baseline['score_categories'])
    comparison.update(correctness_equal=True, pairs=args.pairs,
                      note='Same-machine before/after rates, not an official competition score; negative rates are retained.')
    if original_hashes != {str(p.relative_to(root)): sha256(p) for p in protected}:
        raise RuntimeError('A protected benchmark/scoring input changed during the run')
    (output / 'comparison.json').write_text(json.dumps(comparison, indent=2) + '\n')
    print(json.dumps(comparison, indent=2), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
