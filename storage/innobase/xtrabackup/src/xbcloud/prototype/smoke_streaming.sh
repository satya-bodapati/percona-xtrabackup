#!/usr/bin/env bash
#
# PXB-3671 prototype smoke test: streaming put_func path.
#
# Exercises the xbstream -> xbcloud streaming path (the real backup
# pipeline) with the new dynamic_part_size() schedule and small-file
# fast path. Generates files of three sizes:
#
#   tiny_2m.dat    -> 2 MiB        -> small-file fast path (single PUT)
#   medium_50m.dat -> 50 MiB       -> multipart in tier 1 (16 MiB parts, 4)
#   large_1100m.dat -> 1100 MiB    -> multipart that crosses 1 GiB
#                                     into tier 2 (16 MiB then 64 MiB)
#
# For each file the test verifies:
#   1. xbcloud picked the expected flush size from the log
#   2. The bucket contains one object per file with the real name
#   3. The object payload is the xbstream-framed source and
#      sha256-matches the original after xbstream -x extract
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

export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_DEFAULT_REGION=us-east-1

BUCKET="pxb3671-streaming"
PREFIX="smoke-stream/$(date +%s)"
ENDPOINT="http://localhost:4566"

echo "==> Bringing up LocalStack"
docker-compose -f "$HERE/docker-compose.yml" up -d localstack >/dev/null

echo "==> Waiting for LocalStack S3"
for i in $(seq 1 30); do
  if curl -sf "$ENDPOINT/_localstack/health" | grep -q '"s3": "available"'; then
    break
  fi
  sleep 1
done

echo "==> Recreating bucket s3://$BUCKET"
aws --endpoint-url="$ENDPOINT" s3 rb "s3://$BUCKET" --force >/dev/null 2>&1 || true
aws --endpoint-url="$ENDPOINT" s3 mb "s3://$BUCKET" >/dev/null

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> Generating test inputs in $WORK/src"
mkdir -p "$WORK/src"
# Tiny: 2 MiB -> below --multipart-threshold (16 MiB) -> single PUT.
dd if=/dev/urandom of="$WORK/src/tiny_2m.dat" bs=1M count=2 status=none
# Medium: 50 MiB -> multipart, 4 parts of 16 MiB at tier 1.
dd if=/dev/urandom of="$WORK/src/medium_50m.dat" bs=1M count=50 status=none
# Large: 1100 MiB -> multipart, first 1 GiB at 16 MiB, then 64 MiB tier.
dd if=/dev/urandom of="$WORK/src/large_1100m.dat" bs=1M count=1100 status=none
# xbcloud put requires a file with this prefix to exist (sanity check).
echo "synthetic" > "$WORK/src/xtrabackup_tablespaces"

# Record source sha256 for round-trip verification.
declare -A EXP_SHA
declare -A EXP_SIZE
for f in tiny_2m.dat medium_50m.dat large_1100m.dat; do
  EXP_SHA[$f]=$(sha256sum "$WORK/src/$f" | awk '{print $1}')
  EXP_SIZE[$f]=$(stat -c %s "$WORK/src/$f")
done

echo "==> Streaming xbstream -> xbcloud put (dynamic part_size, default budget)"
LOG="$WORK/upload.log"
cd "$WORK/src"
"$XBSTREAM" -c tiny_2m.dat medium_50m.dat large_1100m.dat xtrabackup_tablespaces | \
  "$XBCLOUD" put \
    --storage=s3 \
    --s3-endpoint="$ENDPOINT" \
    --s3-bucket="$BUCKET" \
    --s3-access-key=test \
    --s3-secret-key=test \
    --s3-region=us-east-1 \
    --s3-bucket-lookup=path \
    --multipart-upload=ON \
    --parallel=4 \
    --rate-log-interval=1 \
    "$PREFIX" 2>&1 | tee "$LOG"

echo
echo "==> Verifying bucket listing"
LIST=$(aws --endpoint-url="$ENDPOINT" s3 ls "s3://$BUCKET/$PREFIX/" \
         | awk '{print $NF}' | sort)
