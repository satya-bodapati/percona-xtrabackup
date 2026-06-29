#
# PXB-2865 release reproducer: AUTO_INC + DELETE all rows + restore.
#
# WL#7806's MAX(id)+1 recompute is the recovery's safety net for
# AUTO_INC. It says: after restore, autoinc = max(MAX(id)+1, persisted).
# This masks the bug while there's data in the table — MAX(id)+1
# always >= the right value.
#
# Empty table breaks this safety net. MAX(id) = 0, so the recompute
# is max(1, persisted). If persisted regressed (due to PXB-2865),
# the next INSERT gets a SMALLER id than the donor would have given.
#

. inc/common.sh

[ -n "$INNODB_VERSION" ] || skip_test "Requires InnoDB plugin or XtraDB"

function test_pxb_2865_release_delete()
{
  # Suppress page-cleaner so mysql.ibd dynamic_metadata pages stay
  # dirty in BP between backups.
  mysqld_additional_args="\
    --innodb_buffer_pool_size=1G \
    --innodb_redo_log_capacity=1G \
    --innodb_max_dirty_pages_pct=99 \
    --innodb_max_dirty_pages_pct_lwm=99 \
    --innodb_adaptive_flushing=OFF \
    --innodb_adaptive_flushing_lwm=99 \
    --innodb_io_capacity=1 \
    --innodb_io_capacity_max=1 \
    --innodb_flush_method=O_DIRECT \
    --innodb_buffer_pool_dump_at_shutdown=OFF \
    --innodb_buffer_pool_load_at_startup=OFF \
    --innodb_flush_neighbors=0 \
    --innodb_idle_flush_pct=0 \
    --innodb_purge_rseg_truncate_frequency=1 \
    --innodb_file_per_table \
    --innodb_strict_mode"

  start_server ${mysqld_additional_args}
  ${MYSQL} ${MYSQL_ARGS} -e "CREATE DATABASE pxb2865"

  rm -rf "$topdir/full" "$topdir/inc"*
  mkdir -p "$topdir/full"

  ${MYSQL} ${MYSQL_ARGS} -e \
    "CREATE TABLE t (id BIGINT AUTO_INCREMENT PRIMARY KEY, v INT) \
     ENGINE=InnoDB" pxb2865

  # Some initial rows so the AUTO_INC starts well above 1.
  for r in $(seq 1 5); do
    ${MYSQL} ${MYSQL_ARGS} -e \
      "INSERT INTO t (v) VALUES ($r)" pxb2865
  done

  vlog "Full backup (AUTO_INC ~= 6)"
  xtrabackup --datadir=$mysql_datadir --backup --target-dir="$topdir/full"

  # Now hammer AUTO_INC with many inserts -- ensures MLOG_TABLE_DYNAMIC_META
  # records emit for the bumps. Then delete all rows so MAX(id) = 0 on
  # the restored side.
  PREV="$topdir/full"
  for n in $(seq 1 5); do
    vlog "Incremental $n: 500 inserts then DELETE-ALL"
    for r in $(seq 1 500); do
      ${MYSQL} ${MYSQL_ARGS} -e \
        "INSERT INTO t (v) VALUES ($r)" pxb2865
    done
    # NOTE: Do NOT call FLUSH or anything else that might trigger a
    # checkpoint. We want the donor's mysql.ibd to stay stale.
    ${MYSQL} ${MYSQL_ARGS} -e "DELETE FROM t" pxb2865

    INC="$topdir/inc${n}"
    mkdir -p "$INC"
    xtrabackup --datadir=$mysql_datadir --backup \
      --target-dir="$INC" --incremental-basedir="$PREV"
    PREV="$INC"
  done

  # Pre-restore in-memory AUTO_INC: 5 (initial) + 5*500 = 2505+ ish.
  expected_min=$(${MYSQL} ${MYSQL_ARGS} -Ns -e \
    "SELECT AUTO_INCREMENT FROM information_schema.tables \
     WHERE table_schema='pxb2865' AND table_name='t'")
  vlog "Pre-restore AUTO_INC for t: $expected_min"

  vlog "Prepare chain"
  xtrabackup --datadir=$mysql_datadir --prepare --apply-log-only \
    --target-dir="$topdir/full"
  for n in $(seq 1 5); do
    xtrabackup --datadir=$mysql_datadir --prepare --apply-log-only \
      --target-dir="$topdir/full" --incremental-dir="$topdir/inc${n}"
  done
  xtrabackup --datadir=$mysql_datadir --prepare --target-dir="$topdir/full"

  stop_server
  rm -rf $mysql_datadir
  xtrabackup --datadir=$mysql_datadir --copy-back \
    --target-dir="$topdir/full" $mysqld_additional_args

  start_server ${mysqld_additional_args}

  # After restore, table is empty (DELETEd all rows). MAX(id) = 0.
  # mysqld will use AUTO_INC = max(0+1, persisted). If persisted
  # regressed (PXB-2865 active), id will be SMALL.
  ${MYSQL} ${MYSQL_ARGS} -e "INSERT INTO t (v) VALUES (999)" pxb2865
  post_id=$(${MYSQL} ${MYSQL_ARGS} -Ns -e \
    "SELECT id FROM pxb2865.t WHERE v=999")
  vlog "Post-restore: new INSERT got id=$post_id (pre-restore AUTO_INC was ~$expected_min)"

  # If the bug regressed the persistent counter, id will be << expected_min.
  if [ "$post_id" -lt "$expected_min" ]; then
    vlog "PXB-2865 REPRODUCED: post-restore id=$post_id regressed below pre-restore $expected_min"
    exit -1
  fi

  vlog "PXB-2865 reproducer PASSED (no AUTO_INC regression)"
  stop_server
}

test_pxb_2865_release_delete
