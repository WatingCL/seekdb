#!/usr/bin/env bash
# seekdb x AMD hipVS contest benchmark — single configuration runner.
#
# Measures, for ONE (backend, env-config) combination, the exact metrics the
# contest scores:
#
#   build window   DDL start -> first APPROXIMATE query returns  (headline)
#                  plus a decomposition parsed from observer.log
#                  (build_us / graph_us / stream_us, and with OB_CUVS_DEBUG=1
#                  the bridge-side phase timings)
#   query          single-connection QPS over nq evaluation statements,
#                  batch-level latency percentiles (8 equal batches)
#   recall         recall@10 / recall@100 vs exact brute-force ground truth
#   vram           peak device memory sampled at 10 Hz across the whole run
#   gpu gates      "serialized GPU CAGRA snapshot built=true" in observer.log
#                  and (if compiled with OB_BUILD_CUVS_TRACE) cuvs_build /
#                  cuvs_serve markers in the trace file
#
# All OB_CUVS_* variables present in the caller environment are passed through
# to the observer, so configs are selected purely by env (no rebuild). With no
# OB_CUVS_* set the bridge reproduces the frozen V5 baseline behaviour.
#
# Usage:
#   bench.sh --observer PATH --bridge PATH --dataset-dir PATH \
#            --evidence DIR --label NAME [--backend cuvs|vsag] \
#            [--port 2881] [--base-dir DIR] [--keep] [--results-csv PATH]
#
# Example (baseline GPU config):
#   ./bench.sh --observer ./build/observer --bridge ./libseekdb_cuvs_bridge.so \
#             --dataset-dir /data/sift250k --evidence /runs/base --label base
#
# Example (candidate config, same binary):
#   OB_CUVS_NN_ITERS=40 OB_CUVS_DEBUG=1 \
#   ./bench.sh --observer ./build/observer --bridge ./libseekdb_cuvs_bridge.so \
#             --dataset-dir /data/sift250k --evidence /runs/niter40 --label niter40
set -Eeuo pipefail

OBSERVER=""; BRIDGE=""; DATASET=""; EVIDENCE=""; LABEL=""
BACKEND="cuvs"; PORT=2881; BASE_DIR=""; KEEP=0; RESULTS_CSV=""
while (($# > 0)); do
  case "$1" in
    --observer) OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --dataset-dir) DATASET=$2; shift 2 ;;
    --evidence) EVIDENCE=$2; shift 2 ;;
    --label) LABEL=$2; shift 2 ;;
    --backend) BACKEND=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --base-dir) BASE_DIR=$2; shift 2 ;;
    --keep) KEEP=1; shift ;;
    --results-csv) RESULTS_CSV=$2; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -x "$OBSERVER" ]] || { echo "observer is not executable: $OBSERVER" >&2; exit 2; }
[[ -f "$BRIDGE" ]] || { echo "bridge does not exist: $BRIDGE" >&2; exit 2; }
[[ -f "$DATASET/meta.json" ]] || { echo "dataset meta.json missing under $DATASET" >&2; exit 2; }
[[ -f "$DATASET/base.tsv" && -f "$DATASET/queries.sql" && -f "$DATASET/queries100.sql" ]] || {
  echo "dataset files missing under $DATASET (run gen_dataset.py)" >&2; exit 2; }
[[ -f "$DATASET/gt10.i32" && -f "$DATASET/gt100.i32" ]] || { echo "ground truth missing" >&2; exit 2; }
[[ "$BACKEND" == "cuvs" || "$BACKEND" == "vsag" ]] || { echo "backend must be cuvs|vsag" >&2; exit 2; }
[[ "$PORT" =~ ^[0-9]+$ ]] || { echo "invalid port" >&2; exit 2; }
[[ -n "$EVIDENCE" && "$EVIDENCE" != "/" ]] || { echo "unsafe evidence path" >&2; exit 2; }
command -v mysql >/dev/null || { echo "mysql client not found" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 2; }
python3 -c 'import numpy' 2>/dev/null || { echo "python3 numpy not found" >&2; exit 2; }

[[ -n "$BASE_DIR" ]] || BASE_DIR=$EVIDENCE/run
RUN_DIR=$BASE_DIR
LOG=$EVIDENCE/observer.log
TRACE=$EVIDENCE/obvsag-trace.log
mkdir -p "$EVIDENCE"
rm -rf "$RUN_DIR"; mkdir -p "$RUN_DIR"

