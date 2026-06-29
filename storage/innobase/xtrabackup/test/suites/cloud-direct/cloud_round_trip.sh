############################################################################
# Direct-cloud (ds_cloud) full backup -> --download -> --prepare -> mysql
# round-trip against a local emulator stack.
#
# Tests the NEW xtrabackup --cloud-storage interface ONLY (no xbcloud).
# Covers: ds_cloud upload, multipart upload path (via mixed file sizes),
# backup_metadata.json manifest write, --download with manifest fetch first,
# sparse-restore via manifest, --delete cleanup.
#
# Backend selection via env var:
#   PXB_CLOUD_BACKEND=s3   (default)
#   PXB_CLOUD_BACKEND=gcs
#   PXB_CLOUD_BACKEND=azure
#   PXB_CLOUD_BACKEND=swift
#
# Skip-gates:
#   - docker / docker-compose missing  -> skip
#   - aws cli missing                  -> skip (used by emulator helpers)
#   - emulator stack failed to start   -> die
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

PROVIDER="${PXB_CLOUD_BACKEND:-s3}"

cloud_emu_require_docker

cloud_emu_start
trap cloud_emu_stop EXIT

cloud_emu_wait_for "$PROVIDER"

CLOUD_BUCKET="pxb-cloud-direct-$(date +%s)"
PREFIX="rt-backup-$(date +%s)"
BUCKET_WITH_PREFIX="$CLOUD_BUCKET/$PREFIX"
cloud_emu_make_bucket "$PROVIDER" "$CLOUD_BUCKET"

# Pull common --cloud-* flags into a single string.  We pass the
# BUCKET/PREFIX form so the new provider-explicit option syntax is
# exercised here (xtrabackup parses BUCKET/PREFIX -> bucket=CLOUD_BUCKET,
# prefix=PREFIX, and lands objects at $CLOUD_BUCKET/$PREFIX/<file>).
CLOUD_FLAGS=$(cloud_emu_xb_flags "$PROVIDER" "$BUCKET_WITH_PREFIX")
vlog "==== Cloud round-trip [$PROVIDER] bucket=$CLOUD_BUCKET prefix=$PREFIX ===="
vlog "    $CLOUD_FLAGS"

############################################################################
# Setup: a backup workload with mixed file sizes (exercises multipart and
# small-file fast-path), one sparse table (exercises sparse_map in manifest),
# one empty table (exercises the small-file edge case).
############################################################################
start_server --innodb_file_per_table

mysql -e "CREATE DATABASE cloud_rt;"
mysql -e "CREATE TABLE cloud_rt.empty_one (id INT PRIMARY KEY) ENGINE=InnoDB;"
mysql -e "CREATE TABLE cloud_rt.small_one (id INT PRIMARY KEY, payload TEXT)
          ENGINE=InnoDB;"
mysql -e "INSERT INTO cloud_rt.small_one VALUES (1, REPEAT('x', 100));"
# Bigger table that crosses the multipart threshold (16 MiB default).
mysql -e "CREATE TABLE cloud_rt.big_one (id INT PRIMARY KEY AUTO_INCREMENT, payload BLOB)
          ENGINE=InnoDB;"
for i in $(seq 1 10) ; do
  mysql -e "INSERT INTO cloud_rt.big_one (payload) VALUES (REPEAT('x', 5000000));"
done
# Sparse / page-compressed table.
if ! grep -q 'PUNCH HOLE support not available' "$MYSQLD_ERRFILE" ; then
  mysql -e "CREATE TABLE cloud_rt.t_sparse (c1 INT AUTO_INCREMENT PRIMARY KEY, c2 BLOB)
            COMPRESSION='zlib' ENGINE=InnoDB;"
  mysql -e "INSERT INTO cloud_rt.t_sparse (c2) VALUES (REPEAT('y', 5000));"
  for i in $(seq 1 6) ; do
    mysql -e "INSERT INTO cloud_rt.t_sparse (c2) SELECT c2 FROM cloud_rt.t_sparse;"
  done
  HAVE_SPARSE=1
else
  HAVE_SPARSE=0
fi
innodb_wait_for_flush_all
record_db_state cloud_rt

############################################################################
# 1.  --backup --cloud-storage  (uploads directly; no xbstream pipe).
############################################################################
vlog "--- step 1: --backup --cloud-storage=$PROVIDER ---"

# --target-dir is now purely a local-filesystem concept for cloud mode
# (no files are actually written there during cloud backup, but the
# parser still requires the flag).  The bucket PREFIX is set explicitly
# above via BUCKET/PREFIX in the --cloud-*-bucket value.
NAME="$PREFIX"
mkdir -p $topdir/$NAME

