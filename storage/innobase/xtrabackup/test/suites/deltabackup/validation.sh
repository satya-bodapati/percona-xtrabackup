###############################################################################
# PXB-XXXX: Delta backup option validation
#
# --delta-backup must be rejected without --lock-ddl=REDUCED and with
# incremental backups; a second prepare of a delta backup must not re-apply
# the .delta files.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if ! $MYSQL $MYSQL_ARGS -Ns -e "SELECT @@innodb_backup_ddl_journal" >/dev/null 2>&1; then
  skip_test "server does not have the backup DDL journal (innodb_backup_ddl_journal)"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.t1 (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.t1 VALUES (), (), ();" test

# 1. --delta-backup without --lock-ddl=REDUCED must fail
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup \
  --target-dir=$topdir/backup_fail --delta-backup \
  2> >( tee $topdir/fail1.log)
if ! egrep -q "delta-backup requires --lock-ddl=REDUCED" $topdir/fail1.log ; then
  die "missing lock-ddl validation error"
fi
rm -rf $topdir/backup_fail

# 2. --delta-backup with incremental must fail
xtrabackup --backup --target-dir=$topdir/full --lock-ddl=REDUCED
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup \
  --target-dir=$topdir/inc --incremental-basedir=$topdir/full \
  --lock-ddl=REDUCED --delta-backup \
  2> >( tee $topdir/fail2.log)
if ! egrep -q "delta-backup cannot be used with incremental" $topdir/fail2.log ; then
  die "missing incremental validation error"
fi
rm -rf $topdir/full $topdir/inc

# 3. second prepare of a delta backup must not re-apply deltas
xtrabackup --backup --target-dir=$topdir/backup_delta \
  --lock-ddl=REDUCED --delta-backup
xtrabackup --prepare --target-dir=$topdir/backup_delta \
  2> >( tee $topdir/prepare1.log)
xtrabackup --prepare --target-dir=$topdir/backup_delta \
  2> >( tee $topdir/prepare2.log)
if egrep -q "Delta backup: applying .delta files" $topdir/prepare2.log ; then
  die "second prepare re-applied the delta files"
fi

stop_server
rm -rf $mysql_datadir $topdir/backup_delta
