###############################################################################
# PXB-XXXX: master-key rotation during a clone-strategy backup
#
# In clone mode XtraBackup keeps only a short redo tail, so the
# fallback that rescues an un-decryptable encrypted tablespace at reopen time
# (recv_find_encryption_key, reading the copied redo) has nothing to find when
# the encryption record predates the tail. Combined with
# ALTER INSTANCE ROTATE INNODB MASTER KEY during the window, the under-lock
# reopen of an encrypted space recopied under the lock then hard-fails with
# DB_INVALID_ENCRYPTION_META ("Tablespace opening under reduced lock reopen
# failed").
#
# Fix (on the ddl-tracker base branch): refresh the master key from the keyring
# under the backup lock (rotation is frozen by the exclusive backup lock),
# before the reopen, so the unwrap succeeds by the header's master_key_id and
# does not depend on redo.
###############################################################################

KEYRING_TYPE="component"
. inc/keyring_common.sh
. inc/keyring_file.sh

require_debug_pxb_version
require_debug_sync_thread

configure_server_with_component

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

# Pre-existing encrypted table (copied during the unlocked file copy).
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.enc_base (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200)) ENCRYPTION='Y';" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.enc_base \({1..400},\'${PAD}\'\)

innodb_wait_for_flush_all

XB_ERROR_LOG=$topdir/backup_rotc.log
xtrabackup_background --backup --target-dir=$topdir/backup_rotc \
  --debug-sync-thread="before_file_copy" --lock-ddl=REDUCED \
  --copy-strategy=clone --xtrabackup-plugin-dir=${plugin_dir} \
  ${keyring_args}

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"

# New encrypted table created in the window -> recopied under the lock.
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.enc_new (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200)) ENCRYPTION='Y'; INSERT INTO test.enc_new (pad) VALUES (REPEAT('n',150)), (REPEAT('m',150));" test

# Rotate the master key: re-wraps every encrypted space's page-0 header with a
# new master key while XtraBackup still holds the old master-key state.
$MYSQL $MYSQL_ARGS -Ns -e "ALTER INSTANCE ROTATE INNODB MASTER KEY;" test

$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.enc_base SET pad = REPEAT('r',120) WHERE id % 4 = 0;" test

# Force a checkpoint so the rotation/create records fall behind the clone tail.
innodb_wait_for_flush_all
$MYSQL $MYSQL_ARGS -Ns -e "SET GLOBAL innodb_checkpoint_now = ON;" 2>/dev/null || true

vlog "Resuming xtrabackup"
resume_debug_sync_thread "before_file_copy" $topdir/backup_rotc
run_cmd wait $job_pid

if egrep -q "Tablespace opening under reduced lock reopen failed" $topdir/backup_rotc.log ; then
  die "master-key rotation broke the under-lock reopen (DB_INVALID_ENCRYPTION_META)"
fi

xtrabackup --prepare --target-dir=$topdir/backup_rotc \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args} \
  2> >( tee $topdir/prepare_rotc.log)

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_rotc \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}
cp ${instance_local_manifest} $mysql_datadir
cp ${keyring_component_cnf} $mysql_datadir
start_server
verify_db_state test
