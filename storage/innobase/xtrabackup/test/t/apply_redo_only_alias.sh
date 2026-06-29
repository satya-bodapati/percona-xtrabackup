#
# Verify --apply-redo-only is accepted as a synonym for --apply-log-only,
# and that a one-time deprecation note fires only when the legacy name is
# used on the command line.
#

. inc/common.sh

mkdir -p $topdir/nonexistent_will_fail

# We don't care about the exit code -- both invocations fail because the
# target dir is empty. What we care about is the startup warning text.

vlog "Case 1: --apply-log-only -> expect the legacy-name warning"
$XB_BIN --apply-log-only --prepare \
        --target-dir=$topdir/nonexistent_will_fail >$topdir/xb1.out 2>&1 || true

if ! grep -q "apply-log-only is the legacy name" $topdir/xb1.out ; then
  vlog "FAIL: --apply-log-only did not produce the legacy-name warning"
  cat $topdir/xb1.out
  exit 1
fi

vlog "Case 2: --apply-redo-only -> warning must NOT appear"
$XB_BIN --apply-redo-only --prepare \
        --target-dir=$topdir/nonexistent_will_fail >$topdir/xb2.out 2>&1 || true

if grep -q "apply-log-only is the legacy name" $topdir/xb2.out ; then
  vlog "FAIL: --apply-redo-only unexpectedly produced the legacy-name warning"
  cat $topdir/xb2.out
  exit 1
fi

vlog "Case 3: both names appear in --help"
$XB_BIN --help >$topdir/xb_help.out 2>&1 || true
if ! grep -q -- "--apply-log-only" $topdir/xb_help.out ; then
  vlog "FAIL: --apply-log-only missing from --help"
  exit 1
fi
if ! grep -q -- "--apply-redo-only" $topdir/xb_help.out ; then
  vlog "FAIL: --apply-redo-only missing from --help"
  exit 1
fi

vlog "PASS"
