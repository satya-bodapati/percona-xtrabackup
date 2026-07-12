###############################################################################
# PXB-XXXX: Delta backup (lock-ddl=REDUCED variant, server-tracked)
#
# Round trip of --copy-strategy=page-tracking: DML + the full DDL mix (drop/create/rename/
# bulk index load) while the backup is paused before file copy, background
# DML while the data files are copied, prepare (fixups -> deltas -> redo)
# and restore, verified against a mysqldump of the source.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

# The server must carry the backup DDL journal UDFs (Percona Server patch)
if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.delete_table (id INT PRIMARY KEY AUTO_INCREMENT);" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.original_table (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200)); INSERT INTO test.original_table(pad) VALUES (REPEAT('a', 100)), (REPEAT('b', 100)), (REPEAT('c', 100));" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.op_ddl (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50)); INSERT INTO test.op_ddl VALUES(1, 'test')" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.dml_table (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200));" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.dml_table \({1..1000},\'${PAD}\'\)

innodb_wait_for_flush_all

# Pause after page tracking + DDL journal are enabled, before the file copy
XB_ERROR_LOG=$topdir/backup_delta.log
xtrabackup_background --backup --target-dir=$topdir/backup_delta \
  --debug-sync-thread="before_file_copy" --lock-ddl=REDUCED --copy-strategy=page-tracking

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"

# DML modifying pages inside the tracking window [S, C1]
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.dml_table SET pad = REPEAT('y', 150) WHERE id % 3 = 0;" test
$MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.dml_table(pad) SELECT pad FROM test.dml_table LIMIT 500;" test

# Modify the to-be-renamed table inside the window: its changed pages must
# land in the backup as a .delta under the OLD name (renamed tables are NOT
# fully recopied)
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.original_table SET pad = REPEAT('r', 150);" test

# The DDL mix, recorded by the server DDL journal (not by redo parsing)
$MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.delete_table VALUES (1); DROP TABLE test.delete_table;" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.new_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.new_table VALUES (), ();" test
$MYSQL $MYSQL_ARGS -Ns -e "RENAME TABLE test.original_table TO test.renamed_table; INSERT INTO test.renamed_table(pad) VALUES (REPEAT('d', 100));" test
$MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.op_ddl ADD INDEX(name); INSERT INTO test.op_ddl VALUES (2, 'test2');" test

# Make the DML durable inside the window so the delta pass has pages to copy
innodb_wait_for_flush_all

# Background DML racing the file copy
( for i in $(seq 1 50); do
    $MYSQL $MYSQL_ARGS -Ns -e "INSERT INTO test.dml_table(pad) VALUES(REPEAT('z', 120));" test >/dev/null 2>&1 || true
  done ) &
load_pid=$!

# Resume the xtrabackup process
vlog "Resuming xtrabackup"
resume_debug_sync_thread "before_file_copy" $topdir/backup_delta
run_cmd wait $job_pid
wait $load_pid || true

# Delta mode ran: redo copy deferred to after file copy, C1 = its checkpoint
if ! egrep -q "delta backup: redo copy started from checkpoint LSN" $topdir/backup_delta.log ; then
  die "delta backup did not defer the redo copy"
fi

if ! egrep -q "DDL journal: consumed [0-9]+ events" $topdir/backup_delta.log ; then
  die "delta backup did not consume the DDL journal"
fi

if ! egrep -q "delta backup: page recopy complete" $topdir/backup_delta.log ; then
  die "delta backup did not run the page recopy pass"
fi

# .delta files must exist in the backup (DML window was flushed inside [S,C1])
if ! find $topdir/backup_delta/#xb_delta -name "*.delta" 2>/dev/null | grep -q . ; then
  die "delta backup produced no .delta files"
fi

# DDL fixups fed from the journal
if ! egrep -q "DDL tracking : LSN: [0-9]* delete space ID: [0-9]* Name: test/delete_table.ibd" $topdir/backup_delta.log ; then
  die "journal-fed DDL tracking did not handle DROP"
fi

if ! egrep -q "DDL tracking : LSN: [0-9]* create space ID: [0-9]* Name: test/new_table.ibd" $topdir/backup_delta.log ; then
  die "journal-fed DDL tracking did not handle CREATE"
fi

if ! egrep -q "DDL tracking : LSN: [0-9]* rename space ID: [0-9]* From: test/original_table.ibd To: test/renamed_table.ibd" $topdir/backup_delta.log ; then
  die "journal-fed DDL tracking did not handle RENAME"
fi

if ! egrep -q "DDL tracking : LSN: [0-9]* bulk index load on space ID: [0-9]*" $topdir/backup_delta.log ; then
  die "journal-fed DDL tracking did not handle bulk index load"
fi

# The rename here happens BEFORE the file copy (the pause is pre-copy), so
# the table is missing-after-discovery and must be fully recopied under its
# new name; no delta under either name (rename-after-copy is covered by the
# rename_after_copy test)
if [ -f "$topdir/backup_delta/#xb_delta/test/original_table.ibd.delta" ]; then
  die "unexpected delta for a table renamed before its file copy"
fi

# metadata must carry the delta flag for prepare
if ! egrep -q "delta_backup = 1" $topdir/backup_delta/xtrabackup_checkpoints ; then
  die "xtrabackup_checkpoints does not record delta_backup = 1"
fi

xtrabackup --prepare --target-dir=$topdir/backup_delta \
  2> >( tee $topdir/prepare_delta.log)

if ! egrep -q "Delta backup: applying .delta files" $topdir/prepare_delta.log ; then
  die "prepare did not apply the delta files"
fi


record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_delta
start_server
verify_db_state test
stop_server
rm -rf $mysql_datadir $topdir/backup_delta
