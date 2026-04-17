############################################################################
# Test backup_size reporting in xtrabackup_info and log output
#
# Validates that:
#  - backup_size is always present in xtrabackup_info (final on-disk size)
#  - uncompressed_size is present only when compression is used
#  - "Backup size:" always appears in log
#  - "Uncompressed size:" and "Compression ratio:" appear only
#    when compression is used
#  - ds_statistics correctly counts bytes on bypass paths (RocksDB, encrypted)
############################################################################

. inc/common.sh

get_backup_size() {
  local info_file=$1
  grep "^backup_size = " "$info_file" | awk '{print $3}'
}

get_uncompressed_size() {
  local info_file=$1
  grep "^uncompressed_size = " "$info_file" | awk '{print $3}'
}

assert_positive() {
  local val=$1
  local label=$2
  if ! [[ "$val" =~ ^[0-9]+$ ]] || [ "$val" -le 0 ]; then
    die "$label: expected positive integer, got '$val'"
  fi
}

assert_no_uncompressed_size() {
  local info_file=$1
  if grep -q "^uncompressed_size = " "$info_file" ; then
    die "uncompressed_size should not be present in $info_file"
  fi
}

start_server --innodb_file_per_table

load_sakila

############################################################################
# Scenario 1: Simple local backup
############################################################################
vlog "=== Scenario 1: Simple local backup ==="

xtrabackup --backup --target-dir=$topdir/backup_local

size1=$(get_backup_size "$topdir/backup_local/xtrabackup_info")
assert_positive "$size1" "local backup_size"
assert_no_uncompressed_size "$topdir/backup_local/xtrabackup_info"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log"
vlog "Scenario 1 passed: backup_size=$size1"

############################################################################
# Scenario 2: Streaming (xbstream) backup
############################################################################
vlog "=== Scenario 2: Streaming (xbstream) backup ==="

mkdir -p $topdir/backup_stream $topdir/lsndir
xtrabackup --backup --stream=xbstream --extra-lsndir=$topdir/lsndir \
    > $topdir/backup_stream.xbs

size2=$(get_backup_size "$topdir/lsndir/xtrabackup_info")
assert_positive "$size2" "stream backup_size"
assert_no_uncompressed_size "$topdir/lsndir/xtrabackup_info"
vlog "Scenario 2 passed: backup_size=$size2"

############################################################################
# Scenario 3: Compressed local backup (lz4)
############################################################################
vlog "=== Scenario 3: Compressed local backup (lz4) ==="

mkdir -p $topdir/lsndir_compress
xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup_compress \
    --extra-lsndir=$topdir/lsndir_compress

size3=$(get_backup_size "$topdir/lsndir_compress/xtrabackup_info")
assert_positive "$size3" "compressed backup_size"
usize3=$(get_uncompressed_size "$topdir/lsndir_compress/xtrabackup_info")
assert_positive "$usize3" "compressed uncompressed_size"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log"
grep -q "Uncompressed size:" $OUTFILE || die "Expected 'Uncompressed size:' in log"
grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log"
vlog "Scenario 3 passed: backup_size=$size3 uncompressed=$usize3"

############################################################################
# Scenario 4: Encrypted local backup
############################################################################
vlog "=== Scenario 4: Encrypted local backup ==="

mkdir -p $topdir/lsndir_encrypt
xtrabackup --backup --encrypt=AES256 \
    --encrypt-key=percona_xtrabackup_is_awesome___ \
    --target-dir=$topdir/backup_encrypt \
    --extra-lsndir=$topdir/lsndir_encrypt

size4=$(get_backup_size "$topdir/lsndir_encrypt/xtrabackup_info")
assert_positive "$size4" "encrypted backup_size"
assert_no_uncompressed_size "$topdir/lsndir_encrypt/xtrabackup_info"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log"
vlog "Scenario 4 passed: backup_size=$size4"

############################################################################
# Scenario 5: xbstream + compress (lz4)
############################################################################
vlog "=== Scenario 5: xbstream + compress (lz4) ==="

mkdir -p $topdir/lsndir_stream_comp
xtrabackup --backup --stream=xbstream --compress=lz4 \
    --extra-lsndir=$topdir/lsndir_stream_comp \
    > $topdir/backup_stream_comp.xbs