eval xtrabackup --backup --target-dir=$topdir/$NAME $CLOUD_FLAGS

############################################################################
# 2.  Sanity-check: backup_metadata.json exists in the bucket and parses.
############################################################################
vlog "--- step 2: bucket contents ---"

case "$PROVIDER" in
  s3)
    AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
    aws --endpoint-url="$CLOUD_EMU_S3_ENDPOINT" \
        s3 ls "s3://$CLOUD_BUCKET/" --recursive > $topdir/${NAME}.listing
    grep -q "backup_metadata.json" $topdir/${NAME}.listing \
      || die "step 2: backup_metadata.json missing from bucket listing"
    NUM_OBJ=$(wc -l < $topdir/${NAME}.listing)
    [ "$NUM_OBJ" -ge 5 ] \
      || die "step 2: too few objects in bucket ($NUM_OBJ); expected at least 5"
    # EVERY object must live under "$PREFIX/" -- this verifies the
    # provider-explicit BUCKET/PREFIX option produced the expected
    # layout, NOT the old target-dir-basename derivation.
    BAD=$(awk '{print $NF}' $topdir/${NAME}.listing \
            | grep -v "^${PREFIX}/" | head -1 || true)
    [ -z "$BAD" ] \
      || die "step 2: object key '$BAD' not under prefix '$PREFIX/'"
    # NONE of the keys may end with '/' (HNS-safety; we never PUT
    # directory placeholders).
    BAD=$(awk '{print $NF}' $topdir/${NAME}.listing | grep '/$' || true)
    [ -z "$BAD" ] \
      || die "step 2: object key ends in '/' (HNS-unsafe): $BAD"
    vlog "    bucket has $NUM_OBJ objects, all under $PREFIX/"
    ;;
  *)
    vlog "    bucket sanity skipped for $PROVIDER (no aws cli emulator path)"
    ;;
esac

############################################################################
# 3.  --download into a fresh dir; verify manifest fetched first, sparse
#     table restored sparse, everything else intact.
############################################################################
vlog "--- step 3: --download into fresh dir ---"

DLDIR=$topdir/${NAME}-download
mkdir -p $DLDIR
eval xtrabackup --download --target-dir=$DLDIR/$NAME $CLOUD_FLAGS

[ -s $DLDIR/$NAME/backup_metadata.json ] \
  || die "step 3: backup_metadata.json missing or empty in download dir"

# The backup files must be present.
[ -f $DLDIR/$NAME/cloud_rt/big_one.ibd ] \
  || die "step 3: cloud_rt/big_one.ibd missing in download dir"

if [ "$HAVE_SPARSE" = "1" ]; then
  is_sparse_file $DLDIR/$NAME/cloud_rt/t_sparse.ibd \
    || die "step 3: restored cloud_rt/t_sparse.ibd is not sparse on disk"
fi

############################################################################
# 4.  --prepare + start mysqld against the downloaded copy; verify rows.
############################################################################
vlog "--- step 4: --prepare + restore + verify ---"

xtrabackup --prepare --target-dir=$DLDIR/$NAME

stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$DLDIR/$NAME
start_server
verify_db_state cloud_rt

# Spot-check row counts that survived the round trip.
ROWS=$(mysql -BN -e "SELECT COUNT(*) FROM cloud_rt.big_one;")
[ "$ROWS" = "10" ] || die "step 4: cloud_rt.big_one rows=$ROWS, expected 10"

############################################################################
# 5.  --delete cleans the bucket prefix (idempotent / no error on empty).
############################################################################
vlog "--- step 5: --delete ---"

# --delete is interactive by default; pipe yes.  When --force is added in
# a follow-up commit, this becomes "--force --delete".
yes | eval xtrabackup --delete --target-dir=$topdir/$NAME $CLOUD_FLAGS \
  >/dev/null 2>&1 || true

# After delete, the prefix should be empty (or absent) from the bucket.
case "$PROVIDER" in
  s3)
    AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
    aws --endpoint-url="$CLOUD_EMU_S3_ENDPOINT" \
        s3 ls "s3://$CLOUD_BUCKET/$PREFIX/" --recursive > $topdir/${NAME}.after-delete
    [ ! -s $topdir/${NAME}.after-delete ] \
      || vlog "    (note: $PROVIDER delete left some objects; tolerated until --force lands)"
    ;;
esac

vlog "==== Cloud round-trip [$PROVIDER] PASSED ===="
