#!/usr/bin/env bash
#
# Round-trip smoke that doesn't need mysqld: uploads synthetic files via
# xbcloud put --multipart-from-file, then xtrabackup --download fetches
# them, and we diff. Validates the ds_cloud_lifecycle download path
# end-to-end against LocalStack.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/cloud_common.sh"

cloud_require_binaries
cloud_up
cloud_create_bucket

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PREFIX="round-trip-$(date +%s)"

echo "==> Generating source files"
mkdir -p "$WORK/src/sub1" "$WORK/src/sub2"
dd if=/dev/urandom of="$WORK/src/tiny.dat" bs=1K count=4 status=none
dd if=/dev/urandom of="$WORK/src/medium.dat" bs=1M count=20 status=none
dd if=/dev/urandom of="$WORK/src/sub1/a.dat" bs=1M count=2 status=none
dd if=/dev/urandom of="$WORK/src/sub2/b.dat" bs=1M count=1 status=none

echo "==> Uploading via xbcloud put --multipart-from-file (one per file)"
XBCLOUD_FLAGS=$(cloud_xbcloud_flags | tr '\n' ' ')
for f in tiny.dat medium.dat sub1/a.dat sub2/b.dat; do
  "$XBCLOUD" put \
    $XBCLOUD_FLAGS \
    --multipart-upload=ON \
    --multipart-from-file="$WORK/src/$f" \
    "$PREFIX" 2>&1 | grep -v "rate up=" | tail -2
done

echo
echo "==> Listing bucket"
aws --endpoint-url="$LOCALSTACK_ENDPOINT" s3 ls "s3://$PXB_BUCKET/$PREFIX/" \
  --recursive

echo
echo "==> Running xtrabackup --download"
XTRA_FLAGS=$(cloud_xtrabackup_flags | tr '\n' ' ')
"$XTRABACKUP" --download \
  --target-dir="$WORK/$PREFIX" \
  $XTRA_FLAGS 2>&1 | tail -20

echo
echo "==> Verifying contents"
fail=0
for f in tiny.dat medium.dat; do
  src_sha=$(sha256sum "$WORK/src/$f" | awk '{print $1}')
  # xbcloud puts each file at "$PREFIX/<basename>" -- so under target_dir
  # we'll find them at $WORK/$PREFIX/<basename>.
  dl="$WORK/$PREFIX/$f"
  if [ ! -f "$dl" ]; then
    echo "FAIL: missing $dl"
    fail=1
    continue
  fi
  dl_sha=$(sha256sum "$dl" | awk '{print $1}')
  if [ "$src_sha" != "$dl_sha" ]; then
    echo "FAIL: sha mismatch for $f"
    fail=1
  else
    echo "    $f OK"
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "==> FAIL"
  exit 1
fi
echo "==> PASS: upload-download round trip works against ds_cloud_lifecycle"
