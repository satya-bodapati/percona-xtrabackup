############################################################################
# Reusable cloud-emulator harness for direct-cloud (ds_cloud) tests.
#
# Brings up the four emulators we already use in the prototype/ harness:
#   - localstack    : S3 (LocalStack community, port 4566)
#   - fake-gcs      : GCS-compatible (fsouza/fake-gcs-server, port 4443)
#   - azurite       : Azure Block Blob (microsoft/azurite, port 10000)
#   - openstackswift: Swift all-in-one (openstackswift/saio, port 8080)
#
# Usage from a test/t/<test>.sh or test/suites/cloud-direct/<test>.sh:
#
#   . inc/cloud_emu.sh
#   cloud_emu_require_docker     # die-skip if docker not available
#   cloud_emu_start              # start the stack (idempotent)
#   trap cloud_emu_stop EXIT     # tear down on exit
#   cloud_emu_wait_for s3        # wait for s3 endpoint to answer
#   ...
#   cloud_emu_make_bucket s3 my-bucket
#   cloud_emu_env_for s3         # exports AWS_* / S3_* env vars
#   xtrabackup --backup --cloud-storage=s3 \
#       --cloud-endpoint=$S3_ENDPOINT \
#       --cloud-s3-bucket=my-bucket/2026-06-23-full \
#       --cloud-region=us-east-1 --cloud-bucket-lookup=path \
#       --target-dir=$topdir/B
#
# The provider-explicit bucket option (--cloud-s3-bucket / --cloud-google-
# bucket / --cloud-azure-container-name) accepts either BUCKET or
# BUCKET/PREFIX form -- the prefix portion is the sub-directory inside
# the bucket where THIS backup's objects land.
#
# The emulator stack is shared across tests in a single test-run.  Each
# test must use a unique bucket name (or call cloud_emu_reset_bucket).
############################################################################

CLOUD_EMU_COMPOSE_FILE=${CLOUD_EMU_COMPOSE_FILE:-\
${XB_TEST_DIR:-$PWD}/../src/xbcloud/prototype/docker-compose.yml}

CLOUD_EMU_S3_ENDPOINT=${CLOUD_EMU_S3_ENDPOINT:-http://localhost:4566}
CLOUD_EMU_GCS_ENDPOINT=${CLOUD_EMU_GCS_ENDPOINT:-http://localhost:4443}
CLOUD_EMU_AZURE_BLOB_ENDPOINT=${CLOUD_EMU_AZURE_BLOB_ENDPOINT:-http://localhost:10000/devstoreaccount1}
CLOUD_EMU_SWIFT_ENDPOINT=${CLOUD_EMU_SWIFT_ENDPOINT:-http://localhost:8080/auth/v1.0}

# Azurite's well-known dev account credentials (publicly documented).
CLOUD_EMU_AZURE_ACCOUNT="devstoreaccount1"
CLOUD_EMU_AZURE_KEY="Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw=="

# Swift saio default credentials.
CLOUD_EMU_SWIFT_USER="test:tester"
CLOUD_EMU_SWIFT_KEY="testing"

cloud_emu_require_docker() {
  command -v docker >/dev/null 2>&1 || skip_test "test requires docker"
  command -v docker-compose >/dev/null 2>&1 \
    || command -v docker compose >/dev/null 2>&1 \
    || skip_test "test requires docker-compose"
  command -v curl >/dev/null 2>&1 || skip_test "test requires curl"
  command -v aws >/dev/null 2>&1 || skip_test "test requires aws cli"
}

# Resolve the docker-compose command (v1 'docker-compose' or v2 'docker compose').
_cloud_emu_compose() {
  if command -v docker-compose >/dev/null 2>&1; then
    docker-compose "$@"
  else
    docker compose "$@"
  fi
}

cloud_emu_start() {
  [ -f "$CLOUD_EMU_COMPOSE_FILE" ] \
    || die "cloud_emu: compose file not found at $CLOUD_EMU_COMPOSE_FILE"
  _cloud_emu_compose -f "$CLOUD_EMU_COMPOSE_FILE" up -d >/dev/null \
    || die "cloud_emu: docker-compose up failed"
}

cloud_emu_stop() {
  _cloud_emu_compose -f "$CLOUD_EMU_COMPOSE_FILE" down -v >/dev/null 2>&1 || true
}

# cloud_emu_wait_for s3|gcs|azure|swift
# Polls the corresponding emulator's health endpoint up to 60 s.
cloud_emu_wait_for() {
  local provider="$1"
  local url
  case "$provider" in
    s3)    url="$CLOUD_EMU_S3_ENDPOINT/_localstack/health" ;;
    gcs)   url="$CLOUD_EMU_GCS_ENDPOINT/storage/v1/b"  ;;
    azure) url="$CLOUD_EMU_AZURE_BLOB_ENDPOINT?comp=list" ;;
    swift) url="$CLOUD_EMU_SWIFT_ENDPOINT" ;;
    *)     die "cloud_emu_wait_for: unknown provider '$provider'" ;;
  esac
  for i in $(seq 1 60); do
    if curl -fs --max-time 2 "$url" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  die "cloud_emu_wait_for $provider: timeout after 60s on $url"
}

