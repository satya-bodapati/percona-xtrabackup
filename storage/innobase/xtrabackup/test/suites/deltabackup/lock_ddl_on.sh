###############################################################################
# PXB-XXXX: --copy-strategy=clone with --lock-ddl=ON
#
# Under lock-ddl=ON the instance lock blocks every DDL for the whole backup,
# so the delta mode needs neither the DDL journal nor the fixup machinery:
# file copy with no redo activity, then recopy of the tracked page window
# as #xb_delta, then the short redo tail. Restore verified against a
# mysqldump of the source. DML only while paused (a DDL would block on the
# instance lock).
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.dml_table (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200));" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.dml_table \({1..1000},\'${PAD}\'\)

innodb_wait_for_flush_all

XB_ERROR_LOG=$topdir/backup_on.log
xtrabackup_background --backup --target-dir=$topdir/backup_on \
  --debug-sync-thread="before_file_copy" --lock-ddl=ON \
  --copy-strategy=clone

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"

# DML inside the tracking window (DDL would block on the instance lock)
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.dml_table SET pad = REPEAT('y', 150) WHERE id % 3 = 0;" test
$MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.dml_table(pad) SELECT pad FROM test.dml_table LIMIT 500;" test
innodb_wait_for_flush_all

vlog "Resuming xtrabackup"
resume_debug_sync_thread "before_file_copy" $topdir/backup_on
run_cmd wait $job_pid

if ! egrep -q "delta backup: redo copy started from checkpoint LSN" $topdir/backup_on.log ; then
  die "delta backup did not defer the redo copy"
fi

if ! egrep -q "delta backup: page recopy complete" $topdir/backup_on.log ; then
  die "delta backup did not run the page recopy pass"
fi

# no journal involvement under lock-ddl=ON
if egrep -q "DDL journal:" $topdir/backup_on.log ; then
  die "lock-ddl=ON backup used the DDL journal"
fi

if ! find "$topdir/backup_on/#xb_delta" -name "*.delta" 2>/dev/null | grep -q . ; then
  die "delta backup produced no .delta files"
fi

if ! egrep -q "delta_backup = 1" $topdir/backup_on/xtrabackup_checkpoints ; then
  die "xtrabackup_checkpoints does not record delta_backup = 1"
fi

xtrabackup --prepare --target-dir=$topdir/backup_on \
  2> >( tee $topdir/prepare_on.log)

if ! egrep -q "Delta backup: applying .delta files" $topdir/prepare_on.log ; then
  die "prepare did not apply the delta files"
fi
if [ -d "$topdir/backup_on/#xb_delta" ]; then
  die "prepare did not remove #xb_delta"
fi

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_on
start_server
verify_db_state test
stop_server
rm -rf $mysql_datadir $topdir/backup_on
