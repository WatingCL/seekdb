# Exact TOKENIZE result caching

This candidate starts at the user-confirmed 45-point commit
`c3c9eb37b31f033f230bd199ce883e489f7ca5d7`. It does not include the subsequent
DDL word-map reuse or parser-configuration memoization changes from the
approximately 43-point candidate.

## Change and constraints

Repeated TOKENIZE calls rebuilt the token map, JSON tree and binary JSON even
when every input was identical. Cache one successful raw binary JSON result per
native thread, keyed by the complete input bytes, normalized parser/version,
canonical properties, collation and output mode. Evaluate and validate all
SQL arguments first. Copy every retained key and result; copy a cache hit into
the current expression's result memory with its normal LOB packaging.

The cache retains at most 64 bytes of parser name, 1,024 bytes of properties,
4,096 bytes of input and 65,536 bytes of result, plus scalar metadata. Larger
values use the normal computation. Results and errors are never hardcoded.
No index-build, retrieval, benchmark or scoring code changes are included.

This relies on the current builtin-analyzer implementation: `ObFTParser`
accepts only builtin analyzers, English stopwords are fixed builtin data, and
`ObFTRangeDict::build_cache_from_ik_dict()` obtains IK main/quantifier/stopword
data from `ObIKDictLoader::dict_text()`, `dict_quen_text()` and `dict_stop()`.
Supporting mutable or user-defined analyzer dictionaries in future requires
disabling this cache or extending its key with a dictionary generation.

Only raw binary JSON is cached. Caching the already packed expression datum
and calling `pack_json_str_res()` on a hit would add a second LOB header. The
repeated-call regression detected this during development; the delivered code
serializes once before expression-specific packaging on both misses and hits.

## Same-machine measurements

Both binaries were compiled using the existing RelWithDebInfo CMake build.
Two baseline/candidate pairs ran sequentially, with no concurrent compilation,
on disposable local macOS arm64 instances. Each run used the unmodified
`fts_large_bench.sh`: ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200,
SAMPLES=3, WARMUP=30 and SKIP_LOAD=0. Instance configuration was CPU count 4,
memory budget 2 GiB and log disk size 2 GiB.

| Metric | c3c9eb37b mean | Candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.0195 | 4.0340 | -0.36% |
| build_ik_content_sec | 3.0540 | 3.0145 | 1.29% |
| build_beng_en_sec | 1.7475 | 1.7500 | -0.14% |
| tokenize_ik_avg_ms | 0.06800 | 0.04745 | 30.22% |
| tokenize_beng_avg_ms | 0.04525 | 0.03890 | 14.03% |
| query_cn_avg_ms | 0.62890 | 0.61270 | 2.58% |
| query_beng_avg_ms | 0.88235 | 0.85940 | 2.60% |
| query_mixed_avg_ms | 0.69850 | 0.67455 | 3.43% |
| query_limit_avg_ms | 0.15145 | 0.15230 | -0.56% |

The two individual IK reductions were 29.46% and 31.00%; BENG reductions were
14.44% and 13.61%. Build/query code is unchanged and small differences in those
metrics are not attributed to this change. The first baseline SELECT 1 sample
was slower; the second pair had identical SELECT 1 averages of 0.0324 ms.

Both versions already saturate the repository's July fixed-baseline score on
this machine. That cross-environment score cannot predict the actual evaluator
score. The approximately 43-point and 45-point evaluator reports were not
available; an official 50-point result has not been established.

## Correctness

The full server builds successfully. The cache unit test verifies binary NUL
bytes, owned input/result buffers, every key dimension, capacity boundaries,
oversized fallback, replacement, empty strings and independent threads:

```sh
python3 unittest/storage/fts/run_ik_chain_lookup.py --build-dir build_release \
  --tokenize-result-cache
```

The existing SQL comparison runner now supports `--repeat` and optional
`--cache-boundaries`. On the same persisted 20,000-row database, start the
baseline binary, capture results, stop it, then start the candidate and compare:

```sh
python3 unittest/storage/fts/run_fts_sql_comparison.py --port 2887 \
  --execution-regressions --assert-fast-plans --repeat 3 --cache-boundaries \
  --output /tmp/fts-c3-results.json
python3 unittest/storage/fts/run_fts_sql_comparison.py --port 2887 \
  --execution-regressions --assert-fast-plans --repeat 3 --cache-boundaries \
  --output /tmp/fts-candidate-results.json --compare /tmp/fts-c3-results.json
```

All 272 captured results match exactly, including TOKENIZE default/all output,
all five parsers, NULL/empty input, Unicode and collation changes, 4,096/4,097-byte
input, invalid-argument errors, MATCH scores, ordering, boolean mode, LIMIT and
OFFSET. Every SQL executes three times to exercise both initial and repeated
evaluation. Query hit assertions remain 8001 / 11000 / 7332 / 20. No floating
point tolerance is used. Separate freshly loaded databases initially differed
in one BM25 score; restarting both binaries on the same persisted data removed
that difference without weakening the comparison.

Raw benchmark reports, scores, metadata and SQL captures are under
`/tmp/fts-full-score-validation/`. The performance runs are
`c3c9eb37b-baseline`, `raw-json-cache-candidate`, `c3-repeat` and `cache-repeat`.
The earlier `exact-cache-candidate` experiment contained the LOB packaging bug
and is excluded from all delivered performance claims.
