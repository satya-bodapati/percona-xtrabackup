#!/bin/bash
# R2 (PS-9459, upstream Bug #116677, likely PXB-3751): a table that got INSTANT ADD COLUMN before 8.0.29
# (old "V1" instant metadata: instant_cols, no row versions) is exported on 8.4 with a V7 .cfg and imported
# into a freshly created table. The import loses the instant metadata, so the old rows are misread.
#
# The old datadir is MySQL's own MTR data std_data/instant_ddl/data_instant_ddl_upgrade.zip (tables were
# created on an 8.0 release before 8.0.29; see the header of mysql-test/suite/innodb/t/instant_ddl_upgrade.test):
#   CREATE TABLE t1_dynamic (c1 char(10), c2 char(10)) ROW_FORMAT=DYNAMIC;
#   INSERT ("r1c1","r1c2"); ADD c3 char(10) default "c3_def" INSTANT; INSERT ("r2c1","r2c2","r2c3");
#   ADD c4 char(10) default "c4_def" INSTANT; INSERT ("r3c1","r3c2","r3c3","r3c4");
. "$(dirname "$0")/common.sh"
ZIP=${ZIP:-$HOME/WORK/ps/ps-8.4-import-repros/mysql-test/std_data/instant_ddl/data_instant_ddl_upgrade.zip}
setup_instance r2_v1_instant
rm -rf "$RUN" && mkdir -p "$RUN"
cp "$ZIP" "$RUN/" && (cd "$RUN" && unzip -qo "$(basename "$ZIP")")
DATADIR=$RUN/data
D=$DATADIR/test
start_server --innodb-page-size=16k || exit 1

info() { sql "SELECT NAME, INSTANT_COLS, TOTAL_ROW_VERSIONS, INITIAL_COLUMN_COUNTS, CURRENT_COLUMN_COUNTS FROM information_schema.innodb_tables WHERE NAME='test/t1_dynamic'"; }

echo "=== Source table after upgrade to 8.4 ==="
info
sql "SELECT * FROM t1_dynamic"
sql "CHECK TABLE t1_dynamic"

echo "=== Export with FLUSH TABLES ... FOR EXPORT (writes a V7 .cfg) ==="
"$MYSQL" -uroot -S "$SOCK" test <<EOF
FLUSH TABLE t1_dynamic FOR EXPORT;
system cp $D/t1_dynamic.ibd $RUN/t1_dynamic.ibd_back
system cp $D/t1_dynamic.cfg $RUN/t1_dynamic.cfg_back
UNLOCK TABLES;
EOF
echo ".cfg version (first 4 bytes, big-endian): $(od -An -tx1 -N4 $RUN/t1_dynamic.cfg_back)"

echo "=== Recreate the table with all columns, DISCARD, IMPORT ==="
sql "DROP TABLE t1_dynamic"
sql "CREATE TABLE t1_dynamic (c1 char(10), c2 char(10), c3 char(10) DEFAULT 'c3_def', c4 char(10) DEFAULT 'c4_def') ROW_FORMAT=DYNAMIC"
sql "ALTER TABLE t1_dynamic DISCARD TABLESPACE"
cp $RUN/t1_dynamic.ibd_back $D/t1_dynamic.ibd; cp $RUN/t1_dynamic.cfg_back $D/t1_dynamic.cfg
sql "ALTER TABLE t1_dynamic IMPORT TABLESPACE"; IMPORT_RC=$?
if kill -0 $MYSQLD_PID 2>/dev/null; then
  info
  sql "SELECT * FROM t1_dynamic"; SELECT_RC=$?
  sql "CHECK TABLE t1_dynamic"
fi
if ! kill -0 $MYSQLD_PID 2>/dev/null || ! "$MYSQL" -uroot -S "$SOCK" -e "SELECT 1" >/dev/null 2>&1; then
  echo "=== mysqld died; error log excerpt ==="
  grep -n "Assertion\|assert\|signal\|ERROR" "$ERRLOG" | tail -10
  wait_server_gone
fi

echo
echo "EXPECTED: IMPORT succeeds, SELECT returns the 3 source rows (r1 with c3_def,c4_def; r2 with c4_def), CHECK TABLE OK,"
echo "          imported table keeps INSTANT_COLS=2 (as the source)."
echo "ACTUAL:   IMPORT rc=$IMPORT_RC, SELECT rc=${SELECT_RC:-n/a}; see output above."
stop_server
