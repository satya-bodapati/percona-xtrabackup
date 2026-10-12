########################################################################
# Bug #1343722: Too easy to backup wrong datadir with multiple instances
########################################################################

start_server_with_id 1

socket=$MYSQLD_SOCKET

start_server_with_id 2

# HACK: make InnoDB to write some log records so that server 2 LSN is ahead of
# the server 1 LSN and xtrabackup is able to reach the LSN obtained from
# server 1 while copying redo log from server 2.
# Otherwise backup will hang forever.
# The two servers start with LSNs that differ by a few KB in either direction,
# so write a few MB of redo on server 2 to stay well ahead of server 1.
mysql -e 'CREATE TABLE t (a LONGTEXT)' test
for i in {1..8} ; do
    mysql -e "INSERT INTO t (a) VALUES (REPEAT('a', 1024 * 1024))" test
done

# Try to backup server 2, but use server 1's connection socket
xtrabackup --backup --socket=$socket --target-dir=$topdir/backup 2>&1 | tee $topdir/pxb1343722.log

run_cmd grep 'has different values' $topdir/pxb1343722.log

