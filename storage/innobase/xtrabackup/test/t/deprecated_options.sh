#
# Verify deprecation warnings fire for options the new design has
# left behind but kept accepted for backward compatibility.  The
# options are still accepted on the command line; the warning fires
# at startup so existing customer scripts keep running.
#

. inc/common.sh

mkdir -p $topdir/will_fail

# Each invocation fails because the target dir is empty.  We only
# care about the startup-time warning text.

vlog "--log emits deprecation warning"
$XB_BIN --log --prepare --target-dir=$topdir/will_fail >$topdir/out 2>&1 || true
grep -q -- "--log is deprecated" $topdir/out \
  || { cat $topdir/out; die "expected --log deprecation warning"; }

vlog "--innodb emits deprecation warning"
$XB_BIN --innodb --prepare --target-dir=$topdir/will_fail >$topdir/out 2>&1 || true
grep -q -- "--innodb is deprecated" $topdir/out \
  || { cat $topdir/out; die "expected --innodb deprecation warning"; }

vlog "--create-ib-logfile emits deprecation warning"
$XB_BIN --create-ib-logfile --prepare --target-dir=$topdir/will_fail \
        >$topdir/out 2>&1 || true
grep -q -- "--create-ib-logfile is deprecated" $topdir/out \
  || { cat $topdir/out; die "expected --create-ib-logfile deprecation warning"; }

vlog "--rebuild-threads emits deprecation warning"
$XB_BIN --rebuild-threads=4 --prepare --target-dir=$topdir/will_fail \
        >$topdir/out 2>&1 || true
grep -q -- "--rebuild-threads is deprecated" $topdir/out \
  || { cat $topdir/out; die "expected --rebuild-threads deprecation warning"; }

vlog "--rebuild_threads (underscore form) emits deprecation warning"
$XB_BIN --rebuild_threads=4 --prepare --target-dir=$topdir/will_fail \
        >$topdir/out 2>&1 || true
grep -q -- "--rebuild-threads is deprecated" $topdir/out \
  || { cat $topdir/out; die "expected --rebuild-threads deprecation warning (underscore form)"; }

vlog "--rsync emits deprecation warning"
$XB_BIN --rsync --prepare --target-dir=$topdir/will_fail >$topdir/out 2>&1 || true
grep -q -- "--rsync is deprecated" $topdir/out \
  || { cat $topdir/out; die "expected --rsync deprecation warning"; }

vlog "--no-version-check emits deprecation warning"
$XB_BIN --no-version-check --prepare --target-dir=$topdir/will_fail \
        >$topdir/out 2>&1 || true
grep -q -- "--no-version-check is deprecated" $topdir/out \
  || { cat $topdir/out; die "expected --no-version-check deprecation warning"; }

# Negative check: a normal invocation without any of the above does
# NOT emit deprecation warnings.
vlog "no deprecated options -> no deprecation warnings"
$XB_BIN --prepare --target-dir=$topdir/will_fail >$topdir/out 2>&1 || true
if grep -q -- "is deprecated" $topdir/out ; then
  cat $topdir/out
  die "did not expect any deprecation warning here"
fi

vlog "PASS: deprecation warnings fire for all listed options"
