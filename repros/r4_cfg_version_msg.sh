#!/bin/bash
# R4: ER_IMP_INCOMPATIBLE_CFG_VERSION names the wrong "current version". row_import_read_meta_data()
# (row0import.cc ~4204) passes IB_EXPORT_CFG_VERSION_V5 as the current version, but the server writes and
# reads V7. Shown by importing a real .cfg whose version field (first 4 bytes, big-endian) is set to 8.
. "$(dirname "$0")/common.sh"
setup_instance r4_cfg_version
init_datadir
start_server || exit 1
D=$DATADIR/test

sql "CREATE TABLE t1 (a INT PRIMARY KEY); INSERT INTO t1 VALUES (1),(2)"
"$MYSQL" -uroot -S "$SOCK" test <<EOF
FLUSH TABLE t1 FOR EXPORT;
system cp $D/t1.ibd $RUN/t1.ibd_back
system cp $D/t1.cfg $RUN/t1.cfg_back
UNLOCK TABLES;
EOF
echo "version written by FLUSH FOR EXPORT: $(od -An -tx1 -N4 $RUN/t1.cfg_back)"
printf '\x00\x00\x00\x08' | dd of=$RUN/t1.cfg_back bs=1 count=4 conv=notrunc status=none
echo "version after patch:                  $(od -An -tx1 -N4 $RUN/t1.cfg_back)"
sql "ALTER TABLE t1 DISCARD TABLESPACE"
cp $RUN/t1.ibd_back $D/t1.ibd; cp $RUN/t1.cfg_back $D/t1.cfg
OUT=$(sql "ALTER TABLE t1 IMPORT TABLESPACE"); echo "$OUT"

echo
echo "EXPECTED: error says the CFG file version (8) is not compatible with the current version (7)."
echo "ACTUAL:   $(echo "$OUT" | grep -o 'Failed to import.*')"
stop_server
