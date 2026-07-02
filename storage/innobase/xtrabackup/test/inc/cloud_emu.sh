############################################################################
# Reusable cloud-emulator harness for cloud-backed tests.
#
# Brings up four provider-authentic emulators in Docker:
#
#   - localstack     : S3 (LocalStack community, port 4566)
#   - fake-gcs-server: GCS (fsouza/fake-gcs-server, port 4443)
#   - azurite        : Azure Blob (mcr.microsoft.com/azure-storage/azurite,
#                                  port 10000)
#   - openstackswift : Swift TempAuth (openstackswift/saio, port 8080)
#
# Cherry-picked and adapted from the 9.7 pxb-9.7-PXB-3671 branch (PXB-3843
# cloud-emulator test harness).  Two differences vs the 9.7 original:
#
#   1. The compose file is co-located here under test/inc/, not under
#      src/xbcloud/prototype/, so the harness is self-contained.
#   2. The xbcloud CLI flag emitter is cloud_emu_xbcloud_flags() and
#      emits xbcloud 8.4-shaped options (--storage, --s3-bucket, ...)
#      rather than the ds_cloud-only --cloud-* options from 9.7.  The
#      9.7 ds_cloud path does not exist on 8.4.
#
# Usage from a test under test/t/*.sh or test/suites/*/*.sh:
#
#   . inc/cloud_emu.sh
#   cloud_emu_require_docker      # skip cleanly if docker not installed
#   cloud_emu_start               # bring up the stack (idempotent)
#   trap cloud_emu_stop EXIT      # tear down on exit
#   cloud_emu_wait_for s3         # wait until s3 endpoint answers
#   cloud_emu_make_bucket s3 my-bucket
#   xbcloud_flags=$(cloud_emu_xbcloud_flags s3 my-bucket)
#   xbstream -c ... | xbcloud put $xbcloud_flags
#
# Set CLOUD_EMU_KEEP=1 in the environment to keep the emulator stack
# running across tests (dev workflow / long-lived Jenkins fixtures).
############################################################################

CLOUD_EMU_COMPOSE_FILE=${CLOUD_EMU_COMPOSE_FILE:-\
${XB_TEST_DIR:-$PWD}/inc/cloud_emu_compose.yml}

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
  # If the caller has the stack already running (dev box or a Jenkins
  # job that brings the stack up once across many tests), let them
  # opt out of the bring-up via CLOUD_EMU_KEEP.
  [ -n "${CLOUD_EMU_KEEP:-}" ] && return 0
  [ -f "$CLOUD_EMU_COMPOSE_FILE" ] \
    || die "cloud_emu: compose file not found at $CLOUD_EMU_COMPOSE_FILE"
  _cloud_emu_compose -f "$CLOUD_EMU_COMPOSE_FILE" up -d >/dev/null \
    || die "cloud_emu: docker-compose up failed"
}

cloud_emu_stop() {
  # Same CLOUD_EMU_KEEP opt-out.  `down -v` removes volumes, which is
  # destructive for any state the user has placed there interactively.
  [ -n "${CLOUD_EMU_KEEP:-}" ] && return 0
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
  local i
  for i in $(seq 1 60); do
    # `curl -f` treats 4xx as a failure -- but Swift's TempAuth endpoint
    # returns 401 to an unauthenticated GET, which still proves the
    # service is up.  Treat any 2xx OR 401 as "ready".
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$url" \
           2>/dev/null || echo 000)
    case "$code" in
      2*|401) return 0 ;;
    esac
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
      # fake-gcs-server: POST /storage/v1/b creates a bucket
      curl -fs -X POST -H "Content-Type: application/json" \
           -d "{\"name\":\"$name\"}" \
           "$CLOUD_EMU_GCS_ENDPOINT/storage/v1/b?project=test" >/dev/null \
        || die "cloud_emu_make_bucket gcs: failed for $name"
      ;;
    azure)
      # Azurite: PUT /account/container?restype=container
      curl -fs -X PUT \
           "$CLOUD_EMU_AZURE_BLOB_ENDPOINT/$name?restype=container" \
           -H "x-ms-version: 2021-12-02" \
           -H "x-ms-date: $(date -u '+%a, %d %b %Y %H:%M:%S GMT')" >/dev/null \
        || die "cloud_emu_make_bucket azure: failed for $name"
      ;;
    swift)
      # Get token first, then PUT /v1/<account>/<container>.  GET with
      # -D- (dump headers) rather than HEAD: openstackswift's TempAuth
      # returns 405 for HEAD but 200 + X-Auth-Token for GET.
      local token
      token=$(curl -fs -D- \
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

# cloud_emu_xbcloud_flags s3|gcs|azure|swift <bucket-or-container>
#
# Emit the xbcloud CLI flags for the given emulator backend to stdout.
# The caller can concatenate them into an xbcloud invocation:
#
#   flags=$(cloud_emu_xbcloud_flags s3 my-bucket)
#   xbstream -c ... | xbcloud put --parallel=4 $flags obj-name
#
# Uses the xbcloud 8.4 CLI shape (--storage=..., --s3-*, --google-*,
# --azure-*, --swift-*).  When 8.4 gains new authentication options
# (e.g. --google-service-account-file for PXB-3592), tests that want to
# exercise those options should build their own flag string rather
# than reuse this helper.
cloud_emu_xbcloud_flags() {
  local provider="$1" bucket="$2"
  case "$provider" in
    s3)
      echo "--storage=s3" \
           "--s3-endpoint=$CLOUD_EMU_S3_ENDPOINT" \
           "--s3-bucket=$bucket" \
           "--s3-region=us-east-1" \
           "--s3-bucket-lookup=path" \
           "--s3-access-key=test" \
           "--s3-secret-key=test"
      ;;
    gcs)
      echo "--storage=google" \
           "--google-endpoint=$CLOUD_EMU_GCS_ENDPOINT" \
           "--google-bucket=$bucket" \
           "--google-region=auto" \
           "--google-access-key=test" \
           "--google-secret-key=test"
      ;;
    azure)
      echo "--storage=azure" \
           "--azure-endpoint=$CLOUD_EMU_AZURE_BLOB_ENDPOINT" \
           "--azure-container-name=$bucket" \
           "--azure-storage-account=$CLOUD_EMU_AZURE_ACCOUNT" \
           "--azure-access-key=$CLOUD_EMU_AZURE_KEY"
      ;;
    swift)
      # openstackswift/saio defaults: TempAuth (--swift-auth-version 1.0)
      # at /auth/v1.0 with credentials test:tester / testing.
      echo "--storage=swift" \
           "--swift-auth-url=$CLOUD_EMU_SWIFT_ENDPOINT" \
           "--swift-auth-version=1.0" \
           "--swift-user=$CLOUD_EMU_SWIFT_USER" \
           "--swift-key=$CLOUD_EMU_SWIFT_KEY" \
           "--swift-container=$bucket"
      ;;
    *) die "cloud_emu_xbcloud_flags: unknown provider '$provider'" ;;
  esac
}
