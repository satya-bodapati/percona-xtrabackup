#!/usr/bin/env bash
#
# Reproducible legacy-vs-direct comparison without needing a real
# MySQL or real AWS. Uses fault_proxy.py in front of LocalStack to
# inject a fixed RTT so the comparison is wire-bound and stable.
#
# Two paths compared, both upload the SAME byte payload:
#
#   Path A (legacy): xbstream -c <files> | xbcloud put
#     - xtrabackup-side simulation: xbstream concatenates the files
#       into a framed stream, exactly as xtrabackup --stream=xbstream
#       would. The cloud-side xbcloud reads the stream, parses frames,
#       and multipart-uploads each file as one bucket object.
#
#   Path B (direct): for each file: xbcloud put --multipart-from-file
#     - ds_cloud-side equivalent: each file uploaded directly as an
#       object via the same multipart machinery. No xbstream parse
#       step. Matches what xtrabackup --cloud-storage does per file.
#
# The fault_proxy delay is the dominant cost in both paths (network
# RTT typically dwarfs CPU work), so any large delta IS in the
# upload protocol itself -- per-file Init/Complete count, small-file
# fast-path triggers, etc.
#
# Runs each path N times (default 3), reports min/median/max.
#
# Environment:
#   DELAY_MS          per-request injected latency (default 50)
#   ITERATIONS        runs per path (default 3)
#   NUM_SMALL_FILES   small files in synthetic backup (default 20)
#   SMALL_SIZE_MIB    per-small-file size (default 1)
#   NUM_MEDIUM_FILES  medium files (default 4)
#   MEDIUM_SIZE_MIB   per-medium-file size (default 30)
#   ONE_LARGE_MIB     large file size (default 200, 0 to skip)
#   PARALLEL          xbcloud parallelism (default 8)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BLD="${BLD:-$HERE/../../../../../../bld}"
RUNDIR="$BLD/runtime_output_directory"
XBCLOUD="$RUNDIR/xbcloud"
XBSTREAM="$RUNDIR/xbstream"
[ -x "$XBCLOUD" ] || { echo "xbcloud not built"; exit 2; }
[ -x "$XBSTREAM" ] || { echo "xbstream not built"; exit 2; }

DELAY_MS="${DELAY_MS:-50}"
ITERATIONS="${ITERATIONS:-3}"
NUM_SMALL_FILES="${NUM_SMALL_FILES:-20}"
SMALL_SIZE_MIB="${SMALL_SIZE_MIB:-1}"
NUM_MEDIUM_FILES="${NUM_MEDIUM_FILES:-4}"
MEDIUM_SIZE_MIB="${MEDIUM_SIZE_MIB:-30}"
ONE_LARGE_MIB="${ONE_LARGE_MIB:-200}"
PARALLEL="${PARALLEL:-8}"
PROXY_PORT="${PROXY_PORT:-4581}"

export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_DEFAULT_REGION=us-east-1
BUCKET="pxb3671-perf-cmp"
ENDPOINT_DIRECT="http://localhost:4566"
ENDPOINT_PROXY="http://localhost:${PROXY_PORT}"

echo "==> Setting up LocalStack + fault_proxy (delay=${DELAY_MS}ms)"
docker-compose -f "$HERE/docker-compose.yml" up -d localstack >/dev/null
for i in $(seq 1 30); do
  if curl -sf "$ENDPOINT_DIRECT/_localstack/health" \
       | grep -q '"s3": "available"'; then break; fi
  sleep 1
done

aws --endpoint-url="$ENDPOINT_DIRECT" s3 rb "s3://$BUCKET" --force \
    >/dev/null 2>&1 || true
aws --endpoint-url="$ENDPOINT_DIRECT" s3 mb "s3://$BUCKET" >/dev/null

WORK="$(mktemp -d)"
trap 'kill $PROXY_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

python3 "$HERE/fault_proxy.py" \
  --listen-port "$PROXY_PORT" \
  --upstream "$ENDPOINT_DIRECT" \
  --fault-rate 0 \
  --delay-ms "$DELAY_MS" \
  >"$WORK/proxy.log" 2>&1 &
PROXY_PID=$!
sleep 1

echo "==> Generating synthetic backup tree in $WORK/src"
mkdir -p "$WORK/src"
total_bytes=0
for i in $(seq 1 "$NUM_SMALL_FILES"); do
  dd if=/dev/urandom of="$WORK/src/small_$(printf '%03d' "$i").dat" \
     bs=1M count="$SMALL_SIZE_MIB" status=none
  total_bytes=$((total_bytes + SMALL_SIZE_MIB * 1024 * 1024))
