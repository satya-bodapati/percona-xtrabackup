###############################################################################
# PXB-XXXX: Delta backup vs undo tablespace DDLs
#
# Undo DDLs bypass the DDL log and historically needed special handling in
# reduced lock (before/after-lock undo rediscovery). In delta mode the
# journal's SPACE_UNDO_DDL maps to drop+recreate so the undo space is
# recopied in full under the lock; deltas must never exist for undo spaces
# involved in DDL. Covers: CREATE UNDO, DROP UNDO, and truncation
# (SET INACTIVE) of both an explicit and a default undo tablespace, all
# while the backup is paused right before the lock.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.t1 (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200));" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.t1 \({1..500},\'${PAD}\'\)

# Undo tablespaces that exist before the backup
$MYSQL $MYSQL_ARGS -Ns -e "CREATE UNDO TABLESPACE UNDO_1 ADD DATAFILE 'undo_1.ibu';"
$MYSQL $MYSQL_ARGS -Ns -e "CREATE UNDO TABLESPACE UNDO_2 ADD DATAFILE 'undo_2.ibu';"

innodb_wait_for_flush_all

# Pause right before the backup lock: the file copy (including the undo
# files) is complete, the redo tail is running
XB_ERROR_LOG=$topdir/backup_undo.log
xtrabackup --backup --lock-ddl=REDUCED --copy-strategy=clone \
  --target-dir=$topdir/backup_undo \
  --debug-sync="ddl_tracker_before_lock_ddl" \
  2> >( tee -a $topdir/backup_undo.log)&
job_pid=$!
pid_file=$topdir/backup_undo/xtrabackup_debug_sync
wait_for_xb_to_suspend $pid_file
xb_pid=`cat $pid_file`
echo "backup pid is $job_pid"

# Generate undo workload, then the undo DDL mix:
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.t1 SET pad = REPEAT('u', 150) WHERE id % 2 = 0;" test

# 1. new undo tablespace during backup
$MYSQL $MYSQL_ARGS -Ns -e "CREATE UNDO TABLESPACE UNDO_3 ADD DATAFILE 'undo_3.ibu';"
# 2. truncation of an explicit and a default undo tablespace
$MYSQL $MYSQL_ARGS -Ns -e "ALTER UNDO TABLESPACE UNDO_1 SET INACTIVE;"
$MYSQL $MYSQL_ARGS -Ns -e "ALTER UNDO TABLESPACE innodb_undo_001 SET INACTIVE;"
# 3. drop of an (empty, inactive) undo tablespace
$MYSQL $MYSQL_ARGS -Ns -e "ALTER UNDO TABLESPACE UNDO_2 SET INACTIVE;"
# the drop succeeds once purge has emptied the undo tablespace
dropped=0
for i in $(seq 1 30); do
  if $MYSQL $MYSQL_ARGS -Ns -e "DROP UNDO TABLESPACE UNDO_2;" 2>/dev/null; then
    dropped=1
    break
  fi
  sleep 1
done
if [ "$dropped" != "1" ]; then
  die "could not drop UNDO_2 (purge did not empty it in time)"
fi
$MYSQL $MYSQL_ARGS -Ns -e "ALTER UNDO TABLESPACE innodb_undo_001 SET ACTIVE;"

vlog "Resuming xtrabackup"
kill -SIGCONT $xb_pid
run_cmd wait $job_pid

if ! egrep -q "DDL journal: consumed [0-9]+ events" $topdir/backup_undo.log ; then
  die "backup did not consume the DDL journal"
fi

# undo files affected by DDL must be handled via the undo rediscovery
if ! egrep -q "New undo file: ./undo_3.ibu" $topdir/backup_undo.log ; then
  die "new undo tablespace was not recopied"
fi

# undo spaces must never carry deltas
if find "$topdir/backup_undo/#xb_delta" -name "undo*" 2>/dev/null | grep -q . ; then
  die "found a delta for an undo tablespace"
fi

xtrabackup --prepare --target-dir=$topdir/backup_undo

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_undo
start_server
verify_db_state test

# undo tablespace set must match the source at backup end
undo_count=$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM information_schema.innodb_tablespaces WHERE space_type='Undo' AND name IN ('UNDO_1','UNDO_3')")
if [ "$undo_count" != "2" ]; then
  die "expected UNDO_1 and UNDO_3 after restore, got $undo_count"
fi
if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM information_schema.innodb_tablespaces WHERE name='UNDO_2'")" != "0" ]; then
  die "dropped UNDO_2 resurrected after restore"
fi

stop_server
rm -rf $mysql_datadir $topdir/backup_undo
