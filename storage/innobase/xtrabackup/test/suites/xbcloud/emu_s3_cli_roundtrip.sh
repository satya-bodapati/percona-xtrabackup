############################################################################
# xbcloud round-trip against LocalStack (S3 emulator) using the CLI-
# provider mode (--s3-use-cli).
#
# This exercises the auth/cli/aws_cli_provider path:
#   1. xbcloud shells out to the mock aws CLI at each credential mint,
#   2. reads the returned credential_process-shape JSON on stdout,
#   3. caches with the Expiration from the JSON,
#   4. hands the resulting HMAC creds to the S3 SigV4 signer,
#   5. LocalStack accepts the request because SigV4 signing is real
#      even though the credentials themselves came from a subprocess.
#
# The shim is inc/mock_aws_cli.sh — reads test creds from env vars and
# prints the standard credential_process JSON to stdout.  In a real
# deployment operators would use the real `aws` CLI instead of the
# shim; the code path in xbcloud is identical.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker

cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for s3

bucket="pxb-s3-cli-$(date +%s)-$$"
cloud_emu_make_bucket s3 "$bucket"

start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

# LocalStack accepts "test" / "test" long-lived HMAC keys.  Wire them
# into the shim via env; the shim emits them as credential_process JSON.
mock_cli="${XB_TEST_DIR:-$PWD}/inc/mock_aws_cli.sh"
[ -x "$mock_cli" ] || die "mock aws CLI shim not executable at $mock_cli"

# Verify the shim itself outputs valid JSON before dragging xbcloud
# into the picture — makes failures easier to localise.
MOCK_AWS_ACCESS_KEY_ID=test MOCK_AWS_SECRET_ACCESS_KEY=test \
    "$mock_cli" > "$topdir/shim-out.json" \
    || die "mock aws CLI shim exited non-zero"
grep -q '"AccessKeyId": "test"' "$topdir/shim-out.json" \
    || die "mock aws CLI shim did not emit expected JSON"

# CLI-mode xbcloud flags — no --s3-access-key/--s3-secret-key here on
# purpose.  Credentials come from the shim.
xbcloud_cli_flags="--storage=s3 \
    --s3-endpoint=$CLOUD_EMU_S3_ENDPOINT \
    --s3-bucket=$bucket \
    --s3-region=us-east-1 \
    --s3-bucket-lookup=path \
    --s3-use-cli \
    --s3-cli-command=$mock_cli"

vlog "Full backup → LocalStack via --s3-use-cli + mock aws CLI shim"
full_dir=$topdir/full
mkdir -p "$full_dir"
MOCK_AWS_ACCESS_KEY_ID=test MOCK_AWS_SECRET_ACCESS_KEY=test \
xtrabackup --backup --stream=xbstream --extra-lsndir="$full_dir" \
    --target-dir="$full_dir" \
    | MOCK_AWS_ACCESS_KEY_ID=test MOCK_AWS_SECRET_ACCESS_KEY=test \
      run_cmd xbcloud put --parallel=4 $xbcloud_cli_flags "$bucket/full"

vlog "Download → xbstream extract → --prepare"
dl=$topdir/downloaded
mkdir -p "$dl"
MOCK_AWS_ACCESS_KEY_ID=test MOCK_AWS_SECRET_ACCESS_KEY=test \
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

vlog "S3 CLI-mode round-trip PASSED (row count $src_count preserved via mock aws CLI)"
