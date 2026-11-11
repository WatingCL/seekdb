# IK chain lookup validation

This change starts from `8e972d7a83bbcb1023c5ee6a4c4440d9258f6ef4`.
It replaces per-document arbitration hash buckets/nodes with a sorted array
containing 16 inline entries. Production chains arrive in offset order; lookup
is binary search. Out-of-order insertion and duplicate-key behavior are retained.
Chinese numeral classification uses constant code points instead of a dynamically
initialized UTF-8-decoded array and a linear scan.

## Reproduce

With an existing CMake unity build and its runtime libraries:

```sh
python3 unittest/storage/fts/run_ik_chain_lookup.py --build-dir build_release --benchmark
python3 unittest/storage/fts/run_ik_chain_lookup.py --build-dir build_release --benchmark \
  --baseline-ref 8e972d7a83bbcb1023c5ee6a4c4440d9258f6ef4
```

The runner recompiles both complete FTS/IK unity translation units using the
build's compiler flags, then links an isolated test executable. It leaves the
server executable and existing build artifacts unchanged. The baseline option
loads the original arbitrator and Unicode sources/headers into a temporary
include overlay.

Coverage includes random lookup operations against `std::map`, ordered inserts,
duplicates, missing keys, extreme offsets, inline storage overflow, allocation
failure, reset/reuse, and exhaustive numeral membership over U+0000–U+10FFFF.
The arbitration path checks 64 separate chains in both smart modes and emits a
deterministic checksum for 200 overlapping-token documents. Both revisions
produce `3054835577729102278`, retained as a regression expectation.

## Local results (macOS arm64, 2026-11-12)

Both production unity units compile with the existing build flags and regression
checks pass. The synthetic benchmark processes 600,000 documents, each with
64 two-character tokens and alternating smart/non-smart mode:

| Revision | Seconds |
| --- | ---: |
| Original baseline | 3.623180 |
| Sorted chain lookup | 2.999321 |

This run reduced synthetic arbitration elapsed time by approximately 17.2%.
It is not a SQL, index-build, or competition benchmark. Official score changes
require rerunning the evaluator; no new official score is claimed.

The supplied evaluator log reports `fts_raw_score=32.76524536192831` and
`fts_mean_improvement=0.16382622680964154` (16.38 when scaling to 50 points).
The checked-in July CI baseline would saturate at full score for those supplied
metrics, so it does not reproduce the evaluator's baseline.
