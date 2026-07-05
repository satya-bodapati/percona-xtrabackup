############################################################################
# xbcloud round-trip against Azurite using the CLI-provider mode
# (--azure-use-cli).
#
# STATUS: skipped.  Azurite (the local Azure Blob emulator) doesn't
# accept OAuth2 Bearer tokens on its Shared Key endpoints — same
# limitation that makes the native Managed-Identity path untestable
# locally (see emu_azure_roundtrip.sh which documents "Managed
# Identity via IMDS cannot be exercised locally; a real Azure VM is
# required for that").  The CLI-mode path is Bearer-only (that's the
# whole point — reuse `az account get-access-token`), so the same
# constraint applies.
#
# The shim inc/mock_azure_cli.sh is included in the tree so the four
# CLI-mode test files stay symmetric, and so anyone testing against a
# real Azure Storage endpoint can point --azure-cli-command at the
# real `az` CLI and rerun this test with the skip removed.  When a
# JWT-validating Azure emulator becomes available (or we swap to a
# different local Bearer-accepting Azure implementation), un-skip.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

skip_test "Azurite doesn't accept Bearer auth; --azure-use-cli untestable locally"

# The lines below are the actual test — kept for the day the skip
# can be removed.

cloud_emu_require_docker
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for azure

container="pxb-azure-cli-$(date +%s)-$$"

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

mock_cli="${XB_TEST_DIR:-$PWD}/inc/mock_azure_cli.sh"
[ -x "$mock_cli" ] || die "mock az CLI shim not executable at $mock_cli"

xbcloud_cli_flags="--storage=azure \
    --azure-development-storage \
    --azure-endpoint=$CLOUD_EMU_AZURE_BLOB_ENDPOINT \
    --azure-container-name=$container \
    --azure-storage-account=$CLOUD_EMU_AZURE_ACCOUNT \
    --azure-use-cli \
    --azure-cli-command=$mock_cli"

vlog "Full backup → Azurite via --azure-use-cli + mock az CLI shim"
full_dir=$topdir/full
mkdir -p "$full_dir"
xtrabackup --backup --stream=xbstream --extra-lsndir="$full_dir" \
    --target-dir="$full_dir" \
    | run_cmd xbcloud put --parallel=4 $xbcloud_cli_flags "$container/full"

vlog "Download → xbstream extract → --prepare"
dl=$topdir/downloaded
mkdir -p "$dl"
run_cmd xbcloud get --parallel=4 $xbcloud_cli_flags "$container/full" \
    | xbstream -xv -C "$dl" --parallel=4

xtrabackup --prepare --target-dir="$dl"

stop_server
rm -rf ${mysql_datadir}
xtrabackup --copy-back --target-dir="$dl"
start_server --innodb_file_per_table

dst_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$dst_count" = "$src_count" ] || \
    die "restored row count $dst_count != source $src_count"

vlog "Azure CLI-mode round-trip PASSED (row count $src_count preserved via mock az CLI)"
