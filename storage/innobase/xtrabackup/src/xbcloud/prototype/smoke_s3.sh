#!/usr/bin/env bash
#
# PXB-3671 prototype smoke test: S3 multipart one-object-per-file.
#
# Brings up LocalStack via docker-compose, creates a bucket, feeds a small
# xbstream blob (two files: 20 MiB and 5 MiB) through the prototype's
# multipart-enabled xbcloud, then verifies that the bucket contains
# exactly two objects named after the source files (no .NNNNN suffix).
#
# Requires: docker, docker-compose, awscli, xbstream and xbcloud built in
# ../../../../bld/runtime_output_directory/.
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

BUCKET="pxb3671-proto"
PREFIX="smoke/$(date +%s)"
ENDPOINT="http://localhost:4566"

echo "==> Bringing up LocalStack"
docker-compose -f "$HERE/docker-compose.yml" up -d localstack >/dev/null

echo "==> Waiting for LocalStack S3 to be ready"
for i in $(seq 1 30); do
  if curl -sf "$ENDPOINT/_localstack/health" | grep -q '"s3": "available"'; then
    break
  fi
  sleep 1
done

echo "==> Creating bucket s3://$BUCKET"
aws --endpoint-url="$ENDPOINT" s3 mb "s3://$BUCKET" 2>/dev/null || true

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> Generating test inputs in $WORK"
mkdir -p "$WORK/src"
dd if=/dev/urandom of="$WORK/src/big_20m.dat"   bs=1M count=20 status=none
dd if=/dev/urandom of="$WORK/src/small_5m.dat" bs=1M count=5  status=none
# xbcloud's PUT mode does a sanity check that a file with prefix
# xtrabackup_tablespaces exists in the upload (real backups always have it).
# Synthesize one here so the test does not trip the check.
echo "synthetic" > "$WORK/src/xtrabackup_tablespaces"
EXP1=$(stat -c %s "$WORK/src/big_20m.dat")
EXP2=$(stat -c %s "$WORK/src/small_5m.dat")
EXPSHA1=$(sha256sum "$WORK/src/big_20m.dat" | awk '{print $1}')
EXPSHA2=$(sha256sum "$WORK/src/small_5m.dat" | awk '{print $1}')

echo "==> Streaming through xbstream | xbcloud put with --multipart-upload"
cd "$WORK/src"
"$XBSTREAM" -c big_20m.dat small_5m.dat xtrabackup_tablespaces | \
  "$XBCLOUD" put \
    --storage=s3 \
    --s3-endpoint="$ENDPOINT" \
    --s3-bucket="$BUCKET" \
    --s3-access-key=test \
    --s3-secret-key=test \
    --s3-region=us-east-1 \
    --s3-bucket-lookup=path \
    --multipart-upload=ON \
    --multipart-part-size=$((6*1024*1024)) \
    --parallel=4 \
    "$PREFIX"

echo "==> Listing bucket contents under s3://$BUCKET/$PREFIX/"
LIST=$(aws --endpoint-url="$ENDPOINT" s3 ls "s3://$BUCKET/$PREFIX/" | awk '{print $NF}' | sort)
echo "$LIST"

echo "==> Expecting exactly three objects with real names, no .NNNNN suffix"
EXPECTED=$(printf 'big_20m.dat\nsmall_5m.dat\nxtrabackup_tablespaces\n' | sort)
if [ "$LIST" != "$EXPECTED" ]; then
  echo "FAIL: object list mismatch"
  echo "expected:"
  echo "$EXPECTED"
  exit 1
fi

echo "==> Downloading objects DIRECTLY (no xbstream -x) and verifying"
echo "    each cloud object should equal the original file bit-for-bit"
mkdir -p "$WORK/out"
aws --endpoint-url="$ENDPOINT" s3 cp "s3://$BUCKET/$PREFIX/big_20m.dat"          "$WORK/out/big_20m.dat"          >/dev/null
aws --endpoint-url="$ENDPOINT" s3 cp "s3://$BUCKET/$PREFIX/small_5m.dat"        "$WORK/out/small_5m.dat"        >/dev/null
aws --endpoint-url="$ENDPOINT" s3 cp "s3://$BUCKET/$PREFIX/xtrabackup_tablespaces" "$WORK/out/xtrabackup_tablespaces" >/dev/null

GOT1=$(stat -c %s "$WORK/out/big_20m.dat")
GOT2=$(stat -c %s "$WORK/out/small_5m.dat")
GOTSHA1=$(sha256sum "$WORK/out/big_20m.dat" | awk '{print $1}')
GOTSHA2=$(sha256sum "$WORK/out/small_5m.dat" | awk '{print $1}')

[ "$GOT1" = "$EXP1" ]       || { echo "FAIL: big_20m.dat size $GOT1 != $EXP1 -- cloud object not raw bytes"; exit 1; }
[ "$GOT2" = "$EXP2" ]       || { echo "FAIL: small_5m.dat size $GOT2 != $EXP2 -- cloud object not raw bytes"; exit 1; }
[ "$GOTSHA1" = "$EXPSHA1" ] || { echo "FAIL: big_20m.dat sha256 mismatch -- cloud object not raw bytes"; exit 1; }
[ "$GOTSHA2" = "$EXPSHA2" ] || { echo "FAIL: small_5m.dat sha256 mismatch -- cloud object not raw bytes"; exit 1; }

echo "==> PASS: cloud objects are the actual file bytes (direct download works)"
