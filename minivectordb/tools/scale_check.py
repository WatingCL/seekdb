"""Scale and memory check for MiniVectorDB.

Loads a table whose vector payload is far larger than what the engine keeps in
memory, then reports peak resident size, exact TopK latency, and recovery cost.
The point of the exercise is section 2.3 of the task: a dataset bigger than the
memory budget must still work, because rows live in the log and only a primary
key -> record offset index stays resident.

Usage (from the repository root):
  py -3 minivectordb/tools/scale_check.py --rows 200000 --dim 128
"""
import argparse
import ctypes
import json
import os
import random
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def peak_rss_bytes(pid, handle):
    if os.name == 'nt':
        class Counters(ctypes.Structure):
            _fields_ = [('cb', ctypes.c_uint32), ('PageFaultCount', ctypes.c_uint32),
                        ('PeakWorkingSetSize', ctypes.c_size_t), ('WorkingSetSize', ctypes.c_size_t),
                        ('QuotaPeakPagedPoolUsage', ctypes.c_size_t), ('QuotaPagedPoolUsage', ctypes.c_size_t),
                        ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t), ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                        ('PagefileUsage', ctypes.c_size_t), ('PeakPagefileUsage', ctypes.c_size_t),
                        ('PrivateUsage', ctypes.c_size_t)]

        c = Counters()
        c.cb = ctypes.sizeof(c)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(handle), ctypes.byref(c), c.cb):
            return None
        return c.PeakWorkingSetSize
    try:
        with open('/proc/%d/status' % pid) as fp:
            for line in fp:
                if line.startswith('VmHWM:'):
                    return int(line.split()[1]) * 1024
    except OSError:
        return None
    return None


