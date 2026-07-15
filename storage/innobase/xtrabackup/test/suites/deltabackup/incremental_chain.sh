###############################################################################
# PXB-XXXX: Classic incremental on top of a clone (delta) full
#
# Invariant under test: after --prepare --apply-log-only, a delta full is
# indistinguishable from a classic prepared full (deltas consumed, #xb_page_delta
# removed), so the incremental prepare pipeline — including the deletion of
# .ibd files dropped between the backups (rm_if_not_found) — needs zero
# awareness of the delta mode.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.keep_table (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200));" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.drop_between (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.drop_between VALUES (), (), ();" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.keep_table \({1..500},\'${PAD}\'\)

innodb_wait_for_flush_all

# Delta full with in-window DML so #xb_page_delta is non-empty
XB_ERROR_LOG=$topdir/full.log
xtrabackup_background --backup --target-dir=$topdir/full \
  --debug-sync-thread="before_file_copy" --lock-ddl=REDUCED \
  --copy-strategy=clone

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.keep_table SET pad = REPEAT('y', 150) WHERE id % 2 = 0;" test
innodb_wait_for_flush_all
resume_debug_sync_thread "before_file_copy" $topdir/full
run_cmd wait $job_pid

if ! find "$topdir/full/#xb_page_delta" -name "*.delta" 2>/dev/null | grep -q . ; then
  die "delta full produced no deltas"
fi

# Changes between the backups: DML, a dropped table, a new table
$MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.keep_table(pad) VALUES (REPEAT('z', 120)), (REPEAT('w', 120));" test
$MYSQL $MYSQL_ARGS -Ns -e "DROP TABLE test.drop_between;" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.new_between (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.new_between VALUES (), ();" test

# Classic incremental chained on the delta full's to_lsn
xtrabackup --backup --target-dir=$topdir/inc \
  --incremental-basedir=$topdir/full --lock-ddl=REDUCED

# Prepare pipeline: apply-log-only on the full first
xtrabackup --prepare --apply-log-only --target-dir=$topdir/full \
  2> >( tee $topdir/prepare_full.log)

if ! egrep -q "Delta backup: applying .delta files" $topdir/prepare_full.log ; then
  die "apply-log-only did not apply the full's deltas"
fi

# After the first prepare pass the delta full must look like a classic full
if [ -d "$topdir/full/#xb_page_delta" ]; then
  die "apply-log-only did not remove #xb_page_delta"
fi

# Apply the incremental (the LAST incremental is applied without
# --apply-log-only; that application is the final prepare). This also
# deletes .ibd files dropped between the backups (rm_if_not_found).
xtrabackup --prepare --target-dir=$topdir/full \
  --incremental-dir=$topdir/inc \
  2> >( tee $topdir/prepare_inc.log)

if [ -f $topdir/full/test/drop_between.ibd ]; then
  die "table dropped between backups survived the incremental apply"
fi
if [ ! -f $topdir/full/test/new_between.ibd ]; then
  die "table created between backups missing after the incremental apply"
fi

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/full
start_server
verify_db_state test
stop_server
rm -rf $mysql_datadir $topdir/full $topdir/inc