size5=$(get_backup_size "$topdir/lsndir_stream_comp/xtrabackup_info")
assert_positive "$size5" "stream+compress backup_size"
usize5=$(get_uncompressed_size "$topdir/lsndir_stream_comp/xtrabackup_info")
assert_positive "$usize5" "stream+compress uncompressed_size"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log"
grep -q "Uncompressed size:" $OUTFILE || die "Expected 'Uncompressed size:' in log"
grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log"
vlog "Scenario 5 passed: backup_size=$size5 uncompressed=$usize5"

############################################################################
# Scenario 6: xbstream + encrypt
############################################################################
vlog "=== Scenario 6: xbstream + encrypt ==="

mkdir -p $topdir/lsndir_stream_enc
xtrabackup --backup --stream=xbstream --encrypt=AES256 \
    --encrypt-key=percona_xtrabackup_is_awesome___ \
    --extra-lsndir=$topdir/lsndir_stream_enc \
    > $topdir/backup_stream_enc.xbs

size6=$(get_backup_size "$topdir/lsndir_stream_enc/xtrabackup_info")
assert_positive "$size6" "stream+encrypt backup_size"
assert_no_uncompressed_size "$topdir/lsndir_stream_enc/xtrabackup_info"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log"
vlog "Scenario 6 passed: backup_size=$size6"

############################################################################
# Scenario 7: Compressed + encrypted + xbstream
############################################################################
vlog "=== Scenario 7: Compressed + encrypted + xbstream ==="

mkdir -p $topdir/lsndir_all
xtrabackup --backup --compress=lz4 --encrypt=AES256 \
    --encrypt-key=percona_xtrabackup_is_awesome___ \
    --stream=xbstream --extra-lsndir=$topdir/lsndir_all \
    > $topdir/backup_all.xbs

size7=$(get_backup_size "$topdir/lsndir_all/xtrabackup_info")
assert_positive "$size7" "all-options backup_size"
usize7=$(get_uncompressed_size "$topdir/lsndir_all/xtrabackup_info")
assert_positive "$usize7" "all-options uncompressed_size"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log"
grep -q "Uncompressed size:" $OUTFILE || die "Expected 'Uncompressed size:' in log"
grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log"
vlog "Scenario 7 passed: backup_size=$size7 uncompressed=$usize7"

############################################################################
# Scenario 8: Restore validation (local backup)
############################################################################
vlog "=== Scenario 8: Restore validation ==="

record_db_state sakila
xtrabackup --prepare --target-dir=$topdir/backup_local
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_local
start_server
verify_db_state sakila
vlog "Scenario 8 passed"

############################################################################
# Scenario 9: Incremental backup chain (full -> inc1 -> inc2) without compress
# Verifies backup_size accounts for .delta files in incremental
# backups, and that incremental sizes are smaller than full backup size.
############################################################################
vlog "=== Scenario 9: Incremental backup chain (no compress) ==="

mysql -e "CREATE TABLE t_inc (a INT PRIMARY KEY AUTO_INCREMENT, b TEXT) ENGINE=InnoDB;" test
for i in $(seq 1 500) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

# Full backup
xtrabackup --backup --target-dir=$topdir/backup_inc_full

size_full=$(get_backup_size "$topdir/backup_inc_full/xtrabackup_info")
assert_positive "$size_full" "inc-full backup_size"
assert_no_uncompressed_size "$topdir/backup_inc_full/xtrabackup_info"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log (inc-full)"
vlog "Inc full backup: backup_size=$size_full"

# Add some data for inc1
for i in $(seq 1 100) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

# Incremental backup 1 (based on full)
xtrabackup --backup --incremental-basedir=$topdir/backup_inc_full \
    --target-dir=$topdir/backup_inc1

size_inc1=$(get_backup_size "$topdir/backup_inc1/xtrabackup_info")
assert_positive "$size_inc1" "inc1 backup_size"
assert_no_uncompressed_size "$topdir/backup_inc1/xtrabackup_info"
grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log (inc1)"

if [ "$size_inc1" -ge "$size_full" ]; then
  die "inc1 size ($size_inc1) should be smaller than full ($size_full)"
fi
vlog "Inc1 backup: backup_size=$size_inc1 (full was $size_full)"

