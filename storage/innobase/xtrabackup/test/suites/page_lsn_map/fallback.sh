############################################################################
# Page LSN map: every way the map can be unusable must degrade to today's
# behaviour, never abort the prepare.
#
# The fallback is what makes the feature safe to ship.  Absence of a map, a
# destroyed trailer, an empty file or a corrupt block all mean "read those
# pages the old way".  If any of them made prepare fail, the map would turn a
# recoverable backup into an unrecoverable one, which is a far worse outcome
# than the slow prepare it was meant to avoid.
############################################################################

. inc/common.sh

start_server

$MYSQL $MYSQL_ARGS test <<SQL
CREATE TABLE t1 (a INT PRIMARY KEY AUTO_INCREMENT, b VARCHAR(200));
SQL
multi_row_insert test.t1 \( NULL, REPEAT\(\'b\',200\) \) 1 5000

xtrabackup --backup --page-lsn-map --target-dir=$topdir/bk
[ -f $topdir/bk/xtrabackup_page_lsn ] || die "no map written"

vlog "1. map absent"
cp -a $topdir/bk $topdir/c1 && rm -f $topdir/c1/xtrabackup_page_lsn
xtrabackup --prepare --page-lsn-map --target-dir=$topdir/c1

vlog "2. trailer destroyed (file truncated)"
cp -a $topdir/bk $topdir/c2
truncate -s -8 $topdir/c2/xtrabackup_page_lsn
xtrabackup --prepare --page-lsn-map --target-dir=$topdir/c2

vlog "3. map is empty"
cp -a $topdir/bk $topdir/c3
: > $topdir/c3/xtrabackup_page_lsn
xtrabackup --prepare --page-lsn-map --target-dir=$topdir/c3

vlog "4. first block body corrupted, so its CRC fails"
cp -a $topdir/bk $topdir/c4
printf 'XXXXXXXX' | dd of=$topdir/c4/xtrabackup_page_lsn bs=1 seek=48 \
    conv=notrunc 2>/dev/null
xtrabackup --prepare --page-lsn-map --target-dir=$topdir/c4

vlog "5. a map is present but never asked for"
cp -a $topdir/bk $topdir/c5
xtrabackup --prepare --target-dir=$topdir/c5

vlog "every unusable-map case prepared successfully"
