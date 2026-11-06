########################################################################
# PXB-3915: backup of a table with an HNSW vector index under write load
#
# Percona Server redo-logs the label counter of a vector-indexed table
# (the hidden column percona_vec_aux_id) as dynamic metadata of type
# PM_TABLE_VEC_IDX_ID = 200. xtrabackup must parse it during --backup,
# carry it through --prepare into mysql.innodb_dynamic_metadata, and the
# restored server must neither lose rows from the vector index nor hand
# out a label twice.
#
# The load mixes committed INSERTs with INSERTs that are rolled back on
# the server (their labels are consumed and their graph nodes become
# orphans), and the backup is taken while one more transaction holds
# uncommitted INSERTs, which --prepare rolls back.
########################################################################

. inc/common.sh

start_server --innodb-redo-log-capacity=1G

if ! $MYSQL $MYSQL_ARGS -e "CREATE TABLE test.vec_probe (id BIGINT UNSIGNED PRIMARY KEY,
      v VECTOR(2) NOT NULL, VECTOR KEY vk (v) TYPE hnsw) ENGINE=InnoDB" \
      >/dev/null 2>&1
then
  skip_test "Requires a server with vector index support"
fi
mysql -e "DROP TABLE test.vec_probe"

DIM=16

mysql test <<EOF
CREATE TABLE t (id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
                v VECTOR($DIM) NOT NULL, c INT,
                VECTOR KEY vk (v) TYPE hnsw (M = 8)) ENGINE=InnoDB;

DELIMITER //
-- A random vector. The first component is the seed, so that every row
-- written by the load has a vector of its own.
CREATE FUNCTION rv(seed INT) RETURNS TEXT NO SQL
BEGIN
  DECLARE s TEXT DEFAULT CONCAT('[', seed);
  DECLARE i INT DEFAULT 1;
  WHILE i < $DIM DO
    SET s = CONCAT(s, ',', ROUND(RAND() * 1000, 3));
    SET i = i + 1;
  END WHILE;
  RETURN CONCAT(s, ']');
END//

-- n rounds of: an INSERT, and, with rb set, every 4th round an INSERT
-- that is rolled back after it took a label. Each INSERT commits on its
-- own unless the caller has a transaction open (and rb is not set). A
-- pause of pause_s seconds after every round keeps the redo rate one the
-- backup can follow.
CREATE PROCEDURE load_rows(writer INT, n INT, rb INT, pause_s DOUBLE)
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < n DO
    INSERT INTO t (v, c) VALUES (STRING_TO_VECTOR(rv(writer * 1000000 + i)),
                                 writer);
    IF rb AND i % 4 = 0 THEN
      START TRANSACTION;
      INSERT INTO t (v, c)
        VALUES (STRING_TO_VECTOR(rv(-(writer * 1000000 + i))), -writer);
      ROLLBACK;
    END IF;
    IF pause_s > 0 THEN
      DO SLEEP(pause_s);
    END IF;
    SET i = i + 1;
  END WHILE;
END//

-- Look every row up through the vector index by its own vector, and
-- return how many rows did not come back as their own nearest neighbour.
CREATE PROCEDURE check_reachable(OUT missed INT)
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE rid BIGINT UNSIGNED;
  DECLARE found BIGINT UNSIGNED;
  DECLARE cur CURSOR FOR SELECT id FROM t FORCE INDEX (PRIMARY);
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;
  SET missed = 0;
  OPEN cur;
  scan: LOOP
    FETCH cur INTO rid;
    IF done THEN LEAVE scan; END IF;
    SELECT v INTO @q FROM t WHERE id = rid;
    SET found = NULL;
    SELECT id INTO found FROM t ORDER BY DISTANCE(v, @q, 'EUCLIDEAN') LIMIT 1;
    SET done = 0;
    IF found IS NULL OR found <> rid THEN
      SET missed = missed + 1;
    END IF;
  END LOOP;
  CLOSE cur;
END//
DELIMITER ;
EOF

vlog "Loading initial rows"
mysql test -e "CALL load_rows(0, 400, 1, 0)"

