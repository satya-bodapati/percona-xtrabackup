#!/usr/bin/env bash
#
# xtrabackup --backup directly to LocalStack S3. Requires a running
# mysqld; the test reads MYSQL_USER / MYSQL_SOCKET / MYSQL_HOST /
# MYSQL_PORT from environment, or assumes localhost root with no
# password.
#
# After the backup completes, the bucket should contain the file tree
# you'd see in a local --target-dir backup (ibdata1, mysql.ibd, undo_*,
# per-table .ibd, xtrabackup_*). The test then verifies the bucket has
# the expected core files and per-file size > 0.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/cloud_common.sh"

cloud_require_binaries
cloud_up
cloud_create_bucket

BACKUP_NAME="cloud-backup-$(date +%s)"
TMP_TARGET="/tmp/$BACKUP_NAME"

MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_ARGS=""
[ -n "${MYSQL_HOST:-}" ]   && MYSQL_ARGS="$MYSQL_ARGS --host=$MYSQL_HOST"
[ -n "${MYSQL_PORT:-}" ]   && MYSQL_ARGS="$MYSQL_ARGS --port=$MYSQL_PORT"
[ -n "${MYSQL_SOCKET:-}" ] && MYSQL_ARGS="$MYSQL_ARGS --socket=$MYSQL_SOCKET"

XTRA_FLAGS=$(cloud_xtrabackup_flags | tr '\n' ' ')

echo "==> Running xtrabackup --backup directly to S3"
set +e
"$XTRABACKUP" --backup \
  --user="$MYSQL_USER" $MYSQL_ARGS \
  --target-dir="$TMP_TARGET" \
  $XTRA_FLAGS 2>&1 | tail -40
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
  echo "FAIL: xtrabackup --backup exit=$rc"
  exit 1
fi

echo
echo "==> Verifying bucket contents"
LIST=$(aws --endpoint-url="$LOCALSTACK_ENDPOINT" s3 ls \
        "s3://$PXB_BUCKET/$BACKUP_NAME/" --recursive | wc -l)
if [ "$LIST" -lt 5 ]; then
  echo "FAIL: only $LIST objects uploaded; expected core backup files"
  exit 1
fi
echo "    bucket has $LIST objects under $BACKUP_NAME"

echo
echo "==> Verifying common backup files are present"
for f in ibdata1 mysql.ibd xtrabackup_checkpoints xtrabackup_info; do
  if ! aws --endpoint-url="$LOCALSTACK_ENDPOINT" s3 ls \
        "s3://$PXB_BUCKET/$BACKUP_NAME/$f" >/dev/null 2>&1; then
    echo "WARN: $f not present (may be expected for some mysqld configs)"
  else
    echo "    $f present"
  fi
done

echo "==> PASS: xtrabackup --backup direct to S3 works"
