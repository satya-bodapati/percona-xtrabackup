#
# PXB-1679: Crash after truncating partition when the backup is taken
#

require_debug_pxb_version
start_server

mysql test <<EOF
CREATE TABLE test01 (id int auto_increment primary key, a TEXT, b TEXT) PARTITION BY HASH(id) PARTITIONS 4;
INSERT INTO test01 (a, b) VALUES (REPEAT('a', 1000), REPEAT('b', 1000));
INSERT INTO test01 (a, b) VALUES (REPEAT('x', 1000), REPEAT('y', 1000));
INSERT INTO test01 (a, b) VALUES (REPEAT('q', 1000), REPEAT('p', 1000));
INSERT INTO test01 (a, b) VALUES (REPEAT('l', 1000), REPEAT('c', 1000));
INSERT INTO test01 (a, b) VALUES (REPEAT('m', 1000), REPEAT('p', 1000));
INSERT INTO test01 (a, b) VALUES (REPEAT('1', 1000), REPEAT('2', 1000));
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;
INSERT INTO test01 (a,b) SELECT a,b FROM test01;

CREATE TABLE test02 (id int auto_increment primary key, a TEXT, b TEXT);
INSERT INTO test02 SELECT * FROM test01;
EOF

# Truncate each table once, when xtrabackup starts copying its file.
function truncate_during_copy()
{
    local line
    while read line ; do
        echo "$line"
        if [[ ! -f $topdir/p3_truncated &&
              $line == *Copying*./test/test01#p#p3.ibd* ]]; then
            mysql -e "ALTER TABLE test01 TRUNCATE PARTITION p3" test
            touch $topdir/p3_truncated
        fi
        if [[ ! -f $topdir/test02_truncated &&
              $line == *Copying*./test/test02.ibd* ]]; then
            mysql -e "TRUNCATE TABLE test02" test
            touch $topdir/test02_truncated
        fi
    done
}

# Pause the backup before it takes the backup lock, until both TRUNCATEs have
# finished. Otherwise a TRUNCATE could wait for the lock and run after the
# backup, and the data would not match the state recorded below.
xtrabackup --backup --lock-ddl=REDUCED --target-dir=$topdir/backup \
    --debug-sync="ddl_tracker_before_lock_ddl" \
    2> >(truncate_during_copy) &
job_pid=$!
wait_for_xb_to_suspend $topdir/backup/xtrabackup_debug_sync
for i in $(seq 1 60); do
    [[ -f $topdir/p3_truncated && -f $topdir/test02_truncated ]] && break
    sleep 1
done
[[ -f $topdir/p3_truncated && -f $topdir/test02_truncated ]] || \
    die "TRUNCATE did not run during the backup"
kill -SIGCONT $(cat $topdir/backup/xtrabackup_debug_sync)
run_cmd wait $job_pid

record_db_state test

shutdown_server

xtrabackup --prepare --target-dir=$topdir/backup
rm -rf $mysql_datadir
xtrabackup --copy-back --target-dir=$topdir/backup

start_server
verify_db_state test
