############################################################################
# PXB-3804 / Issue 6 (general coverage for the issue-5 fix)
#
# Issue 5 (SDI root page) is one instance of a GENERAL vulnerability: any
# index page with a corrupt record header (invalid REC_STATUS) makes
# --check-tables abort in rec_get_offsets() (rec.cc default: ut_error).
# Issue 5 hits it via the SDI/dictionary-load scan; this test hits the SAME
# crash via the ordinary user-index validation path:
#
#   check_tables_thread_func -> btr_validate_index -> btr_validate_level
#   -> page_validate() -> rec_get_offsets()   (rec.cc ut_error)
#
# Trigger: smash the infimum system-record header of a USER leaf page.
#   The 8-byte write at offset 92 of a leaf page covers bytes 94-98 (the
#   infimum record's 5-byte header, incl. the REC_STATUS byte at offset 96).
#
# Expected (after fix):
#   - "is corrupted" / "Table check failed", non-zero exit, no crash.
# Actual (before fix):
#   - InnoDB: Assertion failure: rec.cc:385 ; mysqld got signal 6
#
# This is not a separately-filed JIRA issue; it is regression coverage proving
# the rec_validate_page_chain() gate protects the general index path, not just
# the SDI path of issue 5.
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
  die "Issue 6: $IBD is only $fsize bytes; need >= $need (not enough rows for page $page_no)"

vlog "Smash infimum record header (offset 92, 8 bytes) of user leaf page $page_no"
# 8-byte write covering the infimum header (REC_STATUS byte at offset 96)
printf '\x00\x00\x00\x00\x00\x01\x86\x9F' | \
  dd of="$IBD" bs=1 seek=$(( page_size * page_no + 92 )) count=8 conv=notrunc

vlog "Prepare with --check-tables: must detect corruption gracefully, not crash"
run_cmd_expect_failure $XB_BIN $XB_ARGS --prepare --check-tables \
  --innodb-checksum-algorithm=none \
  --target-dir=$topdir/backup 2>&1 | tee $topdir/check.log

if grep -qiE "Assertion failure|got signal|intentionally generate a memory trap" \
     $topdir/check.log; then
  die "Issue 6: xtrabackup CRASHED on a corrupt user-page record header instead of reporting corruption (PXB-3804)"
fi

grep -q "Starting table checks" $topdir/check.log || \
  die "Issue 6: check-tables did not start"
grep -qiE "invalid record chain|is corrupted" $topdir/check.log || \
  die "Issue 6: corruption not reported"
grep -q "Table check failed" $topdir/check.log || \
  die "Issue 6: 'Table check failed' message not found"

vlog "Issue 6 passed: corrupt user-page record header reported gracefully, no crash"
