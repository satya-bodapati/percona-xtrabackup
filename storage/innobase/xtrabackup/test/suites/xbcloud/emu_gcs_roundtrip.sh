############################################################################
# xbcloud round-trip against fake-gcs-server (GCS emulator via S3 XML API).
#
# Verifies the credential-provider migration for the GOOGLE storage
# type (GcsInteropHmacProvider path).  Skips cleanly when docker
# isn't available.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker

cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for gcs

bucket="pxb-gcs-$(date +%s)-$$"
cloud_emu_make_bucket gcs "$bucket"

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

flags=$(cloud_emu_xbcloud_flags gcs "$bucket")

vlog "Full backup → GCS (fake-gcs-server) via xbstream|xbcloud put"
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

vlog "GCS/fake-gcs-server round-trip PASSED (row count $src_count preserved)"
