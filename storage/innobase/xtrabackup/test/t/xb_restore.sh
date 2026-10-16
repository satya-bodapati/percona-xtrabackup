########################################################################
# xtrabackup --restore end to end: drop tables and a whole database, restore
# them from a prepared backup into the running server, and check that data,
# definitions, AUTO_INCREMENT, triggers, FULLTEXT indexes and foreign keys
# come back. Needs a server that can IMPORT TABLE FROM an .ibd.
########################################################################

. inc/common.sh

require_xtradb
require_server_version_higher_than 8.4.0

start_server

require_import_table_ibd

mysql -e "CREATE DATABASE db1 CHARACTER SET latin1 COLLATE latin1_swedish_ci"
mysql db1 <<EOF
CREATE TABLE parent (id INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE child (id INT PRIMARY KEY, pid INT,
  CONSTRAINT fk_p FOREIGN KEY (pid) REFERENCES parent (id)) ENGINE=InnoDB;
INSERT INTO parent VALUES (1), (2);
INSERT INTO child VALUES (10, 1), (20, 2);

CREATE TABLE t1 (id INT AUTO_INCREMENT PRIMARY KEY, a INT) ENGINE=InnoDB;
INSERT INTO t1 (a) VALUES (1), (2), (3), (4), (5);
DELETE FROM t1 WHERE id > 3;
CREATE TABLE audit (msg VARCHAR(100)) ENGINE=InnoDB;
CREATE TRIGGER trg1 BEFORE INSERT ON t1 FOR EACH ROW SET NEW.a = NEW.a + 100;
CREATE TRIGGER trg2 AFTER INSERT ON t1 FOR EACH ROW
  INSERT INTO audit VALUES (CONCAT('ins ', NEW.id));
CREATE TRIGGER trg3 AFTER INSERT ON t1 FOR EACH ROW PRECEDES trg2
  INSERT INTO audit VALUES ('first');

CREATE TABLE ft (id INT PRIMARY KEY, title VARCHAR(100), body TEXT,
                 FULLTEXT KEY ftk (body), FULLTEXT KEY ftt (title))
  ENGINE=InnoDB;
INSERT INTO ft VALUES (1, 'greeting', 'hello world'),
                      (2, 'animals', 'quick brown fox');

CREATE TABLE tp (a INT, b VARCHAR(10)) ENGINE=InnoDB
  PARTITION BY RANGE (a) (PARTITION p0 VALUES LESS THAN (10),
                          PARTITION p1 VALUES LESS THAN MAXVALUE);
INSERT INTO tp VALUES (1, 'a'), (50, 'b');

CREATE TABLE \`my-table\` (a INT) ENGINE=InnoDB;
INSERT INTO \`my-table\` VALUES (1);

CREATE TABLE inst (a INT PRIMARY KEY, b INT) ENGINE=InnoDB;
INSERT INTO inst VALUES (1, 1);
ALTER TABLE inst ADD COLUMN c INT DEFAULT 7, ALGORITHM=INSTANT;
INSERT INTO inst VALUES (2, 2, 3);
ALTER TABLE inst DROP COLUMN b, ALGORITHM=INSTANT;
INSERT INTO inst VALUES (3, 9);
EOF

tables="parent child t1 audit ft tp my-table inst"

# Print everything about db1 that a restore must bring back.
function db1_state()
{
  mysql -Ns -e "SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME
                FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = 'db1'"
  for t in $tables; do
    mysql -Ns -e "SHOW CREATE TABLE \`$t\`" db1
    echo "checksum $t $(checksum_table db1 "\`$t\`")"
  done
  mysql -Ns -e "SELECT TRIGGER_NAME, ACTION_ORDER, ACTION_TIMING,
                EVENT_MANIPULATION, ACTION_STATEMENT, DEFINER, SQL_MODE,
                CHARACTER_SET_CLIENT, COLLATION_CONNECTION, DATABASE_COLLATION
                FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = 'db1'
                ORDER BY EVENT_OBJECT_TABLE, ACTION_TIMING, ACTION_ORDER"
}

db1_state > $topdir/state_before

xtrabackup --backup --target-dir=$topdir/backup
xtrabackup --prepare --target-dir=$topdir/backup

function restore()
{
  vlog "===> xtrabackup --restore $*"
  set +e
  $XB_BIN $XB_ARGS --restore --target-dir=$topdir/backup "$@" \
    > $topdir/restore.log 2>&1
  restore_rc=$?
  set -e
  cat $topdir/restore.log
}

function expect_rc()
{
  [ "$restore_rc" -eq "$1" ] || \
    die "--restore exited with $restore_rc, expected $1"
}

binlog_before=$(mysql -Ns -e "SHOW BINARY LOG STATUS" | awk '{print $1":"$2}')

vlog "Case 1: the whole database is gone and comes back"
mysql -e "DROP DATABASE db1"
restore --restore-include=db1 --parallel=4 --restore-report=$topdir/report.json
expect_rc 0
db1_state > $topdir/state_after
diff -u $topdir/state_before $topdir/state_after || \
  die "db1 after --restore differs from db1 before the backup"
grep -q '"restored":8' $topdir/report.json || die "report should count 8"

vlog "Case 2: the restored tables work"
mysql db1 -e "INSERT INTO t1 (a) VALUES (1)"
[ "$(mysql -Ns -e 'SELECT CONCAT(id, ",", a) FROM db1.t1 WHERE id = 6')" = \
  "6,101" ] || die "AUTO_INCREMENT or trigger trg1 is wrong"
[ "$(mysql -Ns -e "SELECT GROUP_CONCAT(msg ORDER BY msg) FROM db1.audit")" = \
  "first,ins 6" ] || die "triggers trg2 and trg3 are wrong"
[ "$(mysql -Ns -e "SELECT id FROM db1.ft
                   WHERE MATCH (body) AGAINST ('fox')")" = "2" ] || \
  die "FULLTEXT search on ftk does not work"
[ "$(mysql -Ns -e "SELECT id FROM db1.ft
                   WHERE MATCH (title) AGAINST ('greeting')")" = "1" ] || \
  die "FULLTEXT search on ftt does not work"
run_cmd_expect_failure $MYSQL $MYSQL_ARGS db1 \
  -e "INSERT INTO child VALUES (30, 99)"
[ "$(stat -c %a $mysql_datadir/db1/t1.ibd)" = "640" ] || \
  die "the restored file should have mode 0640"

vlog "Case 3: nothing went to the binary log"
binlog_after=$(mysql -Ns -e "SHOW BINARY LOG STATUS" | awk '{print $1":"$2}')
if [ -n "$binlog_before" ]; then
  if mysql -Ns -e "SHOW BINLOG EVENTS IN '${binlog_before%%:*}'
                   FROM ${binlog_before##*:}" | \
      grep -Eq "IMPORT TABLE|CREATE DATABASE|AUTO_INCREMENT|CREATE DEFINER"; then
    die "--restore wrote to the binary log"
  fi
fi

vlog "Case 4: existing tables are left alone and reported"
restore --restore-include=db1.t1,db1.tp
expect_rc 1
grep -q "FAILED   db1.t1: the table already exists" $topdir/restore.log || \
  die "t1 should fail"
grep -q "FAILED   db1.tp: the table already exists" $topdir/restore.log || \
  die "tp should fail"

vlog "Case 5: a child without its parent gets a warning"
mysql -e "SET foreign_key_checks = 0; DROP TABLE db1.child, db1.parent"
restore --restore-include=db1.child
expect_rc 1
grep -q "RESTORED db1.child" $topdir/restore.log || die "child not restored"
grep -q "WARNING  db1.child: foreign key fk_p references db1.parent" \
  $topdir/restore.log || die "missing warning about the parent"
restore --restore-include=db1.parent
expect_rc 0
run_cmd_expect_failure $MYSQL $MYSQL_ARGS db1 \
  -e "INSERT INTO child VALUES (30, 99)"
mysql db1 -e "INSERT INTO child VALUES (30, 2)"

vlog "Case 6: a single partitioned table and a table with a coded name"
mysql -e "DROP TABLE db1.tp, db1.\`my-table\`"
restore --restore-include='db1.tp,db1.my-table'
expect_rc 0
[ "$(checksum_table db1 tp)" = "$(grep '^checksum tp ' $topdir/state_before | \
  awk '{print $3}')" ] || die "tp differs"
[ "$(mysql -Ns -e 'SELECT a FROM db1.`my-table`')" = "1" ] || \
  die "my-table differs"