done
for i in $(seq 1 "$NUM_MEDIUM_FILES"); do
  dd if=/dev/urandom of="$WORK/src/medium_$(printf '%03d' "$i").dat" \
     bs=1M count="$MEDIUM_SIZE_MIB" status=none
  total_bytes=$((total_bytes + MEDIUM_SIZE_MIB * 1024 * 1024))
done
if [ "$ONE_LARGE_MIB" -gt 0 ]; then
  dd if=/dev/urandom of="$WORK/src/large.dat" \
     bs=1M count="$ONE_LARGE_MIB" status=none
  total_bytes=$((total_bytes + ONE_LARGE_MIB * 1024 * 1024))
fi
echo "synthetic" > "$WORK/src/xtrabackup_tablespaces"

total_mib=$((total_bytes / 1024 / 1024))
file_count=$(ls "$WORK/src/" | wc -l)
echo "    $file_count files, $total_mib MiB total"
echo "    parallelism: $PARALLEL, delay: ${DELAY_MS}ms, iterations: $ITERATIONS"
echo

# ---- common args ----
XBCLOUD_FLAGS=(
  --storage=s3
  --s3-endpoint="$ENDPOINT_PROXY"
  --s3-bucket="$BUCKET"
  --s3-access-key=test
  --s3-secret-key=test
  --s3-region=us-east-1
  --s3-bucket-lookup=path
  --multipart-upload=ON
  --parallel="$PARALLEL"
)

run_legacy() {
  local prefix="legacy-$(date +%s%N)"
  cd "$WORK/src"
  local start=$(date +%s%N)
  "$XBSTREAM" -c $(ls -1) | \
    "$XBCLOUD" put "${XBCLOUD_FLAGS[@]}" "$prefix" >/dev/null 2>&1
  local end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))   # ms
}

run_direct() {
  local prefix="direct-$(date +%s%N)"
  local start=$(date +%s%N)
  for f in "$WORK/src/"*; do
    "$XBCLOUD" put "${XBCLOUD_FLAGS[@]}" \
      --multipart-from-file="$f" "$prefix" >/dev/null 2>&1
  done
  local end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))
}

summarize() {
  local label="$1"; shift
  local -a vals=("$@")
  local min=${vals[0]} max=${vals[0]} sum=0
  for v in "${vals[@]}"; do
    sum=$((sum + v))
    [ "$v" -lt "$min" ] && min=$v
    [ "$v" -gt "$max" ] && max=$v
  done
  local n=${#vals[@]}
  local avg=$((sum / n))
  local mibps=$((total_mib * 1000 / (avg > 0 ? avg : 1)))
  printf "%-12s runs=%d  min=%dms  avg=%dms  max=%dms  throughput=%d MiB/s\n" \
         "$label" "$n" "$min" "$avg" "$max" "$mibps"
}

echo "==> Warming up (first run JIT, connection pool, DNS cache)"
run_legacy >/dev/null
run_direct >/dev/null

echo "==> Running $ITERATIONS iterations of each path"
legacy_times=()
direct_times=()
for i in $(seq 1 "$ITERATIONS"); do
  t=$(run_legacy)
  legacy_times+=("$t")
  echo "    legacy  iter $i: ${t}ms"
done
for i in $(seq 1 "$ITERATIONS"); do
  t=$(run_direct)
  direct_times+=("$t")
  echo "    direct  iter $i: ${t}ms"
done

echo
echo "==> Results (lower is better)"
summarize "legacy"  "${legacy_times[@]}"
summarize "direct"  "${direct_times[@]}"

# Diff
legacy_avg=$(( $(IFS=+; echo "$((${legacy_times[*]}))") / ${#legacy_times[@]} ))
direct_avg=$(( $(IFS=+; echo "$((${direct_times[*]}))") / ${#direct_times[@]} ))
delta=$((direct_avg - legacy_avg))
if [ "$legacy_avg" -gt 0 ]; then
  pct=$(awk -v d=$delta -v l=$legacy_avg 'BEGIN { printf "%.1f", d * 100.0 / l }')
else
  pct="n/a"
fi
echo
echo "==> Delta: direct - legacy = ${delta}ms (${pct}%)"
echo "    negative = direct faster, positive = legacy faster"
echo
echo "Note: this compares xbcloud-pipe (legacy) vs xbcloud-from-file"
echo "(direct stand-in). Both use the same Stream_multipart_writer."
echo "Differences come from: (1) xbstream parse step in legacy adds"
echo "CPU work in xbcloud; (2) xbcloud-from-file's per-file Init"
echo "happens N times sequentially while legacy multiplexes through"
echo "one xbstream-driven loop. A real xtrabackup --cloud-storage"
echo "run would be slightly faster than direct here because data-copy"
echo "threads parallelize across files instead of sequential per-file."
