############################################################################
# xbcloud round-trip against fake-gcs-server using the CLI-provider
# mode (--google-use-cli).
#
# STATUS: skipped, same compat gap that blocks emu_gcs_roundtrip.sh
# and emu_gcs_oauth2_roundtrip.sh — fake-gcs-server's S3 XML API
# response for list-bucket is missing the <IsTruncated> element,
# which xbcloud's list-bucket parser requires.  This is orthogonal
# to CLI mode: it affects every GCS emulator path.
#
# The shim inc/mock_gcloud_cli.sh is kept in the tree so the four
# CLI-mode test files stay symmetric and so anyone testing against
# a real GCS endpoint can point --google-cli-command at the real
# `gcloud auth application-default print-access-token` and rerun
# this test with the skip removed.  When a fully S3-XML-compatible
# GCS emulator becomes available, un-skip.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

skip_test "fake-gcs-server S3 XML compat lacks <IsTruncated>; xbcloud round-trip untestable locally"

# The lines below are the actual test — kept for the day the skip
# can be removed.

cloud_emu_require_docker
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for gcs

bucket="pxb-gcs-cli-$(date +%s)-$$"
cloud_emu_make_bucket gcs "$bucket"

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

mock_cli="${XB_TEST_DIR:-$PWD}/inc/mock_gcloud_cli.sh"
[ -x "$mock_cli" ] || die "mock gcloud CLI shim not executable at $mock_cli"

xbcloud_cli_flags="--storage=google \
    --google-endpoint=$CLOUD_EMU_GCS_ENDPOINT \
    --google-bucket=$bucket \
    --google-region=auto \
    --google-use-cli \
    --google-cli-command=$mock_cli"

vlog "Full backup → fake-gcs-server via --google-use-cli + mock gcloud CLI shim"
full_dir=$topdir/full
mkdir -p "$full_dir"
xtrabackup --backup --stream=xbstream --extra-lsndir="$full_dir" \
    --target-dir="$full_dir" \
    | run_cmd xbcloud put --parallel=4 $xbcloud_cli_flags "$bucket/full"

vlog "Download → xbstream extract → --prepare"
dl=$topdir/downloaded
mkdir -p "$dl"
run_cmd xbcloud get --parallel=4 $xbcloud_cli_flags "$bucket/full" \
    | xbstream -xv -C "$dl" --parallel=4

xtrabackup --prepare --target-dir="$dl"

stop_server
rm -rf ${mysql_datadir}
xtrabackup --copy-back --target-dir="$dl"
start_server --innodb_file_per_table

dst_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$dst_count" = "$src_count" ] || \
    die "restored row count $dst_count != source $src_count"

vlog "GCS CLI-mode round-trip PASSED (row count $src_count preserved via mock gcloud CLI)"