EXPECTED=$(printf 'large_1100m.dat\nmedium_50m.dat\ntiny_2m.dat\nxtrabackup_tablespaces\n' | sort)
if [ "$LIST" != "$EXPECTED" ]; then
  echo "FAIL: object list mismatch"
  echo "expected:"; echo "$EXPECTED"
  echo "got:";      echo "$LIST"
  exit 1
fi
echo "    bucket has the expected 4 objects, no .NNNNN suffix"

echo
echo "==> Verifying small-file fast path took the single-PUT route for tiny_2m.dat"
if ! grep -q "small-file PUT done.*tiny_2m.dat" "$LOG"; then
  echo "FAIL: did not see 'small-file PUT done' for tiny_2m.dat in log"
  exit 1
fi
if grep -q "multipart commit done.*tiny_2m.dat" "$LOG"; then
  echo "FAIL: tiny_2m.dat went through multipart instead of fast path"
  exit 1
fi
echo "    tiny_2m.dat shipped via single PUT (fast path) as expected"

echo
echo "==> Verifying medium_50m.dat used 16 MiB parts at tier 1"
# 50 MiB at 16 MiB parts = 4 parts (last is 2 MiB).
m50_parts=$(grep -c "multipart .*medium_50m.dat part #.*done" "$LOG" || true)
if [ "$m50_parts" -lt 3 ] || [ "$m50_parts" -gt 5 ]; then
  echo "FAIL: medium_50m.dat saw $m50_parts parts; expected ~4"
  exit 1
fi
echo "    medium_50m.dat used $m50_parts parts at tier 1"

echo
echo "==> Verifying large_1100m.dat crossed into tier 2 (64 MiB parts)"
# 1100 MiB: first 1024 MiB at 16 MiB = 64 parts, then ~76 MiB
# accumulating into tier 2 -> 1 or 2 more parts. Expect total > 64.
big_parts=$(grep -c "multipart .*large_1100m.dat part #.*done" "$LOG" || true)
if [ "$big_parts" -lt 65 ]; then
  echo "FAIL: large_1100m.dat saw $big_parts parts; expected > 64"
  exit 1
fi
# Look for any 64-MiB-or-larger part on this file to prove tier 2 fired.
tier2_seen=$(grep "multipart .*large_1100m.dat part #.*done (" "$LOG" \
  | awk -F'[()]' '{print $2}' | awk '{print $1}' \
  | awk -v lim=$((64*1024*1024)) '$1 >= lim {print; exit}')
if [ -z "$tier2_seen" ]; then
  echo "FAIL: large_1100m.dat never produced a part >= 64 MiB"
  exit 1
fi
echo "    large_1100m.dat used $big_parts parts, tier 2 (>=64 MiB) reached"

echo
echo "==> Downloading objects DIRECTLY (no xbstream -x) and verifying"
echo "    each cloud object should equal the original file bit-for-bit"
mkdir -p "$WORK/out"
for f in tiny_2m.dat medium_50m.dat large_1100m.dat; do
  aws --endpoint-url="$ENDPOINT" s3 cp "s3://$BUCKET/$PREFIX/$f" \
    "$WORK/out/$f" >/dev/null
  got_sha=$(sha256sum "$WORK/out/$f" | awk '{print $1}')
  got_size=$(stat -c %s "$WORK/out/$f")
  if [ "$got_size" != "${EXP_SIZE[$f]}" ]; then
    echo "FAIL: $f size $got_size != ${EXP_SIZE[$f]}"
    exit 1
  fi
  if [ "$got_sha" != "${EXP_SHA[$f]}" ]; then
    echo "FAIL: $f sha256 mismatch -- cloud object is not raw file bytes"
    exit 1
  fi
  echo "    $f direct download matches source (${got_size} bytes)"
done

echo
echo "==> Verifying rate logger fired (--rate-log-interval=1)"
rate_lines=$(grep -c "rate up=" "$LOG" || true)
if [ "$rate_lines" -lt 1 ]; then
  echo "FAIL: no 'rate up=' lines in log -- rate logger did not fire"
  exit 1
fi
echo "    rate logger emitted $rate_lines log lines"

echo
echo "==> ALL PASS: streaming put_func + dynamic part_size + small-file fast path"
