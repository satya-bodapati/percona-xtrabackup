################################################################################
# Regression test for percona-xtrabackup PR #1717
#
# R1 - Push-only user.
# A MinIO IAM user with s3:ListBucket + s3:PutObject (no s3:GetObject)
# successfully runs `xbcloud put` today because the pre-flight probe is
# HeadBucket (which only requires s3:ListBucket).
#
# After PR #1717 the probe becomes HeadObject on bucket/<backup_name>, which
# requires s3:GetObject; this test starts failing at "Probe failed".
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
xbcloud_use_unique_bucket pushonly
write_credentials
prepare_xbcloud_bucket

ENDPOINT=$(xbcloud_get_endpoint)
BUCKET=$(xbcloud_get_bucket)
PUSH_USER="pushonly_${now}_$$"
PUSH_PASS="pushonlysecret"

PUSH_POLICY=$(cat <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow",
      "Action": "s3:ListBucket",
      "Resource": "arn:aws:s3:::${BUCKET}" },
    { "Effect": "Allow",
      "Action": "s3:PutObject",
      "Resource": "arn:aws:s3:::${BUCKET}/*" }
  ]
}
EOF
)

minio_create_user_with_policy "$PUSH_USER" "$PUSH_PASS" "$PUSH_POLICY"

cleanup_push_only() {
    local rc=$?
    # We own the bucket; wipe it wholesale rather than enumerating backups.
    xbcloud_remove_bucket "$BUCKET"
    minio_delete_user $PUSH_USER
    return $rc
}
trap cleanup_push_only EXIT

load_dbase_schema sakila
load_dbase_data sakila

vlog "R1: push-only user (ListBucket + PutObject) should be able to xbcloud put"

# Use PIPESTATUS to capture both pipeline segment exit codes; a failure in
# the right-hand xbcloud segment is masked by the left-hand xtrabackup exit
# in plain `$?`. The right-hand segment also runs in a subshell, so `set -e`
# is not reliable here. Combined with the grep for "Probe failed" below, this
# lets us distinguish the PR #1717 regression from any unrelated failure
# (e.g. transient xtrabackup setup races, network blips).
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
        --s3-access-key=$PUSH_USER \
        --s3-secret-key=$PUSH_PASS \
        --s3-bucket=$BUCKET \
        --parallel=4 \
        ${full_backup_name} > $LOG 2>&1
# Snapshot atomically: bash updates PIPESTATUS after every subsequent
# command (including assignments), so two consecutive ${PIPESTATUS[N]}
# reads would give wrong values for index 1.
ps=("${PIPESTATUS[@]}")
rc_xtra=${ps[0]}
rc_xbc=${ps[1]}
set -e

cat $LOG
vlog "R1: xtrabackup exit=$rc_xtra, xbcloud exit=$rc_xbc"

if grep -q "Probe failed" $LOG; then
    die "R1 PR#1717 regression detected: xbcloud probe failed for push-only IAM user (ListBucket+PutObject only). See $LOG"
fi

if [ $rc_xbc -ne 0 ]; then
    die "R1: xbcloud put exited with $rc_xbc unexpectedly (not at probe; xtrabackup=$rc_xtra). See $LOG"
fi
