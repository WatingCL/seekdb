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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2881)
    parser.add_argument('--user', default='root')
    parser.add_argument('--database', default='fts_large_bench')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--compare', type=Path)
    args = parser.parse_args()
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
            for ft_parser in ['ik', 'beng', 'space', 'ngram']:
                for text in texts:
                    for properties in ['[]', '[{"output":"all"}]']:
                        cursor.execute('SELECT tokenize(%s,%s,%s) AS result', (text, ft_parser, properties))
                        outputs.append(cursor.fetchall())
            query_results = []
            for query in queries:
                cursor.execute(query)
                query_results.append(cursor.fetchall())
            assert [rows[0][0] for rows in query_results[:4]] == [8001, 11000, 7332, 20]
            outputs.extend(query_results)
            regression = Path(__file__).resolve().parents[3] / 'tools/deploy/mysql_test/test_suite/fts_index/t/fts_lastwins_regression.test'
            for line in regression.read_text().splitlines():
                if line.lower().startswith('select '):
                    cursor.execute(line)
                    outputs.append(cursor.fetchall())
    finally:
        connection.close()
    captured = {'outputs': outputs}
    args.output.write_text(json.dumps(captured, ensure_ascii=False, indent=2))
    if args.compare:
        assert json.loads(args.output.read_text()) == json.loads(args.compare.read_text()), 'FTS SQL outputs differ'
    print(f'{len(outputs)} FTS SQL result sets verified; hits: 8001 / 11000 / 7332 / 20')


if __name__ == '__main__':
    main()
