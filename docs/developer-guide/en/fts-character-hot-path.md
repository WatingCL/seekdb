# IK character hot-path optimization

IK classifies every input character during segmentation and again when resolving
single-character output. Previously, each classification predicate independently
decoded the same UTF character. A Chinese character could be decoded three times;
a separator could reach four decoding predicates. Classification now decodes once
and applies the original Unicode predicates in their original order. In
particular, full-width letters and digits still take precedence over the broad
CJK block, and UTF-8 does not acquire UTF-16 surrogate classification.

Chinese numeral recognition also previously traversed a runtime-initialized array
for every candidate Chinese character. Its membership is now represented by a
switch over exactly the same code points, without UTF-8 literal decoding or a
local-static initialization guard in that hot path. No dictionary or tokenizer
rule changes are involved.

## Correctness checks

The native differential test compares the optimized classifier with the original
sequence of predicates for every UTF-8-encoded code point from 0 through
0x10ffff, all 65,536 two-byte inputs for each UTF-16 byte order, and explicit
NULL, empty, malformed and truncated inputs. It independently checks Chinese
numeral membership over all 1,114,112 code points. The templates are tested even
when the local build does not register UTF-16 SQL collations.

Using an existing, idle CMake build (libraries must not be concurrently rebuilt):

```sh
python3 unittest/storage/fts/run_ik_char_classification.py \
  --build-dir /absolute/path/to/build_release --benchmark
```

The runner compiles only the native test and Unicode utility translation unit,
using the existing SDK include paths and runtime libraries. It does not modify
or rebuild the server. The benchmark is optional and is not the competition
score. One Apple Silicon run of six balanced AB/BA samples, each classifying the
same mixed-language text 200,000 times, measured a 159.045 ms median for the
original classification sequence versus 146.627 ms for single decoding (7.81%
less time). This only measures the classification helper, not total TOKENIZE,
index build or MATCH performance; it does not measure the numeral-switch benefit.

The SQL regression `fts_index.ik_character_classification` records original-server
TOKENIZE results for ASCII, full-width forms, Chinese numerals, Japanese/Korean,
connectors, supplementary characters, repeated-token frequencies and empty input.
Run the same case against the optimized server with the repository's mysqltest
runner. Expected results were recorded before applying the FTS code changes.

## Full performance comparison

Keep an unmodified binary and an optimized binary built with identical flags.
Run them sequentially with the same server configuration, a dedicated disposable
instance, and no concurrent compilation or background workloads. For each binary,
run the existing `tools/benchmark/fts_large_bench.sh` with the fixed baseline
settings: ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3,
WARMUP=30 and SKIP_LOAD=0. Save a fresh report file for each run and compare all
nine scoring metrics, plus query results. Do not modify the benchmark inputs,
SQL, timing, scoring script or baseline.

On macOS the unmodified shell benchmark requires GNU Bash (associative arrays)
and GNU date (nanosecond timestamps). For Homebrew installations, invoke
`/opt/homebrew/bin/bash` and prepend
`/opt/homebrew/opt/coreutils/libexec/gnubin` to PATH. Full build/query benchmark
scores are not inferred from the helper microbenchmark and must be reported
separately after running the actual workload.

## BENG already-normalized ASCII

The English normalizer previously allocated a lowercase copy for every token,
including an already-lowercase ASCII token. For UTF-8 only, a token containing no
non-ASCII bytes and no uppercase ASCII now borrows its existing document slice.
Any uppercase or non-ASCII token uses the original charset conversion. The BENG
parser still copies returned tokens into its caller's allocator before advancing
the stream, and the grouping normalizer still deep-copies new keys. Consequently,
retained map keys do not borrow the normalizer's temporary memory or change their
lifetime. No case-folding or punctuation rule changes are introduced.

The native English test compares outputs with the original trim-and-lowercase
algorithm for both UTF-8 collations, mixed-case text, non-ASCII characters,
punctuation, long tokens and mixed borrowed/owned tokens:

```sh
python3 unittest/storage/fts/run_ik_char_classification.py \
  --build-dir /absolute/path/to/build_release --english
```

The SQL regression `fts_index.beng_ascii_normalization` additionally checks
retained-token frequencies, including 200 repeated mixed-case tokens. Its
expected result was recorded on the original server. The fast path removes one
normalization allocation/copy for eligible tokens; its complete server-level
performance effect must be measured with the original benchmark described above.
