############################################################################
# Test backup_size / uncompressed_backup_size reporting in xtrabackup_info.
#
# Validates byte-perfect invariants across all transport / format
# combinations (target-dir, xbstream, compress, encrypt, incremental,
# RocksDB bypass, server-encrypted InnoDB, redo encryption, sparse files).
#
# Sampling note: backup_size is sampled inside get_xtrabackup_info() and
# embedded in the file content.  That sample is taken AGAIN when --extra-
# lsndir's xtrabackup_info is written by xtrabackup_write_info(), which
# happens AFTER backup_finish() has already written the target's
# xtrabackup_info through ds_data.  So the value we read from the extra-
# lsndir copy reflects the FINAL leaf bytes_written -- it ALREADY counts
# every byte that ends up on disk, including the target's own
# xtrabackup_info[.lz4|.xbcrypt|...].  Therefore the invariants are:
#
#   Invariant A  (target-dir):  backup_size == sum_file_bytes(target)
#   Invariant A' (stream     ):  backup_size == size of .xbs file
#   Invariant B  (--compress ):  uncompressed_backup_size == sum_file_bytes(decompressed)
#
# All three are exact: zero tolerance.
#
# Sparse scenarios (14, 15) are LOOSE for backup_size because:
#   - backup_size counts packed data bytes only (no holes).
#   - On-disk apparent size INCLUDES the punched holes.
#   - On-disk allocated bytes (%b * 512) would round to FS-block
#     boundaries and the +1-byte trailing-hole fix in local_close()
#     allocates one whole FS block (~4 KiB) which the metric counts as
#     1 byte.  Neither apparent nor allocated equals backup_size to the
#     byte for sparse files.  Invariant B (decompress) still gives a
#     byte-perfect check for scenario 15 because the decompressor writes
#     the data dense (no holes).
############################################################################

. inc/common.sh

require_zstd
require_lz4

############################################################################
# Helpers (get_field, file_size, sum_file_bytes, find_info_file,
# assert_positive, assert_no_field, assert_eq, assert_target_strict,
# assert_stream_strict, assert_decompressed_strict) are sourced from
# inc/common.sh above.
############################################################################

start_server --innodb_file_per_table

load_sakila

ENCKEY="percona_xtrabackup_is_awesome___"

############################################################################
# Scenario 1: Plain --target-dir
############################################################################
vlog "=== Scenario 1: Plain --target-dir ==="

mkdir -p $topdir/lsn1
xtrabackup --backup --target-dir=$topdir/backup1 --extra-lsndir=$topdir/lsn1 \
    2> >(tee $topdir/log1 >&2)

bs1=$(get_field "$topdir/lsn1/xtrabackup_info" backup_size)
assert_positive "$bs1" "scen1 backup_size"
assert_no_field "$topdir/lsn1/xtrabackup_info" uncompressed_backup_size
grep -q "Backup size:" $topdir/log1 || die "scen1: missing 'Backup size:' in log"

assert_target_strict "$topdir/backup1" "$bs1" "scen1 (plain target)"

rm -rf $topdir/backup1 $topdir/lsn1 $topdir/log1

############################################################################
# Scenario 2: Plain --stream=xbstream
############################################################################
vlog "=== Scenario 2: Plain --stream=xbstream ==="

mkdir -p $topdir/lsn2
xtrabackup --backup --stream=xbstream --extra-lsndir=$topdir/lsn2 \
    > $topdir/backup2.xbs 2> >(tee $topdir/log2 >&2)

bs2=$(get_field "$topdir/lsn2/xtrabackup_info" backup_size)
assert_positive "$bs2" "scen2 backup_size"
assert_no_field "$topdir/lsn2/xtrabackup_info" uncompressed_backup_size

assert_stream_strict "$topdir/backup2.xbs" "$bs2" "scen2 (plain stream)"

rm -rf $topdir/backup2.xbs $topdir/lsn2 $topdir/log2

############################################################################
# Scenario 3: Compressed (lz4) --target-dir
############################################################################
vlog "=== Scenario 3: Compressed (lz4) --target-dir ==="

mkdir -p $topdir/lsn3
xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup3 \
    --extra-lsndir=$topdir/lsn3 \
    2> >(tee $topdir/log3 >&2)

