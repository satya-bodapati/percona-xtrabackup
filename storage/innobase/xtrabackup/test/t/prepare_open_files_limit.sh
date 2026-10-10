########################################################################
# Prepare must not hang when the redo touches more tablespaces than
# --innodb-open-files allows.
#
# Applying redo held recv_sys->mutex while opening a tablespace file. With
# all file slots in use, opening waits for pending reads to complete, and the
# I/O threads need recv_sys->mutex to complete them.
########################################################################

. inc/common.sh

start_server --innodb_file_per_table

num_tables=200

for ((i = 1; i <= num_tables; i++)); do
  echo "CREATE TABLE t$i (a INT PRIMARY KEY AUTO_INCREMENT, b CHAR(255));"
done | mysql test

# Write to every table while the backup runs, so that its redo touches all
# of the tablespaces.
stop_file=$topdir/stop_inserts
(
  while [ ! -f $stop_file ]; do
    for ((i = 1; i <= num_tables; i++)); do
      echo "INSERT INTO t$i (b) VALUES ('x');"
    done | mysql test
  done
) &
inserts_pid=$!

xtrabackup --backup --target-dir=$topdir/backup

touch $stop_file
run_cmd wait $inserts_pid

stop_server

# Before the fix, this hung until the timeout.
run_cmd timeout 300 $XB_BIN $XB_ARGS --prepare --target-dir=$topdir/backup \
  --innodb-open-files=10

rm -rf $mysql_datadir
xtrabackup --copy-back --target-dir=$topdir/backup

start_server

for ((i = 1; i <= num_tables; i++)); do
  echo "CHECK TABLE t$i;"
done | mysql test | grep -v 'status.*OK' | grep -q test.t \
  && die "CHECK TABLE failed"

true
