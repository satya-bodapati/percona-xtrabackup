###############################################################################
# PXB-XXXX: Delta backup — RENAME TABLE after the table's file was copied
#
# The table is modified inside the tracking window and copied under its old
# name; the rename happens after the file copy (second pause). The delta
# pass must read the pages through the NEW on-disk path but ship the
# .delta/.meta under the OLD name, and prepare's .ren handling must rename
# the base file and the .delta/.meta together before the delta is applied.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.original_table (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200)); INSERT INTO test.original_table(pad) VALUES (REPEAT('a', 100)), (REPEAT('b', 100)), (REPEAT('c', 100));" test

innodb_wait_for_flush_all

# Pause 1 (thread-sync) before the file copy: modify the table inside the
# tracking window. Pause 2 (SIGSTOP-sync) fires after the file copy, when
# the redo manager has caught up: rename the table there.
XB_ERROR_LOG=$topdir/backup_ren.log
xtrabackup_background --backup --target-dir=$topdir/backup_ren \
  --debug-sync-thread="before_file_copy" \
  --debug-sync="xtrabackup_pause_after_redo_catchup" \
  --lock-ddl=REDUCED --copy-strategy=page-tracking

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"

# In-window DML on the to-be-renamed table
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.original_table SET pad = REPEAT('r', 150);" test
innodb_wait_for_flush_all

resume_debug_sync_thread "before_file_copy" $topdir/backup_ren

# Wait for the post-file-copy pause (redo manager started, C1 fixed)
pid_file=$topdir/backup_ren/xtrabackup_debug_sync
wait_for_xb_to_suspend $pid_file
xb_pid=`cat $pid_file`

# The table's file was copied under its old name; rename it now
$MYSQL $MYSQL_ARGS -Ns -e "RENAME TABLE test.original_table TO test.renamed_table; INSERT INTO test.renamed_table(pad) VALUES (REPEAT('d', 100));" test

vlog "Resuming xtrabackup"
kill -SIGCONT $xb_pid
run_cmd wait $job_pid

# The delta pass must have read through the new path and shipped old-name files
if ! egrep -q "delta backup: renamed space [0-9]+ will be read from ./test/renamed_table.ibd and written as delta of test/original_table.ibd" $topdir/backup_ren.log ; then
  die "delta pass did not repoint the renamed space"
fi

if [ ! -f $topdir/backup_ren/test/original_table.ibd.delta ]; then
  die "renamed table has no .delta under its old name"
fi
if [ ! -f $topdir/backup_ren/test/original_table.ibd.meta ]; then
  die "renamed table has no .meta under its old name"
fi

# base file must be present under the OLD name (copied before the rename),
# and there must be no full recopy (.new) for this table
if [ ! -f $topdir/backup_ren/test/original_table.ibd ]; then
  die "base file missing under the old name"
fi
if ls $topdir/backup_ren/test/*.new >/dev/null 2>&1 ; then
  die "unexpected full recopy for a rename-after-copy"
fi

xtrabackup --prepare --target-dir=$topdir/backup_ren \
  2> >( tee $topdir/prepare_ren.log)

# .ren handling renames the base file AND the .delta/.meta to the new name
if ! egrep -q "Renaming incremental delta file from: .*original_table.ibd.delta to: .*renamed_table.ibd.delta" $topdir/prepare_ren.log ; then
  die "prepare .ren handling did not rename the delta file"
fi

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_ren
start_server
verify_db_state test
stop_server
rm -rf $mysql_datadir $topdir/backup_ren