N=$(python3 -c "import json;print(json.load(open('$DATASET/meta.json'))['n'])")
DIM=$(python3 -c "import json;print(json.load(open('$DATASET/meta.json'))['dim'])")
NQ=$(python3 -c "import json;print(json.load(open('$DATASET/meta.json'))['nq'])")
DSNAME=$(python3 -c "import json;print(json.load(open('$DATASET/meta.json'))['dataset'])")
PROBE=$(cat "$DATASET/probe0.txt")

if [[ "$BACKEND" == "cuvs" ]]; then
  DDL_SQL="create vector index idx_items on items(v) with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);"
else
  DDL_SQL="create vector index idx_items on items(v) with (distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=64);"
fi

mysql_cmd() {
  local port=$1; shift
  local -a args=()
  # every -e statement gets the long session query timeout (the 250K-row DDL
  # build far exceeds OceanBase's 10s default ob_query_timeout)
  while (($#)); do
    if [[ "$1" == "-e" ]]; then
      args+=(-e "set ob_query_timeout=600000000; $2"); shift 2
    else
      args+=("$1"); shift
    fi
  done
  timeout 900 mysql --connect-timeout=5 --protocol=TCP -h127.0.0.1 -P"$port" \
    -uroot --local-infile=1 -A -N -B "${args[@]}"
}

now_s() { date +%s.%N; }

start_observer() {
  local -a env_args=(
    "LD_LIBRARY_PATH=$(dirname "$BRIDGE"):/opt/hipvs/lib:/opt/rocm/lib"
    "OB_VSAG_TRACE=1"
    "OB_VSAG_TRACE_FILE=$TRACE"
  )
  # pass through every OB_CUVS_* knob from the caller environment
  local v
  for v in $(compgen -e | grep '^OB_CUVS_' || true); do
    env_args+=("$v=${!v}")
  done
  ulimit -n 65535
  env -u OB_VSAG_USE_CUVS "${env_args[@]}" "$OBSERVER" \
    --nodaemon --port "$PORT" \
    --data-dir "$RUN_DIR/store" --base-dir "$RUN_DIR" \
    --parameter __min_full_resource_pool_memory=2147483648 \
    --parameter memory_budget=4G \
    --parameter datafile_size=40G \
    --parameter cpu_count=24 \
    --parameter local_ip=127.0.0.1 \
    >"$LOG" 2>&1 &
  OBS_PID=$!
  local deadline=$((SECONDS + 300))
  until mysql_cmd "$PORT" -e 'select 1' >/dev/null 2>&1; do
    if ! [[ -d "/proc/$OBS_PID" ]]; then
      echo "observer died during startup; see $LOG" >&2; tail -40 "$LOG" >&2; exit 1
    fi
    ((SECONDS < deadline)) || { echo "observer not SQL-ready after 300s" >&2; exit 1; }
    sleep 1
  done
}

stop_observer() {
  [[ -n "${OBS_PID:-}" && -d "/proc/$OBS_PID" ]] || return 0
  local state
  state=$(awk '/^State:/{print $2}' "/proc/$OBS_PID/status" 2>/dev/null || true)
  [[ "$state" == "Z" ]] && return 0
  kill -TERM "$OBS_PID" 2>/dev/null || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    [[ ! -d "/proc/$OBS_PID" ]] && break
    state=$(awk '/^State:/{print $2}' "/proc/$OBS_PID/status" 2>/dev/null || true)
    [[ "$state" == "Z" ]] && break
    sleep 1
  done
  [[ -d "/proc/$OBS_PID" ]] && kill -KILL "$OBS_PID" 2>/dev/null || true
  wait "$OBS_PID" 2>/dev/null || true
}
trap 'stop_observer' EXIT

# ---------------------------------------------------------------- vram sampler
VRAM_CSV=$EVIDENCE/vram.csv
sample_vram() {
  while true; do
    local max=0 v f
    for f in /sys/class/drm/card*/device/mem_info_vram_used; do
      [[ -r "$f" ]] || continue
      v=$(cat "$f" 2>/dev/null || echo 0)
      [[ "$v" =~ ^[0-9]+$ ]] || v=0
      (( v > max )) && max=$v
    done
    printf '%s %s\n' "$(date +%s.%N)" "$max" >> "$VRAM_CSV"
    sleep 0.1
  done
}

echo "=== bench label=$LABEL backend=$BACKEND dataset=$DSNAME n=$N dim=$DIM nq=$NQ"
echo "=== env: $(compgen -e | grep '^OB_CUVS_' | tr '\n' ' ')(empty = baseline defaults)"

start_observer

mysql_cmd "$PORT" -e "set ob_query_timeout=300000000;
  alter system set ob_vector_memory_limit_percentage=30;
  create database if not exists bench;
  use bench; drop table if exists items;
  create table items(id bigint primary key, v vector($DIM));" >/dev/null

echo "=== loading $N rows from base.tsv (untimed)"
LOAD_T0=$(now_s)
mysql_cmd "$PORT" -e "use bench;
  load data local infile '$DATASET/base.tsv' into table items
  fields terminated by '\t' lines terminated by '\n' (id,v);" >/dev/null
LOAD_T1=$(now_s)
CNT=$(mysql_cmd "$PORT" -e 'use bench; select count(*) from items')
[[ "$CNT" == "$N" ]] || { echo "row count mismatch: $CNT != $N" >&2; exit 1; }

: > "$VRAM_CSV"
sample_vram & SAMPLER_PID=$!

# ------------------------------------------------- timed window: DDL -> first query
T0=$(now_s)
mysql_cmd "$PORT" -e "use bench; $DDL_SQL" >/dev/null
T1=$(now_s)
mysql_cmd "$PORT" -e "use bench; select id from items order by l2_distance(v,'$PROBE') approximate limit 10" > "$EVIDENCE/first.ids"
T2=$(now_s)
FIRST_ROWS=$(wc -l < "$EVIDENCE/first.ids")
[[ "$FIRST_ROWS" -eq 10 ]] || { echo "first query returned $FIRST_ROWS rows" >&2; exit 1; }

kill "$SAMPLER_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true
VRAM_PEAK=$(awk '{if ($2>m) m=$2} END {print m+0}' "$VRAM_CSV")

# ------------------------------------------------- query throughput + latency
QT0=$(now_s)
mysql_cmd "$PORT" < "$DATASET/queries.sql" > "$EVIDENCE/queries10.out"
QT1=$(now_s)
QPS=$(python3 -c "print(f'{$NQ / ($QT1 - $QT0):.2f}')")

# batch-level latency: 8 equal batches through one connection
grep -v '^use ' "$DATASET/queries.sql" | grep -v '^set ob_query_timeout' | grep -v '^$' > "$EVIDENCE/q10.body.sql"
split -n l/8 -d "$EVIDENCE/q10.body.sql" "$EVIDENCE/q10.batch."
BATCH_TSV=$EVIDENCE/batch.tsv
: > "$BATCH_TSV"
for b in "$EVIDENCE"/q10.batch.*; do
  { echo 'use bench;'; echo 'set ob_query_timeout=600000000;'; cat "$b"; } > "$b.sql"
  b0=$(now_s)
  mysql_cmd "$PORT" < "$b.sql" > /dev/null
  b1=$(now_s)
  rows=$(grep -c ';' "$b" || true)
  per_ms=$(python3 -c "print(f'{($b1 - $b0) * 1000 / $rows:.4f}')")
  printf '%s\t%s\n' "$rows" "$per_ms" >> "$BATCH_TSV"
done

# ------------------------------------------------- recall@10 / recall@100
mysql_cmd "$PORT" < "$DATASET/queries100.sql" > "$EVIDENCE/queries100.out"
python3 - "$EVIDENCE" "$DATASET" "$NQ" <<'PY'
import json, pathlib, sys
import numpy as np
ev, ds, nq = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])

