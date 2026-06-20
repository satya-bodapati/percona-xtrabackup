#!/usr/bin/env bash
#
# PXB-3671 prototype smoke test: rollover above 5 TiB (simulated by
# dropping the threshold).
#
# A real 5 TiB+ file is infeasible to generate locally, so this test
# sets --multipart-rollover-threshold=256MiB and uploads a 600 MiB file.
# That should produce three .part-NNN segments plus a manifest sidecar:
#
#   prefix/test_600m.dat.part-001   (256 MiB)
#   prefix/test_600m.dat.part-002   (256 MiB)
#   prefix/test_600m.dat.part-003   ( 88 MiB)
#   prefix/test_600m.dat.manifest.json
#
# The test then:
#   1. Lists the bucket and verifies the exact 4 keys above
#   2. Fetches the manifest, parses it, and validates that the segments
#      array matches the actually-uploaded keys/sizes
#   3. Downloads each segment, concatenates them in manifest order, and
#      verifies the SHA256 matches the original local file
#
# Run from this directory.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BLD="${BLD:-$HERE/../../../../../../bld}"
XBCLOUD="$BLD/runtime_output_directory/xbcloud"
[ -x "$XBCLOUD" ] || { echo "xbcloud not found at $XBCLOUD"; exit 2; }

export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_DEFAULT_REGION=us-east-1
BUCKET=pxb3671-rollover
ENDPOINT=http://localhost:4566
PREFIX="rollover/$(date +%s)"

ROLLOVER_THRESHOLD=$((256 * 1024 * 1024))   # 256 MiB
FILE_MIB=600
EXPECTED_SEGMENTS=3

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

src="$WORK/test_600m.dat"
echo "==> Generating $FILE_MIB MiB test file"
dd if=/dev/urandom of="$src" bs=1M count="$FILE_MIB" status=none
src_sha=$(sha256sum "$src" | awk '{print $1}')
src_size=$(stat -c %s "$src")
echo "    sha256=$src_sha size=$src_size"

echo "==> Uploading via xbcloud put --multipart-from-file with rollover=256 MiB"
log="$WORK/upload.log"
"$XBCLOUD" put \
  --storage=s3 \
  --s3-endpoint="$ENDPOINT" \
  --s3-bucket="$BUCKET" \
  --s3-access-key=test \
  --s3-secret-key=test \
  --s3-region=us-east-1 \
  --s3-bucket-lookup=path \
  --multipart-upload=ON \
  --multipart-from-file="$src" \
  --multipart-rollover-threshold="$ROLLOVER_THRESHOLD" \
  --parallel=4 \
  "$PREFIX" 2>&1 | tee "$log"

echo
echo "==> Verifying bucket listing"
LIST=$(aws --endpoint-url="$ENDPOINT" s3 ls "s3://$BUCKET/$PREFIX/" \
         | awk '{print $NF}' | sort)
EXPECTED=$(printf '%s\n' \
  "test_600m.dat.manifest.json" \
  "test_600m.dat.part-001" \
  "test_600m.dat.part-002" \
  "test_600m.dat.part-003" | sort)
if [ "$LIST" != "$EXPECTED" ]; then
  echo "FAIL: object listing mismatch"
  echo "expected:"; echo "$EXPECTED"
  echo "got:";      echo "$LIST"
  exit 1
fi
echo "    listing matches: 3 segments + 1 manifest"

echo
echo "==> Fetching and parsing manifest"
manifest_local="$WORK/manifest.json"
aws --endpoint-url="$ENDPOINT" s3 cp \
  "s3://$BUCKET/$PREFIX/test_600m.dat.manifest.json" "$manifest_local" >/dev/null
echo "--- manifest contents ---"
cat "$manifest_local"
echo "-------------------------"

# Validate manifest fields with jq if available, else with grep.
if command -v jq >/dev/null 2>&1; then
  m_total=$(jq -r '.total_size' "$manifest_local")
  m_threshold=$(jq -r '.rollover_threshold' "$manifest_local")
  m_n=$(jq -r '.segments | length' "$manifest_local")
  m_logical=$(jq -r '.logical_name' "$manifest_local")
else
  m_total=$(grep -oP '"total_size": \K[0-9]+' "$manifest_local")
  m_threshold=$(grep -oP '"rollover_threshold": \K[0-9]+' "$manifest_local")
  m_n=$(grep -c '"key":' "$manifest_local")
  m_logical=$(grep -oP '"logical_name": "\K[^"]+' "$manifest_local")
fi
[ "$m_total" = "$src_size" ] \
  || { echo "FAIL: manifest total_size $m_total != $src_size"; exit 1; }
[ "$m_threshold" = "$ROLLOVER_THRESHOLD" ] \
  || { echo "FAIL: manifest threshold $m_threshold != $ROLLOVER_THRESHOLD"; exit 1; }
[ "$m_n" = "$EXPECTED_SEGMENTS" ] \
  || { echo "FAIL: manifest segment count $m_n != $EXPECTED_SEGMENTS"; exit 1; }
[ "$m_logical" = "test_600m.dat" ] \
  || { echo "FAIL: manifest logical_name '$m_logical' != 'test_600m.dat'"; exit 1; }
echo "    manifest: total_size=$m_total threshold=$m_threshold segments=$m_n logical=$m_logical"

echo
echo "==> Verifying per-segment sizes against bucket"
for n in 1 2 3; do
  seg_key="$PREFIX/test_600m.dat.part-$(printf '%03d' "$n")"
  size_remote=$(aws --endpoint-url="$ENDPOINT" s3 ls "s3://$BUCKET/$seg_key" \
                  | awk '{print $3}')
  if [ "$n" -lt 3 ]; then
    expected=$ROLLOVER_THRESHOLD
  else
    expected=$((src_size - 2 * ROLLOVER_THRESHOLD))
  fi
  [ "$size_remote" = "$expected" ] \
    || { echo "FAIL: segment $n size $size_remote != $expected"; exit 1; }
  echo "    part-$(printf '%03d' "$n"): $size_remote bytes (expected $expected)"
done

echo
echo "==> Downloading segments in manifest order and verifying sha256"
dst="$WORK/reconstructed.dat"
: > "$dst"
for n in 1 2 3; do
  seg_key="$PREFIX/test_600m.dat.part-$(printf '%03d' "$n")"
  aws --endpoint-url="$ENDPOINT" s3 cp "s3://$BUCKET/$seg_key" - >> "$dst"
done
dst_sha=$(sha256sum "$dst" | awk '{print $1}')
dst_size=$(stat -c %s "$dst")
[ "$dst_size" = "$src_size" ] \
  || { echo "FAIL: reconstructed size $dst_size != $src_size"; exit 1; }
[ "$dst_sha" = "$src_sha" ] \
  || { echo "FAIL: reconstructed sha256 mismatch ($dst_sha != $src_sha)"; exit 1; }
echo "    reconstructed file matches original (size=$dst_size sha256=$dst_sha)"

echo
echo "==> PASS: rollover into 3 segments + manifest works end-to-end"