# Verify .delta files exist in inc1
ls $topdir/backup_inc1/test/*.delta > /dev/null 2>&1 || die "Expected .delta files in inc1"
ls $topdir/backup_inc1/test/*.meta > /dev/null 2>&1 || die "Expected .meta files in inc1"

# Add more data for inc2
for i in $(seq 1 50) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

# Incremental backup 2 (based on inc1)
xtrabackup --backup --incremental-basedir=$topdir/backup_inc1 \
    --target-dir=$topdir/backup_inc2

size_inc2=$(get_backup_size "$topdir/backup_inc2/xtrabackup_info")
assert_positive "$size_inc2" "inc2 backup_size"
assert_no_uncompressed_size "$topdir/backup_inc2/xtrabackup_info"
vlog "Inc2 backup: backup_size=$size_inc2 (inc1 was $size_inc1)"

# Prepare and restore the chain
record_db_state test
xtrabackup --prepare --apply-log-only --target-dir=$topdir/backup_inc_full
xtrabackup --prepare --apply-log-only --incremental-dir=$topdir/backup_inc1 \
    --target-dir=$topdir/backup_inc_full
xtrabackup --prepare --apply-log-only --incremental-dir=$topdir/backup_inc2 \
    --target-dir=$topdir/backup_inc_full
xtrabackup --prepare --target-dir=$topdir/backup_inc_full
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_inc_full
start_server
verify_db_state test
vlog "Scenario 9 passed"

############################################################################
# Scenario 10: Incremental backup with compress
# Verifies both backup_size and uncompressed_size for incremental backups.
############################################################################
vlog "=== Scenario 10: Incremental backup chain (with compress) ==="

mysql -e "TRUNCATE TABLE t_inc;" test
for i in $(seq 1 500) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

# Full backup with compress
mkdir -p $topdir/lsndir_inc_comp_full
xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup_inc_comp_full \
    --extra-lsndir=$topdir/lsndir_inc_comp_full

size_comp_full=$(get_backup_size "$topdir/lsndir_inc_comp_full/xtrabackup_info")
assert_positive "$size_comp_full" "inc-comp-full backup_size"
usize_comp_full=$(get_uncompressed_size "$topdir/lsndir_inc_comp_full/xtrabackup_info")
assert_positive "$usize_comp_full" "inc-comp-full uncompressed_size"
grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log (inc-comp-full)"
vlog "Inc compressed full: backup_size=$size_comp_full uncompressed=$usize_comp_full"

# Add data for inc1
for i in $(seq 1 100) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

# Incremental backup with compress
mkdir -p $topdir/lsndir_inc_comp1
xtrabackup --backup --compress=lz4 \
    --incremental-basedir=$topdir/lsndir_inc_comp_full \
    --target-dir=$topdir/backup_inc_comp1 \
    --extra-lsndir=$topdir/lsndir_inc_comp1

size_comp_inc1=$(get_backup_size "$topdir/lsndir_inc_comp1/xtrabackup_info")
assert_positive "$size_comp_inc1" "inc-comp1 backup_size"
usize_comp_inc1=$(get_uncompressed_size "$topdir/lsndir_inc_comp1/xtrabackup_info")
assert_positive "$usize_comp_inc1" "inc-comp1 uncompressed_size"
grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log (inc-comp1)"

if [ "$size_comp_inc1" -ge "$size_comp_full" ]; then
  die "inc-comp1 backup_size ($size_comp_inc1) should be smaller than full ($size_comp_full)"
fi
vlog "Inc compressed inc1: backup_size=$size_comp_inc1 uncompressed=$usize_comp_inc1 (full was $size_comp_full/$usize_comp_full)"

vlog "Scenario 10 passed"

############################################################################
# Scenario 11: Compressed backup with RocksDB (bypass path)
# RocksDB SST files bypass xtrabackup compression via ds_uncompressed_data.
# Verifies ds_statistics counts bytes on both paths.
############################################################################
if test -f $(dirname ${MYSQLD})/../lib/plugin/ha_rocksdb.so ; then
  vlog "=== Scenario 11: Compressed backup with RocksDB ==="

  stop_server
  rm -rf $mysql_datadir
  start_server --innodb_file_per_table

  init_rocksdb

  mysql -e "CREATE TABLE t_rocks (a INT PRIMARY KEY AUTO_INCREMENT, b INT, c VARCHAR(200)) ENGINE=ROCKSDB;" test
  for i in $(seq 1 500) ; do
    echo "INSERT INTO t_rocks (b, c) VALUES (FLOOR(RAND() * 1000000), UUID());"
  done | mysql test

  mkdir -p $topdir/lsndir_rocks
  xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup_rocks \
      --extra-lsndir=$topdir/lsndir_rocks

  size11=$(get_backup_size "$topdir/lsndir_rocks/xtrabackup_info")
  assert_positive "$size11" "rocksdb+compress backup_size"
  usize11=$(get_uncompressed_size "$topdir/lsndir_rocks/xtrabackup_info")
  assert_positive "$usize11" "rocksdb+compress uncompressed_size"

  grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log (rocksdb)"
  grep -q "Uncompressed size:" $OUTFILE || die "Expected 'Uncompressed size:' in log (rocksdb)"
  grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log (rocksdb)"

  vlog "Scenario 11 passed: backup_size=$size11 uncompressed=$usize11"
  rm -rf $topdir/backup_rocks $topdir/lsndir_rocks
else
  vlog "=== Scenario 11: SKIPPED (RocksDB not available) ==="
fi

############################################################################
# Scenario 12: Encrypted InnoDB tablespaces + compress (keyring component)
# Server-side encrypted .ibd files bypass xtrabackup's compress datasink.
# Verifies ds_statistics counts bytes on the bypass path.
############################################################################
vlog "=== Scenario 12: Encrypted InnoDB + compress ==="

stop_server

KEYRING_TYPE="component"
. inc/keyring_common.sh
. inc/keyring_file.sh
configure_server_with_component

mysql -e "CREATE TABLE t_enc (a INT PRIMARY KEY AUTO_INCREMENT, b TEXT) ENCRYPTION='y' ENGINE=InnoDB;" test
for i in $(seq 1 500) ; do
  echo "INSERT INTO t_enc (b) VALUES (REPEAT(UUID(), 10));"
done | mysql test

mkdir -p $topdir/lsndir_enc_tbl
xtrabackup --backup --compress=lz4 \
    --target-dir=$topdir/backup_enc_tbl \
    --extra-lsndir=$topdir/lsndir_enc_tbl \
    ${keyring_args}

size12=$(get_backup_size "$topdir/lsndir_enc_tbl/xtrabackup_info")
assert_positive "$size12" "encrypted-innodb+compress backup_size"
usize12=$(get_uncompressed_size "$topdir/lsndir_enc_tbl/xtrabackup_info")
assert_positive "$usize12" "encrypted-innodb+compress uncompressed_size"

grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log (encrypted-innodb)"
grep -q "Uncompressed size:" $OUTFILE || die "Expected 'Uncompressed size:' in log (encrypted-innodb)"
grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log (encrypted-innodb)"

vlog "Scenario 12 passed: backup_size=$size12 uncompressed=$usize12"
rm -rf $topdir/backup_enc_tbl $topdir/lsndir_enc_tbl
cleanup_keyring

############################################################################
# Scenario 13: Redo log encryption + compress (keyring component)
# innodb_redo_log_encrypt=ON requires keyring component. Redo logs are
# encrypted server-side. Verifies sizes are correctly reported.
############################################################################
vlog "=== Scenario 13: Redo log encryption + compress ==="

stop_server

MYSQLD_EXTRA_MY_CNF_OPTS="
innodb_redo_log_encrypt=ON
innodb_undo_log_encrypt=ON
"
KEYRING_TYPE="component"
. inc/keyring_common.sh
. inc/keyring_file.sh
configure_server_with_component

mysql -e "CREATE TABLE t_redo_enc (a INT PRIMARY KEY AUTO_INCREMENT, b TEXT) ENGINE=InnoDB;" test
for i in $(seq 1 500) ; do
  echo "INSERT INTO t_redo_enc (b) VALUES (REPEAT(UUID(), 10));"
done | mysql test

mkdir -p $topdir/lsndir_redo_enc
xtrabackup --backup --compress=lz4 \
    --target-dir=$topdir/backup_redo_enc \
    --extra-lsndir=$topdir/lsndir_redo_enc \
    ${keyring_args}

size13=$(get_backup_size "$topdir/lsndir_redo_enc/xtrabackup_info")
assert_positive "$size13" "redo-encrypt+compress backup_size"
usize13=$(get_uncompressed_size "$topdir/lsndir_redo_enc/xtrabackup_info")
assert_positive "$usize13" "redo-encrypt+compress uncompressed_size"

grep -q "Backup size:" $OUTFILE || die "Expected 'Backup size:' in log (redo-encrypt)"
grep -q "Uncompressed size:" $OUTFILE || die "Expected 'Uncompressed size:' in log (redo-encrypt)"
grep -q "Compression ratio:" $OUTFILE || die "Expected 'Compression ratio:' in log (redo-encrypt)"

vlog "Scenario 13 passed: backup_size=$size13 uncompressed=$usize13"
rm -rf $topdir/backup_redo_enc $topdir/lsndir_redo_enc
cleanup_keyring

vlog "All backup_size scenarios passed."
