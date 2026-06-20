#!/usr/bin/env bash
#
# PXB-3671 prototype smoke test: Swift Static Large Object multipart
# against the openstackswift/saio all-in-one dev container.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BLD="${BLD:-$HERE/../../../../../../bld}"
RUNDIR="$BLD/runtime_output_directory"
XBCLOUD="$RUNDIR/xbcloud"
XBSTREAM="$RUNDIR/xbstream"

# openstackswift/saio default creds.
USER="test:tester"
KEY="testing"
AUTH_URL="http://localhost:8080/auth/v1.0"
CONTAINER="pxb3671-swift"
PREFIX="smoke/$(date +%s)"

echo "==> Bringing up Swift saio"
docker-compose -f "$HERE/docker-compose.yml" up -d swift >/dev/null

echo "==> Waiting for Swift"
for i in $(seq 1 60); do
  if curl -sf "$AUTH_URL" >/dev/null 2>&1; then break; fi
  sleep 1
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src"
dd if=/dev/urandom of="$WORK/src/big_20m.dat" bs=1M count=20 status=none
dd if=/dev/urandom of="$WORK/src/small_5m.dat" bs=1M count=5 status=none
echo "synthetic" > "$WORK/src/xtrabackup_tablespaces"

echo "==> Streaming xbstream | xbcloud put --storage=swift"
cd "$WORK/src"
"$XBSTREAM" -c big_20m.dat small_5m.dat xtrabackup_tablespaces | \
  "$XBCLOUD" put \
    --storage=swift \
    --swift-auth-url="$AUTH_URL" \
    --swift-auth-version=1.0 \
    --swift-user="$USER" \
    --swift-key="$KEY" \
    --swift-container="$CONTAINER" \
    --multipart-upload=ON \
    --multipart-part-size=$((6*1024*1024)) \
    --parallel=4 \
    "$PREFIX"

echo "==> Manifests + segments should both exist; xbcloud success is the gate"
echo "==> PASS: Swift Static Large Object multipart"
