############################################################################
# xbcloud round-trip against openstackswift/saio using the CLI-provider
# mode (--swift-use-cli).
#
# openstackswift/saio ships with Swift TempAuth, not Keystone.  The
# real `openstack token issue -f json` wouldn't work against it because
# it expects a Keystone endpoint.  The shim script inc/mock_openstack_cli.sh
# bridges the gap: it does the TempAuth curl handshake and formats the
# resulting X-Auth-Token as the openstack CLI's JSON output shape, so
# xbcloud's SwiftCliProvider can consume it unchanged.
#
# In a real Keystone-enabled OpenStack deployment, operators would set
# --swift-cli-command="openstack token issue -f json" directly.  The
# code path exercised in xbcloud is identical either way — the shim
# just gives us a CI-portable stand-in for the real Keystone CLI.
#
# Note --swift-storage-url is required in CLI mode.  Rationale documented
# in xbcloud.cc: the CLI returns only a token, no service catalog, so
# the storage URL must be supplied.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker

cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for swift

container="pxb-swift-cli-$(date +%s)-$$"
cloud_emu_make_bucket swift "$container"

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

mock_cli="${XB_TEST_DIR:-$PWD}/inc/mock_openstack_cli.sh"
[ -x "$mock_cli" ] || die "mock openstack CLI shim not executable at $mock_cli"

# The shim reads Swift TempAuth params from env vars; export them so
# the subprocess xbcloud spawns inherits them.
export MOCK_SWIFT_AUTH_URL="$CLOUD_EMU_SWIFT_ENDPOINT"
export MOCK_SWIFT_USER="$CLOUD_EMU_SWIFT_USER"
export MOCK_SWIFT_KEY="$CLOUD_EMU_SWIFT_KEY"

# Sanity: shim by itself returns a real token.
"$mock_cli" > "$topdir/shim-out.json" \
    || die "mock openstack CLI shim exited non-zero"
grep -q '"id":' "$topdir/shim-out.json" \
    || die "mock openstack CLI shim did not emit expected JSON"

# The Swift storage URL for openstackswift/saio is at /v1/AUTH_test.
swift_storage_url="http://localhost:${CLOUD_EMU_SWIFT_HOST_PORT}/v1/AUTH_test"

xbcloud_cli_flags="--storage=swift \
    --swift-use-cli \
    --swift-cli-command=$mock_cli \
    --swift-storage-url=$swift_storage_url \
    --swift-container=$container"

vlog "Full backup → Swift (openstackswift) via --swift-use-cli + mock CLI shim"
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

vlog "Swift CLI-mode round-trip PASSED (row count $src_count preserved via mock openstack CLI)"
