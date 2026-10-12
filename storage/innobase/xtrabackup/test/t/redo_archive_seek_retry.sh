########################################################################
# Backup must switch to the redo log archive even when the archive has not
# reached the redo read so far at the first try.
#
# xtrabackup tried to switch only once. If the archive was behind, it kept
# reading live redo for the rest of the backup, and failed when the server
# removed a redo file not yet read.
########################################################################

. inc/common.sh

require_debug_pxb_version
require_server_version_higher_than 8.0.16

mkdir $TEST_VAR_ROOT/archive

start_server --innodb-redo-log-archive-dirs=":$TEST_VAR_ROOT/archive"

mysql -e "CREATE TABLE t (a INT PRIMARY KEY AUTO_INCREMENT, b BLOB)" test

# Write redo while the backup runs, so that it finds a block in the archive.
stop_file=$topdir/stop_inserts
(
  while [ ! -f $stop_file ]; do
    mysql -e "INSERT INTO t (b) VALUES (REPEAT('a', 60000))" test
  done
) &
inserts_pid=$!

# Fail the first try to switch to the archive.
xtrabackup --backup --target-dir=$topdir/backup \
  --debug=d,simulate_archive_behind 2> >(tee $topdir/backup.log)

touch $stop_file
run_cmd wait $inserts_pid

if ! grep -q "Switched to archived redo log" $topdir/backup.log; then
  die "xtrabackup did not switch to the redo log archive"
fi
