#!/usr/bin/env python3
"""Capture or compare read-only FTS SQL results on an existing benchmark database.

Requires PyMySQL and the unmodified benchmark's 20,000-row fts_large_bench
schema/indexes. Capture with the old binary first, then use --compare with the
new binary. Does not create, drop, load, or modify tables.
"""
import argparse
import json
from pathlib import Path
import pymysql


def execution_queries():
    match="MATCH(content) AGAINST('倒排索引 tokenizer')"
    sqls=[f'SELECT COUNT(*) FROM (SELECT id FROM docs WHERE {match} LIMIT {limit} OFFSET {offset}) t' for limit,offset in [(0,0),(1,0),(20,0),(20,10),(20,100000),(10000,1)]]
    sqls += [f'SELECT SUM(id),MIN(id),MAX(id) FROM (SELECT id FROM docs WHERE {match} LIMIT 20) t', f'SELECT id FROM docs WHERE {match} LIMIT 20',f'SELECT COUNT(*) FROM (SELECT id FROM docs WHERE {match} ORDER BY id DESC LIMIT 20) t', f'SELECT COUNT(*) FROM (SELECT id FROM docs WHERE {match} LIMIT 20) t WHERE id > 500', f'SELECT COUNT(*) FROM docs d JOIN (SELECT id FROM docs WHERE {match} LIMIT 20) t ON d.id=t.id', f'SELECT COUNT(*) FROM docs WHERE id IN (SELECT id FROM docs WHERE {match})']
    sqls += ["SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('全文索引 分词器')", "SELECT COUNT(*) FROM docs WHERE MATCH(title_en,content_en) AGAINST('database performance audit')", "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('OceanBase 稳定 database')"]
    sqls += ["SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('')", "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('zzzznonexistenttokenzzzz')", "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('+数据库 -全文' IN BOOLEAN MODE)", "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('数据库') > 0.5", "SELECT SUM(id),MIN(id),MAX(id) FROM docs WHERE MATCH(title,content) AGAINST('数据库')", "SELECT id,MATCH(title,content) AGAINST('数据库') s FROM docs WHERE MATCH(title,content) AGAINST('数据库') ORDER BY s DESC,id LIMIT 20", "SELECT /*+ INDEX(docs PRIMARY) */ COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('数据库')", "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('全文索引') OR id<20", "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('数据库') AND category=1"]
    sqls += ["SELECT COUNT(*) FROM docs WHERE MATCH(title_en,content_en) AGAINST('"+' '.join(str(x) for x in range(100,400))+"')", "SELECT COUNT(*) FROM docs WHERE MATCH(title_en,content_en) AGAINST('the and is')"]
    return sqls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2881)
    parser.add_argument('--user', default='root')
    parser.add_argument('--database', default='fts_large_bench')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--execution-regressions', action='store_true')
    parser.add_argument('--assert-fast-plans', action='store_true')
    parser.add_argument('--repeat', type=int, default=1, help='repeat each SQL to exercise exact-result cache hits')
    parser.add_argument('--cache-boundaries', action='store_true')
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('--repeat must be positive')
    connection = pymysql.connect(host=args.host, port=args.port, user=args.user,
                                 database=args.database, charset='utf8mb4', autocommit=True)
    outputs = []
    texts = ['', 'Alpha ALPHA alpha beta', '数据库全文索引，优化中文分词。',
             'OceanBase 稳定 database', '🔥"🚀"zzz', 'café cafe', 'cafe café',
             'ＡＢＣ１２３ 中文 〇一二三萬億', 'a@b.com 3.14 你好！',
             'A' * 300, '中文' * 150, '中文', '中华人民共和国人民大会堂']
    queries = [
        "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('全文索引 分词器')",
        "SELECT COUNT(*) FROM docs WHERE MATCH(title_en,content_en) AGAINST('database performance audit')",
        "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('OceanBase 稳定 database')",
        "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20)t",
        "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('数据库') > 0.5",
        "SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('+数据库 -全文' IN BOOLEAN MODE)",
        "SELECT id, MATCH(title,content) AGAINST('数据库') s FROM docs WHERE MATCH(title,content) AGAINST('数据库') ORDER BY s DESC,id LIMIT 20",
    ]
    try:
        with connection.cursor() as cursor:
            def execute(*parameters):
                previous = None
                for index in range(args.repeat):
                    cursor.execute(*parameters)
                    if index + 1 < args.repeat:
                        rows = cursor.fetchall()
                        if previous is not None:
                            assert rows == previous, 'repeated SQL results differ'
                        previous = rows
                # Leave the final result for the normal capture/comparison.

            for ft_parser in ['ik', 'beng', 'space', 'ngram']:
                for text in texts:
                    for properties in ['[]', '[{"output":"all"}]']:
                        execute('SELECT tokenize(%s,%s,%s) AS result', (text, ft_parser, properties))
                        outputs.append(cursor.fetchall())
            query_results = []
            for query in queries:
                execute(query)
                query_results.append(cursor.fetchall())
            assert [rows[0][0] for rows in query_results[:4]] == [8001, 11000, 7332, 20]
            outputs.extend(query_results)
            if args.execution_regressions:
                for query in execution_queries():
                    execute(query)
                    outputs.append(cursor.fetchall())
            if args.assert_fast_plans:
                execute("EXPLAIN SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('全文索引 分词器')")
                plan = '\n'.join(row[0] for row in cursor.fetchall())
                assert 'calc_relevance=false' in plan, plan
                execute("EXPLAIN SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) v")
                plan = '\n'.join(row[0] for row in cursor.fetchall())
                assert 'calc_relevance=false' in plan and 'sort_keys(' not in plan and 'is_index_back=false' in plan, plan
            regression = Path(__file__).resolve().parents[3] / 'tools/deploy/mysql_test/test_suite/fts_index/t/fts_lastwins_regression.test'
            for line in regression.read_text().splitlines():
                if line.lower().startswith('select '):
                    execute(line)
                    outputs.append(cursor.fetchall())
            if args.cache_boundaries:
                for ft_parser in ['ik', 'beng', 'space', 'ngram', 'ngram2']:
                    for collation in ['utf8mb4_bin', 'utf8mb4_general_ci']:
                        for text in [None, '', 'Café CAFÉ 😀 😁', 'a' * 4096, 'a' * 4097]:
                            for properties in ['[]', '[{"output":"all"}]']:
                                execute(f'SELECT tokenize(CONVERT(%s USING utf8mb4) COLLATE {collation},%s,%s)',
                                        (text, ft_parser, properties))
                                outputs.append(cursor.fetchall())
                # A warm entry must not suppress parameter validation errors.
                for sql in ["SELECT tokenize('Alpha','beng','[{\"output\":\"invalid\"}]')",
                            "SELECT tokenize('Alpha','missing_parser')",
                            "SELECT tokenize('Alpha','beng','{}')"]:
                    execute("SELECT tokenize('Alpha','beng')")
                    cursor.fetchall()
                    try:
                        execute(sql)
                    except pymysql.MySQLError as error:
                        outputs.append({'error': list(error.args)})
                    else:
                        raise AssertionError('invalid TOKENIZE arguments succeeded')
    finally:
        connection.close()
    captured = {'outputs': outputs}
    args.output.write_text(json.dumps(captured, ensure_ascii=False, indent=2, default=str))
    if args.compare:
        assert json.loads(args.output.read_text()) == json.loads(args.compare.read_text()), 'FTS SQL outputs differ'
    print(f'{len(outputs)} FTS SQL result sets verified; hits: 8001 / 11000 / 7332 / 20')


if __name__ == '__main__':
    main()
