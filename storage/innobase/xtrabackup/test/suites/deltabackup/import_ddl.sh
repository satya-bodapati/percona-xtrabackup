###############################################################################
# PXB-XXXX: Delta backup vs DISCARD/IMPORT TABLESPACE (both ordering cases)
#
# Import is a physical, non-redo-logged bring-in preceded by a DISCARD. Two
# flows, both exercised here with a post-import INSERT that must survive:
#
#   same-id : ALTER t DISCARD; IMPORT           -> table keeps its space id
#   new-id  : DROP t; CREATE t; DISCARD; IMPORT -> table gets a fresh space id
#
# In both, the journal records DROP(x) then CREATE(x) for the imported id, so
# journal_create() resolves the recreate (recopy, not .del), and prepare
# ignores the logged DISCARD delete (.reimport marker) so the post-import
# INSERT is not dropped by recovery. Restore is verified against a mysqldump.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

# $1 = "same_id" | "new_id"
function run_import_test() {
  local MODE=$1
  vlog "=== import test: $MODE ==="

  $MYSQL $MYSQL_ARGS -Ns -e "DROP TABLE IF EXISTS test.t_imp; CREATE TABLE test.t_imp (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200));" test
  PAD=$(printf 'x%.0s' {1..100})
  multi_row_insert test.t_imp \({1..200},\'${PAD}\'\)
  innodb_wait_for_flush_all

  # Export a consistent copy for the mid-backup import (same client session so
  # the .cfg exists while we copy).
  rm -rf $topdir/export; mkdir -p $topdir/export
  run_cmd $MYSQL $MYSQL_ARGS test <<SQL
FLUSH TABLES t_imp FOR EXPORT;
system cp $mysql_datadir/test/t_imp.ibd $topdir/export/ && cp $mysql_datadir/test/t_imp.cfg $topdir/export/ && echo EXPORTED
UNLOCK TABLES;
SQL
  [ -f $topdir/export/t_imp.cfg ] || die "export failed (.cfg missing)"

  local BK=$topdir/backup_$MODE
  XB_ERROR_LOG=$BK.log
  xtrabackup --backup --lock-ddl=REDUCED --copy-strategy=clone \
    --target-dir=$BK --debug-sync="ddl_tracker_before_lock_ddl" \
    2> >( tee -a $BK.log)&
  local job_pid=$!
  local pid_file=$BK/xtrabackup_debug_sync
  wait_for_xb_to_suspend $pid_file
  local xb_pid=`cat $pid_file`

  # DISCARD+IMPORT during the pause. new-id adds a DROP+CREATE first so the
  # re-created table gets a fresh space id.
  if [ "$MODE" = "new_id" ]; then
    $MYSQL $MYSQL_ARGS -Ns -e "DROP TABLE test.t_imp; CREATE TABLE test.t_imp (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200));" test
  fi
  $MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.t_imp DISCARD TABLESPACE;" test
  cp $topdir/export/t_imp.ibd $topdir/export/t_imp.cfg $mysql_datadir/test/
  $MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.t_imp IMPORT TABLESPACE;" test
  # post-import DML in the SAME window -- the row that used to be lost
  $MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.t_imp(pad) VALUES (REPEAT('i', 120));" test

  [ "$($MYSQL $MYSQL_ARGS -Ns -e 'SELECT COUNT(*) FROM test.t_imp')" = "201" ] \
    || die "IMPORT did not succeed on the source ($MODE)"

  vlog "Resuming xtrabackup ($MODE)"
  kill -SIGCONT $xb_pid
  run_cmd wait $job_pid

  egrep -q "recreate/import.* space ID: [0-9]*" $BK.log \
    || die "reimport not resolved as a recreate ($MODE)"
  [ ! -f "$BK/#xb_delta/test/t_imp.ibd.delta" ] \
    || die "unexpected delta for the reimported tablespace ($MODE)"

  xtrabackup --prepare --target-dir=$BK 2> >( tee $BK.prepare.log)
  egrep -q "Ignoring logged delete for reimported tablespace" $BK.prepare.log \
    || die "prepare did not suppress the logged delete ($MODE)"

  record_db_state test
  stop_server
  rm -rf $mysql_datadir/*
  xtrabackup --copy-back --target-dir=$BK
  start_server
  verify_db_state test
  rm -rf $BK $BK.log $BK.prepare.log $topdir/export
}

run_import_test same_id
run_import_test new_id

stop_server
rm -rf $mysql_datadir
