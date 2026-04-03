############################################################################
# Test --check-tables on prepare: positive multi-combination scenario,
# real IBD corruption (broken sibling link), and page checksum corruption.
############################################################################

. inc/keyring_file.sh

start_server --innodb_file_per_table

mysql test <<EOF
CREATE TABLE t_enc (a INT PRIMARY KEY AUTO_INCREMENT, b VARCHAR(100)) ENCRYPTION='y';
CREATE TABLE t_comp (a INT PRIMARY KEY AUTO_INCREMENT, b VARCHAR(100)) ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4;
CREATE TABLE t_plain (a INT PRIMARY KEY AUTO_INCREMENT, b VARCHAR(100));
EOF

for i in $(seq 1 100); do
  run_cmd $MYSQL $MYSQL_ARGS test -e \
    "INSERT INTO t_enc (b) VALUES ('enc_init_${i}');
     INSERT INTO t_comp (b) VALUES ('comp_init_${i}');
     INSERT INTO t_plain (b) VALUES ('plain_init_${i}');"
done

vlog "Full backup"
xtrabackup --backup --target-dir=$topdir/full \
           --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}

for i in $(seq 101 200); do
  run_cmd $MYSQL $MYSQL_ARGS test -e \
    "INSERT INTO t_enc (b) VALUES ('enc_inc1_${i}');
     INSERT INTO t_comp (b) VALUES ('comp_inc1_${i}');
     INSERT INTO t_plain (b) VALUES ('plain_inc1_${i}');"
done

vlog "Incremental backup 1"
xtrabackup --backup --incremental-basedir=$topdir/full \
           --target-dir=$topdir/inc1 \
           --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}

for i in $(seq 201 300); do
  run_cmd $MYSQL $MYSQL_ARGS test -e \
    "INSERT INTO t_enc (b) VALUES ('enc_inc2_${i}');
     INSERT INTO t_comp (b) VALUES ('comp_inc2_${i}');
     INSERT INTO t_plain (b) VALUES ('plain_inc2_${i}');"
done

vlog "Incremental backup 2"
xtrabackup --backup --incremental-basedir=$topdir/inc1 \
           --target-dir=$topdir/inc2 \
           --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}

record_db_state test

vlog "Prepare full with --apply-log-only --check-tables (should skip check)"
xtrabackup --prepare --apply-log-only --check-tables --target-dir=$topdir/full \
           --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args} \
           2>&1 | tee $topdir/prepare_full.log
grep log-applied $topdir/full/xtrabackup_checkpoints
if grep -q "Starting table checks" $topdir/prepare_full.log; then
  die "check-tables should not run during --apply-log-only prepare"
fi

vlog "Prepare inc1 with --apply-log-only --check-tables (should skip check)"
xtrabackup --prepare --apply-log-only --check-tables \
           --incremental-dir=$topdir/inc1 --target-dir=$topdir/full \
           --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args} \
           2>&1 | tee $topdir/prepare_inc1.log
grep log-applied $topdir/full/xtrabackup_checkpoints
if grep -q "Starting table checks" $topdir/prepare_inc1.log; then
  die "check-tables should not run during incremental --apply-log-only prepare"
fi

vlog "Prepare inc2 with --check-tables (final prepare, should run check)"
xtrabackup --prepare --check-tables \
           --incremental-dir=$topdir/inc2 --target-dir=$topdir/full \
           --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args} \
           2>&1 | tee $topdir/prepare_inc2.log
grep full-prepared $topdir/full/xtrabackup_checkpoints
grep -q "Starting table checks" $topdir/prepare_inc2.log || \
  die "check-tables did not start on final prepare"
grep -q "Checking: test/t_enc" $topdir/prepare_inc2.log || \
  die "Encrypted table not checked"
grep -q "Checking: test/t_comp" $topdir/prepare_inc2.log || \
  die "Compressed table not checked"
grep -q "Checking: test/t_plain" $topdir/prepare_inc2.log || \
  die "Plain table not checked"
grep -q "All table checks passed" $topdir/prepare_inc2.log || \
  die "Table checks did not pass"

vlog "Restore and verify"
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/full
start_server
verify_db_state test

#
# Scenario 2: Real IBD corruption (broken FIL_PAGE_NEXT sibling link)
#
vlog "=== Scenario 2: Real IBD corruption ==="

vlog "Create a table with enough rows for multiple leaf pages"
mysql test <<EOF
CREATE TABLE t1 (a INT PRIMARY KEY AUTO_INCREMENT, b VARCHAR(200));
EOF

for i in $(seq 1 200); do
  run_cmd $MYSQL $MYSQL_ARGS test -e \
    "INSERT INTO t1 (b) VALUES (REPEAT('x', 200));"
done

xtrabackup --backup --target-dir=$topdir/backup2

vlog "Prepare with --apply-log-only first"
xtrabackup --prepare --apply-log-only --target-dir=$topdir/backup2

vlog "Corrupt PAGE_INDEX_ID on leaf page 6 of test/t1.ibd"
page_size=16384
page_no=6
# PAGE_INDEX_ID sits at PAGE_HEADER(38) + 28 = 66 bytes from page start
index_id_offset=$((page_size * page_no + 66))
printf '\xff\xff\xff\xff\xff\xff\xff\xff' | dd of=$topdir/backup2/test/t1.ibd \
  bs=1 seek=$index_id_offset count=8 conv=notrunc
$MYSQL_BASEDIR/bin/innochecksum -w crc32 --no-check $topdir/backup2/test/t1.ibd

vlog "Prepare with --check-tables should fail"
run_cmd_expect_failure $XB_BIN $XB_ARGS --prepare --check-tables \
  --target-dir=$topdir/backup2 2>&1 | tee $topdir/prepare_corrupt.log

grep -q "Starting table checks" $topdir/prepare_corrupt.log || \
  die "check-tables did not start"
grep -q "is corrupted" $topdir/prepare_corrupt.log || \
  die "Corruption not detected"
grep -q "Table check failed" $topdir/prepare_corrupt.log || \
  die "Table check failed message not found"

#
# Scenario 3: Page checksum corruption (no checksum fix)
#
vlog "=== Scenario 3: Page checksum corruption ==="

mysql test <<EOF
CREATE TABLE t2 (a INT PRIMARY KEY AUTO_INCREMENT, b VARCHAR(200));
EOF

for i in $(seq 1 200); do
  run_cmd $MYSQL $MYSQL_ARGS test -e \
    "INSERT INTO t2 (b) VALUES (REPEAT('y', 200));"
done

xtrabackup --backup --target-dir=$topdir/backup3

vlog "Prepare with --apply-log-only first"
xtrabackup --prepare --apply-log-only --target-dir=$topdir/backup3

vlog "Corrupt a data byte in page 4 WITHOUT fixing checksum"
page_size=16384
page_no=4
offset=$((page_size * page_no + 100))
printf '\xDE' | dd of=$topdir/backup3/test/t2.ibd \
  bs=1 seek=$offset count=1 conv=notrunc

vlog "Prepare with --check-tables should fail (checksum mismatch)"
run_cmd_expect_failure $XB_BIN $XB_ARGS --prepare --check-tables \
  --target-dir=$topdir/backup3 2>&1 | tee $topdir/prepare_checksum.log

vlog "Scenario 3 passed: xtrabackup exited with non-zero status"
