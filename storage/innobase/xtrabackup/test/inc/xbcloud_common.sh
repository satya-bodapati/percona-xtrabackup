################################################################################
# Test xbcloud
#
# Set following environment variables to enable this test:
#     XBCLOUD_CREDENTIALS
#
# Example:
#     export XBCLOUD_CREDENTIALS="--storage=swift \
#         --swift-url=http://192.168.8.80:8080/ \
#         --swift-user=test:tester \
#         --swift-key=testing \
#         --swift-container=test_backup"
#
# NOTE: Do not set XBCLOUD_CREDENTIALS with quotes like:
# export XBCLOUD_CREDENTIALS="--storage='swift'" Only use the sorounding double
# quotes.
################################################################################
. inc/common.sh

MYSQLD_EXTRA_MY_CNF_OPTS="
secure-file-priv=$TEST_VAR_ROOT
"
is_galera && skip_test "skipping"

function is_xbcloud_credentials_set() {
  if [ -z ${XBCLOUD_CREDENTIALS+x} ];
  then
    skip_test "Requires XBCLOUD_CREDENTIALS"
  fi
}

now=$(date +%s)
uuid=($(cat /proc/sys/kernel/random/uuid))
full_backup_name=${now}-${uuid}-full_backup
inc_backup_name=${now}-${uuid}-inc_backup
inc2_backup_name=${now}-${uuid}-inc2_backup

full_backup_dir=$topdir/${full_backup_name}
inc_backup_dir=$topdir/${inc_backup_name}

function write_credentials() {
  # write credentials into xbcloud.cnf
  echo '[xbcloud]' > $topdir/xbcloud.cnf
  echo ${XBCLOUD_CREDENTIALS} | sed 's/ *--/\'$'\n/g' >> $topdir/xbcloud.cnf
}

function is_minio_server() {
  if [[ "$XBCLOUD_CREDENTIALS" =~ .*"s3-endpoint".* ]]; then
    ENDPOINT=$(echo ${XBCLOUD_CREDENTIALS} | awk -F's3-endpoint=' '{print $2}' | awk '{print $1}' | tr -d "'" | tr -d '\\')
    SERVER=$(curl -sI ${ENDPOINT} | grep Server)
    if [[ "$SERVER" =~ .*"MinIO".* ]]; then
      return 0
    fi
  fi
  return 1
}

function is_ec2_with_profile() {
  TOKEN=`curl -sS --connect-timeout 3 -X PUT "http://169.254.169.254/latest/api/token" -H "X-aws-ec2-metadata-token-ttl-seconds: 21600"` || \
  return 1

  PROFILE=`curl -sS --connect-timeout 3 -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/iam/security-credentials/` || \
  return 1

  curl -sS --connect-timeout 3 -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/iam/security-credentials/${PROFILE} || \
  return 1
}

xbcloud_cleanup() {
     xbcloud --defaults-file=$topdir/xbcloud.cnf delete --parallel=10 ${full_backup_name}
     xbcloud --defaults-file=$topdir/xbcloud.cnf delete --parallel=10 ${inc_backup_name}
     xbcloud --defaults-file=$topdir/xbcloud.cnf delete --parallel=10 ${inc2_backup_name}
}

# Pre-create the S3 bucket extracted from XBCLOUD_CREDENTIALS.
# Required after percona-xtrabackup PR #1717, which removes bucket auto-create
# from xbcloud_put. Safe no-op for non-S3 storage, when no endpoint is set
# (real AWS - assume bucket exists), or when docker CLI is not available.
function prepare_xbcloud_bucket() {
  if [[ ! "$XBCLOUD_CREDENTIALS" =~ "s3-bucket=" ]]; then
    return 0
  fi
  if [[ ! "$XBCLOUD_CREDENTIALS" =~ "s3-endpoint=" ]]; then
    return 0
  fi
  if ! command -v docker >/dev/null 2>&1; then
    vlog "prepare_xbcloud_bucket: docker CLI not available; skipping (assuming bucket pre-exists)"
    return 0
  fi

  local bucket=$(echo "$XBCLOUD_CREDENTIALS" \
    | awk -F's3-bucket=' '{print $2}' | awk '{print $1}' | tr -d "'")
  local access=$(echo "$XBCLOUD_CREDENTIALS" \
    | awk -F's3-access-key=' '{print $2}' | awk '{print $1}' | tr -d "'")
  local secret=$(echo "$XBCLOUD_CREDENTIALS" \
    | awk -F's3-secret-key=' '{print $2}' | awk '{print $1}' | tr -d "'")
  local container=${MINIO_CONTAINER:-s3}

  [[ -z "$bucket" || -z "$access" || -z "$secret" ]] && return 0

  # The image entrypoint hardcodes 'admin/password' for the in-container mc alias.
  # That fails silently when minio is started with USER/PASSWORD env vars, so
  # repoint with the real root creds before mb.
  docker exec "$container" mc alias set s3 http://127.0.0.1:9000 "$access" "$secret" \
    >/dev/null 2>&1 || true
  docker exec "$container" mc mb -p "s3/${bucket}" >/dev/null 2>&1 || true
  vlog "prepare_xbcloud_bucket: ensured s3/${bucket} exists (via docker exec ${container})"
}

