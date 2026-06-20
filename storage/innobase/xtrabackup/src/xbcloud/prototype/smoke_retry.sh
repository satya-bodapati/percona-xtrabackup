#!/usr/bin/env bash
#
# PXB-3671 prototype smoke test: retry/backoff path exercises.
#
# LocalStack itself does not offer fault injection in the free tier, so
# this test puts a small Python proxy (fault_proxy.py) between xbcloud
# and LocalStack. The proxy returns HTTP 503 for every third request
# (~33% fault rate), which is recognized by http.cc as retriable. The
# test then:
#
#   1. Uploads a 100 MiB file via --multipart-from-file through the
#      proxy. Expects success despite 503s.
#   2. Verifies http.cc's retry log line ("Sleeping for X ms before
#      retrying ...") appears in the xbcloud log.
#   3. Downloads via the direct LocalStack endpoint (bypassing the
#      proxy) and confirms sha256 matches.
#
# Run from this directory.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BLD="${BLD:-$HERE/../../../../../../bld}"
RUNDIR="$BLD/runtime_output_directory"

XBCLOUD="$RUNDIR/xbcloud"
[ -x "$XBCLOUD" ] || { echo "xbcloud not found at $XBCLOUD"; exit 2; }

PROXY_PORT=4567
PROXY_FAULT_RATE=0.33

export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_DEFAULT_REGION=us-east-1
BUCKET="pxb3671-retry"
ENDPOINT_DIRECT="http://localhost:4566"
ENDPOINT_PROXY="http://localhost:${PROXY_PORT}"

echo "==> Bringing up LocalStack"
docker-compose -f "$HERE/docker-compose.yml" up -d localstack >/dev/null
echo "==> Waiting for LocalStack S3"
for i in $(seq 1 30); do
  if curl -sf "$ENDPOINT_DIRECT/_localstack/health" | grep -q '"s3": "available"'; then
    break
  fi
  sleep 1
done

echo "==> Recreating bucket s3://$BUCKET"
aws --endpoint-url="$ENDPOINT_DIRECT" s3 rb "s3://$BUCKET" --force >/dev/null 2>&1 || true
aws --endpoint-url="$ENDPOINT_DIRECT" s3 mb "s3://$BUCKET" >/dev/null

WORK="$(mktemp -d)"
trap 'kill $PROXY_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "==> Starting fault_proxy on :$PROXY_PORT (fault_rate=$PROXY_FAULT_RATE)"
# Inject on PUT (UploadPart, sync upload_object), POST (InitiateMultipart,
# CompleteMultipartUpload), and DELETE (AbortMultipartUpload, delete_object)
# so we exercise BOTH the async retry wrap (PUT) and the sync retry wrap
# added in this commit (POST/DELETE/etc).
python3 "$HERE/fault_proxy.py" \
  --listen-port "$PROXY_PORT" \
  --upstream "$ENDPOINT_DIRECT" \
  --fault-rate "$PROXY_FAULT_RATE" \
  --inject-methods "PUT,POST,DELETE" \
  >"$WORK/proxy.log" 2>&1 &
PROXY_PID=$!
sleep 1
if ! kill -0 "$PROXY_PID" 2>/dev/null; then
  echo "FAIL: fault_proxy did not start"
  cat "$WORK/proxy.log"
  exit 1
fi

echo "==> Generating 100 MiB test file"
src="$WORK/retry_test.dat"
dd if=/dev/urandom of="$src" bs=1M count=100 status=none
src_sha=$(sha256sum "$src" | awk '{print $1}')

echo "==> Uploading through fault_proxy (expects retries to fire)"
log="$WORK/upload.log"
"$XBCLOUD" put \
  --storage=s3 \
  --s3-endpoint="$ENDPOINT_PROXY" \
  --s3-bucket="$BUCKET" \
  --s3-access-key=test \
  --s3-secret-key=test \
  --s3-region=us-east-1 \
  --s3-bucket-lookup=path \
  --multipart-upload=ON \
  --multipart-from-file="$src" \
  --parallel=4 \
  --max-retries=20 \
  "test-retry" 2>&1 | tee "$log"

echo
echo "==> Verifying retry path fired (look for 'Sleeping ... before retrying')"
retry_lines=$(grep -c "Sleeping for .* ms before retrying" "$log" || true)
if [ "$retry_lines" -lt 1 ]; then
  echo "FAIL: no 'Sleeping ... before retrying' lines in xbcloud log;"
  echo "      either retries did not fire or the proxy did not inject."
  echo "--- fault_proxy.log (tail) ---"
  tail -40 "$WORK/proxy.log"
  exit 1
fi
inject_lines=$(grep -c "INJECT 503" "$WORK/proxy.log" || true)
echo "    fault_proxy injected $inject_lines 503s"
echo "    xbcloud retry path logged $retry_lines sleep/retry events"

echo
echo "==> Verifying upload completed despite faults"
obj_key="test-retry/$(basename "$src")"
remote_size=$(aws --endpoint-url="$ENDPOINT_DIRECT" s3 ls "s3://$BUCKET/$obj_key" \
                | awk '{print $3}')
src_size=$(stat -c %s "$src")
if [ "$remote_size" != "$src_size" ]; then
  echo "FAIL: remote size $remote_size != source size $src_size"
  exit 1
fi
echo "    remote size matches: $remote_size bytes"

echo
echo "==> Downloading directly (bypassing proxy) and verifying sha256"
dst="$WORK/dl.dat"
aws --endpoint-url="$ENDPOINT_DIRECT" s3 cp "s3://$BUCKET/$obj_key" "$dst" >/dev/null
dst_sha=$(sha256sum "$dst" | awk '{print $1}')
if [ "$dst_sha" != "$src_sha" ]; then
  echo "FAIL: sha256 mismatch ($dst_sha != $src_sha)"
  exit 1
fi
echo "    sha256 round-trip OK"

echo
echo "==> PASS: retry/backoff path survives $PROXY_FAULT_RATE fault injection"