bs3=$(get_field "$topdir/lsn3/xtrabackup_info" backup_size)
us3=$(get_field "$topdir/lsn3/xtrabackup_info" uncompressed_backup_size)
assert_positive "$bs3" "scen3 backup_size"
assert_positive "$us3" "scen3 uncompressed_backup_size"
grep -q "Backup size:"       $topdir/log3 || die "scen3: missing 'Backup size:' in log"
grep -q "Uncompressed backup size:" $topdir/log3 || die "scen3: missing 'Uncompressed backup size:' in log"
grep -q "Compression ratio:" $topdir/log3 || die "scen3: missing 'Compression ratio:' in log"

assert_target_strict      "$topdir/backup3" "$bs3" "scen3 (compress target)"
assert_decompressed_strict "$topdir/backup3" "$us3" "scen3 (compress target)" ""

rm -rf $topdir/backup3 $topdir/lsn3 $topdir/log3

############################################################################
# Scenario 4: Encrypted (no compress) --target-dir
############################################################################
vlog "=== Scenario 4: Encrypted --target-dir ==="

mkdir -p $topdir/lsn4
xtrabackup --backup --encrypt=AES256 --encrypt-key="$ENCKEY" \
    --target-dir=$topdir/backup4 --extra-lsndir=$topdir/lsn4 \
    2> >(tee $topdir/log4 >&2)

bs4=$(get_field "$topdir/lsn4/xtrabackup_info" backup_size)
assert_positive "$bs4" "scen4 backup_size"
assert_no_field "$topdir/lsn4/xtrabackup_info" uncompressed_backup_size

assert_target_strict "$topdir/backup4" "$bs4" "scen4 (encrypt target)"

rm -rf $topdir/backup4 $topdir/lsn4 $topdir/log4

############################################################################
# Scenario 5: Compressed + xbstream
############################################################################
vlog "=== Scenario 5: --compress=lz4 --stream=xbstream ==="

mkdir -p $topdir/lsn5
xtrabackup --backup --stream=xbstream --compress=lz4 \
    --extra-lsndir=$topdir/lsn5 \
    > $topdir/backup5.xbs 2> >(tee $topdir/log5 >&2)

bs5=$(get_field "$topdir/lsn5/xtrabackup_info" backup_size)
us5=$(get_field "$topdir/lsn5/xtrabackup_info" uncompressed_backup_size)
assert_positive "$bs5" "scen5 backup_size"
assert_positive "$us5" "scen5 uncompressed_backup_size"

assert_stream_strict "$topdir/backup5.xbs" "$bs5" "scen5 (compress stream)"

mkdir -p $topdir/extract5
xbstream -x -C $topdir/extract5 < $topdir/backup5.xbs
assert_decompressed_strict "$topdir/extract5" "$us5" "scen5 (compress stream)" ""

rm -rf $topdir/backup5.xbs $topdir/extract5 $topdir/lsn5 $topdir/log5

############################################################################
# Scenario 6: Encrypted + xbstream (no compress)
############################################################################
vlog "=== Scenario 6: --encrypt + --stream=xbstream ==="

mkdir -p $topdir/lsn6
xtrabackup --backup --stream=xbstream --encrypt=AES256 --encrypt-key="$ENCKEY" \
    --extra-lsndir=$topdir/lsn6 \
    > $topdir/backup6.xbs 2> >(tee $topdir/log6 >&2)

bs6=$(get_field "$topdir/lsn6/xtrabackup_info" backup_size)
assert_positive "$bs6" "scen6 backup_size"
assert_no_field "$topdir/lsn6/xtrabackup_info" uncompressed_backup_size

assert_stream_strict "$topdir/backup6.xbs" "$bs6" "scen6 (encrypt stream)"

rm -rf $topdir/backup6.xbs $topdir/lsn6 $topdir/log6

############################################################################
# Scenario 7: Compressed + Encrypted + xbstream (the kitchen sink)
############################################################################
vlog "=== Scenario 7: --compress + --encrypt + --stream=xbstream ==="

mkdir -p $topdir/lsn7
xtrabackup --backup --compress=lz4 --encrypt=AES256 --encrypt-key="$ENCKEY" \
    --stream=xbstream --extra-lsndir=$topdir/lsn7 \
    > $topdir/backup7.xbs 2> >(tee $topdir/log7 >&2)

bs7=$(get_field "$topdir/lsn7/xtrabackup_info" backup_size)
us7=$(get_field "$topdir/lsn7/xtrabackup_info" uncompressed_backup_size)
assert_positive "$bs7" "scen7 backup_size"
assert_positive "$us7" "scen7 uncompressed_backup_size"

