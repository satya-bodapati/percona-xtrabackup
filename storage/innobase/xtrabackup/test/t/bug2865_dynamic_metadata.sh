#
# PXB-2865 reproducer: dynamic metadata loss when no checkpoint between backups.
#
# Steps:
#  1. Full backup.
#  2. Disable page cleaner so MLOG_TABLE_DYNAMIC_META records emitted next
#     never get flushed to mysql.ibd (no checkpoint will happen).
#  3. CREATE TABLE + INSERT to bump AUTO_INC -> persistence emits
#     MLOG_TABLE_DYNAMIC_META that stays in redo only.
#  4. Incremental backup.
#  5. Prepare chain (apply-log-only on full + incremental, then bare final
#     --prepare). Under bug, the dynamic metadata records are dropped
#     because the intermediate --apply-log-only neither applies them nor
#     persists them anywhere the final prepare can pick them up.
#  6. Restore + start. Try CREATE TABLE -- a fresh DDL after the bug
#     surfaces in CREATE TABLE failing or AUTO_INC regression.
#  7. With PXB-2865 fix (xtrabackup_dynamic_metadata.json sidecar), records
#     survive across prepares, AUTO_INC matches, CREATE TABLE succeeds.
#
. inc/common.sh

require_debug_server

[ -n "$INNODB_VERSION" ] || skip_test "Requires InnoDB plugin or XtraDB"

FULL_DIR="$topdir/full"
DELTA_DIR="$topdir/delta"

function test_bug_2865()
{
  mysqld_additional_args="--innodb_file_per_table --innodb_strict_mode"

  start_server ${mysqld_additional_args}

  load_dbase_schema incremental_sample

  rm -rf $FULL_DIR
  mkdir -p $FULL_DIR
  rm -rf $DELTA_DIR
  mkdir -p $DELTA_DIR

  vlog "Starting full backup"
  xtrabackup --datadir=$mysql_datadir --backup --target-dir=$FULL_DIR
  vlog "Full backup done"

  # Disable page cleaner so AUTO_INC persistence stays in redo only.
  ${MYSQL} ${MYSQL_ARGS} -e \
    "set global innodb_page_cleaner_disabled_debug=on"

  vlog "Making changes (with page cleaner disabled)"

  for i in $PAGE_SIZES; do
    ${MYSQL} ${MYSQL_ARGS} -e \
      "CREATE TABLE t${i} (a INT(11) AUTO_INCREMENT PRIMARY KEY, \
       number INT(11) DEFAULT NULL) ENGINE=INNODB \
       ROW_FORMAT=compressed KEY_BLOCK_SIZE=$i" incremental_sample
    # Insert enough rows to push AUTO_INC past persistence threshold
    for n in $(seq 1 50); do
      ${MYSQL} ${MYSQL_ARGS} -e \
        "INSERT INTO t${i} (number) VALUES ($n)" incremental_sample
    done
  done

  vlog "Capturing AUTO_INC values before incremental backup"
  declare -A autoinc_before
  for i in $PAGE_SIZES; do
    autoinc_before[$i]=$(${MYSQL} ${MYSQL_ARGS} -Ns -e \
      "SELECT AUTO_INCREMENT FROM information_schema.tables \
       WHERE table_schema='incremental_sample' AND table_name='t${i}'")
    vlog "AUTO_INC for t${i} before incremental: ${autoinc_before[$i]}"
  done

  vlog "Making incremental backup"
  xtrabackup --datadir=$mysql_datadir --backup \
    --target-dir=$DELTA_DIR --incremental-basedir=$FULL_DIR
  vlog "Incremental backup done"

  vlog "Preparing backup"
  xtrabackup --datadir=$mysql_datadir --prepare --apply-log-only \
    --target-dir=$FULL_DIR
  vlog "Base log-applied"

  xtrabackup --datadir=$mysql_datadir --prepare --apply-log-only \
    --target-dir=$FULL_DIR --incremental-dir=$DELTA_DIR
  vlog "Delta log-applied"

  xtrabackup --datadir=$mysql_datadir --prepare --target-dir=$FULL_DIR
  vlog "Final prepare done"

  stop_server

  vlog "Restoring"
  rm -rf $mysql_datadir
  xtrabackup --datadir=$mysql_datadir --copy-back \
    --target-dir=$FULL_DIR $mysqld_additional_args

  start_server ${mysqld_additional_args}

  vlog "Verifying AUTO_INC survived prepare chain"
  for i in $PAGE_SIZES; do
    actual=$(${MYSQL} ${MYSQL_ARGS} -Ns -e \
      "SELECT AUTO_INCREMENT FROM information_schema.tables \
       WHERE table_schema='incremental_sample' AND table_name='t${i}'")
    expected=${autoinc_before[$i]}
    vlog "t${i}: expected AUTO_INC=$expected, actual=$actual"
    if [ "$actual" -lt "$expected" ]; then
      vlog "REGRESSION: t${i} AUTO_INC regressed from $expected to $actual"
      vlog "This is PXB-2865 — dynamic metadata records were lost"
      exit -1
    fi
  done

  vlog "Verifying that DDL on a new table still works after restore"
  ${MYSQL} ${MYSQL_ARGS} -e \
    "CREATE TABLE postrestore_check (x INT PRIMARY KEY)" incremental_sample
  ${MYSQL} ${MYSQL_ARGS} -e \
    "INSERT INTO postrestore_check VALUES (1), (2), (3)" incremental_sample

  vlog "PXB-2865 reproducer PASSED"

  stop_server
}

PAGE_SIZES="1 2 4 8 16"
test_bug_2865
