###############################################################################
# PXB-3279: Remove requirement to have open file handles equal to files in datadir
###############################################################################

. inc/common.sh

require_debug_pxb_version

open_file_limit=$(ulimit -n)
ulimit -n 1024

start_server

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.drop_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.drop_table VALUES(1);" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.drop_create_same_name_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.drop_create_same_name_table VALUES(1);" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.rename_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.rename_table VALUES(1);" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.alter_rename_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.alter_rename_table VALUES(1)" test
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.alter_drop_column_table (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50)); INSERT INTO test.alter_drop_column_table VALUES(1, 'test')" test

# Create the tables in one session, and compare the data of the 1500 tables
# with one CHECKSUM TABLE statement. A mysqldump of 1500 tables takes minutes,
# and twice that made the test time out on a slow host.
for i in {1..1500} ; do
    echo "CREATE TABLE test.tb_${i} (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.tb_${i} VALUES (1);"
done | $MYSQL $MYSQL_ARGS test

function checksum_tables()
{
    local tables=$($MYSQL $MYSQL_ARGS -Ns \
        --init-command="SET SESSION group_concat_max_len=1000000" \
        -e "SELECT GROUP_CONCAT(table_name ORDER BY table_name)
            FROM information_schema.tables WHERE table_schema='test'")
    $MYSQL $MYSQL_ARGS -Ns -e "CHECKSUM TABLE $tables EXTENDED" test > $1
}

innodb_wait_for_flush_all

xtrabackup --backup --target-dir=$topdir/backup \
  --debug-sync="xtrabackup_suspend_between_file_discovery_and_open" --lock-ddl=REDUCED \
  2> >( tee $topdir/backup.log)&

job_pid=$!
pid_file=$topdir/backup/xtrabackup_debug_sync
wait_for_xb_to_suspend $pid_file
xb_pid=`cat $pid_file`
echo "backup pid is $job_pid"

# Delete table
run_cmd $MYSQL $MYSQL_ARGS -Ns -e "DROP TABLE test.drop_table;" test
run_cmd $MYSQL $MYSQL_ARGS -Ns -e "DROP TABLE test.drop_create_same_name_table;" test
# Create table and generate redo; Table name is same as the one that was dropped but different space id; This should cause space_id mismatch and added to missing list 
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.drop_create_same_name_table (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.drop_create_same_name_table VALUES(1);" test
# Rename table and generate redo
$MYSQL $MYSQL_ARGS -Ns -e "RENAME TABLE test.rename_table TO test.new_rename_table; INSERT INTO test.new_rename_table VALUES(2);" test
# Alter table rename and generate redo
$MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.alter_rename_table RENAME test.new_alter_rename_table; INSERT INTO test.new_alter_rename_table VALUES(2);" test
# Alter table drop column and generate redo
$MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.alter_drop_column_table DROP COLUMN name; INSERT INTO test.alter_drop_column_table VALUES(2);" test

# Resume the xtrabackup process
vlog "Resuming xtrabackup"
kill -SIGCONT $xb_pid
run_cmd wait $job_pid

if ! egrep -q "DDL tracking : LSN: [0-9]* delete space ID: [0-9]* Name: test/drop_table.ibd" $topdir/backup.log ; then
    die "xtrabackup did not handle delete table DDL"
fi

if ! egrep -q "DDL tracking : LSN: [0-9]* rename space ID: [0-9]* From: test/rename_table.ibd To: test/new_rename_table.ibd" $topdir/backup.log ; then
    die "xtrabackup did not handle rename table DDL"
fi

if ! egrep -q "DDL tracking : LSN: [0-9]* rename space ID: [0-9]* From: test/alter_rename_table.ibd To: test/new_alter_rename_table.ibd" $topdir/backup.log ; then
    die "xtrabackup did not handle alter table rename DDL"
fi

if ! egrep -q "DDL tracking : missing after discovery space ID: [0-9]*" $topdir/backup.log ; then
    die "xtrabackup did not add dropped table to missing list"
fi

xtrabackup --prepare --target-dir=$topdir/backup
checksum_tables $topdir/checksum_old
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup
start_server
checksum_tables $topdir/checksum_new
run_cmd diff -u $topdir/checksum_old $topdir/checksum_new

stop_server
ulimit -n $open_file_limit
rm -rf $mysql_datadir $topdir/backup
rm $topdir/backup.log
