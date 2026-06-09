############################################################################
# PXB-3804 / Issue 7  (JIRA's updated "Issue 5": FIL_PAGE_TYPE corruption)
#
# xtrabackup --prepare --check-tables crashes when an index page's
# FIL_PAGE_TYPE is corrupted from FIL_PAGE_INDEX (0x45BF) to
# FIL_PAGE_TYPE_ALLOCATED (0x0000).  During level validation the
# father-pointer search descends into the (now non-index) root page and
# trips the page-type assertion:
#
#   InnoDB: Assertion failure: btr0cur.cc:1099:fil_page_index_page_check(page)
#   mysqld got signal 6
#
#   btr_cur_search_to_nth_level (btr0cur.cc:1099)
#   <- btr_page_get_father_node_ptr_func (btr0btr.cc)
#   <- btr_validate_level <- btr_validate_index <- check_tables_thread_func
#
# Trigger: FIL_PAGE_TYPE (offset 24, 2 bytes) of page 4 -- the PRIMARY index
#          root -- set to 0 (FIL_PAGE_TYPE_ALLOCATED).
#
# Expected (after fix):
#   - index reported "is corrupted", "Table check failed"
#   - non-zero exit, clean shutdown (NO assertion / signal 6)
#
# --innodb-checksum-algorithm=none is required so the corrupted page is
# accepted by the buffer pool and reaches the --check-tables path.
############################################################################

. inc/common.sh

require_debug_pxb_version

start_server --innodb_file_per_table

vlog "Create test_users and populate 10000 rows (multi-level B-tree)"
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
  die "Issue 7: $IBD is only $fsize bytes; need >= $need"

vlog "Corrupt FIL_PAGE_TYPE (offset 24) of root page $page_no: INDEX -> ALLOCATED(0)"
# FIL_PAGE_TYPE_ALLOCATED = 0 as a big-endian 2-byte value
printf '\x00\x00' | \
  dd of="$IBD" bs=1 seek=$(( page_size * page_no + 24 )) count=2 conv=notrunc

vlog "Prepare with --check-tables: must detect corruption gracefully, not crash"
run_cmd_expect_failure $XB_BIN $XB_ARGS --prepare --check-tables \
  --innodb-checksum-algorithm=none \
  --target-dir=$topdir/backup 2>&1 | tee $topdir/check.log

if grep -qiE "Assertion failure|got signal|intentionally generate a memory trap" \
     $topdir/check.log; then
  die "Issue 7: xtrabackup CRASHED on a non-index FIL_PAGE_TYPE instead of reporting corruption (PXB-3804)"
fi

grep -q "Starting table checks" $topdir/check.log || \
  die "Issue 7: check-tables did not start"
grep -qiE "non-index FIL_PAGE_TYPE|is corrupted" $topdir/check.log || \
  die "Issue 7: corruption not reported"
grep -q "Table check failed" $topdir/check.log || \
  die "Issue 7: 'Table check failed' message not found"

vlog "Issue 7 passed: non-index FIL_PAGE_TYPE reported gracefully, no crash"