# Thin parsers that extract a single value from XBCLOUD_CREDENTIALS.
# These return the value on stdout or an empty string when not present.
function xbcloud_get_endpoint() {
  echo "$XBCLOUD_CREDENTIALS" | awk -F's3-endpoint=' '{print $2}' | awk '{print $1}' | tr -d "'"
}

function xbcloud_get_bucket() {
  echo "$XBCLOUD_CREDENTIALS" | awk -F's3-bucket=' '{print $2}' | awk '{print $1}' | tr -d "'"
}

function xbcloud_get_root_access_key() {
  echo "$XBCLOUD_CREDENTIALS" | awk -F's3-access-key=' '{print $2}' | awk '{print $1}' | tr -d "'"
}

function xbcloud_get_root_secret_key() {
  echo "$XBCLOUD_CREDENTIALS" | awk -F's3-secret-key=' '{print $2}' | awk '{print $1}' | tr -d "'"
}

# Gate function for the IAM-based permission regression tests.
# Returns 0 only when an S3 endpoint is configured, the docker CLI is reachable
# from the test host, and the in-container mc admin alias works against MinIO.
function is_minio_iam_supported() {
  if [[ ! "$XBCLOUD_CREDENTIALS" =~ "s3-endpoint=" ]]; then
    vlog "is_minio_iam_supported: XBCLOUD_CREDENTIALS has no s3-endpoint"
    return 1
  fi
  if ! command -v docker >/dev/null 2>&1; then
    vlog "is_minio_iam_supported: docker CLI not available"
    return 1
  fi
  local container=${MINIO_CONTAINER:-s3}
  if ! docker exec "$container" mc admin info s3 >/dev/null 2>&1; then
    vlog "is_minio_iam_supported: docker exec ${container} mc admin info failed"
    return 1
  fi
  return 0
}

# Create a MinIO IAM user with an attached policy. Idempotent (safe to rerun).
# Args: $1=username, $2=password, $3=policy JSON content (as a string).
# The user's attached policy is named "${user}_pol".
function minio_create_user_with_policy() {
  local user=$1
  local pass=$2
  local policy_json=$3
  local container=${MINIO_CONTAINER:-s3}

  printf '%s' "$policy_json" \
    | docker exec -i "$container" sh -c "cat > /tmp/${user}_pol.json"
  docker exec "$container" mc admin policy create s3 "${user}_pol" \
    "/tmp/${user}_pol.json" >/dev/null 2>&1 || true
  docker exec "$container" mc admin user add s3 "$user" "$pass" \
    >/dev/null 2>&1 || true
  docker exec "$container" mc admin policy attach s3 "${user}_pol" \
    --user "$user" >/dev/null 2>&1 || true
  vlog "minio_create_user_with_policy: created user ${user} with policy ${user}_pol"
}

# Remove a MinIO IAM user and its associated policy created by
# minio_create_user_with_policy. Idempotent.
function minio_delete_user() {
  local user=$1
  local container=${MINIO_CONTAINER:-s3}

  docker exec "$container" mc admin user remove s3 "$user" \
    >/dev/null 2>&1 || true
  docker exec "$container" mc admin policy remove s3 "${user}_pol" \
    >/dev/null 2>&1 || true
  vlog "minio_delete_user: removed user ${user} and policy ${user}_pol"
}

# Force XBCLOUD_CREDENTIALS to point at a per-test unique bucket so that
# concurrent test workers do not race on bucket-level state (mc mb /
# mc rb) when sharing a single MinIO instance. After this call all helpers
# that derive from XBCLOUD_CREDENTIALS (write_credentials,
# prepare_xbcloud_bucket, xbcloud_get_bucket, ...) see the new name.
#
# The caller's XBCLOUD_CREDENTIALS does NOT need to include --s3-bucket=,
# since the bucket is owned by the test itself; we append one if missing
# and overwrite one if present. The unique suffix uses $now + $uuid
# (per-worker unique, recomputed each time the test is sourced). Caller
# can pass a short prefix to keep mc ls output readable.
function xbcloud_use_unique_bucket() {
  local prefix=${1:-pxbperm}
  # MinIO bucket names must be lowercase, 3-63 chars, [a-z0-9-]; the
  # prefix + 10 digit timestamp + 36 char UUID with hyphens stays well
  # under 63.
  local bucket="${prefix}-${now}-${uuid}"
  if [[ "$XBCLOUD_CREDENTIALS" =~ --s3-bucket= ]]; then
    XBCLOUD_CREDENTIALS=$(echo "$XBCLOUD_CREDENTIALS" \
      | sed "s|--s3-bucket=[^ ]*|--s3-bucket=${bucket}|")
  else
    XBCLOUD_CREDENTIALS="${XBCLOUD_CREDENTIALS} --s3-bucket=${bucket}"
  fi
  vlog "xbcloud_use_unique_bucket: switched to ${bucket}"
}

# Remove a MinIO bucket and all objects inside it. Used as the per-test
# cleanup hook when the test owns its bucket (see xbcloud_use_unique_bucket).
# Idempotent: missing bucket, missing container, or missing docker are
# silent no-ops.
function xbcloud_remove_bucket() {
  local bucket=$1
  [[ -z "$bucket" ]] && return 0
  if ! command -v docker >/dev/null 2>&1; then
    return 0
  fi
  docker exec "${MINIO_CONTAINER:-s3}" mc rb --force "s3/${bucket}" \
    >/dev/null 2>&1 || true
  vlog "xbcloud_remove_bucket: removed s3/${bucket}"
}
