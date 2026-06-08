############################################################################
# PXB-3804 / Issue 2 (CRITICAL)
#
# xtrabackup --prepare --check-tables HANGS INDEFINITELY when a leaf page
# has a corrupt forward sibling link (FIL_PAGE_NEXT). InnoDB follows the
# bogus page number and tries to preallocate a ~61 PB file.
#
#   Location : sibling traversal in btr_validate_level()
#   Trigger  : FIL_PAGE_NEXT (offset 12) on a leaf page set to 0xDEADBEEF
#
# Expected (after fix):
#   - index reported "is corrupted", "Table check failed"
#   - completes within seconds, non-zero exit (NO hang, NO 61PB fallocate)
#
# Actual (bug):
#   [MY-012144] posix_fallocate(): ... desired size 61209453068288 bytes
#   process hangs forever (exit 124 under `timeout`)
#
# The check-tables invocation is wrapped in `timeout` so a regression of
# the hang fails the test deterministically instead of stalling the suite.
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
  die "Issue 2: $IBD is only $fsize bytes; need >= $need (not enough rows for page $page_no)"

vlog "Corrupt FIL_PAGE_NEXT (offset 12) of page $page_no to 0xDEADBEEF"
printf '\xDE\xAD\xBE\xEF' | \
  dd of="$IBD" bs=1 seek=$(( page_size * page_no + 12 )) count=4 conv=notrunc

CHECK_TIMEOUT=120
vlog "Prepare with --check-tables under a ${CHECK_TIMEOUT}s timeout (must not hang)"
set +e
timeout $CHECK_TIMEOUT $XB_BIN $XB_ARGS --prepare --check-tables \
  --innodb-checksum-algorithm=none \
  --target-dir=$topdir/backup 2>&1 | tee $topdir/check.log
rc=${PIPESTATUS[0]}
set -e
vlog "check-tables exit code: $rc"

if [ "$rc" -eq 124 ]; then
  die "Issue 2: --check-tables HUNG (timed out after ${CHECK_TIMEOUT}s) on corrupt FIL_PAGE_NEXT (PXB-3804)"
fi
if grep -qi "posix_fallocate" $topdir/check.log; then
  die "Issue 2: xtrabackup attempted an absurd preallocation following the corrupt FIL_PAGE_NEXT (PXB-3804)"
fi
if grep -qiE "Assertion failure|got signal|intentionally generate a memory trap" \
     $topdir/check.log; then
  die "Issue 2: xtrabackup CRASHED instead of reporting corruption (PXB-3804)"
fi
if [ "$rc" -eq 0 ]; then
  die "Issue 2: --check-tables succeeded on a corrupt tablespace"
fi

grep -q "is corrupted" $topdir/check.log || \
  die "Issue 2: corruption not reported"
grep -q "Table check failed" $topdir/check.log || \
  die "Issue 2: 'Table check failed' message not found"

vlog "Issue 2 passed: corrupt FIL_PAGE_NEXT reported gracefully, no hang"
