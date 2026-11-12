########################################################################
# --lock-ddl=reduced and a FULLTEXT index built during the backup
#
# ALTER TABLE ... ADD FULLTEXT stops after it created the FTS aux tables
# and before it fills them without redo, until xtrabackup has copied the
# files. The aux tables are copied empty, so xtrabackup must copy them
# again under the lock.
########################################################################

. inc/common.sh

require_debug_pxb_version
require_debug_server

MYSQLD_EXTRA_MY_CNF_OPTS="
debug-sync-timeout=600
"

start_server

mysql test -e "CREATE TABLE f (id INT PRIMARY KEY AUTO_INCREMENT,
                               body TEXT NOT NULL) ENGINE=InnoDB"
mysql test -e "SET SESSION cte_max_recursion_depth = 2000;
    INSERT INTO f (body)
    WITH RECURSIVE n (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n
                             WHERE i < 2000)
    SELECT CONCAT('alpha', i, ' beta', i MOD 7, ' gamma', i MOD 100) FROM n"

$MYSQL $MYSQL_ARGS test -e "SET DEBUG_SYNC =
    'alter_table_inplace_after_lock_downgrade SIGNAL aux_created WAIT_FOR fill';
    ALTER TABLE f ADD FULLTEXT KEY ft (body)" &
alter_pid=$!
mysql -e "SET DEBUG_SYNC = 'now WAIT_FOR aux_created'"
ls $mysql_datadir/test

xtrabackup --backup --lock-ddl=reduced --target-dir=$topdir/backup \
           --debug-sync="ddl_tracker_before_lock_ddl" &
job_pid=$!
wait_for_xb_to_suspend $topdir/backup/xtrabackup_debug_sync

mysql -e "SET DEBUG_SYNC = 'now SIGNAL fill'"
run_cmd wait $alter_pid

# Rows found through the FULLTEXT index, for words in every row, in some
# rows and in one row.
function fts_snapshot()
{
  for w in beta3 gamma42 alpha1999 alpha7
  do
    mysql -Ns test -e "SELECT id FROM f WHERE MATCH (body) AGAINST ('$w')
                       ORDER BY id"
  done > $topdir/$1.fts
  [ -s $topdir/$1.fts ] || die "$1: the FULLTEXT index found no rows"
}

fts_snapshot server

resume_suspended_xb $topdir/backup/xtrabackup_debug_sync
run_cmd wait $job_pid

stop_server
rm -rf $mysql_datadir
xtrabackup --prepare --target-dir=$topdir/backup
xtrabackup --copy-back --target-dir=$topdir/backup
start_server

fts_snapshot restored
diff $topdir/server.fts $topdir/restored.fts ||
  die "The restored FULLTEXT index is not what the server had"
