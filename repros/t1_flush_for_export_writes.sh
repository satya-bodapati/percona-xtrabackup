#!/bin/bash
# Do writes to a table block while another session holds FLUSH TABLES t1 FOR EXPORT?
# Session A: FLUSH TABLES t1 FOR EXPORT, then sleep while holding it.
# Session B: INSERT into t1 with lock_wait_timeout=3; also INSERT into t2 (not exported).
. "$(dirname "$0")/common.sh"
setup_instance t1_export_writes
init_datadir
start_server

sql "CREATE TABLE t1 (a INT PRIMARY KEY) ENGINE=InnoDB;
     CREATE TABLE t2 (a INT PRIMARY KEY) ENGINE=InnoDB;
     INSERT INTO t1 VALUES (1); INSERT INTO t2 VALUES (1);"

# Session A holds the export lock for 15 seconds.
"$MYSQL" -uroot -S "$SOCK" test -e "FLUSH TABLES t1 FOR EXPORT; SELECT SLEEP(15); UNLOCK TABLES;" \
  > "$RUN/sessA.out" 2>&1 &
A_PID=$!
sleep 3
echo "--- .cfg present while locked:"; ls "$DATADIR/test/" | grep -E "t1\.(cfg|cfp)" || echo "(none)"

echo "--- MDL held by session A:"
sql_val "SELECT OBJECT_NAME, LOCK_TYPE, LOCK_STATUS FROM performance_schema.metadata_locks
         WHERE OBJECT_SCHEMA='test' AND OBJECT_NAME IN ('t1','t2');"

echo "--- Session B: INSERT INTO t1 (exported), lock_wait_timeout=3:"
START=$(date +%s.%N)
"$MYSQL" -uroot -S "$SOCK" test -e "SET SESSION lock_wait_timeout=3; INSERT INTO t1 VALUES (2);" 2>&1
END=$(date +%s.%N)
echo "elapsed: $(echo "$END - $START" | bc) s"

echo "--- Session B: UPDATE t1 (exported), lock_wait_timeout=3:"
"$MYSQL" -uroot -S "$SOCK" test -e "SET SESSION lock_wait_timeout=3; UPDATE t1 SET a=10 WHERE a=1;" 2>&1

echo "--- Session B: SELECT from t1 (exported):"
sql_val "SELECT COUNT(*) FROM t1;"

echo "--- Session B: INSERT INTO t2 (not exported):"
"$MYSQL" -uroot -S "$SOCK" test -e "SET SESSION lock_wait_timeout=3; INSERT INTO t2 VALUES (2);" 2>&1 && echo "t2 insert OK"

wait $A_PID
echo "--- After UNLOCK: .cfg present?"; ls "$DATADIR/test/" | grep -E "t1\.(cfg|cfp)" || echo "(none, deleted on UNLOCK)"
echo "--- After UNLOCK: INSERT INTO t1:"
"$MYSQL" -uroot -S "$SOCK" test -e "INSERT INTO t1 VALUES (3);" 2>&1 && echo "t1 insert OK after unlock"
sql_val "SELECT * FROM t1 ORDER BY a;"
stop_server
