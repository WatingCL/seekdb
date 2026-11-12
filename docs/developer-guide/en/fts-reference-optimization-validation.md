# FTS allocation and dictionary lookup optimization

Based on `9e77ea413ba10ae16ccd83d23732b62a181e7f2b`, taking the
normalization/range-search ideas from `/Users/shuai/Downloads/seekdb`
(`b6d4b594b`, especially `614957a84`). Existing parser pools, character
classification, token containers, and retrieval optimizations are retained.

## Changes

- Short normalized tokens use a 256-byte stack buffer. Map keys are copied to
  the parse allocator only when a new key must be retained or collation-equal
  bytes must replace the prior spelling. Long-token conversion retains its
  allocator-backed fallback without copying the result again.
- A new opt-in hash-map operation prepares a retained key under the bucket
  lock only on insertion, using one hash lookup. Existing map operations are
  unchanged. Byte-identical repeats only update the count. Last-occurrence
  spelling, counts, and hash iteration order are preserved.
- IK range memoization is retained. Misses use binary search when both range
  starts and ends are monotonic, checked during construction. The first
  inclusive matching range wins, including overlapping boundaries. Unordered
  ranges retain the original linear fallback and comparison semantics.
- Pooled IK/BENG parsers reuse scratch pages up to 64 KiB per slot. Bigger
  working sets are fully released. All scratch references are dropped before
  recycling, and non-pooled parsers retain their original lifecycle.

The reference's global dictionary sharing was not transplanted: this tree
already retains dictionaries with pooled parsers, and its range memo is mutable
per parser. The reference's unconditional natural-language filter removal was
also not adopted: the existing DAAT path already recognizes an exact relevance
bool cast and applies the corresponding nonzero check without expression
execution. That existing semantic guard is retained.

## Verification

```sh
python3 unittest/storage/fts/run_ik_chain_lookup.py --build-dir build_release
python3 unittest/storage/fts/run_ik_chain_lookup.py --build-dir build_release \
  --reference-optimizations
```

These commands compile both affected complete FTS/IK unity units with the build's
production flags and link an isolated regression executable. Added checks cover
transient token buffers, UTF8MB4 BIN/GENERAL_CI normalization against the old
conversion/map algorithm, both word-map locking flavors, long tokens,
collation-equal last-wins replacements, failed allocations, unordered ranges,
range gaps/boundaries, repeated memo lookups, capped scratch reuse after large
allocations, and concurrent insertion/update of the new map operation.
One thousand repeated short tokens require zero additional retained-key
allocations, versus 1,000 with the original implementation. Long conversion
requires one allocation, with no extra retained-key copy.

For SQL comparison, first load the unmodified benchmark's 20,000-row schema and
indexes. With PyMySQL installed, capture results under the baseline server,
then restart with the candidate and compare (adjust the port as needed):

```sh
python3 unittest/storage/fts/run_fts_sql_comparison.py \
  --port 3881 --output /tmp/fts-before.json
python3 unittest/storage/fts/run_fts_sql_comparison.py \
  --port 3881 --output /tmp/fts-after.json --compare /tmp/fts-before.json
```

A locally linked server with the changed production unity units passed all
143 result-set comparisons with `9e77ea413`: four parsers, default/all output,
Unicode/emoji/fullwidth/long/repeated text, MATCH counts, boolean mode, score
predicates, ordered score projection, and the existing last-wins SQL cases.
Benchmark query counts remain `8001 / 11000 / 7332 / 20`.

## Performance limits

Local measurements use macOS arm64, 20,000 benchmark rows, three samples,
3,000 TOKENIZE statements and 200 statements per query per sample, via PyMySQL.
They are not the official benchmark's client/timing harness or environment.

| Metric (ms) | Baseline | Candidate |
| --- | ---: | ---: |
| IK tokenize | 0.079038 | 0.077960 |
| BENG tokenize | 0.062123 | 0.059715 |
| Chinese query | 0.937119 | 0.960070 |
| English query | 1.408458 | 1.401480 |
| Mixed query | 1.066476 | 1.069319 |
| LIMIT query | 1.323447 | 1.338356 |

These samples show small tokenization reductions (about 1.4% and 3.9%), with
small mixed changes in the unchanged query path. Index builds completed for all
three indexes; individual local build timings are not sufficient to establish
an improvement. Reduced allocations and bounded page retention are verified,
but a substantial end-to-end speedup or official score increase is not claimed.
A final ownership-only adjustment to the long-token fallback was checked with
the regression suite and the full SQL corpus after these timing samples.
No benchmark inputs, scoring rules, baselines, or SQL timing logic were changed.
