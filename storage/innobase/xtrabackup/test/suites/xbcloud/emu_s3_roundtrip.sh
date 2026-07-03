############################################################################
# xbcloud round-trip against LocalStack (S3 emulator).
#
# Full backup → xbcloud put → xbcloud get → --prepare.  Verifies the
# credential-provider migration (C5-C10) hasn't regressed the S3 HMAC
# signing path.  Skips cleanly when docker isn't available.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker

# Start the stack + arrange EXIT-trap teardown.  Bucket names are
# per-test-run unique so successive runs don't collide.
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for s3

bucket="pxb-s3-$(date +%s)-$$"
cloud_emu_make_bucket s3 "$bucket"

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

# Baseline row count on the source.
src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

flags=$(cloud_emu_xbcloud_flags s3 "$bucket")

vlog "Full backup → S3 (LocalStack) via xbstream|xbcloud put"
full_dir=$topdir/full
mkdir -p "$full_dir"
xtrabackup --backup --stream=xbstream --extra-lsndir="$full_dir" \
    --target-dir="$full_dir" \
    | run_cmd xbcloud put --parallel=4 $flags "$bucket/full"

vlog "Download → xbstream extract → --prepare"
dl=$topdir/downloaded
mkdir -p "$dl"
run_cmd xbcloud get --parallel=4 $flags "$bucket/full" \
    | xbstream -xv -C "$dl" --parallel=4

xtrabackup --prepare --target-dir="$dl"

stop_server
rm -rf ${mysql_datadir}
xtrabackup --copy-back --target-dir="$dl"
start_server --innodb_file_per_table

dst_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$dst_count" = "$src_count" ] || \
    die "restored row count $dst_count != source $src_count"

vlog "S3/LocalStack round-trip PASSED (row count $src_count preserved)"