# A transaction that is still open when the backup ends: --prepare rolls
# its INSERTs back. It inserts before the load starts: the server itself
# can leave a row unreachable through the index when INSERTs into the graph
# run concurrently, so the INSERTs of this test are serialized.
$MYSQL $MYSQL_ARGS test >/dev/null 2>&1 <<EOF &
START TRANSACTION;
CALL load_rows(9, 50, 0, 0);
SELECT SLEEP(10000);
EOF
open_trx_pid=$!

for i in $(seq 60)
do
  n=$($MYSQL $MYSQL_ARGS -Ns -e "SELECT COALESCE(SUM(trx_rows_modified), 0)
        FROM information_schema.innodb_trx")
  [ "$n" -ge 50 ] && break
  sleep 1
done
[ "$n" -ge 50 ] || die "The open transaction did not insert its rows"

vlog "Starting the write load"
$MYSQL $MYSQL_ARGS test -e "CALL load_rows(1, 1000000, 1, 0.002)" \
       >/dev/null 2>&1 &
load_pid=$!

sleep 3

xtrabackup --backup --target-dir=$topdir/backup

kill -9 $load_pid $open_trx_pid
wait $load_pid $open_trx_pid 2>/dev/null || true

stop_server

# The server refuses to import a table with a vector index, but preparing
# with --export must still work, and export the other tables.
cp -a $topdir/backup $topdir/backup_export
xtrabackup --prepare --export --target-dir=$topdir/backup_export
[ -f $topdir/backup_export/test/t.cfg ] || die "t.cfg was not written"
rm -rf $topdir/backup_export

xtrabackup --prepare --target-dir=$topdir/backup

rm -rf $mysql_datadir
xtrabackup --copy-back --target-dir=$topdir/backup

start_server --innodb-redo-log-capacity=1G

rows=$($MYSQL $MYSQL_ARGS -Ns -e \
  "SELECT COUNT(*) FROM test.t FORCE INDEX (PRIMARY)")
vlog "Restored $rows rows"
[ "$rows" -gt 400 ] || die "Expected the write load in the backup"

rolled_back=$($MYSQL $MYSQL_ARGS -Ns -e \
  "SELECT COUNT(*) FROM test.t FORCE INDEX (PRIMARY) WHERE c < 0 OR c = 9")
[ "$rolled_back" -eq 0 ] || die "$rolled_back rolled back rows were restored"

# The statistics in the backup can predate the load, and a table that
# looks empty is scanned instead of searched through vk.
mysql test -e "ANALYZE TABLE t"

$MYSQL $MYSQL_ARGS -Ns test -e "SET @q = STRING_TO_VECTOR(rv(0));
  EXPLAIN SELECT id FROM t ORDER BY DISTANCE(v, @q, 'EUCLIDEAN') LIMIT 1" |
  grep "Vector index search on t using vk" ||
  die "The nearest-neighbour query does not use the vector index"

# How many rows the index cannot find is logged, not checked: the server
# itself can leave a row unreachable on some data (PS-11888), so a check
# would fail at random. The graph itself is checked on a debug server.
log_unreachable()
{
  local missed
  missed=$($MYSQL $MYSQL_ARGS -Ns test -e \
    "SET SESSION innodb_hnsw_ef_search = 400;
     CALL check_reachable(@m); SELECT @m")
  vlog "Rows not reachable through vk $1: $missed"
}

# Read every row through vk and compare with the rows of the table. The
# search goes on past LIMIT's batch until the graph is exhausted, so it
# returns every row whose node can be reached from the entry point. A
# committed row without a node is never returned. This works on any
# build; it can also miss a row the server left unreachable (PS-11888,
# PS-11780), which the data of this test does not trigger.
check_rows_in_index()
{
  $MYSQL $MYSQL_ARGS -Ns test -e \
    "SELECT id FROM t FORCE INDEX (vk)
       ORDER BY DISTANCE(v, STRING_TO_VECTOR(rv(0)), 'EUCLIDEAN')
       LIMIT 100000000" | sort -u > $topdir/index_pks
  $MYSQL $MYSQL_ARGS -Ns test -e "SELECT id FROM t FORCE INDEX (PRIMARY)" |
    sort -u > $topdir/table_pks
  local missing
  missing=$(comm -23 $topdir/table_pks $topdir/index_pks | wc -l)
  vlog "Rows not returned through vk $1: $missing of $(wc -l < $topdir/table_pks)"
  [ "$missing" -eq 0 ] ||
    die "$missing rows are not returned through vk $1:" \
        "$(comm -23 $topdir/table_pks $topdir/index_pks | head -10 | tr '\n' ' ')"
  [ -z "$(comm -13 $topdir/table_pks $topdir/index_pks)" ] ||
    die "vk returned rows that are not in the table $1"
}

# On a debug server, check the graph against the table: one node per
# label and per row id. A label handed out twice, or a row id reused
# after AUTO_INCREMENT went back, gives two nodes for one row id. Rows
# rolled back keep their node, so there are at least as many nodes as
# rows.
check_graph()
{
  local out rows
  if ! is_debug_server
  then
    vlog "Not a debug server: graph not checked"
    return
  fi
  out=$($MYSQL $MYSQL_ARGS -Ns -e \
    "SET SESSION innodb_interpreter = 'vec_aux_verify test/t';
     SELECT @@session.innodb_interpreter_output") ||
    die "vec_aux_verify failed"
  rows=$($MYSQL $MYSQL_ARGS -Ns -e \
    "SELECT COUNT(*) FROM test.t FORCE INDEX (PRIMARY)")
  vlog "Graph $1: $out, rows=$rows"
  echo "$out" | grep -q " dup_ids=0 dup_base_pks=0 " ||
    die "The vector index graph has duplicate nodes $1: $out"
  local nodes=${out#nodes=}
  nodes=${nodes%% *}
  [ "$nodes" -ge "$rows" ] ||
    die "The vector index graph has $nodes nodes for $rows rows $1"

  # Every committed row has a node: its primary key is the base_pk of a
  # node in the aux table. The server commits a node before its row, so
  # every row committed in the redo of the backup has its node too.
  # This does not depend on the search finding the row.
  $MYSQL $MYSQL_ARGS -Ns -e \
    "SET SESSION innodb_interpreter = 'vec_aux_dump test/t';
     SELECT @@session.innodb_interpreter_output" |
    sed 's/\\n/\n/g' > $topdir/graph.dump ||
    die "vec_aux_dump failed"
  # id=0 is the index metadata, its base_pk the entry point: not a row.
  grep -v '^id=0 ' $topdir/graph.dump |
    sed -n 's/.* base_pk=\([0-9]*\) .*/\1/p' |
    sort -u > $topdir/node_pks
  $MYSQL $MYSQL_ARGS -Ns -e "SELECT id FROM test.t FORCE INDEX (PRIMARY)" |
    sort -u > $topdir/row_pks
  local missing
  missing=$(comm -23 $topdir/row_pks $topdir/node_pks | wc -l)
  vlog "Committed rows without a node $1: $missing"
  [ "$missing" -eq 0 ] ||
    die "$missing committed rows have no node in the vector index $1:" \
        "$(comm -23 $topdir/row_pks $topdir/node_pks | head -10 | tr '\n' ' ')"

  # Neighbours that name no node: a server-side issue (PS-11907), logged.
  sed -n 's/^id=\([0-9]*\) .*/\1/p' $topdir/graph.dump |
    sort -u > $topdir/node_ids
  sed -n 's/.* nb=\([0-9,]*\).*/\1/p' $topdir/graph.dump | tr ',' '\n' |
    grep . | sort -u > $topdir/nb_ids
  vlog "Neighbours that name no node $1:" \
       "$(comm -23 $topdir/nb_ids $topdir/node_ids | wc -l)"
}

check_rows_in_index "after the restore"
log_unreachable "after the restore"
check_graph "after the restore"

# New rows must get labels that the restored graph has not seen: a label
# handed out twice collides with the node of a restored or rolled-back row.
vlog "Inserting after the restore"
mysql test -e "CALL load_rows(5, 400, 1, 0)"

check_rows_in_index "after the new INSERTs"
log_unreachable "after the new INSERTs"
check_graph "after the new INSERTs"