assert_stream_strict "$topdir/backup7.xbs" "$bs7" "scen7 (compress+encrypt stream)"

mkdir -p $topdir/extract7
xbstream -x -C $topdir/extract7 < $topdir/backup7.xbs
assert_decompressed_strict "$topdir/extract7" "$us7" "scen7 (compress+encrypt stream)" "$ENCKEY"

rm -rf $topdir/backup7.xbs $topdir/extract7 $topdir/lsn7 $topdir/log7

############################################################################
# Scenario 8: Restore validation (plain backup, full prepare + copy-back)
############################################################################
vlog "=== Scenario 8: Restore validation ==="

xtrabackup --backup --target-dir=$topdir/backup8
record_db_state sakila
xtrabackup --prepare --target-dir=$topdir/backup8
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup8
start_server
verify_db_state sakila
rm -rf $topdir/backup8

############################################################################
# Scenario 9: Incremental chain (no compress)
############################################################################
vlog "=== Scenario 9: Incremental chain (no compress) ==="

mysql -e "CREATE TABLE t_inc (a INT PRIMARY KEY AUTO_INCREMENT, b TEXT) ENGINE=InnoDB;" test
for i in $(seq 1 500) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

mkdir -p $topdir/lsn9full
xtrabackup --backup --target-dir=$topdir/backup9full \
    --extra-lsndir=$topdir/lsn9full

bs9full=$(get_field "$topdir/lsn9full/xtrabackup_info" backup_size)
assert_target_strict "$topdir/backup9full" "$bs9full" "scen9 (inc full)"

for i in $(seq 1 100) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

mkdir -p $topdir/lsn9inc1
xtrabackup --backup --incremental-basedir=$topdir/backup9full \
    --target-dir=$topdir/backup9inc1 --extra-lsndir=$topdir/lsn9inc1

bs9inc1=$(get_field "$topdir/lsn9inc1/xtrabackup_info" backup_size)
assert_target_strict "$topdir/backup9inc1" "$bs9inc1" "scen9 (inc1)"
[ "$bs9inc1" -lt "$bs9full" ] || die "scen9: inc1 ($bs9inc1) should be < full ($bs9full)"