# cloud_emu_make_bucket s3|gcs|azure|swift <name>
cloud_emu_make_bucket() {
  local provider="$1" name="$2"
  case "$provider" in
    s3)
      AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
      aws --endpoint-url="$CLOUD_EMU_S3_ENDPOINT" \
          s3 rb "s3://$name" --force >/dev/null 2>&1 || true
      AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
      aws --endpoint-url="$CLOUD_EMU_S3_ENDPOINT" \
          s3 mb "s3://$name" >/dev/null \
        || die "cloud_emu_make_bucket s3: mb failed for $name"
      ;;
    gcs)
      # fake-gcs-server: POST to /storage/v1/b creates a bucket
      curl -fs -X POST -H "Content-Type: application/json" \
           -d "{\"name\":\"$name\"}" \
           "$CLOUD_EMU_GCS_ENDPOINT/storage/v1/b?project=test" >/dev/null \
        || die "cloud_emu_make_bucket gcs: failed for $name"
      ;;
    azure)
      # Azurite: PUT https://account/container?restype=container
      curl -fs -X PUT \
           "$CLOUD_EMU_AZURE_BLOB_ENDPOINT/$name?restype=container" \
           -H "x-ms-version: 2021-12-02" \
           -H "x-ms-date: $(date -u '+%a, %d %b %Y %H:%M:%S GMT')" >/dev/null \
        || die "cloud_emu_make_bucket azure: failed for $name"
      ;;
    swift)
      # Get token first, then PUT /v1/<account>/<container>
      local token=$(curl -fs -I \
                    -H "X-Auth-User: $CLOUD_EMU_SWIFT_USER" \
                    -H "X-Auth-Key: $CLOUD_EMU_SWIFT_KEY" \
                    "$CLOUD_EMU_SWIFT_ENDPOINT" | grep -i 'X-Auth-Token:' \
                    | sed 's/.*: //' | tr -d '\r\n')
      [ -n "$token" ] || die "cloud_emu_make_bucket swift: no auth token"
      curl -fs -X PUT -H "X-Auth-Token: $token" \
           "http://localhost:8080/v1/AUTH_test/$name" >/dev/null \
        || die "cloud_emu_make_bucket swift: failed for $name"
      ;;
    *) die "cloud_emu_make_bucket: unknown provider '$provider'" ;;
  esac
}

# Emit the --cloud-* flags for a backend to stdout; the caller can
# pass these directly into xtrabackup invocations.
cloud_emu_xb_flags() {
  local provider="$1" bucket="$2"
  # The `bucket' argument here may be a plain name ("my-bucket") OR
  # a BUCKET/PREFIX combination ("my-bucket/2026-06-23-full"); the
  # PREFIX part lands inside the bucket under a sub-directory and is
  # parsed by xtrabackup's parse_cloud_bucket_with_prefix() helper.
  case "$provider" in
    s3)
      echo "--cloud-storage=s3" \
           "--cloud-endpoint=$CLOUD_EMU_S3_ENDPOINT" \
           "--cloud-s3-bucket=$bucket" \
           "--cloud-region=us-east-1" \
           "--cloud-bucket-lookup=path" \
           "--cloud-access-key=test" \
           "--cloud-secret-key=test"
      ;;
    gcs)
      echo "--cloud-storage=gcs" \
           "--cloud-endpoint=$CLOUD_EMU_GCS_ENDPOINT" \
           "--cloud-google-bucket=$bucket" \
           "--cloud-region=auto" \
           "--cloud-bucket-lookup=path" \
           "--cloud-access-key=test" \
           "--cloud-secret-key=test"
      ;;
    azure)
      echo "--cloud-storage=azure" \
           "--cloud-azure-endpoint=$CLOUD_EMU_AZURE_BLOB_ENDPOINT" \
           "--cloud-azure-container-name=$bucket" \
           "--cloud-azure-account=$CLOUD_EMU_AZURE_ACCOUNT" \
           "--cloud-azure-access-key=$CLOUD_EMU_AZURE_KEY"
      ;;
    swift)
      # Swift options will switch to --cloud-swift-* (Keystone v3) in
      # a follow-up commit.  Until then, this helper does not emit a
      # working swift command-line.  Tests that need swift will skip
      # themselves until the swift batch lands.
      die "cloud_emu_xb_flags: swift requires --cloud-swift-* options " \
          "(not yet implemented); skip the test for now"
      ;;
    *) die "cloud_emu_xb_flags: unknown provider '$provider'" ;;
  esac
}
