################################################################################
# Regression test for percona-xtrabackup PR #1717
#
# R2 - Prefix-scoped permissions with the backup name equal to the literal
# prefix.
#
# A MinIO IAM user with unconditional s3:ListBucket + per-prefix
# s3:GetObject/PutObject/DeleteObject on arn:aws:s3:::<bucket>/tenant1/*
# successfully runs `xbcloud put s3://<bucket>/tenant1` today, because the
# pre-flight probe is HeadBucket (which only requires s3:ListBucket).
#
# After PR #1717 the probe becomes HeadObject on bucket/tenant1; that ARN
# (no slash after "tenant1") does NOT match the IAM resource
# arn:aws:s3:::<bucket>/tenant1/* (which requires a slash plus at least one
# character after "tenant1"). The result is 403 -> "Probe failed".
#
# Workflow:
#   1. The bucket is pre-created by prepare_xbcloud_bucket (root creds).
#   2. The restricted user runs `xbcloud put` with backup_name=tenant1.
#   3. Cleanup deletes the tenant1 backup using root creds.
#
# This test must pass on current trunk and fail after the PR is applied.
################################################################################
. inc/xbcloud_common.sh
is_xbcloud_credentials_set
is_minio_iam_supported || skip_test "permission tests require MinIO + docker exec access"

start_server --innodb_file_per_table

# Use a per-test unique bucket so concurrent run.sh workers do not race on
# the shared MinIO bucket (this was the source of flakes when running the
# permission suite with default parallelism).
xbcloud_use_unique_bucket prefixscope
write_credentials
prepare_xbcloud_bucket

ENDPOINT=$(xbcloud_get_endpoint)
BUCKET=$(xbcloud_get_bucket)
TENANT_PREFIX="tenant1"
TEN_USER="prefixscope_${now}_$$"
TEN_PASS="prefixscopesecret"
TEN_BACKUP="${TENANT_PREFIX}_${now}_$$"

TEN_POLICY=$(cat <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow",
      "Action": "s3:ListBucket",
      "Resource": "arn:aws:s3:::${BUCKET}" },
    { "Effect": "Allow",
      "Action": ["s3:GetObject", "s3:PutObject", "s3:DeleteObject"],
      "Resource": "arn:aws:s3:::${BUCKET}/${TEN_BACKUP}/*" }
  ]
}
EOF
)

minio_create_user_with_policy "$TEN_USER" "$TEN_PASS" "$TEN_POLICY"

cleanup_prefix_scope() {
    local rc=$?
    # We own the bucket; wipe it wholesale.
    xbcloud_remove_bucket "$BUCKET"
    minio_delete_user $TEN_USER
    return $rc
}
trap cleanup_prefix_scope EXIT

load_dbase_schema sakila
load_dbase_data sakila

vlog "R2: prefix-scoped user (Get/Put/Delete on ${TEN_BACKUP}/*) should be able to xbcloud put with backup_name=${TEN_BACKUP}"

# Use PIPESTATUS to capture both pipeline exit codes; grep "Probe failed"
# in xbcloud's output distinguishes the PR #1717 regression from any other
# unrelated non-zero exit.
# Write the xbcloud log next to the worker's result file (results/<test>).
# $topdir is wiped by cleanup_worker after the test exits, so a log placed
# there would not survive a failure. $OUTFILE is exported by run.sh.
LOG=${OUTFILE}.xbcloud_put.log
set +e
xtrabackup --backup --stream=xbstream --extra-lsndir=$full_backup_dir \
           --target-dir=$full_backup_dir | \
    xbcloud put \
        --storage=s3 \
        --s3-endpoint=$ENDPOINT \
        --s3-bucket-lookup=path \
        --s3-region=us-east-1 \
        --s3-access-key=$TEN_USER \
        --s3-secret-key=$TEN_PASS \
        --s3-bucket=$BUCKET \
        --parallel=4 \
        ${TEN_BACKUP} > $LOG 2>&1
# Snapshot atomically: bash updates PIPESTATUS after every subsequent
# command (including assignments), so two consecutive ${PIPESTATUS[N]}
# reads would give wrong values for index 1.
ps=("${PIPESTATUS[@]}")
rc_xtra=${ps[0]}
rc_xbc=${ps[1]}
set -e

cat $LOG
vlog "R2: xtrabackup exit=$rc_xtra, xbcloud exit=$rc_xbc"

if grep -q "Probe failed" $LOG; then
    die "R2 PR#1717 regression detected: xbcloud probe failed for prefix-scoped IAM user (Get/Put/Delete on bucket/${TEN_BACKUP}/*). Backup name = literal prefix '${TEN_BACKUP}'; probe HEAD ${BUCKET}/${TEN_BACKUP} does not match ${BUCKET}/${TEN_BACKUP}/*. See $LOG"
fi

if [ $rc_xbc -ne 0 ]; then
    die "R2: xbcloud put exited with $rc_xbc unexpectedly (not at probe; xtrabackup=$rc_xtra). See $LOG"
fi
