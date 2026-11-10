#!/usr/bin/env python3
"""Native file-SQL integration checks against a disposable seekdb instance."""
import argparse
import json
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True)
    parser.add_argument('--port', type=int, default=2885)
    args = parser.parse_args()
    client = [args.client, '--no-defaults', '-h127.0.0.1', '-P%d' % args.port,
              '-uroot', '-N', '-s', '--default-character-set=utf8mb4', '-Dtest']
    count = 0

    def sql(query, expected=None, fail=False, enable=True):
        nonlocal count
        prefix = 'SET SESSION enable_file_sql=ON;' if enable else ''
        run = subprocess.run(client + ['-e', prefix + query], capture_output=True,
                             text=True, timeout=60)
        assert (run.returncode != 0) == fail, (query, run.stdout, run.stderr)
        result = run.stdout.strip()
        if expected is not None:
            assert result == expected, (query, expected, result)
        count += 1
        return result

    def literal(value):
        return "'" + str(value).replace("'", "''") + "'"

    with tempfile.TemporaryDirectory(prefix='native-file-sql-') as directory:
        root = pathlib.Path(directory)
        csv = root / 'items.csv'
        csv.write_text('id,label,score,active,day\r\n1,"a,b",1.5,true,2024-02-29\r\n'
                       '2,"line\nbreak",2.5,false,\r\n3,"quote ""yes""",3.0,true,2025-01-02\r\n')
        jsonl = root / 'items.jsonl'
        jsonl.write_text('\n'.join(json.dumps(x) for x in [
            {'id': 1, 'category': 'first'}, {'id': 3, 'category': 'third'},
            {'id': 4, 'extra': 9}]) + '\n')
        c, j = literal(csv), literal(jsonl)
        sql('SELECT @@session.enable_file_sql', '0', enable=False)
        sql('SELECT * FROM read_csv(%s)' % c, fail=True, enable=False)
        previous = sql('SELECT @@global.secure_file_priv')
        sql('SET GLOBAL secure_file_priv=%s' % literal(root))
        try:
            sql('SELECT COUNT(*),SUM(id),SUM(score),SUM(active) FROM read_csv(%s)' % c,
                '3\t6\t7\t2')
            sql('SELECT id,REPLACE(label,CHAR(10),\'<NL>\') FROM read_csv(%s) ORDER BY id' % c,
                '1\ta,b\n2\tline<NL>break\n3\tquote "yes"')
            sql('SELECT category,COUNT(*) FROM read_json(%s) GROUP BY category HAVING COUNT(*)>0 '
                'ORDER BY category' % j, 'NULL\t1\nfirst\t1\nthird\t1')
            sql('SELECT c.id,j.category FROM read_csv(%s) c INNER JOIN read_ndjson(%s) j '
                'ON c.id=j.id ORDER BY c.id' % (c,j), '1\tfirst\n3\tthird')
            sql('SELECT c.id,j.category FROM read_csv(%s) c LEFT JOIN read_json(%s) j '
                'ON c.id=j.id ORDER BY c.id' % (c,j), '1\tfirst\n2\tNULL\n3\tthird')
            sql('SELECT id*2 AS doubled FROM read_csv(%s) WHERE score>1.5 ORDER BY doubled DESC LIMIT 1' % c,
                '6')
            sql('SELECT * FROM read_csv(%s,sample_size=1)' % c, fail=True)
            sql('SELECT * FROM read_csv(%s,unexpected=1)' % c, fail=True)
            raw = root/'headerless.csv'
            raw.write_text('1;"";NULL\n2;"NULL";x\n')
            sql('SELECT column1,column2,column3 FROM read_csv(%s,header=false,delim=\';\',nullstr=\'NULL\') '
                'ORDER BY column1' % literal(raw), '1\t\tNULL\n2\tNULL\tx')
            bad = root/'bad.csv'
            bad.write_bytes(b'a\n\xff')
            sql('SELECT * FROM read_csv(%s)' % literal(bad), fail=True)
            bad.write_text('a,b\n1,2,3\n')
            sql('SELECT * FROM read_csv(%s)' % literal(bad), fail=True)
            bad.write_text('a\n"unterminated\n')
            sql('SELECT * FROM read_csv(%s)' % literal(bad), fail=True)
            nested = root/'nested.jsonl'
            nested.write_text('{"nested":{"x":1}}\n')
            sql('SELECT * FROM read_json(%s)' % literal(nested), fail=True)
            sql("SELECT * FROM read_csv('/etc/passwd')", fail=True)
            link = root/'escape.csv'
            link.symlink_to('/etc/passwd')
            sql('SELECT * FROM read_csv(%s)' % literal(link), fail=True)
            link.unlink()
            sql('PREPARE p FROM %s; SET SESSION enable_file_sql=OFF; EXECUTE p' %
                literal('SELECT COUNT(*) FROM read_csv(%s)' % c), fail=True)
            # Ordinary SQL remains usable with the feature disabled.
            sql('SELECT 1+2', '3', enable=False)
            print('PASS: %d native file-SQL checks (scans, expressions, aggregation, JOIN, NULL and security)' % count)
        finally:
            sql('SET GLOBAL secure_file_priv=%s' % ('NULL' if previous=='NULL' else literal(previous)))


if __name__ == '__main__':
    main()
