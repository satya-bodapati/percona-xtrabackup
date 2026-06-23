# Common helpers for xtrabackup direct-cloud tests.
#
# Each test runs against a docker-compose stack (LocalStack / Azurite /
# openstackswift / fake-gcs-server) lifted from prototype/. Tests pick
# a backend via PXB_CLOUD_BACKEND (default: s3) and the harness sets up
# the bucket, runs the test body, and tears down.
#
# Usage: source cloud_common.sh in a test script. Then call:
#   cloud_setup                        # bring up docker stack, create bucket
#   cloud_teardown                     # remove bucket (containers stay up)
#
# Environment knobs read by helpers:
#   PXB_CLOUD_BACKEND   s3 | azure | swift | gcs (default: s3)
#   PXB_BUCKET          bucket/container name (default: pxb-cloud-test)
#   BLD                 build dir (default: $REPO_ROOT/bld)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../../../.." && pwd)"
BLD="${BLD:-$REPO_ROOT/bld}"
RUNDIR="$BLD/runtime_output_directory"
XBSTREAM="$RUNDIR/xbstream"
XBCLOUD="$RUNDIR/xbcloud"
XTRABACKUP="$RUNDIR/xtrabackup"

PXB_CLOUD_BACKEND="${PXB_CLOUD_BACKEND:-s3}"
PXB_BUCKET="${PXB_BUCKET:-pxb-cloud-test}"

COMPOSE_FILE="$REPO_ROOT/storage/innobase/xtrabackup/src/xbcloud/prototype/docker-compose.yml"

export AWS_ACCESS_KEY_ID="${AWS_ACCESS_KEY_ID:-test}"
export AWS_SECRET_ACCESS_KEY="${AWS_SECRET_ACCESS_KEY:-test}"
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-us-east-1}"

LOCALSTACK_ENDPOINT="http://localhost:4566"

cloud_require_binaries() {
  for b in "$XBSTREAM" "$XBCLOUD" "$XTRABACKUP"; do
    if [ ! -x "$b" ]; then
      echo "FATAL: not built: $b"
      echo "Run: (cd $BLD && make xbcloud xbstream xtrabackup -j8)"
      exit 2
    fi
  done
}

cloud_up() {
  docker-compose -f "$COMPOSE_FILE" up -d >/dev/null
  case "$PXB_CLOUD_BACKEND" in
    s3|gcs)
      for i in $(seq 1 30); do
        if curl -sf "$LOCALSTACK_ENDPOINT/_localstack/health" \
            | grep -q '"s3": "available"'; then
          break
        fi
        sleep 1
      done
      ;;
    azure)
      for i in $(seq 1 30); do
        if curl -sf "http://localhost:10000/devstoreaccount1?comp=list" \
            >/dev/null 2>&1; then
          break
        fi
        sleep 1
      done
      ;;
    swift)
      for i in $(seq 1 60); do
        if curl -sf "http://localhost:8080/info" >/dev/null 2>&1; then
          break
        fi
        sleep 1
      done
      ;;
  esac
}

cloud_create_bucket() {
  case "$PXB_CLOUD_BACKEND" in
    s3|gcs)
      aws --endpoint-url="$LOCALSTACK_ENDPOINT" s3 rb \
        "s3://$PXB_BUCKET" --force >/dev/null 2>&1 || true
      aws --endpoint-url="$LOCALSTACK_ENDPOINT" s3 mb \
        "s3://$PXB_BUCKET" >/dev/null
      ;;
    *)
      echo "cloud_create_bucket: backend $PXB_CLOUD_BACKEND not handled yet"
      return 1
      ;;
  esac
}

# Print the --cloud-* flag set for the configured backend.
cloud_xtrabackup_flags() {
  case "$PXB_CLOUD_BACKEND" in
    s3)
      cat <<EOF
--cloud-storage=s3
--cloud-endpoint=$LOCALSTACK_ENDPOINT
--cloud-s3-bucket=$PXB_BUCKET
--cloud-region=$AWS_DEFAULT_REGION
--cloud-access-key=$AWS_ACCESS_KEY_ID
--cloud-secret-key=$AWS_SECRET_ACCESS_KEY
--cloud-bucket-lookup=path
EOF
      ;;
    gcs)
      cat <<EOF
--cloud-storage=gcs
--cloud-endpoint=$LOCALSTACK_ENDPOINT
--cloud-google-bucket=$PXB_BUCKET
--cloud-region=$AWS_DEFAULT_REGION
--cloud-access-key=$AWS_ACCESS_KEY_ID
--cloud-secret-key=$AWS_SECRET_ACCESS_KEY
--cloud-bucket-lookup=path
EOF
      ;;
    azure)
      cat <<EOF
--cloud-storage=azure
--cloud-azure-endpoint=http://localhost:10000/devstoreaccount1
--cloud-azure-account=devstoreaccount1
--cloud-azure-access-key=Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==
--cloud-azure-container-name=$PXB_BUCKET
EOF
      ;;
    swift)
      cat <<EOF
--cloud-storage=swift
--cloud-url=http://localhost:8080/v1/AUTH_test/$PXB_BUCKET
EOF
      ;;
  esac
}

cloud_xbcloud_flags() {
  # Same as xtrabackup but with xbcloud's option spelling.
  case "$PXB_CLOUD_BACKEND" in
    s3|gcs)
      cat <<EOF
--storage=$PXB_CLOUD_BACKEND
--s3-endpoint=$LOCALSTACK_ENDPOINT
--s3-bucket=$PXB_BUCKET
--s3-region=$AWS_DEFAULT_REGION
--s3-access-key=$AWS_ACCESS_KEY_ID
--s3-secret-key=$AWS_SECRET_ACCESS_KEY
--s3-bucket-lookup=path
EOF
      ;;
  esac
}
