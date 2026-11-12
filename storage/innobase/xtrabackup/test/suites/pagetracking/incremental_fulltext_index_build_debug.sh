########################################################################
# A page-tracking incremental backup of a FULLTEXT index built after the
# full backup
#
# ALTER TABLE ... ADD FULLTEXT fills the FTS aux index tables without redo.
# The checkpoint does not move between the full backup and the incremental,
# so the incremental gets no changed pages from the server and takes the
# changes from its redo; it must copy the FTS aux tables in full.
########################################################################

. inc/common.sh

require_debug_server

start_server
mysql -e "INSTALL COMPONENT 'file://component_mysqlbackup'"

# The checkpoint passes the start of page tracking, then stays at the
# first page dirtied below: the page cleaner is stopped.
mysql -e "SELECT mysqlbackup_page_track_set(1)" >/dev/null
innodb_wait_for_flush_all
mysql -e "SET GLOBAL innodb_page_cleaner_disabled_debug = ON"

mysql test -e "CREATE TABLE f (id INT PRIMARY KEY AUTO_INCREMENT,
                               body TEXT NOT NULL) ENGINE=InnoDB"
mysql test -e "SET SESSION cte_max_recursion_depth = 2000;
    INSERT INTO f (body)
    WITH RECURSIVE n (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n
                             WHERE i < 2000)
    SELECT CONCAT('alpha', i, ' beta', i MOD 7, ' gamma', i MOD 100) FROM n"

xtrabackup --backup --page-tracking --target-dir=$topdir/full

mysql test -e "ALTER TABLE f ADD FULLTEXT KEY ft (body)"

xtrabackup --backup --page-tracking --incremental-basedir=$topdir/full \
           --target-dir=$topdir/inc
grep -q "incremental backup LSN is same as last checkpoint LSN" $OUTFILE ||
  die "The checkpoint moved after the full backup"

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

stop_server
rm -rf $mysql_datadir
xtrabackup --prepare --apply-log-only --target-dir=$topdir/full
xtrabackup --prepare --target-dir=$topdir/full --incremental-dir=$topdir/inc
xtrabackup --copy-back --target-dir=$topdir/full
start_server

fts_snapshot restored
diff $topdir/server.fts $topdir/restored.fts ||
  die "The restored FULLTEXT index is not what the server had"
