########################################################################
# Bug #1291299: xtrabackup_56 crashes with segfault during --prepare
########################################################################

. inc/common.sh
require_debug_pxb_version

remote_dir=$TEST_VAR_ROOT/var1/remote_dir

mkdir -p $remote_dir
start_server --innodb_directories=$remote_dir

xtrabackup --backup --lock-ddl=REDUCED --target-dir=$topdir/backup \
           --debug-sync="data_copy_thread_func" &

job_pid=$!
pid_file=$topdir/backup/xtrabackup_debug_sync

# Wait for xtrabackup to suspend
i=0
while [ ! -r "$pid_file" ]
do
    sleep 1
    i=$((i+1))
    echo "Waited $i seconds for $pid_file to be created"
done

xb_pid=`cat $pid_file`

# Create a remote tablespace while the files are copied. DDL during a backup
# with --lock-ddl=OFF is not safe, the reduced lock tracks the new tablespace
# and copies it under the backup lock.


run_cmd $MYSQL $MYSQL_ARGS test <<EOF
CREATE TABLE t1 (a INT) DATA DIRECTORY='$remote_dir';
INSERT INTO t1 VALUES (1), (2), (3);
EOF

# Resume xtrabackup
vlog "Resuming xtrabackup"
kill -SIGCONT $xb_pid

run_cmd wait $job_pid

stop_server

# Prepare. Would crash here with the bug present
xtrabackup --datadir=$mysql_datadir --prepare --target-dir=$topdir/backup

# Check that the tablespace has been created locally
if [ ! -f $topdir/backup/test/t1.ibd ]
then
    die "Cannot find $topdir/backup/test/t1.ibd"
fi
