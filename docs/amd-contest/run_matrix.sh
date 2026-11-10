#!/usr/bin/env bash
# seekdb x AMD hipVS contest benchmark — configuration sweep.
#
# Runs bench.sh once per configuration (each in a fresh observer process, so
# the OB_CUVS_* env knobs take effect), then prints a comparison table.
# No rebuild is ever needed: the bridge reads all knobs from the environment.
#
# Usage:
#   run_matrix.sh --observer PATH --bridge PATH --dataset-dir PATH \
#                 --evidence-root DIR [--backend cuvs|vsag] [--port 2881] \
#                 [--configs "base,niter40,..."]
set -Eeuo pipefail

OBSERVER=""; BRIDGE=""; DATASET=""; EVIDENCE_ROOT=""; BACKEND="cuvs"; PORT=2881
CONFIGS="base,niter40,niter20,niter20-deg96,inter192,ivfpq,itopk1024,width8"
while (($# > 0)); do
  case "$1" in
    --observer) OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --dataset-dir) DATASET=$2; shift 2 ;;
    --evidence-root) EVIDENCE_ROOT=$2; shift 2 ;;
    --backend) BACKEND=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --configs) CONFIGS=$2; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[[ -x "$OBSERVER" ]] || { echo "observer is not executable: $OBSERVER" >&2; exit 2; }
[[ -f "$BRIDGE" ]] || { echo "bridge does not exist: $BRIDGE" >&2; exit 2; }
[[ -d "$DATASET" ]] || { echo "dataset dir missing: $DATASET" >&2; exit 2; }
[[ -n "$EVIDENCE_ROOT" && "$EVIDENCE_ROOT" != "/" ]] || { echo "unsafe evidence-root" >&2; exit 2; }
mkdir -p "$EVIDENCE_ROOT"
RESULTS=$EVIDENCE_ROOT/results.csv

# config -> env assignment (empty means frozen-baseline defaults)
cfg_env() {
  case "$1" in
    base)         echo "" ;;
    niter40)      echo "OB_CUVS_NN_ITERS=40" ;;
    niter20)      echo "OB_CUVS_NN_ITERS=20" ;;
    niter20-deg96) echo "OB_CUVS_NN_ITERS=20 OB_CUVS_GRAPH_DEGREE=96 OB_CUVS_INTER_DEGREE=192" ;;
    inter192)     echo "OB_CUVS_INTER_DEGREE=192" ;;
    ivfpq)        echo "OB_CUVS_BUILD_ALGO=1 OB_CUVS_IVF_N_PROBES=64 OB_CUVS_IVF_REFINE_RATE=2.0" ;;
    itopk1024)    echo "OB_CUVS_ITOPK=1024" ;;
    width8)       echo "OB_CUVS_SEARCH_WIDTH=8" ;;
    *) echo "" ;;
  esac
}

IFS=',' read -ra WANT <<< "$CONFIGS"
for name in "${WANT[@]}"; do
  envs=$(cfg_env "$name")
  echo "##############################################################"
  echo "### config=$name backend=$BACKEND env=[$envs]"
  echo "##############################################################"
  # shellcheck disable=SC2086
  env $envs "$HERE/bench.sh" \
    --observer "$OBSERVER" --bridge "$BRIDGE" \
    --dataset-dir "$DATASET" \
    --evidence "$EVIDENCE_ROOT/$name" --label "$name" \
    --backend "$BACKEND" --port "$PORT" \
    --base-dir "$EVIDENCE_ROOT/$name/run" \
    --results-csv "$RESULTS"
done

echo
echo "==================== summary ===================="
python3 - "$RESULTS" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    raise SystemExit("no results")
base = next((r for r in rows if r["label"] == "base"), rows[0])
b_total = float(base["ddl_to_first_query_s"])
b_qps = float(base["qps"])
print(f"{'config':<14}{'build_s':>9}{'x_base':>8}{'qps':>9}{'x_base':>8}"
      f"{'r@10':>7}{'r@100':>7}{'vram_GB':>9}{'gpu':>5}")
for r in rows:
    total = float(r["ddl_to_first_query_s"]); qps = float(r["qps"])
    print(f"{r['label']:<14}{total:>9.2f}{b_total/total:>8.2f}{qps:>9.1f}{qps/b_qps:>8.2f}"
          f"{float(r['recall10']):>7.4f}{float(r['recall100']):>7.4f}"
          f"{int(r['vram_peak_bytes'])/2**30:>9.2f}{r['gpu_build_log']:>5}")
print("\nrecall gates (contest): sift@10>=0.95 sift@100>=0.95 | hidden@10>=0.90 hidden@100>=0.85")
print("qps gate: >= 0.9x baseline. vram gate: <= 1.25x baseline.")
PY
