###############################################################################
# PXB-XXXX: DISCARD/IMPORT during a classic incremental (lock-ddl=REDUCED)
#
# copy-strategy=clone is full-only, but a classic incremental still
# uses lock-ddl=REDUCED with the DDL journal, so an import can happen in the
# incremental's no-lock window. The same machinery must apply at incremental
# prepare: journal_create resolves the recreate, and the .reimport marker
# (persisted in the incremental dir) makes the incremental's recovery ignore
# the logged DISCARD delete so the post-import INSERT survives.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.t_imp (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200));" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.t_imp \({1..200},\'${PAD}\'\)
innodb_wait_for_flush_all

# base full backup (classic REDUCED)
xtrabackup --backup --target-dir=$topdir/full --lock-ddl=REDUCED

# export a consistent copy for the mid-incremental import
mkdir -p $topdir/export
run_cmd $MYSQL $MYSQL_ARGS test <<SQL
FLUSH TABLES t_imp FOR EXPORT;
system cp $mysql_datadir/test/t_imp.ibd $topdir/export/ && cp $mysql_datadir/test/t_imp.cfg $topdir/export/ && echo EXPORTED
UNLOCK TABLES;
SQL
[ -f $topdir/export/t_imp.cfg ] || die "export failed (.cfg missing)"

# some changes between backups
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.t_imp SET pad = REPEAT('y', 150) WHERE id % 2 = 0;" test

# incremental backup, paused before the lock; DISCARD+IMPORT + post-import DML
XB_ERROR_LOG=$topdir/inc.log
xtrabackup --backup --target-dir=$topdir/inc \
  --incremental-basedir=$topdir/full --lock-ddl=REDUCED \
  --debug-sync="ddl_tracker_before_lock_ddl" \
  2> >( tee -a $topdir/inc.log)&
job_pid=$!
pid_file=$topdir/inc/xtrabackup_debug_sync
wait_for_xb_to_suspend $pid_file
xb_pid=`cat $pid_file`

$MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.t_imp DISCARD TABLESPACE;" test
cp $topdir/export/t_imp.ibd $topdir/export/t_imp.cfg $mysql_datadir/test/
$MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.t_imp IMPORT TABLESPACE;" test
$MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.t_imp(pad) VALUES (REPEAT('i', 120));" test
[ "$($MYSQL $MYSQL_ARGS -Ns -e 'SELECT COUNT(*) FROM test.t_imp')" = "201" ] \
  || die "IMPORT did not succeed on the source"

vlog "Resuming xtrabackup"
kill -SIGCONT $xb_pid
run_cmd wait $job_pid

# prepare chain: apply-log-only the base, then the incremental as final prepare
xtrabackup --prepare --apply-log-only --target-dir=$topdir/full
xtrabackup --prepare --target-dir=$topdir/full --incremental-dir=$topdir/inc \
  2> >( tee $topdir/prepare_inc.log)

egrep -q "Ignoring logged delete for reimported tablespace" $topdir/prepare_inc.log \
  || die "incremental prepare did not suppress the logged delete for the import"

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/full
start_server
verify_db_state test
stop_server
rm -rf $mysql_datadir $topdir/full $topdir/inc $topdir/export
