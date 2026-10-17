#!/bin/bash
# R3: ALTER TABLE ... DISCARD TABLESPACE is not atomic. InnoDB deletes the .ibd inside the handler call
# (row_discard_tablespace_for_mysql -> fil_discard_tablespace -> fil_delete_tablespace), before the SQL layer
# stores the DD change and commits. If anything fails after that point the statement rolls back, the DD still
# says the table is normal, but its data file is already gone.
# Needs a debug server: a DD store failure is injected with the debug point fail_while_storing_dd_object
# (sql/dd/impl/types/weak_object_impl.cc). It first fires inside InnoDB's own dd_table_discard_tablespace ->
# dd_tablespace_set_state, which asserts (dict0dd.cc:6873) - i.e. the server dies AFTER the .ibd was deleted
# and BEFORE the DD change committed. That is the same window any crash or kill would hit. After restart the
# DD still describes a normal table whose data file is gone: the data is lost. With atomic DDL the file delete
# would be deferred to post-DDL (DDL log) and survive a crash before commit.
. "$(dirname "$0")/common.sh"
setup_instance r3_discard
init_datadir
start_server || exit 1
D=$DATADIR/test

sql "CREATE TABLE t1 (a INT PRIMARY KEY, b VARCHAR(10)); INSERT INTO t1 VALUES (1,'one'),(2,'two')"
echo "files before: $(ls $D)"
sql "SET SESSION debug='+d,fail_while_storing_dd_object'; ALTER TABLE t1 DISCARD TABLESPACE"; DISCARD_RC=$?
if wait_server_gone; then
  echo "mysqld died during DISCARD:"; grep -m1 "Assertion failure" "$ERRLOG"
fi
echo "files after the failed DISCARD: $(ls $D)"
echo "=== restart ==="
restart_server || exit 1
sql "SELECT * FROM t1"; SELECT2_RC=$?
grep -n "t1" "$ERRLOG" | grep -i "missing\|cannot\|not found\|discard" | tail -5

echo
echo "EXPECTED: a DISCARD that fails or crashes before its DD commit leaves t1 intact: t1.ibd on disk,"
echo "          SELECT returns 2 rows after restart (DISCARD rc here=$DISCARD_RC)."
echo "ACTUAL:   t1.ibd present after failed DISCARD: $([ -f $D/t1.ibd ] && echo yes || echo NO);"
echo "          SELECT rc after restart=$SELECT2_RC (non-zero = error, see above)."
stop_server
