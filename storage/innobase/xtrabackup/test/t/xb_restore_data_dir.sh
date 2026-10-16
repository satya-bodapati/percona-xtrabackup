########################################################################
# xtrabackup --restore of a table created with DATA DIRECTORY: the file goes
# back to its directory, and a directory InnoDB doesn't know any more is a
# per-table error found before anything is copied.
########################################################################

. inc/common.sh

require_xtradb
require_server_version_higher_than 8.4.0

ext_dir=$TEST_VAR_ROOT/ext_data
mkdir -p $ext_dir

MYSQLD_EXTRA_MY_CNF_OPTS="
innodb_directories=$ext_dir
"
start_server

mysql -e "CREATE DATABASE db1"
mysql db1 -e "CREATE TABLE remote (id INT PRIMARY KEY) DATA DIRECTORY = '$ext_dir'"
mysql db1 -e "INSERT INTO remote VALUES (1), (2), (3)"
mysql db1 -e "CREATE TABLE local_t (id INT PRIMARY KEY)"
sum=$(checksum_table db1 remote)

xtrabackup --backup --target-dir=$topdir/backup
xtrabackup --prepare --target-dir=$topdir/backup

vlog "--dry-run shows where the file goes"
set +e
$XB_BIN $XB_ARGS --restore --target-dir=$topdir/backup \
  --restore-include=db1.remote --dry-run > $topdir/restore.log 2>&1
rc=$?
set -e
cat $topdir/restore.log
[ $rc -eq 0 ] || die "--dry-run failed"
grep -q "db1.remote: 1 file(s), DATA DIRECTORY $ext_dir" $topdir/restore.log \
  || die "--dry-run doesn't show the DATA DIRECTORY"

mysql -e "DROP TABLE db1.remote"
[ ! -f $ext_dir/db1/remote.ibd ] || die "DROP TABLE left the file"

vlog "A DATA DIRECTORY the server doesn't know is refused before copying"
stop_server
MYSQLD_EXTRA_MY_CNF_OPTS=""
start_server
set +e
$XB_BIN $XB_ARGS --restore --target-dir=$topdir/backup \
  --restore-include=db1.remote > $topdir/restore.log 2>&1
rc=$?
set -e
cat $topdir/restore.log
[ $rc -eq 1 ] || die "--restore exited with $rc, expected 1"
grep -q "is not in a directory InnoDB knows" $topdir/restore.log || \
  die "missing the known-directory error"
[ ! -f $ext_dir/db1/remote.ibd ] || die "the file was copied anyway"

vlog "With the directory known again, the table comes back to it"
stop_server
MYSQLD_EXTRA_MY_CNF_OPTS="
innodb_directories=$ext_dir
"
start_server
require_import_table_ibd

xtrabackup --restore --target-dir=$topdir/backup --restore-include=db1.remote
[ -f $ext_dir/db1/remote.ibd ] || die "the file is not back in $ext_dir"
[ ! -f $mysql_datadir/db1/remote.ibd ] || die "the file went to the datadir"
[ "$(checksum_table db1 remote)" = "$sum" ] || die "remote differs"
