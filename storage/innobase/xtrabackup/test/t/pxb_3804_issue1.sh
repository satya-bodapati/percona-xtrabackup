############################################################################
# PXB-3804 / Issue 1 (CRITICAL)
#
# xtrabackup --prepare --check-tables crashes inside btr_validate_level()
# when a leaf page has a broken sibling link (FIL_PAGE_PREV corrupted).
#
#   Location : btr0btr.cc:4402  btr_validate_level()
#   Trigger  : FIL_PAGE_PREV (offset 8) on a leaf page set to 0xDEADBEEF
#
# Expected (after fix):
#   - "broken FIL_PAGE_NEXT or FIL_PAGE_PREV" logged
#   - index reported "is corrupted", "Table check failed"
#   - non-zero exit, clean InnoDB shutdown (NO assertion / signal 6)
#
# Actual (bug):
#   InnoDB: Assertion failure: btr0btr.cc:4402:siblings_link_correct
#   mysqld got signal 6
#
# --innodb-checksum-algorithm=none is required so the corrupted page is
# accepted by the buffer pool and reaches the --check-tables validation
# path (mirrors the original reproduction).
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
page_no=10

fsize=$(stat -c %s "$IBD")
need=$(( (page_no + 1) * page_size ))
[ "$fsize" -ge "$need" ] || \
  die "Issue 1: $IBD is only $fsize bytes; need >= $need (not enough rows for page $page_no)"

vlog "Corrupt FIL_PAGE_PREV (offset 8) of page $page_no to 0xDEADBEEF"
printf '\xDE\xAD\xBE\xEF' | \
  dd of="$IBD" bs=1 seek=$(( page_size * page_no + 8 )) count=4 conv=notrunc

vlog "Prepare with --check-tables: must detect corruption gracefully, not crash"
run_cmd_expect_failure $XB_BIN $XB_ARGS --prepare --check-tables \
  --innodb-checksum-algorithm=none \
  --target-dir=$topdir/backup 2>&1 | tee $topdir/check.log

if grep -qiE "Assertion failure|got signal|intentionally generate a memory trap" \
     $topdir/check.log; then
  die "Issue 1: xtrabackup CRASHED on broken FIL_PAGE_PREV instead of reporting corruption (PXB-3804)"
fi

grep -q "Starting table checks" $topdir/check.log || \
  die "Issue 1: check-tables did not start"
grep -q "is corrupted" $topdir/check.log || \
  die "Issue 1: corruption not reported"
grep -q "Table check failed" $topdir/check.log || \
  die "Issue 1: 'Table check failed' message not found"

vlog "Issue 1 passed: broken sibling link reported gracefully, no crash"
