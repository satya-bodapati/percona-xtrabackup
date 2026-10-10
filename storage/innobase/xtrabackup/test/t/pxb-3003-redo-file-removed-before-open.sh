########################################################################
# PXB-3003: the server removes a redo log file between xtrabackup's
# existence check and its open. xtrabackup must treat the file as missing,
# not exit with "Cannot continue operation".
#
# The debug keywords make the next open after redo catch-up use a path that
# does not exist, in log_check_file() ("check") or Log_file_handle::open()
# ("open"), so each window is hit on every run.
#
# A file that was not copied yet must still fail the backup with an error.
# The redo consumer keeps the server from removing it, so the "switch"
# keyword makes the open of the next file fail while xtrabackup reads redo.
########################################################################
. inc/common.sh

require_debug_pxb_version

MYSQLD_EXTRA_MY_CNF_OPTS="
innodb_redo_log_capacity=16M
"
start_server

# Fill several redo files, so the oldest one is no longer needed by the
# time the backup has caught up and reopens the files.
mysql -e "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, b BLOB)" test
for i in $(seq 60) ; do
  mysql -e "INSERT INTO t VALUES (NULL, REPEAT('a', 63 * 1024))" test
done

for point in check open ; do
  vlog "Case: redo file removed before the open in the $point step"
  backup_dir=$topdir/backup_$point
  log=$topdir/backup_$point.log
  xtrabackup --backup --target-dir=$backup_dir \
    --debug="d,xtrabackup_reopen_files_after_catchup,xtrabackup_redo_vanish_on_$point" \
    2> >(tee $log) &
  job_pid=$!
  pid_file=$backup_dir/xtrabackup_debug_sync
  wait_for_xb_to_suspend $pid_file
  kill -SIGCONT `cat $pid_file`
  run_cmd wait $job_pid

  grep -q "was removed before it could be opened" $log \
    || die "xtrabackup did not reach the removed-file path in the $point step"
done

vlog "Case: next redo file is gone when the reader switches to it"
backup_dir=$topdir/backup_switch
log=$topdir/backup_switch.log
xtrabackup --backup --target-dir=$backup_dir \
  --debug="d,xtrabackup_reopen_files_after_catchup,xtrabackup_redo_vanish_on_switch" \
  2> >(tee $log) &
job_pid=$!
pid_file=$backup_dir/xtrabackup_debug_sync
wait_for_xb_to_suspend $pid_file
# Write more redo than one file holds (capacity / 32), so the reader has
# to switch to the next file after it resumes.
for i in $(seq 20) ; do
  mysql -e "INSERT INTO t VALUES (NULL, REPEAT('b', 63 * 1024))" test
done
kill -SIGCONT `cat $pid_file`
if wait $job_pid ; then
  die "xtrabackup succeeded although a redo file it needed was gone"
fi

grep -q "that has not been copied yet" $log \
  || die "xtrabackup did not report the missing redo file"
grep -q "read_logfile() failed" $log \
  || die "xtrabackup did not fail the redo copy"
