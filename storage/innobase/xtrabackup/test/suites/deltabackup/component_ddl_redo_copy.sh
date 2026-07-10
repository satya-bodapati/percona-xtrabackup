###############################################################################
# PXB-XXXX: Classic redo-copy backup with component DDL tracking
#
# With --lock-ddl=REDUCED on a server that provides the backup DDL journal,
# DDL tracking comes from the journal (component) even for the default
# --copy-strategy=redo: full-duration redo copy, journal-fed fixups, no
# .delta files. The full DDL mix runs while the backup is paused before the
# file copy; restore is verified against a mysqldump of the source.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.delete_table (id INT PRIMARY KEY AUTO_INCREMENT);" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.original_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.original_table VALUES(1)" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.op_ddl (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50)); INSERT INTO test.op_ddl VALUES(1, 'test')" test

innodb_wait_for_flush_all

# Default --copy-strategy (redo); --ddl-tracking defaults to auto and must
# resolve to the component on this server
XB_ERROR_LOG=$topdir/backup_classic.log
xtrabackup_background --backup --target-dir=$topdir/backup_classic \
  --debug-sync-thread="before_file_copy" --lock-ddl=REDUCED

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"

$MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.delete_table VALUES (1); DROP TABLE test.delete_table;" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.new_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.new_table VALUES (), ();" test
$MYSQL $MYSQL_ARGS -Ns -e "RENAME TABLE test.original_table TO test.renamed_table; INSERT INTO test.renamed_table VALUES (2);" test
$MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.op_ddl ADD INDEX(name); INSERT INTO test.op_ddl VALUES (2, 'test2');" test

vlog "Resuming xtrabackup"
resume_debug_sync_thread "before_file_copy" $topdir/backup_classic
run_cmd wait $job_pid

# auto must have picked the component
if ! egrep -q "DDL tracking source: server DDL journal \(component\)" $topdir/backup_classic.log ; then
  die "auto ddl-tracking did not resolve to the component"
fi

if ! egrep -q "DDL journal: consumed [0-9]+ events" $topdir/backup_classic.log ; then
  die "backup did not consume the DDL journal"
fi

# journal-fed fixups
if ! egrep -q "DDL tracking : LSN: [0-9]* delete space ID: [0-9]* Name: test/delete_table.ibd" $topdir/backup_classic.log ; then
  die "journal-fed DDL tracking did not handle DROP"
fi
if ! egrep -q "DDL tracking : LSN: [0-9]* create space ID: [0-9]* Name: test/new_table.ibd" $topdir/backup_classic.log ; then
  die "journal-fed DDL tracking did not handle CREATE"
fi
if ! egrep -q "DDL tracking : LSN: [0-9]* bulk index load on space ID: [0-9]*" $topdir/backup_classic.log ; then
  die "journal-fed DDL tracking did not handle bulk index load"
fi

# classic copy strategy: no deltas, no delta metadata flag
if find $topdir/backup_classic -name "*.delta" | grep -q . ; then
  die "classic redo-copy backup produced .delta files"
fi
if ! egrep -q "delta_backup = 0" $topdir/backup_classic/xtrabackup_checkpoints ; then
  die "xtrabackup_checkpoints does not record delta_backup = 0"
fi

xtrabackup --prepare --target-dir=$topdir/backup_classic

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_classic
start_server
verify_db_state test

# Explicit legacy parser must still work on this server
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.after (id INT PRIMARY KEY);" test
xtrabackup --backup --target-dir=$topdir/backup_legacy \
  --lock-ddl=REDUCED --ddl-tracking=redo \
  2> >( tee $topdir/backup_legacy.log)
if ! egrep -q "DDL tracking source: redo log parsing" $topdir/backup_legacy.log ; then
  die "--ddl-tracking=redo did not use the parser"
fi

stop_server
rm -rf $mysql_datadir $topdir/backup_classic $topdir/backup_legacy
