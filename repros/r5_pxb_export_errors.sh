#!/bin/bash
# R5: xtrabackup --prepare --export ignores .cfp and .cfg write errors and still exits 0.
#  - .cfp: the loop tests `if (!xb_export_cfp_write(table))` (xtrabackup.cc ~7565), but the function returns a
#    dberr_t where DB_SUCCESS is 10, so every error is non-zero, "!err" is false, and nothing is reported.
#  - .cfg: on failure the loop does `goto next_node`, which is the very next statement, and no error is
#    recorded, so the exit code stays 0 as well.
# Failure is forced by pre-creating <table>.cfp / <table>.cfg as DIRECTORIES in the backup, so fopen() fails.
# Server: ./server (PS 8.4.11 debug tarball). xtrabackup: an existing 8.4.11 build (read-only use).
. "$(dirname "$0")/common.sh"
XB=${XB:-$HOME/WORK/pxb/pxb-8.4.11/bld_rel/runtime_output_directory/xtrabackup}
PLUGIN_DIR=$BASEDIR/lib/plugin
setup_instance r5_pxb_export
init_datadir
# Keyring: the global manifest server/bin/mysqld.my says "read_local_manifest", so the component is loaded
# only for this datadir (its own mysqld.my). The component config must be the global one in lib/plugin
# (a local-only config leaves the component disabled); it is written here and removed at the end.
printf '{ "read_local_manifest": true }\n' > $BASEDIR/bin/mysqld.my
printf '{ "components": "file://component_keyring_file" }\n' > $DATADIR/mysqld.my
printf '{ "path": "%s", "read_only": false }\n' "$RUN/keyring" > $RUN/component_keyring_file.cnf
cp $RUN/component_keyring_file.cnf $PLUGIN_DIR/component_keyring_file.cnf
trap 'stop_server; rm -f $PLUGIN_DIR/component_keyring_file.cnf' EXIT
KEYRING_ARGS="--component-keyring-config=$RUN/component_keyring_file.cnf --xtrabackup-plugin-dir=$PLUGIN_DIR"
start_server || exit 1
sql "SELECT * FROM performance_schema.keyring_component_status"
sql "CREATE TABLE t1 (c1 INT) ENCRYPTION='Y'; INSERT INTO t1 VALUES (1),(2),(3);
     CREATE TABLE t2 (c1 INT) ENCRYPTION='Y'; INSERT INTO t2 VALUES (4),(5),(6);
     CREATE TABLE t3 (c1 INT) ENCRYPTION='Y'; INSERT INTO t3 VALUES (7),(8),(9)"

B=$RUN/backup
echo "=== backup ==="
"$XB" --no-defaults --backup --target-dir=$B -uroot --socket=$SOCK $KEYRING_ARGS > $REPRO_DIR/logs/r5_backup.log 2>&1
echo "backup exit code: $?"

# Make the .cfp of t1 and the .cfg of t2 impossible to create. t3 is the control.
mkdir $B/test/t1.cfp $B/test/t2.cfg
echo "=== prepare --export (t1.cfp and t2.cfg pre-created as directories) ==="
"$XB" --no-defaults --prepare --export --target-dir=$B $KEYRING_ARGS > $REPRO_DIR/logs/r5_prepare.log 2>&1
PREP_RC=$?
echo "prepare --export exit code: $PREP_RC"
grep -n "t1.cfp\|t2.cfg\|cannot open\|Can't create\|error" $REPRO_DIR/logs/r5_prepare.log | grep -iv "error_log\|log_error\|errors=" | head -10
echo "export files in backup/test:"
for f in t1 t2 t3; do for e in cfg cfp; do
  p=$B/test/$f.$e
  if [ -d $p ]; then s="(directory we created)"; elif [ -f $p ]; then s="file"; else s="missing"; fi
  echo "  $f.$e: $s"
done; done

echo "=== consequence: import t1 using the backup's t1.ibd + t1.cfg (no .cfp was written) ==="
sql "DROP TABLE t1; CREATE TABLE t1 (c1 INT) ENCRYPTION='Y'; ALTER TABLE t1 DISCARD TABLESPACE"
cp $B/test/t1.ibd $B/test/t1.cfg $DATADIR/test/
sql "ALTER TABLE t1 IMPORT TABLESPACE"; IMP_RC=$?

echo
echo "EXPECTED: prepare --export fails (non-zero exit) because t1.cfp and t2.cfg could not be written."
echo "ACTUAL:   prepare --export exit code=$PREP_RC; import of t1 without its .cfp rc=$IMP_RC (see above)."
stop_server
