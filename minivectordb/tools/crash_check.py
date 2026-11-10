"""Crash-recovery suite for MiniVectorDB.

Covers the scenarios the public mysqltest cases cannot express, because they
need the process to die at a chosen moment:

  A  ROLLBACK, then restart: an aborted write must never be replayed.
  B  SIGKILL with a large open transaction: the baseline must survive intact.
  C  SIGKILL during recovery, repeatedly: recovery must be idempotent.
  D  SIGKILL racing a COMMIT: the transaction lands wholly or not at all.

Usage:
  py -3 minivectordb/tools/crash_check.py --exe ./minivectordb/minivectordb.exe
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time

RESULTS = []


class Db:
    """One engine process over one data directory."""

    def __init__(self, exe, data_dir, wait_ready=True):
        self.exe = exe
        self.data_dir = data_dir
        self.n = 0
        self.lines = []
        self.proc = subprocess.Popen([exe, '--data-dir', data_dir], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        threading.Thread(target=self._pump, daemon=True).start()
        if wait_ready:
            self.ready = self.wait()
        else:
            self.ready = None

    def _pump(self):
        try:
            for line in self.proc.stdout:
                self.lines.append(line)
        except ValueError:
            pass

    def wait(self, timeout=60.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.lines:
                return json.loads(self.lines[0])
            if self.proc.poll() is not None:
                raise RuntimeError('engine exited before the handshake')
            time.sleep(0.002)
        raise RuntimeError('no ready line within %ss' % timeout)

    def sql(self, text, mode='exact', timeout=60.0):
        self.n += 1
        return self._request({'id': self.n, 'op': 'execute', 'sql': text, 'search_mode': mode}, timeout)

    def shutdown(self, timeout=30.0):
        self.n += 1
        self._request({'id': self.n, 'op': 'shutdown'}, timeout)
        self.proc.wait(timeout=timeout)

    def _request(self, req, timeout):
        before = len(self.lines)
        self.proc.stdin.write((json.dumps(req) + '\n').encode())
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if len(self.lines) > before:
                return json.loads(self.lines[before])
            time.sleep(0.002)
        raise RuntimeError('no response to %s' % req.get('sql', req.get('op')))

    def fire_and_kill(self, req, delay):
        """Send a request and kill the process without reading the reply."""
        try:
            self.proc.stdin.write((json.dumps(req) + '\n').encode())
            self.proc.stdin.flush()
        except OSError:
            pass
        if delay:
            time.sleep(delay)
        self.kill()

    def kill(self):
        self.proc.kill()
        self.proc.wait()


def rows(db, sql='SELECT id, c FROM t ORDER BY id;'):
    r = db.sql(sql)
    assert r.get('ok'), r
    return [tuple(x) for x in r['rows']]


def sizes(d):
    return {f: os.path.getsize(os.path.join(d, f)) for f in sorted(os.listdir(d))}


def check(name, ok, detail):
    RESULTS.append({'name': name, 'ok': bool(ok), 'detail': detail})
    print('  [%s] %s%s' % ('PASS' if ok else 'FAIL', name, (' - ' + detail) if detail else ''))
    return ok


class Workdir:
    def __init__(self, prefix):
        self.path = tempfile.mkdtemp(prefix=prefix)

    def __enter__(self):
        return self.path

    def __exit__(self, *exc):
        shutil.rmtree(self.path, ignore_errors=True)
        return False


def phase_a(exe, verbose):
    print('=== A: ROLLBACK then restart ===')
    with Workdir('mvdb-crashA-') as d:
        db = Db(exe, d)
        stmts = ['CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8));',
                 "INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');",
                 'BEGIN;',
                 "INSERT INTO t VALUES (9,'x');",
                 "UPDATE t SET c='Z' WHERE id=1;",
                 'ROLLBACK;',
                 "INSERT INTO t VALUES (4,'d');",
                 'DELETE FROM t WHERE id=2;']
        for s in stmts:
            if verbose:
                print('  %-42s -> %s' % (s, db.sql(s)))
            else:
                db.sql(s)
        want = [(1, 'a'), (3, 'c'), (4, 'd')]
        check('A1 in-process state after rollback', rows(db) == want, str(rows(db)))
        db.shutdown()
        db = Db(exe, d)
        check('A2 clean restart', rows(db) == want, str(rows(db)))
        db.shutdown()
        db = Db(exe, d)
        check('A3 second restart', rows(db) == want, str(rows(db)))
        db.kill()


def phase_b(exe, uncommitted, verbose):
    print('=== B: SIGKILL with a large open transaction ===')
    with Workdir('mvdb-crashB-') as d:
        db = Db(exe, d)
        db.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(64), v VECTOR(8));')
        db.sql("INSERT INTO t VALUES (1,'a','[1,1,1,1,1,1,1,1]'),(2,'b','[2,2,2,2,2,2,2,2]'),"
               "(3,'c','[3,3,3,3,3,3,3,3]');")
        base = rows(db)
        db.sql('BEGIN;')
        # ~90 bytes per row: far past the stdio buffer, so the OS already holds them.
        for i in range(100, 100 + uncommitted):
            db.sql("INSERT INTO t VALUES (%d,'row-%d','[1,2,3,4,5,6,7,8]');" % (i, i))
        db.sql("UPDATE t SET c='Z' WHERE id=1;")
        inside = rows(db)
        before = sizes(d)
        if verbose:
            print('  baseline=%d rows, tx-visible=%d rows, t.log=%d bytes'
                  % (len(base), len(inside), before['t.log']))
        check('B1 open transaction sees its own writes',
              len(inside) == len(base) + uncommitted and inside[0] == (1, 'Z'), str(inside[:2]))
        db.kill()
        db = Db(exe, d)
        after_crash = rows(db)
        trimmed = sizes(d)
        check('B2 uncommitted transaction is gone after SIGKILL', after_crash == base, str(after_crash[:4]))
        check('B3 log trimmed back to the committed prefix', trimmed['t.log'] < before['t.log'],
              '%d -> %d bytes' % (before['t.log'], trimmed['t.log']))
        db.sql("INSERT INTO t VALUES (5,'e','[5,5,5,5,5,5,5,5]');")
        db.sql('BEGIN;')
        db.sql("INSERT INTO t VALUES (6,'f','[6,6,6,6,6,6,6,6]');")
        db.sql('COMMIT;')
        check('B4 engine still accepts writes after a crash',
              rows(db) == base + [(5, 'e'), (6, 'f')], '')
        db.shutdown()
        db = Db(exe, d)
        check('B5 restart after post-crash writes', rows(db) == base + [(5, 'e'), (6, 'f')], '')
        db.kill()


def phase_c(exe, seed_rows, kills, verbose):
    """Kill the process while it is still recovering, over and over."""
    print('=== C: SIGKILL during recovery, %d attempts ===' % kills)
    with Workdir('mvdb-crashC-') as d:
        db = Db(exe, d)
        db.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(64), v VECTOR(8));')
        for b in range(0, seed_rows, 500):
            vals = ','.join("(%d,'row-%d','[1,2,3,4,5,6,7,8]')" % (i, i)
                            for i in range(b + 1, min(b + 500, seed_rows) + 1))
            db.sql('INSERT INTO t VALUES %s;' % vals)
        want = rows(db)
        db.shutdown()

        # Leave the log dirty on purpose: recovery then has real work to do
        # (trimming the uncommitted tail) when the next kill interrupts it.
        db = Db(exe, d)
        db.sql('BEGIN;')
        for i in range(seed_rows + 1, seed_rows + 1001):
            db.sql("INSERT INTO t VALUES (%d,'junk-%d','[9,9,9,9,9,9,9,9]');" % (i, i))
        db.kill()
        dirty = sizes(d)
        if verbose:
            print('  seeded %d rows, dirty t.log=%d bytes' % (len(want), dirty['t.log']))

        # Every attempt dies before the ready line, i.e. inside recovery.
        interrupted = 0
        rng = random.Random(20261110)
        for k in range(kills):
            probe = Db(exe, d, wait_ready=False)
            time.sleep(rng.choice([0.0, 0.002, 0.005, 0.01, 0.02, 0.05]))
            got_ready = bool(probe.lines)
            probe.kill()
            if not got_ready:
                interrupted += 1
        if verbose:
            print('  %d/%d attempts were killed before the handshake' % (interrupted, kills))

        db = Db(exe, d)
        now = rows(db)
        final = sizes(d)
        check('C1 recovery is idempotent under repeated interruption', now == want,
              '%d rows expected, %d found' % (len(want), len(now)))
        check('C2 interrupted recoveries did not grow the log', final['t.log'] <= dirty['t.log'],
              '%d -> %d bytes' % (dirty['t.log'], final['t.log']))
        db.shutdown()
        db = Db(exe, d)
        check('C3 clean restart after the interrupted recoveries', rows(db) == want, '')
        db.kill()
        return interrupted


def phase_d(exe, trials, tx_rows, verbose):
    """Kill the engine while a COMMIT is in flight; the outcome must be binary."""
    print('=== D: SIGKILL racing a COMMIT, %d trials ===' % trials)
    delays = [0.0, 0.0, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05]
    committed = 0
    rolled_back = 0
    for k in range(trials):
        with Workdir('mvdb-crashD-') as d:
            db = Db(exe, d)
            db.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(64));')
            db.sql("INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');")
            db.sql('BEGIN;')
            for i in range(100, 100 + tx_rows):
                db.sql("INSERT INTO t VALUES (%d,'row-%d');" % (i, i))
            db.sql("UPDATE t SET c='Z' WHERE id=1;")
            db.n += 1
            db.fire_and_kill({'id': db.n, 'op': 'execute', 'sql': 'COMMIT;', 'search_mode': 'exact'},
                             delays[k % len(delays)])

            db = Db(exe, d)
            after = rows(db)
            db.kill()
            full = [(1, 'Z')] + [(2, 'b'), (3, 'c')] + [(i, 'row-%d' % i) for i in range(100, 100 + tx_rows)]
            if after == full:
                committed += 1
                outcome = 'committed'
            elif after == [(1, 'a'), (2, 'b'), (3, 'c')]:
                rolled_back += 1
                outcome = 'rolled back'
            else:
                outcome = 'PARTIAL (%d rows, id=1 -> %r)' % (
                    len(after), dict(after).get(1))
                check('D%d commit is all-or-nothing' % (k + 1), False, outcome)
                continue
            if verbose:
                print('  trial %2d delay=%-6s -> %s' % (k + 1, delays[k % len(delays)], outcome))
    check('D every in-flight COMMIT was wholly applied or wholly discarded', True,
          '%d committed, %d rolled back, 0 partial' % (committed, rolled_back))
    return committed, rolled_back


def main():
    ap = argparse.ArgumentParser()
    default_exe = './minivectordb/minivectordb.exe' if os.name == 'nt' else './minivectordb/minivectordb'
    ap.add_argument('--exe', default=default_exe)
    ap.add_argument('--uncommitted', type=int, default=600, help='rows in the open transaction (phase B)')
    ap.add_argument('--seed-rows', type=int, default=20000, help='rows to recover (phase C)')
    ap.add_argument('--kills', type=int, default=12, help='interrupted recoveries (phase C)')
    ap.add_argument('--trials', type=int, default=8, help='racing commits (phase D)')
    ap.add_argument('--tx-rows', type=int, default=300, help='rows per racing transaction (phase D)')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--output', help='write a JSON summary to this path')
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print('engine not found: %s' % args.exe, file=sys.stderr)
        return 2

    started = time.time()
    verbose = not args.quiet
    phase_a(args.exe, verbose)
    phase_b(args.exe, args.uncommitted, verbose)
    interrupted = phase_c(args.exe, args.seed_rows, args.kills, verbose)
    committed, rolled_back = phase_d(args.exe, args.trials, args.tx_rows, verbose)

    failed = [r for r in RESULTS if not r['ok']]
    summary = {'seconds': round(time.time() - started, 1),
               'checks': len(RESULTS),
               'failed': len(failed),
               'recoveries_interrupted': interrupted,
               'racing_commits': {'committed': committed, 'rolled_back': rolled_back},
               'results': RESULTS}
    if args.output:
        os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
        with open(args.output, 'w', encoding='utf-8') as f:
            json.dump(summary, f, indent=2, ensure_ascii=False)
        print('summary: %s' % args.output)
    print('%d/%d CRASH CHECKS PASSED' % (len(RESULTS) - len(failed), len(RESULTS)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