def recall(out_name, gt_name, k):
    got = np.fromiter((int(x) for x in open(ev / out_name)), dtype=np.int64,
                      count=nq * k).reshape(nq, k)
    gt = np.fromfile(ds / gt_name, dtype=np.int32).reshape(nq, k)
    hits = sum(len(set(got[i].tolist()) & set(gt[i].tolist())) for i in range(nq))
    return hits / (nq * k)

r10 = recall("queries10.out", "gt10.i32", 10)
r100 = recall("queries100.out", "gt100.i32", 100)
(ev / "recall.json").write_text(json.dumps({"recall@10": r10, "recall@100": r100}, indent=2) + "\n")
print(f"recall@10={r10:.4f} recall@100={r100:.4f}")
PY

# ------------------------------------------------- log-derived decomposition
BUILD_US=$(grep -a 'serialized GPU CAGRA snapshot' "$LOG" | tail -1 | grep -o 'build_us=[0-9]*' | cut -d= -f2 || echo 0)
GRAPH_US=$(grep -a 'serialized GPU CAGRA snapshot' "$LOG" | tail -1 | grep -o 'graph_us=[0-9]*' | cut -d= -f2 || echo 0)
STREAM_US=$(grep -a 'serialized GPU CAGRA snapshot' "$LOG" | tail -1 | grep -o 'stream_us=[0-9]*' | cut -d= -f2 || echo 0)
GRAPH_BYTES=$(grep -a 'serialized GPU CAGRA snapshot' "$LOG" | tail -1 | grep -o 'graph_bytes=[0-9]*' | cut -d= -f2 || echo 0)
BUILT_TRUE=$(grep -a 'serialized GPU CAGRA snapshot' "$LOG" | tail -1 | grep -c 'built=true' || true)
count_trace() {  # count_trace MARKER FILE -> integer (never multiline)
  local n
  n=$(grep -ac "$1" "$2" 2>/dev/null || true)
  [[ "$n" =~ ^[0-9]+$ ]] || n=0
  echo "$n"
}
TRACE_BUILD=$(count_trace '^cuvs_build' "$TRACE")
TRACE_SERVE=$(count_trace '^cuvs_serve' "$TRACE")
DBG_BUILD_MS=$(grep -a '\[seekdb_cuvs\] build n=' "$LOG" | tail -1 | grep -o '([0-9.]* ms)' | tr -dc '0-9.' || echo "")

