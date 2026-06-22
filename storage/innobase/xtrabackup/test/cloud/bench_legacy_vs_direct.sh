#!/usr/bin/env bash
#
# Real backup comparison: xtrabackup --cloud-storage (direct) vs
# xtrabackup --stream=xbstream | xbcloud put (legacy). Both run
# against the same mysqld + same LocalStack bucket via fault_proxy
# (default 50ms RTT injection) so wire latency is controlled and
# AWS variance is eliminated.
#
# Requires:
#   - mysqld running at /tmp/mysql.sock (or override via MYSQL_SOCKET)
#   - data already loaded (use the schema setup above this script)
#   - xtrabackup/xbcloud/xbstream built in bld/runtime_output_directory/
#
# Reads --user=root no password. Override with MYSQL_USER / MYSQL_PASS.
#
# Tunables:
#   DELAY_MS    fault_proxy latency per request (default 50)
#   PARALLEL    --parallel for xtrabackup data-copy threads (default 8)
#   ITERATIONS  runs per path (default 3)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BLD="${BLD:-$HERE/../../../../../bld}"
RUNDIR="$BLD/runtime_output_directory"
XBCLOUD="$RUNDIR/xbcloud"
XBSTREAM="$RUNDIR/xbstream"
XTRABACKUP="$RUNDIR/xtrabackup"
PROTOTYPE="$HERE/../../src/xbcloud/prototype"

MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_SOCKET="${MYSQL_SOCKET:-/tmp/mysql.sock}"
DELAY_MS="${DELAY_MS:-50}"
PARALLEL="${PARALLEL:-8}"
ITERATIONS="${ITERATIONS:-3}"
PROXY_PORT="${PROXY_PORT:-4583}"

export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_DEFAULT_REGION=us-east-1
BUCKET="pxb-bench"
ENDPOINT_DIRECT="http://localhost:4566"
ENDPOINT_PROXY="http://localhost:${PROXY_PORT}"

echo "==> Bringing up LocalStack + fault_proxy (delay=${DELAY_MS}ms)"
docker-compose -f "$PROTOTYPE/docker-compose.yml" up -d localstack >/dev/null
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

python3 "$PROTOTYPE/fault_proxy.py" \
  --listen-port "$PROXY_PORT" \
  --upstream "$ENDPOINT_DIRECT" \
  --fault-rate 0 \
  --delay-ms "$DELAY_MS" \
  >"$WORK/proxy.log" 2>&1 &
PROXY_PID=$!
sleep 1

reset_bucket() {
  aws --endpoint-url="$ENDPOINT_DIRECT" s3 rb "s3://$BUCKET" --force \
      >/dev/null 2>&1 || true
  aws --endpoint-url="$ENDPOINT_DIRECT" s3 mb "s3://$BUCKET" >/dev/null
}

run_legacy() {
  reset_bucket
  local name="legacy-$(date +%s%N)"
  rm -rf /home/satya/tmp/legacy-tmp
  local start=$(date +%s%N)
  "$XTRABACKUP" --backup \
      --user="$MYSQL_USER" --socket="$MYSQL_SOCKET" \
      --target-dir=/home/satya/tmp/legacy-tmp \
      --parallel="$PARALLEL" \
      --stream=xbstream 2>"$WORK/legacy.err" \
   | "$XBCLOUD" put \
      --storage=s3 \
      --s3-endpoint="$ENDPOINT_PROXY" \
      --s3-bucket="$BUCKET" \
      --s3-access-key=test --s3-secret-key=test \
      --s3-region=us-east-1 --s3-bucket-lookup=path \
      --multipart-upload=ON --parallel="$PARALLEL" \
      --multipart-memory-budget=268435456 \
      "$name" 2>"$WORK/legacy.xbc.err"
  local end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))
}

run_direct() {
  reset_bucket
  local name="direct-$(date +%s%N)"
  rm -rf /home/satya/tmp/direct-tmp
  local start=$(date +%s%N)
  "$XTRABACKUP" --backup \
      --user="$MYSQL_USER" --socket="$MYSQL_SOCKET" \
      --target-dir="/home/satya/tmp/direct-tmp" \
      --parallel="$PARALLEL" \
      --cloud-storage=s3 \
      --cloud-endpoint="$ENDPOINT_PROXY" \
      --cloud-bucket="$BUCKET" \
      --cloud-access-key=test --cloud-secret-key=test \
      --cloud-region=us-east-1 --cloud-bucket-lookup=path \
      --cloud-parallel="$PARALLEL" \
      >"$WORK/direct.out" 2>"$WORK/direct.err"
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
  printf "%-12s runs=%d  min=%dms  avg=%dms  max=%dms\n" \
         "$label" "$n" "$min" "$avg" "$max"
}

echo "==> Warming up"
run_legacy >/dev/null
run_direct >/dev/null

echo "==> Running $ITERATIONS iterations of each path"
legacy_times=()
direct_times=()
for i in $(seq 1 "$ITERATIONS"); do
  t=$(run_legacy)
  legacy_times+=("$t")
  echo "    legacy iter $i: ${t}ms"
done
for i in $(seq 1 "$ITERATIONS"); do
  t=$(run_direct)
  direct_times+=("$t")
  echo "    direct iter $i: ${t}ms"
done

echo
echo "==> Results (lower is better)"
summarize "legacy"  "${legacy_times[@]}"
summarize "direct"  "${direct_times[@]}"

legacy_avg=$(( $(IFS=+; echo "$((${legacy_times[*]}))") / ${#legacy_times[@]} ))
direct_avg=$(( $(IFS=+; echo "$((${direct_times[*]}))") / ${#direct_times[@]} ))
delta=$((direct_avg - legacy_avg))
pct=$(awk -v d=$delta -v l=$legacy_avg \
      'BEGIN { if (l>0) printf "%.1f", d * 100.0 / l; else print "n/a" }')
echo
echo "==> direct - legacy = ${delta}ms (${pct}%)"
echo "    negative = direct faster"
