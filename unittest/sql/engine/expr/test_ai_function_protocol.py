#!/usr/bin/env python3
"""SQL/HTTP integration assertions for the AI fixture; run against a disposable DB.

Start ai_function/mock_ai_service.py on --mock-port before running this script.
The --client argument is a MySQL-compatible command-line client, not an SQL engine.
"""
import argparse
import json
import subprocess
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True)
    parser.add_argument('--port', type=int, default=2885)
    parser.add_argument('--mock-port', type=int, default=18000)
    args = parser.parse_args()
    base = 'http://127.0.0.1:%d' % args.mock_port
    client = [args.client, '--no-defaults', '-h127.0.0.1', '-P%d' % args.port,
              '-uroot', '-N', '-s', '--default-character-set=utf8mb4', '-Dtest']
    checks = 0

    def sql(statement, succeeds=True):
        nonlocal checks
        result = subprocess.run(client + ['-e', statement], text=True,
                                capture_output=True, timeout=60)
        if succeeds:
            assert result.returncode == 0, (statement, result.stderr)
        else:
            assert result.returncode != 0, ('expected failure', statement, result.stdout)
        checks += 1
        return result.stdout.strip()

    def stats(reset=False):
        with urllib.request.urlopen(base + ('/reset' if reset else '/'), timeout=5) as response:
            return json.load(response)

    def endpoint(path):
        config = json.dumps({'url': base + path})
        sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('protocol_embed_ep', '%s')" % config)

    try:
        sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('protocol_embed')", False)
    except AssertionError:
        pass
    # Drop order matches the existing suite's supported model/endpoint lifecycle.
    subprocess.run(client + ['-e', "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('protocol_embed_ep')"],
                   capture_output=True, timeout=30)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('protocol_embed', "
        "'{\"type\":\"dense_embedding\",\"model_name\":\"protocol-fixture\"}')")
    config = json.dumps({'ai_model_name': 'protocol_embed', 'scope': 'all',
                         'url': base + '/compatible-mode/v1/embeddings',
                         'access_key': 'mock-key', 'provider': 'openai'})
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('protocol_embed_ep', '%s')" % config)
    sql('DROP TABLE IF EXISTS protocol_embed_rows')
    sql('CREATE TABLE protocol_embed_rows(id INT PRIMARY KEY, content TEXT, dim INT)')
    sql("INSERT INTO protocol_embed_rows VALUES (1,'a',4),(2,NULL,4),(3,'bbb',4),(4,'cc',6)")
    try:
        stats(True)
        result = sql("SELECT id, AI_EMBED('protocol_embed',content,4) FROM protocol_embed_rows ORDER BY id")
        rows = [line.split('\t') for line in result.splitlines()]
        assert [int(row[0]) for row in rows] == [1, 2, 3, 4]
        assert rows[1][1] == 'NULL'
        assert abs(json.loads(rows[0][1])[0] - .031) < 1e-6
        assert abs(json.loads(rows[2][1])[0] - .093) < 1e-6
        assert abs(json.loads(rows[3][1])[0] - .062) < 1e-6
        assert stats()['max_embed_batch'] == 3, stats()

        stats(True)
        assert sql("SELECT JSON_LENGTH(AI_EMBED('protocol_embed',content,4)) "
                   "FROM protocol_embed_rows ORDER BY id") == '4\nNULL\n4\n4'
        assert stats()['max_embed_batch'] == 3, stats()

        stats(True)
        result = sql("SELECT id, JSON_LENGTH(AI_EMBED('protocol_embed',content,dim)) "
                     "FROM protocol_embed_rows ORDER BY id")
        assert result == '1\t4\n2\tNULL\n3\t4\n4\t6', result
        assert stats()['request_count'] == 2, stats()

        stats(True)
        assert sql("SELECT AI_EMBED('protocol_embed',content) FROM protocol_embed_rows WHERE id=2") == 'NULL'
        assert stats()['request_count'] == 0, stats()
        stats(True)
        sql("SELECT id,AI_EMBED('protocol_embed',content,4) FROM protocol_embed_rows WHERE id>2 LIMIT 1")
        assert stats()['max_embed_batch'] == 1, stats()

        for expression in ["''", 'NULL', '0', '-1', '1.5']:
            sql("SELECT AI_EMBED('protocol_embed','a',%s)" % expression, False)
        sql("SELECT AI_EMBED('protocol_embed','')", False)
        sql("SELECT AI_EMBED('protocol_embed',content) FROM protocol_embed_rows WHERE id=1 AND content=''", True)
        sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('protocol_complete', "
            "'{\"type\":\"completion\",\"model_name\":\"protocol-fixture\"}')")
        complete_config = json.dumps({'ai_model_name': 'protocol_complete', 'scope': 'all',
                                     'url': base + '/compatible-mode/v1/chat/completions',
                                     'access_key': 'mock-key', 'provider': 'openai'})
        sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('protocol_complete_ep', '%s')" % complete_config)
        sql("SELECT AI_COMPLETE('protocol_complete', AI_PROMPT('{18446744073709551616}','a'))", False)
        sql("SELECT LENGTH(AI_COMPLETE('protocol_complete','hello',JSON_OBJECT('temperature',0.2,'max_tokens',16)))>0")
        sql("SELECT LENGTH(AI_COMPLETE('protocol_complete',REPEAT('x',50000)))>0")
        sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('protocol_complete')")
        sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('protocol_complete_ep')")
        for fault in ['missing-index', 'duplicate-index', 'invalid-index', 'wrong-count',
                      'empty-vector', 'inconsistent-dimension', 'nonnumeric', 'overflow']:
            endpoint('/fault/%s/embeddings' % fault)
            sql("SELECT AI_EMBED('protocol_embed',content,4) FROM protocol_embed_rows WHERE id IN (1,3)", False)
        # Missing an explicit dimension must still reject empty/inconsistent vectors.
        for fault in ['empty-vector', 'inconsistent-dimension']:
            endpoint('/fault/%s/embeddings' % fault)
            sql("SELECT AI_EMBED('protocol_embed',content) FROM protocol_embed_rows WHERE id IN (1,3)", False)
        endpoint('/compatible-mode/v1/embeddings')
        sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('protocol_rerank', "
            "'{\"type\":\"rerank\",\"model_name\":\"protocol-fixture\"}')")
        rerank_config = json.dumps({'ai_model_name': 'protocol_rerank', 'scope': 'all',
                                   'url': base + '/v1/rerank', 'access_key': 'mock-key',
                                   'provider': 'siliconflow'})
        sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('protocol_rerank_ep', '%s')" % rerank_config)
        try:
            docs = ['doc-%02d' % i for i in range(45)]
            stats(True)
            result = json.loads(sql("SELECT AI_RERANK('protocol_rerank','doc-32','%s')" % json.dumps(docs)))
            expected = [32] + [i for i in range(45) if i != 32]
            assert [item['index'] for item in result] == expected, result
            assert stats()['request_count'] == 3, stats()
            objects = [{'id': 'id-%02d' % i, 'body': value} for i,value in enumerate(docs)]
            stats(True)
            result = json.loads(sql("SELECT AI_RERANK('protocol_rerank','doc-32','%s','body')" % json.dumps(objects)))
            assert [item['id'] for item in result] == ['id-%02d' % i for i in expected], result
            assert stats()['request_count'] == 3, stats()
            for fault in ['wrong-count','duplicate-index','out-of-range','missing-score','invalid-results']:
                config = json.dumps({'url': base + '/fault/%s/rerank' % fault})
                sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('protocol_rerank_ep','%s')" % config)
                sql("SELECT AI_RERANK('protocol_rerank','a','[\"a\",\"b\"]')", False)
                sql("SELECT AI_RERANK('protocol_rerank','a','[{\"id\":1,\"body\":\"a\"},{\"id\":2,\"body\":\"b\"}]','body')", False)
        finally:
            sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('protocol_rerank')")
            sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('protocol_rerank_ep')")
        # Force more than one native rowset and assert every row is mapped.
        sql('TRUNCATE TABLE protocol_embed_rows')
        for start in range(0, 600, 100):
            values = ','.join("(%d,'value-%d',4)" % (i, i) for i in range(start, start+100))
            sql('INSERT INTO protocol_embed_rows VALUES ' + values)
        stats(True)
        rows = sql("SELECT id,JSON_LENGTH(AI_EMBED('protocol_embed',content,4)) "
                   "FROM protocol_embed_rows ORDER BY id").splitlines()
        assert len(rows) == 600 and all(row == '%d\t4' % i for i,row in enumerate(rows))
        assert stats()['max_embed_batch'] > 1 and stats()['request_count'] > 1, stats()
        print('PASS: %d SQL checks plus HTTP batch, mapping, NULL, limit and fault assertions' % checks)
    finally:
        sql('DROP TABLE IF EXISTS protocol_embed_rows')
        sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('protocol_embed')")
        sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('protocol_embed_ep')")


if __name__ == '__main__':
    main()
