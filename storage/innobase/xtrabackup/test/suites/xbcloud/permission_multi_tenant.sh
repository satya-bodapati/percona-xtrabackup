################################################################################
# Regression test for percona-xtrabackup PR #1717 - motivating use case.
#
# R4 - Multi-tenant S3 bucket with prefix-conditioned ListBucket.
#
# This is the exact scenario PR #1717's description targets: a multi-tenant
# bucket where each tenant has permissions scoped to their own prefix and
# MUST NOT be able to enumerate other tenants' object names.
#
# The tenant's IAM policy is:
#   s3:ListBucket  on arn:aws:s3:::<bucket>
#     conditioned on StringLike s3:prefix = tenant1/*
#     (so HeadBucket - which carries no prefix - is denied, and so is
#     ListObjectsV2 with no prefix; only ListObjectsV2 with a tenant1/*
#     prefix succeeds, which is what xbcloud_put / xbcloud_delete actually
#     call after the probe)
#   s3:GetObject / s3:PutObject / s3:DeleteObject on
#     arn:aws:s3:::<bucket>/tenant1/*
#
# Backup name is chosen as tenant1/<unique> so that:
#   - HeadObject probe (PR #1717's probe) targets bucket/tenant1/<unique>,
#     which matches the GetObject resource and returns 404 -> probe pass.
#   - HeadBucket probe (our probe_endpoint) targets bucket itself,
#     gets 403 AccessDenied, which probe_endpoint accepts as a valid
#     signature -> probe pass.
#   - ListObjectsV2 with prefix tenant1/<unique>/ matches the
#     s3:prefix StringLike condition -> allowed.
#   - PutObject on bucket/tenant1/<unique>/foo matches the GetObject /
#     PutObject resource -> allowed.
#
# Expected outcomes by build:
#
#   rel-8406 HEAD (today)         FAIL - HeadBucket probe denied
#   rel-8406 + PR #1717 alone     PASS (this is what PR #1717 enables)
#   rel-8406 + our probe fix only FAIL - container_exists() still does
#                                          HeadBucket and returns failure
#                                          on 403
#   rel-8406 + our full fix       PASS - probe_endpoint accepts 403,
#                                        and the container_exists pre-check
#                                        in xbcloud_put has been removed
#                                        (same change PR #1717 makes)
#
# This test exists so the no-regression suite (R1, R1b, R2) is not the
# whole picture: it complements them by proving the multi-tenant scenario
# - PR #1717's reason for existing - actually works under our combined fix.
################################################################################
. inc/xbcloud_common.sh
is_xbcloud_credentials_set
is_minio_iam_supported || skip_test "permission tests require MinIO + docker exec access"

start_server --innodb_file_per_table

# Per-test unique bucket so concurrent run.sh workers do not race
# (same approach as the other R-tests).
xbcloud_use_unique_bucket multitenant
write_credentials
prepare_xbcloud_bucket

ENDPOINT=$(xbcloud_get_endpoint)
BUCKET=$(xbcloud_get_bucket)
TENANT_PREFIX="tenant1"
MT_USER="multitenant_${now}_$$"
MT_PASS="multitenantsecret"
# backup_name lives INSIDE the tenant's prefix - this is the realistic
# multi-tenant naming convention (tenantA/2026-05-21-backup, etc.) and
# what makes the IAM scope match the runtime object keys.
MT_BACKUP="${TENANT_PREFIX}/backup_${now}_$$"

# Note the conditional ListBucket - this is the key element that
# distinguishes a multi-tenant deployment from R2 (which has unconditional
# ListBucket on the whole bucket).
MT_POLICY=$(cat <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow",
      "Action": "s3:ListBucket",
      "Resource": "arn:aws:s3:::${BUCKET}",
      "Condition": {
        "StringLike": {
          "s3:prefix": [ "${TENANT_PREFIX}/*" ]
        }
      } },
    { "Effect": "Allow",
      "Action": [ "s3:GetObject", "s3:PutObject", "s3:DeleteObject" ],
      "Resource": "arn:aws:s3:::${BUCKET}/${TENANT_PREFIX}/*" }
  ]
}
EOF
)

minio_create_user_with_policy "$MT_USER" "$MT_PASS" "$MT_POLICY"

cleanup_multi_tenant() {
    local rc=$?
    xbcloud_remove_bucket "$BUCKET"
    minio_delete_user $MT_USER
    return $rc
}
trap cleanup_multi_tenant EXIT

load_dbase_schema sakila
load_dbase_data sakila

vlog "R4: multi-tenant user (prefix-conditioned ListBucket + scoped Get/Put/Del on ${TENANT_PREFIX}/*) should be able to xbcloud put with backup_name=${MT_BACKUP}"

LOG=${OUTFILE}.xbcloud_put.log
set +e
xtrabackup --backup --stream=xbstream --extra-lsndir=$full_backup_dir \
           --target-dir=$full_backup_dir | \
    xbcloud put \
        --storage=s3 \
        --s3-endpoint=$ENDPOINT \
        --s3-bucket-lookup=path \
        --s3-region=us-east-1 \
        --s3-access-key=$MT_USER \
        --s3-secret-key=$MT_PASS \
        --s3-bucket=$BUCKET \
        --parallel=4 \
        ${MT_BACKUP} > $LOG 2>&1
ps=("${PIPESTATUS[@]}")
rc_xtra=${ps[0]}
rc_xbc=${ps[1]}
set -e

cat $LOG
vlog "R4: xtrabackup exit=$rc_xtra, xbcloud exit=$rc_xbc"

# Distinguish "the bug PR #1717 is about" (probe rejects the request) from
# the post-probe gap (probe passes but container_exists still does
# HeadBucket and fails). Both manifest as xbcloud exit != 0; the messages
# differ in the log.
if grep -q "Probe failed" $LOG; then
    die "R4 PR#1717 motivating case FAILED at probe: HeadBucket-style \
probe was denied by IAM (no s3:prefix in HeadBucket request -> condition \
does not match). Either probe_endpoint is broken or the probe is still \
the strict HeadObject variant. See $LOG"
fi

if [ $rc_xbc -ne 0 ]; then
    die "R4 PR#1717 motivating case FAILED post-probe (xbcloud exit=$rc_xbc, \
no 'Probe failed' in log). Most likely cause: xbcloud_put still calls \
container_exists() which does HeadBucket and returns failure on 403 \
AccessDenied. Both the probe AND the container_exists pre-check must be \
made permission-tolerant (or the pre-check removed) for this scenario to \
work end-to-end. See $LOG"
fi
