############################################################################
# xbcloud round-trip against Azurite (Azure Blob emulator).
#
# Verifies the credential-provider migration for Azure Blob storage
# (SharedKeyProvider path).  Managed Identity via IMDS cannot be
# exercised locally; a real Azure VM is required for that.  Skips
# cleanly when docker isn't available.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker

cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for azure

container="pxb-azure-$(date +%s)-$$"
# xbcloud auto-creates the container on `put` if it doesn't exist
# (xbcloud.cc:1048 create_container branch), so we don't need to
# pre-create via a signed Azure REST call from the shell — Azurite
# accepts the container-create request from xbcloud with the
# devstoreaccount1 Shared Key.

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

flags=$(cloud_emu_xbcloud_flags azure "$container")

vlog "Full backup → Azure Blob (Azurite) via xbstream|xbcloud put"
full_dir=$topdir/full
mkdir -p "$full_dir"
xtrabackup --backup --stream=xbstream --extra-lsndir="$full_dir" \
    --target-dir="$full_dir" \
    | run_cmd xbcloud put --parallel=4 $flags "$container/full"

vlog "Download → xbstream extract → --prepare"
dl=$topdir/downloaded
mkdir -p "$dl"
run_cmd xbcloud get --parallel=4 $flags "$container/full" \
    | xbstream -xv -C "$dl" --parallel=4

xtrabackup --prepare --target-dir="$dl"

stop_server
rm -rf ${mysql_datadir}
xtrabackup --copy-back --target-dir="$dl"
start_server --innodb_file_per_table

dst_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$dst_count" = "$src_count" ] || \
    die "restored row count $dst_count != source $src_count"

vlog "Azure/Azurite round-trip PASSED (row count $src_count preserved)"
