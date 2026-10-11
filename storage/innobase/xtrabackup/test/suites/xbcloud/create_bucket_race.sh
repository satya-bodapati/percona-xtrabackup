################################################################################
# xbcloud put creates the bucket when it does not exist.  It first asks whether
# the bucket exists and then creates it, so two clients that start at the same
# time can both see "no bucket" and both send CreateBucket.  The loser gets
# BucketAlreadyOwnedByYou.  Before the fix xbcloud treated that as a failure
# and aborted the upload, which also broke the pipe of the xtrabackup feeding
# it.  On CI this showed up as random failures of xbcloud tests that start
# together on a fresh MinIO (most often on ASAN, where everything is slower).
#
# Start several puts at once into a bucket that does not exist yet; all of
# them must succeed.  Repeat with new buckets to make the race likely.
################################################################################
. inc/xbcloud_common.sh
is_xbcloud_credentials_set
is_minio_server || skip_test "requires MinIO"

MC_BIN=${XBCLOUD_MC:-$(command -v mc 2>/dev/null || true)}
[ -n "$MC_BIN" ] && [ -x "$MC_BIN" ] \
    || skip_test "requires the MinIO client: set XBCLOUD_MC or put mc in PATH"

ENDPOINT=$(echo "$XBCLOUD_CREDENTIALS" \
    | awk -F's3-endpoint=' '{print $2}' | awk '{print $1}' | tr -d "'")
ROOT_KEY=$(echo "$XBCLOUD_CREDENTIALS" \
    | awk -F's3-access-key=' '{print $2}' | awk '{print $1}' | tr -d "'")
ROOT_SECRET=$(echo "$XBCLOUD_CREDENTIALS" \
    | awk -F's3-secret-key=' '{print $2}' | awk '{print $1}' | tr -d "'")
[ -n "$ENDPOINT" ] && [ -n "$ROOT_KEY" ] && [ -n "$ROOT_SECRET" ] \
    || skip_test "XBCLOUD_CREDENTIALS lacks s3-endpoint/access-key/secret-key"

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

workdir=$topdir/create_bucket_race
rm -rf $workdir
mkdir -p $workdir

MC="$MC_BIN --config-dir $workdir/mcconf"
export MC_HOST_pxb="${ENDPOINT%%://*}://${ROOT_KEY}:${ROOT_SECRET}@${ENDPOINT#*://}"

BUCKETS=""
cleanup_create_bucket_race() {
    local rc=$?
    for b in $BUCKETS; do
        $MC rb --force "pxb/${b}" >/dev/null 2>&1 || true
    done
    return $rc
}
trap cleanup_create_bucket_race EXIT

# One real backup, streamed to a file, so every put below uploads the same
# complete backup.
vlog "take a streamed backup"
xtrabackup --backup --stream=xbstream --target-dir=$workdir/backup \
    > $workdir/stream.xbs || die "backup failed"

PUTS=12
ROUNDS=3

for round in $(seq 1 $ROUNDS); do
    BUCKET="pxbrace-${uuid}-${round}"
    BUCKETS="$BUCKETS $BUCKET"
    XB_FLAGS="--storage=s3 --s3-endpoint=${ENDPOINT} --s3-bucket=${BUCKET} \
--s3-access-key=${ROOT_KEY} --s3-secret-key=${ROOT_SECRET} --s3-bucket-lookup=path"

    vlog "round $round: $PUTS concurrent puts into new bucket $BUCKET"
    pids=""
    for i in $(seq 1 $PUTS); do
        xbcloud put --parallel=2 $XB_FLAGS race_${i} \
            < $workdir/stream.xbs > $workdir/put_${round}_${i}.log 2>&1 &
        pids="$pids $!"
    done

    failed=0
    for p in $pids; do
        wait $p || failed=$((failed + 1))
    done

    if [ $failed -ne 0 ]; then
        cat $workdir/put_${round}_*.log >&2
        die "round $round: $failed of $PUTS concurrent puts failed"
    fi
done

vlog "all concurrent puts succeeded"

# The uploads must be usable backups: download one and prepare it.
restore_dir=$workdir/restore
mkdir -p $restore_dir
run_cmd xbcloud get --parallel=4 $XB_FLAGS race_1 | xbstream -x -C $restore_dir \
    || die "could not download race_1"
xtrabackup --prepare --target-dir=$restore_dir
vlog "downloaded backup prepared"
