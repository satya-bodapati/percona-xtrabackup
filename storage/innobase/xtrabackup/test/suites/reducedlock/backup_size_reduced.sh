############################################################################
# Test backup_size reporting with --lock-ddl=reduced and concurrent TRUNCATE.
#
# Uses debug sync to pause backup, TRUNCATE a table (creating a 0-size
# tablespace scenario), then resume.  Verifies reduced-lock + 0-size
# tablespaces still produce a byte-perfect backup_size.
#
# Requires debug build (debug sync support).
############################################################################

. inc/common.sh

require_debug_pxb_version

# get_field / assert_target_strict are sourced from inc/common.sh.

start_server

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.t1 (id INT PRIMARY KEY AUTO_INCREMENT, data TEXT) ENGINE=InnoDB;" test
for i in $(seq 1 200) ; do
  echo "INSERT INTO test.t1 (data) VALUES (REPEAT(UUID(), 5));"
done | $MYSQL $MYSQL_ARGS test

$MYSQL $MYSQL_ARGS -Ns -e "CREATE TABLE test.t2 (id INT PRIMARY KEY AUTO_INCREMENT, val INT) ENGINE=InnoDB;" test
for i in $(seq 1 100) ; do
  echo "INSERT INTO test.t2 (val) VALUES (FLOOR(RAND() * 1000));"
done | $MYSQL $MYSQL_ARGS test

innodb_wait_for_flush_all

############################################################################
# Backup with reduced lock + TRUNCATE during backup (creates 0-size file)
############################################################################
vlog "=== Reduced lock backup with concurrent TRUNCATE ==="

xtrabackup --backup --target-dir=$topdir/backup_reduced \
  --extra-lsndir=$topdir/lsndir_reduced \
  --debug-sync="xtrabackup_load_tablespaces_pause" --lock-ddl=REDUCED \
  2> >( tee $topdir/backup_reduced.log)&

job_pid=$!
pid_file=$topdir/backup_reduced/xtrabackup_debug_sync
wait_for_xb_to_suspend $pid_file
xb_pid=`cat $pid_file`
vlog "backup pid is $job_pid"

$MYSQL $MYSQL_ARGS -Ns -e "TRUNCATE TABLE test.t2;" test

vlog "Resuming xtrabackup"
kill -SIGCONT $xb_pid
run_cmd wait $job_pid

bs=$(get_field "$topdir/lsndir_reduced/xtrabackup_info" backup_size)

if grep -q "^uncompressed_backup_size = " "$topdir/lsndir_reduced/xtrabackup_info" ; then
  die "uncompressed_backup_size should not be present without --compress"
fi

grep -q "Backup size:" $topdir/backup_reduced.log || die "Expected 'Backup size:' in log"

assert_target_strict "$topdir/backup_reduced" "$bs" "reduced-lock"

xtrabackup --prepare --target-dir=$topdir/backup_reduced
record_db_state test
stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_reduced
start_server
verify_db_state test

vlog "Reduced lock backup_size test passed."
