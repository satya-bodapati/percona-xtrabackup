############################################################################
# PXB-3804 / Issue 3 (HIGH)
#
# xtrabackup --prepare --check-tables crashes in fseg_inode_get() when an
# index root page carries a nonsensical PAGE_LEVEL.
#
#   Location : fsp0fsp.cc:2181  fseg_inode_get()  (ut_a(inode))
#   Trigger  : PAGE_LEVEL (offset 90) on the root page set to 99
#
# Expected (after fix):
#   - index reported "is corrupted", "Table check failed"
#   - non-zero exit, clean shutdown (NO assertion / signal 6)
#
# Actual (bug):
#   InnoDB: Assertion failure: fsp0fsp.cc:2181:inode
#   mysqld got signal 6
#
# --innodb-checksum-algorithm=none is required so the corrupted page is
# accepted by the buffer pool and reaches the --check-tables path.
############################################################################

. inc/common.sh

require_debug_pxb_version

start_server --innodb_file_per_table

vlog "Create test_users and populate 10000 rows (multi-page B-tree)"
mysql test <<'EOF'
SET SESSION cte_max_recursion_depth = 20000;
CREATE TABLE test_users (id INT PRIMARY KEY, name VARCHAR(100));
INSERT INTO test_users
WITH RECURSIVE seq(n) AS (
  SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 10000
)
SELECT n, CONCAT('user', n) FROM seq;
EOF

vlog "Full backup"
xtrabackup --backup --target-dir=$topdir/backup

vlog "Prepare with --apply-log-only (leave the backup re-preparable)"
xtrabackup --prepare --apply-log-only --target-dir=$topdir/backup

IBD=$topdir/backup/test/test_users.ibd
page_size=16384
page_no=4

fsize=$(stat -c %s "$IBD")
need=$(( (page_no + 1) * page_size ))
[ "$fsize" -ge "$need" ] || \
  die "Issue 3: $IBD is only $fsize bytes; need >= $need"

vlog "Corrupt PAGE_LEVEL (offset 90) of page $page_no to 99"
# 99 as a big-endian 2-byte value = 0x0063
printf '\x00\x63' | \
  dd of="$IBD" bs=1 seek=$(( page_size * page_no + 90 )) count=2 conv=notrunc

vlog "Prepare with --check-tables: must detect corruption gracefully, not crash"
run_cmd_expect_failure $XB_BIN $XB_ARGS --prepare --check-tables \
  --innodb-checksum-algorithm=none \
  --target-dir=$topdir/backup 2>&1 | tee $topdir/check.log

if grep -qiE "Assertion failure|got signal|intentionally generate a memory trap" \
     $topdir/check.log; then
  die "Issue 3: xtrabackup CRASHED on bad PAGE_LEVEL instead of reporting corruption (PXB-3804)"
fi

grep -q "Starting table checks" $topdir/check.log || \
  die "Issue 3: check-tables did not start"
grep -q "is corrupted" $topdir/check.log || \
  die "Issue 3: corruption not reported"
grep -q "Table check failed" $topdir/check.log || \
  die "Issue 3: 'Table check failed' message not found"

vlog "Issue 3 passed: bad PAGE_LEVEL reported gracefully, no crash"
