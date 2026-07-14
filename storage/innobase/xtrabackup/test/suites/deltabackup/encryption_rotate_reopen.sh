###############################################################################
# PXB-XXXX: master-key rotation during a reduced-lock backup
#
# An encrypted tablespace that must be reopened/recopied under the backup lock
# (here: created during the tracking window) combined with
# ALTER INSTANCE ROTATE INNODB MASTER KEY during the same window used to fail
# the backup with DB_INVALID_ENCRYPTION_META ("Tablespace opening under reduced
# lock reopen failed"): the rotation re-wraps the tablespace's page-0
# encryption header with the new master key, but XtraBackup still holds the
# master-key state it read at backup start.
#
# ROTATE takes the exclusive backup lock, so it cannot run while XtraBackup
# holds LOCK INSTANCE FOR BACKUP; the fix refreshes the master key from the
# keyring under the lock, before the under-lock reopens.
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

# A pre-existing encrypted table (copied during the unlocked file copy).
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.enc_base (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200)) ENCRYPTION='Y';" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.enc_base \({1..300},\'${PAD}\'\)

innodb_wait_for_flush_all

XB_ERROR_LOG=$topdir/backup_rot.log
xtrabackup_background --backup --target-dir=$topdir/backup_rot \
  --debug-sync-thread="before_file_copy" --lock-ddl=REDUCED \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"

# New encrypted table created in the window -> recopied under the lock.
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.enc_new (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200)) ENCRYPTION='Y'; INSERT INTO test.enc_new (pad) VALUES (REPEAT('n',150)), (REPEAT('m',150));" test

# Rotate the master key: re-wraps every encrypted space's page-0 header with
# the new master key while XtraBackup's master-key state is still the old one.
$MYSQL $MYSQL_ARGS -Ns -e "ALTER INSTANCE ROTATE INNODB MASTER KEY;" test

# Some post-rotation DML so the backup is non-trivial.
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.enc_base SET pad = REPEAT('r',120) WHERE id % 3 = 0;" test

innodb_wait_for_flush_all

vlog "Resuming xtrabackup"
resume_debug_sync_thread "before_file_copy" $topdir/backup_rot
run_cmd wait $job_pid

# The backup must not fail on the under-lock reopen.
if egrep -q "Tablespace opening under reduced lock reopen failed" $topdir/backup_rot.log ; then
  die "master-key rotation broke the under-lock reopen (DB_INVALID_ENCRYPTION_META)"
fi

# The fix reloads the keyring before the under-lock reopen, so the reopened
# encrypted tablespace must decrypt cleanly -- no "can't be decrypted" /
# "Failed to decrypt" warnings (which appear when the reload is not done).
if egrep -q "can't be decrypted|Failed to decrypt table" $topdir/backup_rot.log ; then
  die "encrypted tablespace could not be decrypted at reopen after rotation (keyring not reloaded)"
fi

# The reload must have run.
if ! egrep -q "Reloaded keyring to pick up keys rotated during the backup" $topdir/backup_rot.log ; then
  die "keyring was not reloaded before the under-lock reopen"
fi

xtrabackup --prepare --target-dir=$topdir/backup_rot \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args} \
  2> >( tee $topdir/prepare_rot.log)

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_rot \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}
cp ${instance_local_manifest} $mysql_datadir
cp ${keyring_component_cnf} $mysql_datadir
start_server
verify_db_state test