TOTAL=$(python3 -c "print(f'{$T2 - $T0:.3f}')")
DDL_S=$(python3 -c "print(f'{$T1 - $T0:.3f}')")
FIRSTQ_S=$(python3 -c "print(f'{$T2 - $T1:.3f}')")
LOAD_S=$(python3 -c "print(f'{$LOAD_T1 - $LOAD_T0:.1f}')")

python3 - "$EVIDENCE/result.json" <<PY
import json, os, sys
stats = {"p50_ms": 0.0, "p95_ms": 0.0, "max_ms": 0.0}
try:
    rows = [float(l.split("\t")[1]) for l in open("$BATCH_TSV") if l.strip()]
    rows.sort()
    stats = {"p50_ms": rows[len(rows)//2], "p95_ms": rows[int(len(rows)*0.95)-1 if len(rows) > 1 else 0], "max_ms": rows[-1]}
except Exception:
    pass
result = {
    "label": "$LABEL", "backend": "$BACKEND", "dataset": "$DSNAME",
    "n": $N, "dim": $DIM, "nq": $NQ,
    "env": {k: v for k, v in os.environ.items() if k.startswith("OB_CUVS_")},
    "load_s": $LOAD_S,
    "ddl_to_first_query_s": $TOTAL,
    "ddl_s": $DDL_S,
    "first_query_s": $FIRSTQ_S,
    "build_us": ${BUILD_US:-0}, "graph_us": ${GRAPH_US:-0}, "stream_us": ${STREAM_US:-0},
    "graph_bytes": ${GRAPH_BYTES:-0},
    "bridge_build_ms": "${DBG_BUILD_MS}",
    "qps_single_conn": $QPS,
    "batch_latency_ms": stats,
    "vram_peak_bytes": $VRAM_PEAK,
    "gpu_build_log": $BUILT_TRUE,
    "trace_cuvs_build": $TRACE_BUILD,
    "trace_cuvs_serve": $TRACE_SERVE,
}
json.dump(result, open(sys.argv[1], "w"), indent=2)
print(json.dumps(result, indent=2))
PY

if [[ -n "$RESULTS_CSV" ]]; then
  read -r P50 P95 PMAX < <(python3 -c "
import json; r=json.load(open('$EVIDENCE/result.json'))['batch_latency_ms']
print(r['p50_ms'], r['p95_ms'], r['max_ms'])")
  R10=$(python3 -c "import json;print(json.load(open('$EVIDENCE/recall.json'))['recall@10'])")
  R100=$(python3 -c "import json;print(json.load(open('$EVIDENCE/recall.json'))['recall@100'])")
  if [[ ! -f "$RESULTS_CSV" ]]; then
    echo "label,backend,dataset,n,dim,nq,load_s,ddl_to_first_query_s,ddl_s,first_query_s,build_us,graph_us,stream_us,graph_bytes,bridge_build_ms,qps,p50_ms,p95_ms,max_ms,recall10,recall100,vram_peak_bytes,gpu_build_log,trace_build,trace_serve" > "$RESULTS_CSV"
  fi
  echo "$LABEL,$BACKEND,$DSNAME,$N,$DIM,$NQ,$LOAD_S,$TOTAL,$DDL_S,$FIRSTQ_S,${BUILD_US:-0},${GRAPH_US:-0},${STREAM_US:-0},${GRAPH_BYTES:-0},${DBG_BUILD_MS:-},$QPS,$P50,$P95,$PMAX,$R10,$R100,$VRAM_PEAK,$BUILT_TRUE,$TRACE_BUILD,$TRACE_SERVE" >> "$RESULTS_CSV"
fi

stop_observer
trap - EXIT
[[ "$KEEP" == "1" ]] || rm -rf "$RUN_DIR"
echo "=== bench done: $EVIDENCE/result.json"
