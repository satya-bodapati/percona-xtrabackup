#!/usr/bin/env bash
#
# PXB-3671 prototype smoke test: GCS via S3-compatible XML multipart.
#
# Uses fake-gcs-server with a real GCS XML/S3 endpoint configured. GCS
# is handled in xbcloud.cc by an S3_object_store instance, so this
# exercises the same multipart code path that smoke_s3.sh exercises,
# just pointed at a different endpoint.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BLD="${BLD:-$HERE/../../../../../../bld}"
RUNDIR="$BLD/runtime_output_directory"
XBCLOUD="$RUNDIR/xbcloud"
XBSTREAM="$RUNDIR/xbstream"

ENDPOINT="http://localhost:4443"
BUCKET="pxb3671-gcs"
PREFIX="smoke/$(date +%s)"

echo "==> Bringing up fake-gcs-server"
docker-compose -f "$HERE/docker-compose.yml" up -d fake-gcs-server >/dev/null

echo "==> Waiting for fake-gcs-server"
for i in $(seq 1 30); do
  if curl -sf "$ENDPOINT/storage/v1/b" >/dev/null 2>&1; then break; fi
  sleep 1
done

echo "==> Creating bucket via JSON API"
curl -s -X POST "$ENDPOINT/storage/v1/b" -H 'Content-Type: application/json' \
  -d "{\"name\":\"$BUCKET\"}" >/dev/null || true

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src"
dd if=/dev/urandom of="$WORK/src/big_20m.dat" bs=1M count=20 status=none
dd if=/dev/urandom of="$WORK/src/small_5m.dat" bs=1M count=5 status=none
echo "synthetic" > "$WORK/src/xtrabackup_tablespaces"
EXP1=$(stat -c %s "$WORK/src/big_20m.dat")
EXP2=$(stat -c %s "$WORK/src/small_5m.dat")
EXPSHA1=$(sha256sum "$WORK/src/big_20m.dat" | awk '{print $1}')
EXPSHA2=$(sha256sum "$WORK/src/small_5m.dat" | awk '{print $1}')

echo "==> Streaming xbstream | xbcloud put --storage=google"
cd "$WORK/src"
"$XBSTREAM" -c big_20m.dat small_5m.dat xtrabackup_tablespaces | \
  "$XBCLOUD" put \
    --storage=google \
    --google-endpoint="$ENDPOINT/" \
    --google-bucket="$BUCKET" \
    --google-access-key=fake \
    --google-secret-key=fake \
    --google-region=us-east1 \
    --multipart-upload=ON \
    --multipart-part-size=$((6*1024*1024)) \
    --parallel=4 \
    "$PREFIX"

echo "==> Listing bucket via JSON API"
LIST=$(curl -s "$ENDPOINT/storage/v1/b/$BUCKET/o?prefix=$PREFIX/" \
       | python3 -c 'import sys,json; d=json.load(sys.stdin); print("\n".join(sorted(o["name"].rsplit("/",1)[-1] for o in d.get("items",[]))))')
echo "$LIST"
EXPECTED=$(printf 'big_20m.dat\nsmall_5m.dat\nxtrabackup_tablespaces\n' | sort)
[ "$LIST" = "$EXPECTED" ] || { echo "FAIL: object list mismatch"; echo "expected:"; echo "$EXPECTED"; exit 1; }

mkdir -p "$WORK/out" "$WORK/extract"
for f in big_20m.dat small_5m.dat xtrabackup_tablespaces; do
  curl -s "$ENDPOINT/storage/v1/b/$BUCKET/o/$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=''))" "$PREFIX/$f")?alt=media" \
    > "$WORK/out/$f"
done

cat "$WORK/out/big_20m.dat" "$WORK/out/small_5m.dat" "$WORK/out/xtrabackup_tablespaces" | \
  "$XBSTREAM" -x -C "$WORK/extract"

[ "$(stat -c %s "$WORK/extract/big_20m.dat")" = "$EXP1" ] || { echo "FAIL: big size"; exit 1; }
[ "$(stat -c %s "$WORK/extract/small_5m.dat")" = "$EXP2" ] || { echo "FAIL: small size"; exit 1; }
[ "$(sha256sum "$WORK/extract/big_20m.dat" | awk '{print $1}')" = "$EXPSHA1" ] || { echo "FAIL: big sha256"; exit 1; }
[ "$(sha256sum "$WORK/extract/small_5m.dat" | awk '{print $1}')" = "$EXPSHA2" ] || { echo "FAIL: small sha256"; exit 1; }

echo "==> PASS: GCS one-object-per-file via S3-compatible XML multipart"
