#!/bin/bash
# R1 (PS-8687): AUTO_INCREMENT after IMPORT TABLESPACE is wrong after a restart.
# Case A follows the reporter's MTR test (export_autoinc.test attached to PS-8687).
# Case B imports into a freshly created table instead of the same one.
. "$(dirname "$0")/common.sh"
setup_instance r1_autoinc
init_datadir
start_server || exit 1
D=$DATADIR/test

autoinc() { sql_val "SELECT AUTO_INCREMENT FROM information_schema.tables WHERE table_schema='test' AND table_name='$1'"; }

echo "=== Case A: same table, DISCARD + IMPORT with .cfg (reporter's test) ==="
sql "SET SESSION information_schema_stats_expiry=0; SET PERSIST information_schema_stats_expiry=0"
sql "CREATE TABLE t1 (i SERIAL); INSERT INTO t1 VALUES (NULL)"
"$MYSQL" -uroot -S "$SOCK" test <<EOF
FLUSH TABLE t1 FOR EXPORT;
system cp $D/t1.ibd $RUN/t1.ibd_back
system cp $D/t1.cfg $RUN/t1.cfg_back
UNLOCK TABLES;
EOF
sql "ALTER TABLE t1 DISCARD TABLESPACE"
cp $RUN/t1.ibd_back $D/t1.ibd; cp $RUN/t1.cfg_back $D/t1.cfg
sql "ALTER TABLE t1 IMPORT TABLESPACE"
sql "INSERT INTO t1 VALUES (NULL); SELECT * FROM t1"
echo "AUTO_INCREMENT before restart: $(autoinc t1)"
restart_server || exit 1
echo "AUTO_INCREMENT after restart: $(autoinc t1)"
sql "INSERT INTO t1 VALUES (NULL)"; A_RC=$?
sql "SELECT * FROM t1"

echo
echo "=== Case B: export t2 (ids 1..3), DROP, CREATE fresh t2, DISCARD + IMPORT with .cfg ==="
sql "CREATE TABLE t2 (i SERIAL); INSERT INTO t2 VALUES (NULL),(NULL),(NULL)"
"$MYSQL" -uroot -S "$SOCK" test <<EOF
FLUSH TABLE t2 FOR EXPORT;
system cp $D/t2.ibd $RUN/t2.ibd_back
system cp $D/t2.cfg $RUN/t2.cfg_back
UNLOCK TABLES;
EOF
sql "DROP TABLE t2; CREATE TABLE t2 (i SERIAL); ALTER TABLE t2 DISCARD TABLESPACE"
cp $RUN/t2.ibd_back $D/t2.ibd; cp $RUN/t2.cfg_back $D/t2.cfg
sql "ALTER TABLE t2 IMPORT TABLESPACE"
echo "AUTO_INCREMENT before restart: $(autoinc t2)"
restart_server || exit 1
echo "AUTO_INCREMENT after restart: $(autoinc t2)"
sql "INSERT INTO t2 VALUES (NULL)"; B_RC=$?
sql "SELECT * FROM t2"

echo
echo "=== Case C: like B, but the fresh target table took rows before DISCARD ==="
sql "CREATE TABLE t3 (i SERIAL); INSERT INTO t3 VALUES (NULL),(NULL),(NULL),(NULL),(NULL)"
"$MYSQL" -uroot -S "$SOCK" test <<EOF
FLUSH TABLE t3 FOR EXPORT;
system cp $D/t3.ibd $RUN/t3.ibd_back
system cp $D/t3.cfg $RUN/t3.cfg_back
UNLOCK TABLES;
EOF
sql "DROP TABLE t3; CREATE TABLE t3 (i SERIAL); INSERT INTO t3 VALUES (NULL); DELETE FROM t3; ALTER TABLE t3 DISCARD TABLESPACE"
cp $RUN/t3.ibd_back $D/t3.ibd; cp $RUN/t3.cfg_back $D/t3.cfg
sql "ALTER TABLE t3 IMPORT TABLESPACE"
echo "AUTO_INCREMENT before restart: $(autoinc t3)"
restart_server || exit 1
echo "AUTO_INCREMENT after restart: $(autoinc t3)"
sql "INSERT INTO t3 VALUES (NULL)"; C_RC=$?
sql "SELECT * FROM t3"

echo
echo "=== Case D: same as B but INSERT before any restart (control) ==="
sql "CREATE TABLE t4 (i SERIAL); INSERT INTO t4 VALUES (NULL),(NULL),(NULL)"
"$MYSQL" -uroot -S "$SOCK" test <<EOF
FLUSH TABLE t4 FOR EXPORT;
system cp $D/t4.ibd $RUN/t4.ibd_back
system cp $D/t4.cfg $RUN/t4.cfg_back
UNLOCK TABLES;
EOF
sql "DROP TABLE t4; CREATE TABLE t4 (i SERIAL); ALTER TABLE t4 DISCARD TABLESPACE"
cp $RUN/t4.ibd_back $D/t4.ibd; cp $RUN/t4.cfg_back $D/t4.cfg
sql "ALTER TABLE t4 IMPORT TABLESPACE"
sql "INSERT INTO t4 VALUES (NULL)"; D_RC=$?
sql "SELECT * FROM t4"

echo
echo "EXPECTED: each INSERT gets the next id (A: 3, B: 4, C: 6, D: 4), no duplicate-key error."
echo "ACTUAL:   A insert rc=$A_RC, B insert rc=$B_RC, C insert rc=$C_RC, D (no restart) rc=$D_RC (non-zero = error, see above)"
stop_server
