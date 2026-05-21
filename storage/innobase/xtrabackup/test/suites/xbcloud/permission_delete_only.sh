################################################################################
# Regression test for percona-xtrabackup PR #1717
#
# R1b - Delete-only retention user.
# A MinIO IAM user with s3:ListBucket + s3:DeleteObject (no s3:GetObject)
# successfully runs `xbcloud delete` today because the pre-flight probe is
# HeadBucket (which only requires s3:ListBucket).
#
# After PR #1717 the probe becomes HeadObject on bucket/<backup_name>, which
# requires s3:GetObject; this test starts failing at "Probe failed".
#
# Workflow:
#   1. Upload a backup using root credentials (from XBCLOUD_CREDENTIALS).
#   2. Delete the same backup using the restricted delete-only user.
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
xbcloud_use_unique_bucket deleteonly
write_credentials
prepare_xbcloud_bucket

ENDPOINT=$(xbcloud_get_endpoint)
BUCKET=$(xbcloud_get_bucket)
DEL_USER="deleteonly_${now}_$$"
DEL_PASS="deleteonlysecret"

DEL_POLICY=$(cat <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow",
      "Action": "s3:ListBucket",
      "Resource": "arn:aws:s3:::${BUCKET}" },
    { "Effect": "Allow",
      "Action": "s3:DeleteObject",
      "Resource": "arn:aws:s3:::${BUCKET}/*" }
  ]
}
EOF
)

minio_create_user_with_policy "$DEL_USER" "$DEL_PASS" "$DEL_POLICY"

cleanup_delete_only() {
    local rc=$?
    # We own the bucket; wipe it wholesale (covers both the case where the
    # restricted-user delete succeeded and the case where it failed at
    # probe and left the uploaded objects behind).
    xbcloud_remove_bucket "$BUCKET"
    minio_delete_user $DEL_USER
    return $rc
}
trap cleanup_delete_only EXIT

load_dbase_schema sakila
load_dbase_data sakila

vlog "R1b: upload a backup with root creds so the delete-only user has something to remove"

# Step 1: upload backup with root creds (must succeed both pre- and post-PR).
# PIPESTATUS lets us distinguish a setup-level pipeline failure (which we
# also want to surface, but as "test setup" rather than "PR regression").
# Write xbcloud logs next to the worker's result file (results/<test>).
# $topdir is wiped by cleanup_worker after the test exits, so logs placed
# there would not survive a failure. $OUTFILE is exported by run.sh.
LOG_UPLOAD=${OUTFILE}.xbcloud_put.log
set +e
xtrabackup --backup --stream=xbstream --extra-lsndir=$full_backup_dir \
           --target-dir=$full_backup_dir | \
    xbcloud --defaults-file=$topdir/xbcloud.cnf put \
        --parallel=4 \
        ${full_backup_name} > $LOG_UPLOAD 2>&1
# Snapshot atomically: bash updates PIPESTATUS after every subsequent
# command (including assignments), so two consecutive ${PIPESTATUS[N]}
# reads would give wrong values for index 1.
ps=("${PIPESTATUS[@]}")
rc_xtra=${ps[0]}
rc_xbc_put=${ps[1]}
set -e

cat $LOG_UPLOAD
vlog "R1b upload: xtrabackup exit=$rc_xtra, xbcloud(put) exit=$rc_xbc_put"

if [ $rc_xbc_put -ne 0 ] || [ $rc_xtra -ne 0 ]; then
    die "R1b: test SETUP failed (root-cred upload, xbcloud=$rc_xbc_put xtrabackup=$rc_xtra). Not the regression we are looking for. See $LOG_UPLOAD"
fi

vlog "R1b: delete-only user (ListBucket + DeleteObject) should be able to xbcloud delete"

# Step 2: the actual regression-sensitive call - delete with restricted creds.
LOG_DELETE=${OUTFILE}.xbcloud_delete.log
set +e
xbcloud delete \
    --storage=s3 \
    --s3-endpoint=$ENDPOINT \
    --s3-bucket-lookup=path \
    --s3-region=us-east-1 \
    --s3-access-key=$DEL_USER \
    --s3-secret-key=$DEL_PASS \
    --s3-bucket=$BUCKET \
    --parallel=4 \
    ${full_backup_name} > $LOG_DELETE 2>&1
rc_del=$?
set -e

cat $LOG_DELETE
vlog "R1b delete: xbcloud exit=$rc_del"

if grep -q "Probe failed" $LOG_DELETE; then
    die "R1b PR#1717 regression detected: xbcloud probe failed for delete-only IAM user (ListBucket+DeleteObject only). See $LOG_DELETE"
fi

if [ $rc_del -ne 0 ]; then
    die "R1b: xbcloud delete exited with $rc_del unexpectedly (not at probe). See $LOG_DELETE"
fi
