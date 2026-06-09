############################################################################
# PXB-3804 : --check-tables must report (not crash, not silently pass) a page
# whose FIL_PAGE_TYPE has been masked to FIL_PAGE_TYPE_ALLOCATED (0) -- at
# EVERY B-tree level: leaf (level 0), middle/non-leaf (level 1), and the
# upper levels (level 2 ...).
#
# Issue 7 (pxb_3804_issue7.sh) only covered the ROOT page.  The remaining
# levels behave differently without a per-page guard:
#   - a corrupt internal (non-leaf) page aborts the father-pointer search in
#     btr_cur_search_to_nth_level()  (ut_ad(fil_page_index_page_check(page))),
#   - a corrupt leaf SILENTLY PASSES (nothing else checks a leaf's page type).
#
# This verifies the per-page fil_page_index_page_check() gate in
# btr_validate_level() handles all levels gracefully.
#
# A wide PRIMARY KEY + wide row force a multi-level (>=3) clustered B-tree so
# that a genuine non-root, non-leaf middle page exists.  A small Python helper
# locates a clustered-index page at each level; the page's FIL_PAGE_TYPE is
# then set to 0 and --check-tables is run (with --innodb-checksum-algorithm=
# none so the page is accepted past the checksum layer).
############################################################################

. inc/common.sh

require_debug_pxb_version

start_server --innodb_file_per_table

vlog "Create a wide-key/wide-row table to force a >=3-level clustered B-tree"
mysql test <<'EOF'
SET SESSION cte_max_recursion_depth = 100000;
CREATE TABLE t (k VARCHAR(255) NOT NULL PRIMARY KEY, pad VARCHAR(4000) NOT NULL)
  ROW_FORMAT=DYNAMIC;
INSERT INTO t
WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 6000)
SELECT LPAD(n, 250, '0'), REPEAT('x', 4000) FROM seq;
EOF

vlog "Full backup + apply-log-only prepare"
xtrabackup --backup --target-dir=$topdir/backup
xtrabackup --prepare --apply-log-only --target-dir=$topdir/backup

IBD=$topdir/backup/test/t.ibd

vlog "Locate a clustered-index page at each level (0=leaf, 1=middle, 2=upper)"
LEVELS=$(python3 - "$IBD" <<'PYEOF'
import sys, struct
PAGE = 16384
FIL_PAGE_TYPE = 24
FIL_PAGE_INDEX = 0x45bf
PAGE_HEADER = 38
PAGE_LEVEL = 26      # within PAGE_HEADER
PAGE_INDEX_ID = 28   # within PAGE_HEADER
data = open(sys.argv[1], 'rb').read()
pages = []
for i in range(len(data) // PAGE):
    off = i * PAGE
    if struct.unpack_from('>H', data, off + FIL_PAGE_TYPE)[0] != FIL_PAGE_INDEX:
        continue
    level = struct.unpack_from('>H', data, off + PAGE_HEADER + PAGE_LEVEL)[0]
    idx = struct.unpack_from('>Q', data, off + PAGE_HEADER + PAGE_INDEX_ID)[0]
    pages.append((i, level, idx))
maxlevel = max(p[1] for p in pages)
# clustered index = the index whose root has the maximum level
clid = [p[2] for p in pages if p[1] == maxlevel][0]
sel = {}
for i, level, idx in pages:
    if idx == clid:
        sel.setdefault(level, i)
print("MAXLEVEL=%d L0=%s L1=%s L2=%s" %
      (maxlevel, sel.get(0, 'NONE'), sel.get(1, 'NONE'), sel.get(2, 'NONE')))
PYEOF
)
vlog "B-tree layout: $LEVELS"
eval "$LEVELS"

[ "$MAXLEVEL" -ge 2 ] || \
  die "clustered B-tree is only $((MAXLEVEL + 1)) levels deep ($LEVELS); need >=3 (increase row count)"
for v in "$L0" "$L1" "$L2"; do
  [ "$v" != "NONE" ] || die "could not find a page at some level: $LEVELS"
done
vlog "Using pages: leaf(level0)=$L0  middle(level1)=$L1  upper(level2)=$L2"

corrupt_and_check() {
  local level=$1 page=$2
  vlog "=== Level $level: corrupt FIL_PAGE_TYPE -> 0 (ALLOCATED) on page $page ==="
  rm -rf $topdir/b_$level
  cp -r $topdir/backup $topdir/b_$level
  # FIL_PAGE_TYPE is 2 bytes at offset 24; set to 0 (FIL_PAGE_TYPE_ALLOCATED)
  printf '\x00\x00' | dd of=$topdir/b_$level/test/t.ibd bs=1 \
    seek=$(( page * 16384 + 24 )) count=2 conv=notrunc

  run_cmd_expect_failure $XB_BIN $XB_ARGS --prepare --check-tables \
    --innodb-checksum-algorithm=none \
    --target-dir=$topdir/b_$level 2>&1 | tee $topdir/check_$level.log

  if grep -qiE "Assertion failure|got signal|intentionally generate a memory trap" \
       $topdir/check_$level.log; then
    die "Level $level: xtrabackup CRASHED on a non-index FIL_PAGE_TYPE (PXB-3804)"
  fi
  grep -q "Starting table checks" $topdir/check_$level.log || \
    die "Level $level: check-tables did not start"
  grep -qiE "non-index FIL_PAGE_TYPE|is corrupted" $topdir/check_$level.log || \
    die "Level $level: corruption not reported (silent pass?)"
  grep -q "Table check failed" $topdir/check_$level.log || \
    die "Level $level: 'Table check failed' message not found"
  vlog "Level $level passed: non-index FIL_PAGE_TYPE reported gracefully, no crash"
}

corrupt_and_check 0 "$L0"   # leaf
corrupt_and_check 1 "$L1"   # middle / non-leaf, non-root
corrupt_and_check 2 "$L2"   # upper level (root if 3-level tree)

vlog "All levels (0=leaf, 1=middle, 2=upper) reported gracefully, no crash"
