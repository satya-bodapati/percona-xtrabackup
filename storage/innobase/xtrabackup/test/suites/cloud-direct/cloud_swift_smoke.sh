############################################################################
# --cloud-swift-* batch smoke test against the openstackswift/saio
# emulator (TempAuth at /auth/v1.0).
#
# Verifies:
#   1. all 17 --cloud-swift-* options are accepted by the parser
#      (presence in --help output).
#   2. End-to-end backup completes against the saio emulator:
#        * Keystone (TempAuth v1.0) authentication succeeds.
#        * Object PUTs land in the container.
#        * backup_meta.json is present after backup.
#        * --download into a fresh dir reproduces the files.
#   3. BUCKET/PREFIX parsing works for --cloud-swift-container the same
#      way it does for --cloud-s3-bucket (HNS-safe normalization).
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker
cloud_emu_start
trap cloud_emu_stop EXIT

###############################################
# Step 0: --help discoverability for ALL 17 swift options.
###############################################
EXPECTED_OPTS="
cloud-swift-container
cloud-swift-auth-url
cloud-swift-key
cloud-swift-user
cloud-swift-user-id
cloud-swift-password
cloud-swift-tenant
cloud-swift-tenant-id
cloud-swift-project
cloud-swift-project-id
cloud-swift-domain
cloud-swift-domain-id
cloud-swift-project-domain
cloud-swift-project-domain-id
cloud-swift-region
cloud-swift-storage-url
cloud-swift-auth-version
"
HELP=$(xtrabackup --help 2>&1)
for opt in $EXPECTED_OPTS; do
  echo "$HELP" | grep -q -- "--$opt" \
    || die "swift_smoke step 0: --$opt not registered"
done
vlog "    PASS: all 17 --cloud-swift-* options registered"

###############################################
# Step 1: skip the live-emulator test if openstackswift saio isn't
# available (some CI envs only spin up s3 + azure).
###############################################
cloud_emu_wait_for swift \
  || skip_test "openstackswift/saio not available; skipping live swift smoke"

start_server --innodb_file_per_table
mysql -e "CREATE DATABASE sw_test;"
mysql -e "CREATE TABLE sw_test.t1 (id INT PRIMARY KEY, v TEXT) ENGINE=InnoDB;"
mysql -e "INSERT INTO sw_test.t1 VALUES (1, REPEAT('z', 200));"
innodb_wait_for_flush_all

CONTAINER="pxb-swift-$(date +%s)"
PREFIX="2026-06-23-swift"
cloud_emu_make_bucket swift "$CONTAINER"

FLAGS=$(cloud_emu_xb_flags swift "$CONTAINER/$PREFIX")
vlog "==== swift smoke flags: $FLAGS ===="

TDIR="$topdir/swift-backup"
rm -rf "$TDIR"; mkdir -p "$TDIR"

eval xtrabackup --backup --target-dir="$TDIR" $FLAGS

###############################################
# Step 2: list the container -- backup_meta.json must be present, all
# keys must live under $PREFIX/, none end in '/'.
###############################################
TOKEN=$(curl -fsSI -H "X-Auth-User: $CLOUD_EMU_SWIFT_USER" \
                  -H "X-Auth-Key: $CLOUD_EMU_SWIFT_KEY" \
                  "${CLOUD_EMU_SWIFT_ENDPOINT}" \
        | grep -i '^X-Auth-Token:' | sed 's/.*: //' | tr -d '\r\n')
[ -n "$TOKEN" ] || die "swift_smoke step 2: no X-Auth-Token after auth"

curl -fs -H "X-Auth-Token: $TOKEN" \
     "http://localhost:8080/v1/AUTH_test/$CONTAINER?format=plain" \
     > "$TDIR/listing"

[ -s "$TDIR/listing" ] || die "swift_smoke step 2: container listing empty"
grep -q "${PREFIX}/backup_meta.json" "$TDIR/listing" \
  || die "swift_smoke step 2: ${PREFIX}/backup_meta.json missing"
bad=$(grep -v "^${PREFIX}/" "$TDIR/listing" | head -1 || true)
[ -z "$bad" ] \
  || die "swift_smoke step 2: '$bad' not under prefix '$PREFIX/'"
bad=$(grep '/$' "$TDIR/listing" || true)
[ -z "$bad" ] \
  || die "swift_smoke step 2: '$bad' ends in '/' (HNS-unsafe)"
vlog "    PASS: $(wc -l < $TDIR/listing) keys under $PREFIX/, none ending in '/'"

###############################################
# Step 3: --download reconstructs the file tree locally.
###############################################
DL="$topdir/swift-download"
rm -rf "$DL"; mkdir -p "$DL"
eval xtrabackup --download --target-dir="$DL" $FLAGS
[ -s "$DL/backup_meta.json" ] \
  || die "swift_smoke step 3: backup_meta.json missing in download dir"
[ -f "$DL/sw_test/t1.ibd" ] \
  || die "swift_smoke step 3: sw_test/t1.ibd missing in download dir"

vlog "==== cloud_swift_smoke.sh PASSED ===="
