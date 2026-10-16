########################################################################
# xtrabackup --restore: table selection, --dry-run, refusals, exit codes
# and the per-table failure path. Runs on any Percona Server 8.4: it does not
# need a server that can IMPORT TABLE FROM an .ibd.
########################################################################

. inc/common.sh

require_xtradb
require_server_version_higher_than 8.4.0

start_server

mysql -e "CREATE DATABASE db1; CREATE DATABASE \`db-2\`"
mysql db1 <<EOF
CREATE TABLE t1 (id INT AUTO_INCREMENT PRIMARY KEY, a INT) ENGINE=InnoDB;
INSERT INTO t1 (a) VALUES (1), (2), (3), (4), (5);
DELETE FROM t1 WHERE id > 3;
CREATE TABLE t2 (a INT, b TEXT, FULLTEXT KEY ft (b)) ENGINE=InnoDB;
CREATE TABLE tp (a INT) ENGINE=InnoDB PARTITION BY HASH (a) PARTITIONS 3;
CREATE TABLE \`my-table\` (a INT) ENGINE=InnoDB;
CREATE TABLE audit (msg VARCHAR(100)) ENGINE=InnoDB;
CREATE TRIGGER trg1 BEFORE INSERT ON t1 FOR EACH ROW SET NEW.a = NEW.a + 1;
CREATE TRIGGER trg2 AFTER INSERT ON t1 FOR EACH ROW INSERT INTO audit VALUES ('x');
EOF
mysql -e "CREATE TABLE \`db-2\`.x1 (a INT) ENGINE=InnoDB"

xtrabackup --backup --target-dir=$topdir/backup
cp -a $topdir/backup $topdir/backup_unprepared
xtrabackup --prepare --target-dir=$topdir/backup

# Run --restore, keep its output in $topdir/restore.log and its exit code in
# $restore_rc.
function restore()
{
  vlog "===> xtrabackup --restore $*"
  set +e
  $XB_BIN $XB_ARGS --restore "$@" > $topdir/restore.log 2>&1
  restore_rc=$?
  set -e
  cat $topdir/restore.log
}

function expect_rc()
{
  if [ "$restore_rc" -ne "$1" ]; then
    die "--restore exited with $restore_rc, expected $1"
  fi
}

function expect_listed()
{
  grep -q "^.*  $1: " $topdir/restore.log || die "$1 is not in the list"
}

function expect_not_listed()
{
  if grep -q "^.*  $1: " $topdir/restore.log; then
    die "$1 is in the list but should not be"
  fi
}

vlog "Case 1: a whole database"
restore --target-dir=$topdir/backup --restore-include=db1 --dry-run
expect_rc 0
for t in db1.t1 db1.t2 db1.tp db1.my-table db1.audit; do
  expect_listed $t
done
expect_not_listed db-2.x1
grep -q "db1.t1: 1 file(s), AUTO_INCREMENT 6, 2 trigger(s)" $topdir/restore.log \
  || die "t1 should show its AUTO_INCREMENT counter 6 and 2 triggers"
grep -q "db1.tp: 3 file(s), partitioned" $topdir/restore.log \
  || die "tp should be one table of 3 partition files"
grep -q "db1.t2: 1 file(s), 1 FULLTEXT index(es)" $topdir/restore.log \
  || die "t2 should show its FULLTEXT index"
if grep -q "fts_" $topdir/restore.log; then
  die "FULLTEXT auxiliary tablespaces must not be listed as tables"
fi

vlog "Case 2: globs, exclude, and a name that needs decoding"
restore --target-dir=$topdir/backup --restore-include='db1.t*,db-2' \
  --restore-exclude='db1.t2' --dry-run
expect_rc 0
expect_listed db1.t1
expect_listed db1.tp
expect_listed db-2.x1
expect_not_listed db1.t2
expect_not_listed db1.audit

vlog "Case 3: pattern files"
echo "# comment" > $topdir/include.txt
echo "db1.my-table" >> $topdir/include.txt
echo "db1.audit" >> $topdir/include.txt
restore --target-dir=$topdir/backup --restore-include-file=$topdir/include.txt \
  --dry-run
expect_rc 0
expect_listed db1.my-table
expect_listed db1.audit
expect_not_listed db1.t1

vlog "Case 4: a pattern that matches nothing is fatal"
restore --target-dir=$topdir/backup --restore-include='db1.t1,nosuch.*' \
  --dry-run
expect_rc 2
grep -q "pattern 'nosuch.\*' matches no table in the backup" \
  $topdir/restore.log || die "missing unmatched pattern error"

vlog "Case 5: no selection is fatal"
restore --target-dir=$topdir/backup
expect_rc 2
grep -q "needs --restore-include" $topdir/restore.log \
  || die "missing selection error"

vlog "Case 6: an unprepared backup is refused"
restore --target-dir=$topdir/backup_unprepared --restore-include=db1
expect_rc 2
grep -q "is not prepared" $topdir/restore.log || die "missing not prepared error"

vlog "Case 7: --restore and --prepare don't mix"
restore --prepare --target-dir=$topdir/backup --restore-include=db1
expect_rc 2

vlog "Case 8: an existing table is never replaced"
restore --target-dir=$topdir/backup --restore-include=db1.t1 \
  --restore-report=$topdir/report.json
expect_rc 1
grep -q "FAILED   db1.t1: the table already exists" $topdir/restore.log \
  || die "t1 should fail because it exists"
grep -q '"status":"failed"' $topdir/report.json || die "report should say failed"
grep -q '"failed":1' $topdir/report.json || die "report should count 1 failure"
[ "$(mysql -Ns -e 'SELECT COUNT(*) FROM db1.t1')" = 3 ] \
  || die "the existing t1 must be left alone"

vlog "Case 9: a failed import leaves no files behind"
mysql -e "DROP TABLE db1.audit"
restore --target-dir=$topdir/backup --restore-include=db1.audit
if grep -q "RESTORED db1.audit" $topdir/restore.log; then
  vlog "This server can IMPORT TABLE FROM .ibd; the failure path is covered by"
  vlog "xb_restore.sh on such a server"
  expect_rc 0
else
  expect_rc 1
  grep -q "FAILED   db1.audit: IMPORT TABLE failed" $topdir/restore.log \
    || die "audit should fail at IMPORT TABLE"
  if ls $mysql_datadir/db1/audit.* 2>/dev/null; then
    die "--restore left the copied files of audit in the datadir"
  fi
fi

vlog "Case 10: --dry-run changes nothing"
# An earlier case may have restored the table on a server that can import.
mysql -e "DROP TABLE IF EXISTS db1.audit"
restore --target-dir=$topdir/backup --restore-include=db1 --dry-run
expect_rc 0
if [ -n "$(mysql -Ns -e "SHOW TABLES FROM db1 LIKE 'audit'")" ]; then
  die "--dry-run created a table"
fi
