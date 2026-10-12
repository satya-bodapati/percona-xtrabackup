#
# --transition-key with --lock-ddl=reduced: an encrypted table dropped during
# the backup. Prepare opens the copied .ibd of the dropped table before it
# handles the .del file, so the backup must save the key of that table.
#

. inc/common.sh
require_debug_pxb_version

KEYRING_TYPE="component"
. inc/keyring_common.sh
. inc/keyring_file.sh
configure_server_with_component

mysql -e "CREATE TABLE t (a INT) ENCRYPTION='y'" test
mysql -e "INSERT INTO t VALUES (1), (2), (3)" test
mysql -e "CREATE TABLE t2 (a INT) ENCRYPTION='y'" test

# Restart so that the CREATE of t2 is behind the checkpoint: the backup can
# then get the key of t2 only from the t2.ibd it opens, not from the redo
shutdown_server
start_server

BACKUP_DIR=$topdir/backup

# Sleep before the first key dump, so that the redo thread has parsed the
# DROP of t2 and the DDL tracker knows t2 is dropped
xtrabackup --backup --transition-key=123 --target-dir=$BACKUP_DIR \
  --debug-sync="ddl_tracker_before_lock_ddl" \
  --debug=d,ddl_tracker_sleep_before_key_dump \
  --lock-ddl=REDUCED 2> >( tee $topdir/backup.log)&

job_pid=$!
pid_file=$BACKUP_DIR/xtrabackup_debug_sync
wait_for_xb_to_suspend $pid_file
xb_pid=`cat $pid_file`
echo "backup pid is $job_pid"

mysql -e "INSERT INTO t2 VALUES (1); DROP TABLE t2" test

vlog "Resuming xtrabackup"
kill -SIGCONT $xb_pid
run_cmd wait $job_pid

if ! grep -q "delete space ID: .* Name: test/t2.ibd" $topdir/backup.log ; then
  die "The backup did not track the DROP of t2"
fi

record_db_state test

xtrabackup --prepare --transition-key=123 --target-dir=$BACKUP_DIR \
  --lock-ddl=reduced --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}

if [ -f $BACKUP_DIR/test/t2.ibd ] ; then
  die "Prepare did not delete t2.ibd"
fi

stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$BACKUP_DIR \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}
cp ${instance_local_manifest} $mysql_datadir
cp ${keyring_component_cnf} $mysql_datadir
start_server
verify_db_state test

rm -rf $BACKUP_DIR
rm $topdir/backup.log
