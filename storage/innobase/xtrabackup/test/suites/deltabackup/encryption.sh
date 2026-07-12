###############################################################################
# PXB-XXXX: Delta backup vs encrypted tablespaces
#
# Encrypted pages travel in the delta exactly like in the file copy: the
# on-disk (encrypted) bytes are shipped as-is, no re-encryption anywhere.
# An encryption ALTER during the backup lands in the DDL journal
# (SPACE_ALTER_ENCRYPT / MLOG_WRITE_STRING) and forces a full recopy of the
# affected space — which must therefore have no delta.
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

# enc_keep: encrypted file-per-table, only DML in the window -> encrypted delta
# enc_alter: encrypted, gets an encryption ALTER in the window -> full recopy
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.enc_keep (id INT PRIMARY KEY AUTO_INCREMENT, pad VARCHAR(200)) ENCRYPTION='Y';" test
PAD=$(printf 'x%.0s' {1..100})
multi_row_insert test.enc_keep \({1..500},\'${PAD}\'\)
$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.enc_alter (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.enc_alter VALUES (), (), ();" test

innodb_wait_for_flush_all

XB_ERROR_LOG=$topdir/backup_enc.log
xtrabackup_background --backup --target-dir=$topdir/backup_enc \
  --debug-sync-thread="before_file_copy" --lock-ddl=REDUCED \
  --copy-strategy=page-tracking --xtrabackup-plugin-dir=${plugin_dir} \
  ${keyring_args}

job_pid=$XB_PID
wait_for_debug_sync_thread "before_file_copy"

# DML on the encrypted table inside the tracking window -> encrypted delta
$MYSQL $MYSQL_ARGS -Ns -e "UPDATE test.enc_keep SET pad = REPEAT('e', 150) WHERE id % 2 = 0;" test

# encryption ALTER on the other table -> journal event -> full recopy
$MYSQL $MYSQL_ARGS -Ns -e "ALTER TABLE test.enc_alter ENCRYPTION='Y';" test

innodb_wait_for_flush_all

vlog "Resuming xtrabackup"
resume_debug_sync_thread "before_file_copy" $topdir/backup_enc
run_cmd wait $job_pid

if ! egrep -q "DDL journal: consumed [0-9]+ events" $topdir/backup_enc.log ; then
  die "backup did not consume the DDL journal"
fi

# encrypted file-per-table table with only DML must have an (encrypted) delta
if [ ! -f "$topdir/backup_enc/#xb_delta/test/enc_keep.ibd.delta" ]; then
  die "encrypted table has no delta"
fi

xtrabackup --prepare --target-dir=$topdir/backup_enc \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args} \
  2> >( tee $topdir/prepare_enc.log)

if ! egrep -q "Delta backup: applying .delta files" $topdir/prepare_enc.log ; then
  die "prepare did not apply the delta files"
fi

record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_enc \
  --xtrabackup-plugin-dir=${plugin_dir} ${keyring_args}
cp ${instance_local_manifest} $mysql_datadir
cp ${keyring_component_cnf} $mysql_datadir
start_server
verify_db_state test
stop_server
rm -rf $mysql_datadir $topdir/backup_enc
