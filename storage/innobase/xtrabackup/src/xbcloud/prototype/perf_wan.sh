#!/usr/bin/env bash
#
# PXB-3671 prototype: WAN-latency repro of the multipart-vs-legacy
# performance regression the user saw against real AWS S3.
#
# Sets up fault_proxy with a per-request --delay-ms (default 100) to
# simulate WAN RTT between xbcloud and the upstream. Runs the same
# xbstream payload twice:
#
#   1. --multipart-upload=OFF  (legacy chunk-per-PUT)
#   2. --multipart-upload=ON   (new code path)
#
# Both runs use --http-timing so we get aggregated DNS / CONNECT / TLS
# / TOTAL numbers at the end. Wall times are timed too. The output
# makes the regression cause obvious (e.g. CONNECT > 0 on every sync
# call => connection cache not being reused).
#
# Run from this directory.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BLD="${BLD:-$HERE/../../../../../../bld}"
RUNDIR="$BLD/runtime_output_directory"
XBCLOUD="$RUNDIR/xbcloud"
XBSTREAM="$RUNDIR/xbstream"
[ -x "$XBCLOUD" ] || { echo "xbcloud not found at $XBCLOUD"; exit 2; }
[ -x "$XBSTREAM" ] || { echo "xbstream not found at $XBSTREAM"; exit 2; }

DELAY_MS="${DELAY_MS:-100}"
PROXY_PORT="${PROXY_PORT:-4569}"
N_FILES="${N_FILES:-40}"        # many small files = worst case for sync round-trips
FILE_MIB="${FILE_MIB:-1}"       # each file is tiny -- triggers small-file path
BIG_FILE_MIB="${BIG_FILE_MIB:-100}"  # one larger file to exercise multipart parts

export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_DEFAULT_REGION=us-east-1
BUCKET="pxb3671-perf"
ENDPOINT_DIRECT="http://localhost:4566"
ENDPOINT_PROXY="http://localhost:${PROXY_PORT}"

echo "==> Bringing up LocalStack"
docker-compose -f "$HERE/docker-compose.yml" up -d localstack >/dev/null
for i in $(seq 1 30); do
  if curl -sf "$ENDPOINT_DIRECT/_localstack/health" | grep -q '"s3": "available"'; then
    break
  fi
  sleep 1
done

WORK="$(mktemp -d)"
trap 'kill $PROXY_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "==> Starting fault_proxy on :$PROXY_PORT (delay=${DELAY_MS}ms)"
python3 "$HERE/fault_proxy.py" \
  --listen-port "$PROXY_PORT" \
  --upstream "$ENDPOINT_DIRECT" \
  --fault-rate 0 \
  --delay-ms "$DELAY_MS" \
  >"$WORK/proxy.log" 2>&1 &
PROXY_PID=$!
sleep 1
if ! kill -0 "$PROXY_PID" 2>/dev/null; then
  echo "FAIL: fault_proxy did not start"; cat "$WORK/proxy.log"; exit 1
fi

echo "==> Generating test input"
mkdir -p "$WORK/src"
# N small files to stress the sync per-file control plane.
for i in $(seq 1 "$N_FILES"); do
  dd if=/dev/urandom of="$WORK/src/small_${i}.dat" \
     bs=1M count="$FILE_MIB" status=none
done
# One bigger file to exercise the multipart parts path under latency.
dd if=/dev/urandom of="$WORK/src/big.dat" bs=1M count="$BIG_FILE_MIB" \
   status=none
echo "synthetic" > "$WORK/src/xtrabackup_tablespaces"

run_one() {
  local label="$1" mp_flag="$2"
  echo
  echo "============================================================"
  echo "==> RUN: $label  ($mp_flag)"
  echo "============================================================"
  local prefix="perf-${label}/$(date +%s)"
  aws --endpoint-url="$ENDPOINT_DIRECT" s3 rb "s3://$BUCKET" --force \
    >/dev/null 2>&1 || true
  aws --endpoint-url="$ENDPOINT_DIRECT" s3 mb "s3://$BUCKET" >/dev/null

  local log="$WORK/${label}.log"
  cd "$WORK/src"
  local start_ns=$(date +%s%N)
  set +e
  "$XBSTREAM" -c \
      $(ls small_*.dat) big.dat xtrabackup_tablespaces | \
    "$XBCLOUD" put \
      --storage=s3 \
      --s3-endpoint="$ENDPOINT_PROXY" \
      --s3-bucket="$BUCKET" \
      --s3-access-key=test \
      --s3-secret-key=test \
      --s3-region=us-east-1 \
      --s3-bucket-lookup=path \
      "$mp_flag" \
      --parallel=8 \
      --http-timing \
      "$prefix" >"$log" 2>&1
  local exit_rc=$?
  set -e
  local end_ns=$(date +%s%N)
  local elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

  echo "exit=$exit_rc wall=${elapsed_ms} ms"
  echo "--- HTTP timing summary ---"
  grep -E "sync HTTP timing summary|sync (GET|POST|PUT|DELETE|HEAD)" "$log" || \
    echo "(no http_timing output captured)"
  echo "---------------------------"
  # Stash for the table.
  printf "%s\t%s\n" "$label" "$elapsed_ms" >> "$WORK/wall_times.tsv"
}

: > "$WORK/wall_times.tsv"
run_one "legacy"    "--multipart-upload=OFF"
run_one "multipart" "--multipart-upload=ON"

echo
echo "============================================================"
echo "==> SUMMARY (wall time)"
echo "============================================================"
printf "label\twall_ms\n"
cat "$WORK/wall_times.tsv"
