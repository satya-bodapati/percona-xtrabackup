############################################################################
# Page LSN map: preparing with the map must produce exactly the same datadir
# as preparing without it.
#
# This is the only test that catches the single unsafe failure mode of the
# feature.  A recorded LSN that is too LOW is harmless -- redundant records
# get applied and the check inside apply discards them.  A recorded LSN that
# is too HIGH makes prepare skip a record that was needed, leaving a stale
# page that still passes its own checksum: nothing downstream detects it, and
# it surfaces much later as unexplained corruption.
#
# So: take one backup with --page-lsn-map, prepare two copies of it -- one
# consulting the map, one ignoring it -- and compare the two prepared
# datadirs byte for byte.  Applying the same records in the same order must
# give identical pages, so any difference is the bug.
#
# --use-memory is deliberately small so recovery runs several apply batches.
# That is where the map does most of its work, and also where an ordering
# mistake would show up.
############################################################################

. inc/common.sh

start_server

vlog "Load a table, then flush it fully, so the pages the backup copies carry"
vlog "LSNs from BEFORE the write workload below"
$MYSQL $MYSQL_ARGS test <<SQL
CREATE TABLE t1 (a INT PRIMARY KEY AUTO_INCREMENT, b VARCHAR(200), c INT, KEY(c));
SQL
multi_row_insert test.t1 \( NULL, REPEAT\(\'a\',200\), FLOOR\(RAND\(\)*1000\) \) 1 20000
$MYSQL $MYSQL_ARGS -e "SET GLOBAL innodb_fast_shutdown = 0" test
shutdown_server
start_server

vlog "Back up with a concurrent write workload, so redo covers pages the copy"
vlog "thread has already passed over"
( for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 ; do
    $MYSQL $MYSQL_ARGS -e \
      "UPDATE t1 SET c = c + 1 WHERE a % 7 = $(( i % 7 ))" test >/dev/null 2>&1
  done ) &
WORKLOAD_PID=$!

xtrabackup --backup --page-lsn-map --target-dir=$topdir/bk

wait $WORKLOAD_PID 2>/dev/null || true

if ! [ -f $topdir/bk/xtrabackup_page_lsn ] ; then
    die "no xtrabackup_page_lsn was written"
fi
vlog "map size: $(stat -c %s $topdir/bk/xtrabackup_page_lsn) bytes"

vlog "Two identical copies of the same backup"
cp -a $topdir/bk $topdir/bk_map
cp -a $topdir/bk $topdir/bk_nomap

xtrabackup --prepare --use-memory=64M --use-page-lsn-map \
           --target-dir=$topdir/bk_map
xtrabackup --prepare --use-memory=64M --target-dir=$topdir/bk_nomap

vlog "Compare the prepared datadirs page by page"
DIFFS=0
for f in $( cd $topdir/bk_map && find . -name '*.ibd' -o -name 'ibdata1' ) ; do
    if ! cmp -s "$topdir/bk_map/$f" "$topdir/bk_nomap/$f" ; then
        vlog "MISMATCH in $f"
        cmp -l "$topdir/bk_map/$f" "$topdir/bk_nomap/$f" 2>/dev/null | head -5
        DIFFS=$(( DIFFS + 1 ))
    fi
done

if [ $DIFFS -ne 0 ] ; then
    die "$DIFFS file(s) differ between the map and no-map prepare; the map dropped a record that recovery would have applied"
fi

vlog "prepared datadirs are identical with and without the map"
