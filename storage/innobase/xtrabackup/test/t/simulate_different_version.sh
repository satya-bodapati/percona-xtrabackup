########################################################################
# PXB should error out with upper version of MySQL or PS
########################################################################

. inc/common.sh

require_debug_pxb_version

start_server

vlog "case#1 backup should fail with 5.7 version"
run_cmd_expect_failure $XB_BIN $XB_ARGS --backup --debug=d,simulate_24_version --target-dir=$topdir/backup 2>&1 | tee $topdir/pxb.log

if ! grep -q 'Please use Percona XtraBackup 2.4 for this database.' $topdir/pxb.log
then
  die "xtrabackup did not show error about 2.4 version"
fi

rm $topdir/pxb.log

vlog "case#2 backup should fail with 8.0 version"

run_cmd_expect_failure $XB_BIN $XB_ARGS --backup --debug=d,simulate_80_version --target-dir=$topdir/backup 2>&1 | tee $topdir/pxb.log

if ! grep -q 'Please use Percona XtraBackup 8.0 for this database.' $topdir/pxb.log
then
  die "xtrabackup did not show error about 8.0 version"
fi

rm $topdir/pxb.log

vlog "case#3 backup should fail with 8.1 version"

run_cmd_expect_failure $XB_BIN $XB_ARGS --backup --debug=d,simulate_81_version --target-dir=$topdir/backup 2>&1 | tee $topdir/pxb.log

if ! grep -q 'Please use Percona XtraBackup 8.1 for this database.' $topdir/pxb.log
then
  die "xtrabackup did not show error about 8.1 version"
fi

rm $topdir/pxb.log

vlog "case#4 backup should fail with 9.0 version"

run_cmd_expect_failure $XB_BIN $XB_ARGS --backup --debug=d,simulate_90_version --target-dir=$topdir/backup 2>&1 | tee $topdir/pxb.log

if ! grep -q 'Please use Percona XtraBackup 9.0 for this database.' $topdir/pxb.log
then
  die "xtrabackup did not show error about 9.0 version"
fi

rm $topdir/pxb.log

vlog "case#5 backup should fail with 9.7 version (previous LTS, not 26.7.x)"

run_cmd_expect_failure $XB_BIN $XB_ARGS --backup --debug=d,simulate_97_version --target-dir=$topdir/backup 2>&1 | tee $topdir/pxb.log

if ! grep -q "Unsupported server version: '9.7.99'" $topdir/pxb.log
then
  die "xtrabackup did not show 'Unsupported server version' error for 9.7.99"
fi

if ! grep -q 'only supports backing up and restoring MySQL and Percona Servers of version 26.7.x' $topdir/pxb.log
then
  die "xtrabackup did not show 26.7.x-only support message for 9.7.99"
fi

if ! grep -q 'Please use Percona XtraBackup 9.7 for this database.' $topdir/pxb.log
then
  die "xtrabackup did not suggest Percona XtraBackup 9.7 for a 9.7 server"
fi

rm $topdir/pxb.log

vlog "case#6 backup should fail with 26.10 version (newer, not 26.7.x)"

run_cmd_expect_failure $XB_BIN $XB_ARGS --backup --debug=d,simulate_2610_version --target-dir=$topdir/backup 2>&1 | tee $topdir/pxb.log

if ! grep -q "Unsupported server version: '26.10.0'" $topdir/pxb.log
then
  die "xtrabackup did not show 'Unsupported server version' error for 26.10.0"
fi

if ! grep -q 'only supports backing up and restoring MySQL and Percona Servers of version 26.7.x' $topdir/pxb.log
then
  die "xtrabackup did not show 26.7.x-only support message for 26.10.0"
fi

if ! grep -q 'Please use Percona XtraBackup 26.10 for this database.' $topdir/pxb.log
then
  die "xtrabackup did not suggest Percona XtraBackup 26.10 for a 26.10 server"
fi

rm $topdir/pxb.log

vlog "case#7 backup should succeed on a simulated 26.7.99 server (highest 26.7.x patch)"
xtrabackup --backup --debug=d,simulate_higher_version --no-server-version-check --target-dir=$topdir/backup
xtrabackup --prepare --debug=d,simulate_higher_version --no-server-version-check --target-dir=$topdir/backup
rm -r $topdir/backup
