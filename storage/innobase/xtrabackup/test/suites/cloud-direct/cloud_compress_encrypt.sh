############################################################################
# Cloud backup with compress, encrypt, and combinations -- mirrors the
# xbcloud-equivalent feature surface, but uses ONLY the new
# xtrabackup --cloud-storage interface (no xbcloud, no xbstream pipe).
#
# Scenarios:
#   1.  --cloud-storage + --compress=zstd
#   2.  --cloud-storage + --compress=lz4
#   3.  --cloud-storage + --encrypt
#   4.  --cloud-storage + --compress=zstd + --encrypt (the layered case
#       xbcloud users commonly run)
#   5.  --cloud-storage + sparse + --compress=zstd (the manifest-driven
#       sparse-after-decompress case for cloud)
#
# Each scenario: backup -> verify object names in bucket -> --download
# into fresh dir -> --decompress (if compressed) -> --prepare ->
# --copy-back -> verify db state.
#
# Test runs against PXB_CLOUD_BACKEND (default s3).
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

PROVIDER="${PXB_CLOUD_BACKEND:-s3}"

cloud_emu_require_docker
command -v jq >/dev/null 2>&1 || skip_test "test requires jq"
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for "$PROVIDER"

start_server --innodb_file_per_table

mysql -e "CREATE DATABASE cce;"
mysql -e "CREATE TABLE cce.a (id INT PRIMARY KEY AUTO_INCREMENT, p BLOB) ENGINE=InnoDB;"
mysql -e "CREATE TABLE cce.b (id INT PRIMARY KEY AUTO_INCREMENT, p TEXT) ENGINE=InnoDB;"
for i in $(seq 1 5) ; do
  mysql -e "INSERT INTO cce.a (p) VALUES (REPEAT('x', 5000000));"
done
for i in $(seq 1 50) ; do
  mysql -e "INSERT INTO cce.b (p) VALUES (REPEAT('y', 100));"
done

HAVE_SPARSE=0
if ! grep -q 'PUNCH HOLE support not available' "$MYSQLD_ERRFILE" 2>/dev/null ; then
  mysql -e "CREATE TABLE cce.t_sparse (c1 INT AUTO_INCREMENT PRIMARY KEY, c2 BLOB)
            COMPRESSION='zlib' ENGINE=InnoDB;"
  mysql -e "INSERT INTO cce.t_sparse (c2) VALUES (REPEAT('s', 5000));"
  for i in $(seq 1 6) ; do
    mysql -e "INSERT INTO cce.t_sparse (c2) SELECT c2 FROM cce.t_sparse;"
  done
  HAVE_SPARSE=1
fi
innodb_wait_for_flush_all
record_db_state cce

ENCKEY="percona_xtrabackup_is_awesome___"

############################################################################
# Helper: backup -> download -> optional decompress -> prepare -> copy-back
# -> verify.  Arg $1 is the label; the rest are extra xtrabackup --backup
# flags (compress / encrypt etc).  Sets need_decompress=1 iff --compress
# is in the flag string.
############################################################################
run_scenario() {
  local label="$1" ; shift
  local extra="$*"
  local bucket="cce-$label-$(date +%s)"
  cloud_emu_make_bucket "$PROVIDER" "$bucket"
  local flags=$(cloud_emu_xb_flags "$PROVIDER" "$bucket")
  local name="be-$label"

  vlog "--- $label : extra='$extra' ---"

  rm -rf $topdir/$name $topdir/$name-dl
  mkdir -p $topdir/$name
  eval xtrabackup --backup --target-dir=$topdir/$name $flags $extra \
       2> $topdir/$name.log

  # Manifest always present, always plaintext.
  case "$PROVIDER" in
    s3)
      AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
      aws --endpoint-url="$CLOUD_EMU_S3_ENDPOINT" \
          s3 cp "s3://$bucket/${name##be-}/backup_metadata.json" \
              $topdir/$name.manifest >/dev/null 2>&1 \
        || die "$label: failed to fetch manifest from bucket"
      jq empty < $topdir/$name.manifest \
        || die "$label: backup_metadata.json in bucket is not valid JSON (encrypted?)"
      ;;
  esac

  # Download into a fresh dir.
  mkdir -p $topdir/$name-dl
  eval xtrabackup --download --target-dir=$topdir/$name-dl/$name $flags

  # If compressed, --decompress.  Pass --decrypt flags too when encrypted.
  local needs_decompress=0
  local needs_decrypt=0
  case "$extra" in
    *compress*)  needs_decompress=1 ;;
  esac
  case "$extra" in
    *encrypt*)   needs_decrypt=1 ;;
  esac

  local dec_args=""
  if [ "$needs_decrypt" = "1" ]; then
    dec_args="--decrypt=AES256 --encrypt-key=$ENCKEY"
  fi
  if [ "$needs_decompress" = "1" ] || [ "$needs_decrypt" = "1" ]; then
    xtrabackup --decompress $dec_args --target-dir=$topdir/$name-dl/$name
  fi

  # Sparse table (if present) must come back sparse.
  if [ "$HAVE_SPARSE" = "1" ] && [ -f $topdir/$name-dl/$name/cce/t_sparse.ibd ]; then
    is_sparse_file $topdir/$name-dl/$name/cce/t_sparse.ibd \
      || die "$label: restored cce/t_sparse.ibd is NOT sparse"
  fi

  # Prepare + restore + verify DB.
  xtrabackup --prepare --target-dir=$topdir/$name-dl/$name
  stop_server
  rm -rf $mysql_datadir/*
  xtrabackup --copy-back --target-dir=$topdir/$name-dl/$name
  start_server
  verify_db_state cce

  [ "$(mysql -BN -e 'SELECT COUNT(*) FROM cce.a;')" = "5" ] \
    || die "$label: cce.a row count mismatch"
  [ "$(mysql -BN -e 'SELECT COUNT(*) FROM cce.b;')" = "50" ] \
    || die "$label: cce.b row count mismatch"

  vlog "    $label PASSED"
}

############################################################################
# 1) compress=zstd
############################################################################
run_scenario zstd "--compress=zstd"

############################################################################
# 2) compress=lz4
############################################################################
run_scenario lz4 "--compress=lz4"

############################################################################
# 3) encrypt (AES256)
############################################################################
run_scenario encrypt "--encrypt=AES256 --encrypt-key=$ENCKEY"

############################################################################
# 4) compress + encrypt (the common xbcloud combo)
############################################################################
run_scenario zstd-encrypt "--compress=zstd --encrypt=AES256 --encrypt-key=$ENCKEY"

############################################################################
# 5) compress + sparse (manifest-driven sparse restore on the cloud path)
############################################################################
if [ "$HAVE_SPARSE" = "1" ]; then
  run_scenario zstd-sparse "--compress=zstd"
fi

vlog "All cloud compress/encrypt/sparse scenarios passed for $PROVIDER."
