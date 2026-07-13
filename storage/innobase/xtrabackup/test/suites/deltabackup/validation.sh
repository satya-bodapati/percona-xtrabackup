###############################################################################
# PXB-XXXX: Delta backup option validation
#
# --copy-strategy=clone must be rejected without --lock-ddl=REDUCED and with
# incremental backups; a second prepare of a delta backup must not re-apply
# the .delta files.
###############################################################################

. inc/common.sh

require_debug_pxb_version

start_server

if [ "$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COUNT(*) FROM performance_schema.user_defined_functions WHERE udf_name = 'innodb_backup_ddl_journal_start'")" != "1" ]; then
  skip_test "server does not have the backup DDL journal UDFs"
fi

run_cmd $MYSQL $MYSQL_ARGS -e "INSTALL COMPONENT \"file://component_mysqlbackup\""

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.t1 (id INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO test.t1 VALUES (), (), ();" test

# 1. --copy-strategy=clone with --lock-ddl=OFF must fail
# (REDUCED and ON are both supported)
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup \
  --target-dir=$topdir/backup_fail --copy-strategy=clone \
  --lock-ddl=OFF \
  2> >( tee $topdir/fail1.log)
if ! egrep -q "copy-strategy=clone requires --lock-ddl=REDUCED or --lock-ddl=ON" $topdir/fail1.log ; then
  die "missing lock-ddl validation error"
fi
rm -rf $topdir/backup_fail

# 1b. bogus --copy-strategy value must be rejected by option parsing
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup \
  --target-dir=$topdir/backup_fail --copy-strategy=bogus \
  2> >( tee $topdir/fail1b.log)
rm -rf $topdir/backup_fail

# 1c. --ddl-tracking=server without --lock-ddl=REDUCED must fail
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup \
  --target-dir=$topdir/backup_fail --ddl-tracking=server \
  2> >( tee $topdir/fail1c.log)
if ! egrep -q "ddl-tracking=server requires --lock-ddl=REDUCED" $topdir/fail1c.log ; then
  die "missing ddl-tracking lock-ddl validation error"
fi
rm -rf $topdir/backup_fail

# 1d. clone copy strategy with the parser explicitly forced must fail
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup \
  --target-dir=$topdir/backup_fail --lock-ddl=REDUCED \
  --copy-strategy=clone --ddl-tracking=redo \
  2> >( tee $topdir/fail1d.log)
if ! egrep -q "copy-strategy=clone requires the server backup DDL journal" $topdir/fail1d.log ; then
  die "missing clone/ddl-tracking cross validation error"
fi
rm -rf $topdir/backup_fail

# 2. --copy-strategy=clone with incremental must fail
xtrabackup --backup --target-dir=$topdir/full --lock-ddl=REDUCED
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup \
  --target-dir=$topdir/inc --incremental-basedir=$topdir/full \
  --lock-ddl=REDUCED --copy-strategy=clone \
  2> >( tee $topdir/fail2.log)
if ! egrep -q "copy-strategy=clone cannot be used with incremental" $topdir/fail2.log ; then
  die "missing incremental validation error"
fi
rm -rf $topdir/full $topdir/inc

# 3. second prepare of a delta backup must not re-apply deltas
xtrabackup --backup --target-dir=$topdir/backup_delta \
  --lock-ddl=REDUCED --copy-strategy=clone
xtrabackup --prepare --target-dir=$topdir/backup_delta \
  2> >( tee $topdir/prepare1.log)
xtrabackup --prepare --target-dir=$topdir/backup_delta \
  2> >( tee $topdir/prepare2.log)
if egrep -q "Delta backup: applying .delta files" $topdir/prepare2.log ; then
  die "second prepare re-applied the delta files"
fi

stop_server
rm -rf $mysql_datadir $topdir/backup_delta