ls $topdir/backup9inc1/test/*.delta > /dev/null 2>&1 || die "scen9: missing .delta in inc1"
ls $topdir/backup9inc1/test/*.meta  > /dev/null 2>&1 || die "scen9: missing .meta  in inc1"

for i in $(seq 1 50) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

mkdir -p $topdir/lsn9inc2
xtrabackup --backup --incremental-basedir=$topdir/backup9inc1 \
    --target-dir=$topdir/backup9inc2 --extra-lsndir=$topdir/lsn9inc2

bs9inc2=$(get_field "$topdir/lsn9inc2/xtrabackup_info" backup_size)
assert_target_strict "$topdir/backup9inc2" "$bs9inc2" "scen9 (inc2)"

# Restore the chain to validate end-to-end
record_db_state test
xtrabackup --prepare --apply-log-only --target-dir=$topdir/backup9full
xtrabackup --prepare --apply-log-only --incremental-dir=$topdir/backup9inc1 \
    --target-dir=$topdir/backup9full
xtrabackup --prepare --apply-log-only --incremental-dir=$topdir/backup9inc2 \
    --target-dir=$topdir/backup9full
xtrabackup --prepare --target-dir=$topdir/backup9full
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup9full
start_server
verify_db_state test

rm -rf $topdir/backup9full $topdir/backup9inc1 $topdir/backup9inc2 \
       $topdir/lsn9full   $topdir/lsn9inc1   $topdir/lsn9inc2

############################################################################
# Scenario 10: Incremental chain with compress
############################################################################
vlog "=== Scenario 10: Incremental chain (compress=lz4) ==="

mysql -e "TRUNCATE TABLE t_inc;" test
for i in $(seq 1 500) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

mkdir -p $topdir/lsn10full
xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup10full \
    --extra-lsndir=$topdir/lsn10full

bs10full=$(get_field "$topdir/lsn10full/xtrabackup_info" backup_size)
us10full=$(get_field "$topdir/lsn10full/xtrabackup_info" uncompressed_backup_size)
assert_positive "$us10full" "scen10 full uncompressed_backup_size"

assert_target_strict       "$topdir/backup10full" "$bs10full" "scen10 (full compress target)"
assert_decompressed_strict "$topdir/backup10full" "$us10full" "scen10 (full compress target)" ""

for i in $(seq 1 100) ; do
  echo "INSERT INTO t_inc (b) VALUES (REPEAT(UUID(), 20));"
done | mysql test

mkdir -p $topdir/lsn10inc1
xtrabackup --backup --compress=lz4 \
    --incremental-basedir=$topdir/lsn10full \
    --target-dir=$topdir/backup10inc1 --extra-lsndir=$topdir/lsn10inc1

bs10inc1=$(get_field "$topdir/lsn10inc1/xtrabackup_info" backup_size)
us10inc1=$(get_field "$topdir/lsn10inc1/xtrabackup_info" uncompressed_backup_size)
assert_positive "$us10inc1" "scen10 inc1 uncompressed_backup_size"

assert_target_strict       "$topdir/backup10inc1" "$bs10inc1" "scen10 (inc1 compress target)"
assert_decompressed_strict "$topdir/backup10inc1" "$us10inc1" "scen10 (inc1 compress target)" ""

[ "$bs10inc1" -lt "$bs10full" ] || die "scen10: inc1 ($bs10inc1) should be < full ($bs10full)"

rm -rf $topdir/backup10full $topdir/backup10inc1 $topdir/lsn10full $topdir/lsn10inc1

############################################################################
# Scenario 11: --compress + RocksDB (bypass via ds_uncompressed_data)
############################################################################
if test -f $(dirname ${MYSQLD})/../lib/plugin/ha_rocksdb.so ; then
  vlog "=== Scenario 11: --compress + RocksDB ==="

  stop_server
  rm -rf $mysql_datadir
  start_server --innodb_file_per_table

  init_rocksdb

  mysql -e "CREATE TABLE t_rocks (a INT PRIMARY KEY AUTO_INCREMENT, b INT, c VARCHAR(200)) ENGINE=ROCKSDB;" test
  for i in $(seq 1 500) ; do
    echo "INSERT INTO t_rocks (b, c) VALUES (FLOOR(RAND() * 1000000), UUID());"
  done | mysql test

  mkdir -p $topdir/lsn11
  xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup11 \
      --extra-lsndir=$topdir/lsn11

  bs11=$(get_field "$topdir/lsn11/xtrabackup_info" backup_size)
  us11=$(get_field "$topdir/lsn11/xtrabackup_info" uncompressed_backup_size)
  assert_positive "$us11" "scen11 uncompressed_backup_size"

  assert_target_strict       "$topdir/backup11" "$bs11" "scen11 (rocksdb+compress target)"
  assert_decompressed_strict "$topdir/backup11" "$us11" "scen11 (rocksdb+compress target)" ""

  rm -rf $topdir/backup11 $topdir/lsn11
else
  vlog "=== Scenario 11: SKIPPED (RocksDB not available) ==="
fi

############################################################################
# Scenario 12: --compress + server-encrypted InnoDB tablespaces
############################################################################
vlog "=== Scenario 12: --compress + server-encrypted InnoDB ==="

stop_server

KEYRING_TYPE="component"
. inc/keyring_common.sh
. inc/keyring_file.sh
configure_server_with_component

mysql -e "CREATE TABLE t_enc (a INT PRIMARY KEY AUTO_INCREMENT, b TEXT) ENCRYPTION='y' ENGINE=InnoDB;" test
for i in $(seq 1 500) ; do
  echo "INSERT INTO t_enc (b) VALUES (REPEAT(UUID(), 10));"
done | mysql test

mkdir -p $topdir/lsn12
xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup12 \
    --extra-lsndir=$topdir/lsn12 ${keyring_args}

bs12=$(get_field "$topdir/lsn12/xtrabackup_info" backup_size)
us12=$(get_field "$topdir/lsn12/xtrabackup_info" uncompressed_backup_size)
assert_positive "$us12" "scen12 uncompressed_backup_size"

assert_target_strict       "$topdir/backup12" "$bs12" "scen12 (enc-innodb+compress target)"
assert_decompressed_strict "$topdir/backup12" "$us12" "scen12 (enc-innodb+compress target)" ""

rm -rf $topdir/backup12 $topdir/lsn12
cleanup_keyring

############################################################################
# Scenario 13: --compress + server redo log encryption
############################################################################
vlog "=== Scenario 13: --compress + redo log encryption ==="

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

mkdir -p $topdir/lsn13
xtrabackup --backup --compress=lz4 --target-dir=$topdir/backup13 \
    --extra-lsndir=$topdir/lsn13 ${keyring_args}

bs13=$(get_field "$topdir/lsn13/xtrabackup_info" backup_size)
us13=$(get_field "$topdir/lsn13/xtrabackup_info" uncompressed_backup_size)
assert_positive "$us13" "scen13 uncompressed_backup_size"

assert_target_strict       "$topdir/backup13" "$bs13" "scen13 (redo-enc+compress target)"
assert_decompressed_strict "$topdir/backup13" "$us13" "scen13 (redo-enc+compress target)" ""

rm -rf $topdir/backup13 $topdir/lsn13
cleanup_keyring

############################################################################
# Scenarios 14, 15: Sparse files (InnoDB page compression).  See header
# comment for why backup_size cannot be byte-perfectly compared on disk
# for sparse files.  Invariant B (decompress) still gives a strict check.
############################################################################
stop_server
rm -rf $mysql_datadir
MYSQLD_EXTRA_MY_CNF_OPTS=""
start_server

if grep -q 'PUNCH HOLE support not available' $MYSQLD_ERRFILE ; then
  vlog "=== Scenarios 14-15: SKIPPED (no PUNCH HOLE support) ==="
else
  vlog "=== Scenario 14: Sparse files, no compress ==="

  mysql -e "CREATE TABLE t_sparse (c1 INT AUTO_INCREMENT PRIMARY KEY, c2 BLOB) COMPRESSION='zlib' ENGINE=InnoDB;" test
  mysql -e "INSERT INTO t_sparse (c2) VALUES (REPEAT('x', 5000));" test
  for i in $(seq 1 10) ; do
    mysql -e "INSERT INTO t_sparse (c2) SELECT c2 FROM t_sparse;" test
  done
  innodb_wait_for_flush_all

  if ! is_sparse_file $mysql_datadir/test/t_sparse.ibd ; then
    die "t_sparse.ibd is expected to be sparse but is NOT"
  fi
  record_db_state test

  mkdir -p $topdir/lsn14
  xtrabackup --backup --target-dir=$topdir/backup14 --extra-lsndir=$topdir/lsn14

  bs14=$(get_field "$topdir/lsn14/xtrabackup_info" backup_size)
  assert_positive "$bs14" "scen14 backup_size"
  assert_no_field "$topdir/lsn14/xtrabackup_info" uncompressed_backup_size

  is_sparse_file $topdir/backup14/test/t_sparse.ibd \
      || die "scen14: backed-up t_sparse.ibd is not sparse"

  # Sanity: backup_size must be smaller than apparent on-disk total because
  # apparent includes the holes that backup_size omits.
  apparent14=$(sum_file_bytes "$topdir/backup14")
  [ "$bs14" -lt "$apparent14" ] \
      || die "scen14: backup_size ($bs14) should be < apparent total ($apparent14) due to holes"

  # Restore + verify
  xtrabackup --prepare --target-dir=$topdir/backup14
  stop_server
  rm -rf $mysql_datadir/*
  xtrabackup --copy-back --target-dir=$topdir/backup14
  start_server
  verify_db_state test

  rm -rf $topdir/backup14 $topdir/lsn14

  vlog "=== Scenario 15: Sparse files + --compress=zstd ==="

  mkdir -p $topdir/lsn15
  xtrabackup --backup --compress=zstd --target-dir=$topdir/backup15 \
      --extra-lsndir=$topdir/lsn15

  bs15=$(get_field "$topdir/lsn15/xtrabackup_info" backup_size)
  us15=$(get_field "$topdir/lsn15/xtrabackup_info" uncompressed_backup_size)
  assert_positive "$us15" "scen15 uncompressed_backup_size"

  # Invariant B: decompress and check uncompressed_backup_size against the
  # decompressed payload sum.  Inputs to ds_data for sparse files are
  # packed bytes (no holes), so this matches exactly.
  assert_decompressed_strict "$topdir/backup15" "$us15" "scen15 (sparse+compress target)" ""

  rm -rf $topdir/backup15 $topdir/lsn15
fi

vlog "All backup_size scenarios passed (byte-perfect where strict)."