class Db:
    def __init__(self, exe, data_dir):
        self.exe = exe
        self.data_dir = data_dir
        self.n = 0
        self.lines = []
        self.proc = subprocess.Popen([exe, '--data-dir', data_dir], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.append(line)

    def _send(self, payload):
        self.n += 1
        before = len(self.lines)
        payload = dict(payload, id=self.n)
        self.proc.stdin.write((json.dumps(payload) + '\n').encode())
        self.proc.stdin.flush()
        for _ in range(30000):
            if len(self.lines) > before:
                return json.loads(self.lines[before])
            time.sleep(0.001)
        raise RuntimeError('no response to request %d' % self.n)

    def ready(self):
        for _ in range(30000):
            if self.lines:
                return json.loads(self.lines[0])
            time.sleep(0.001)
        raise RuntimeError('no ready line')

    def sql(self, text, mode='exact'):
        return self._send({'op': 'execute', 'sql': text, 'search_mode': mode})

    def rss(self):
        handle = self.proc._handle if os.name == 'nt' else 0
        return peak_rss_bytes(self.proc.pid, handle)

    def shutdown(self):
        self._send({'op': 'shutdown'})
        self.proc.wait(timeout=60)

    def kill(self):
        self.proc.kill()
        self.proc.wait()


def vector(rng, dim):
    return '[' + ','.join('%.6f' % rng.uniform(-1, 1) for _ in range(dim)) + ']'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(ROOT, 'minivectordb', 'minivectordb.exe'))
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--dim', type=int, default=128)
    ap.add_argument('--queries', type=int, default=20)
    ap.add_argument('--batch', type=int, default=200, help='rows per INSERT statement')
    ap.add_argument('--data-dir', default=None)
    ap.add_argument('--output', default=None, help='write the JSON summary here')
    args = ap.parse_args()

    data_dir = args.data_dir or tempfile.mkdtemp(prefix='scale-')
    os.makedirs(data_dir, exist_ok=True)
    rng = random.Random(20261110)
    report = {'rows': args.rows, 'dim': args.dim, 'data_dir': data_dir}

    db = Db(args.exe, data_dir)
    print('handshake:', db.ready())
    t0 = time.monotonic()
    r = db.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, tag VARCHAR(32), v VECTOR(%d));' % args.dim)
    assert r.get('ok'), r
    batch_secs = 0.0
    for start in range(1, args.rows + 1, args.batch):
        stop = min(start + args.batch, args.rows + 1)
        values = ','.join("(%d,'tag-%d','%s')" % (i, i % 97, vector(rng, args.dim)) for i in range(start, stop))
        t1 = time.monotonic()
        r = db.sql('INSERT INTO t VALUES %s;' % values)
        batch_secs += time.monotonic() - t1
        assert r.get('ok'), r
    load_rss = db.rss()
    log_path = os.path.join(data_dir, 't.log')
    report['load'] = {
        'seconds': round(batch_secs, 3),
        'rows_per_second': int(args.rows / batch_secs) if batch_secs else None,
        'peak_rss_bytes': load_rss,
        'log_bytes': os.path.getsize(log_path),
    }
    print('load: %.1fs (%d rows/s) peakRSS=%.1f MiB log=%.1f MiB' % (
        batch_secs, report['load']['rows_per_second'], load_rss / 1048576.0,
        report['load']['log_bytes'] / 1048576.0))
    print('  bytes of vector payload per resident byte: %.1f' % (
        report['load']['log_bytes'] / max(load_rss, 1)))

    lat = []
    first_answer = None
    for q in range(args.queries):
        t1 = time.monotonic()
        r = db.sql("SELECT id, L2_DISTANCE(v, '%s') AS d FROM t ORDER BY d LIMIT 10;" % vector(rng, args.dim))
        lat.append(time.monotonic() - t1)
        assert r.get('ok'), r
        assert len(r['rows']) == 10, r
        if q == 0:
            first_answer = [row[0] for row in r['rows']]
    lat.sort()
    report['query'] = {
        'count': args.queries,
        'median_seconds': round(lat[len(lat) // 2], 6),
        'max_seconds': round(lat[-1], 6),
        'peak_rss_bytes': db.rss(),
        'first_top10_ids': first_answer,
    }
    print('exact TopK over %d rows: median %.1f ms, max %.1f ms, peakRSS=%.1f MiB' % (
        args.rows, lat[len(lat) // 2] * 1000, lat[-1] * 1000, report['query']['peak_rss_bytes'] / 1048576.0))

    r = db.sql("UPDATE t SET tag='hot' WHERE id <= 1000;")
    assert r.get('ok') and r['affected_rows'] == 1000, r
    r = db.sql('DELETE FROM t WHERE id > %d;' % (args.rows - 1000))
    assert r.get('ok') and r['affected_rows'] == 1000, r
    r = db.sql('SELECT id, tag FROM t WHERE id <= 3 ORDER BY id;')
    assert r.get('ok') and r['rows'] == [[1, 'hot'], [2, 'hot'], [3, 'hot']], r
    print('update/delete over the streamed table: ok')

    probe = "SELECT id, L2_DISTANCE(v, '%s') AS d FROM t ORDER BY d LIMIT 10;" % vector(rng, args.dim)
    before = [row[0] for row in db.sql(probe)['rows']]

    # The LIMIT form takes the bounded-heap path; the same query without LIMIT
    # sorts every match. Both must agree, filtered and unfiltered.
    qvec = vector(rng, args.dim)
    for where in ['', "WHERE tag = 'tag-7'"]:
        bounded = db.sql("SELECT id, L2_DISTANCE(v, '%s') AS d FROM t %s ORDER BY d LIMIT 10;" % (qvec, where))
        full = db.sql("SELECT id, L2_DISTANCE(v, '%s') AS d FROM t %s ORDER BY d;" % (qvec, where))
        assert bounded.get('ok') and full.get('ok'), (bounded, full)
        assert bounded['rows'] == full['rows'][:10], 'TopK heap disagrees with the full sort (%s)' % (where or 'no filter')
        print('TopK == full sort (%s): %d matches' % (where or 'no filter', len(full['rows'])))
    report['topk_matches_full_sort'] = True

    t0 = time.monotonic()
    db.shutdown()
    shutdown_secs = time.monotonic() - t0

    t0 = time.monotonic()
    db = Db(args.exe, data_dir)
    db.ready()
    recover_secs = time.monotonic() - t0
    r = db.sql(probe)
    assert r.get('ok'), r
    after = [row[0] for row in r['rows']]
    assert before == after, 'restart changed the answer: %s vs %s' % (before, after)
    report['restart'] = {
        'shutdown_seconds': round(shutdown_secs, 3),
        'recovery_seconds': round(recover_secs, 3),
        'peak_rss_bytes': db.rss(),
        'top10_ids_before': before,
        'top10_ids_after': after,
    }
    print('restart: shutdown %.2fs, recovery %.2fs (%d rows indexed), peakRSS=%.1f MiB, same TopK: %s' % (
        shutdown_secs, recover_secs, args.rows - 1000,
        report['restart']['peak_rss_bytes'] / 1048576.0, before == after))
    db.kill()

    resident_per_row = load_rss / float(args.rows)
    report['resident_bytes_per_row'] = round(resident_per_row, 2)
    report['row_bytes_on_disk'] = round(report['load']['log_bytes'] / float(args.rows), 2)
    print('resident %.1f B/row vs %.1f B/row on disk' % (resident_per_row, report['row_bytes_on_disk']))
    if args.output:
        os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
        with open(args.output, 'w', encoding='utf-8') as fp:
            json.dump(report, fp, indent=2, ensure_ascii=False)
        print('summary:', args.output)
    return 0


if __name__ == '__main__':
    sys.exit(main())
