########################################################################
# xtrabackup --restore of encrypted tables: --restore writes the .cfp of each
# tablespace from the backup's keyring, the server imports with it, and no
# .cfp is left in the datadir afterwards.
########################################################################

require_server_version_higher_than 8.4.0

KEYRING_TYPE="component"
. inc/keyring_common.sh
. inc/keyring_file.sh
configure_server_with_component

require_xtradb

mysql -e "CREATE DATABASE db1"
mysql db1 <<EOF
CREATE TABLE enc (id INT PRIMARY KEY, a VARCHAR(100)) ENCRYPTION='Y';
INSERT INTO enc VALUES (1, 'secret'), (2, 'more secret');
CREATE TABLE encp (id INT) ENCRYPTION='Y' PARTITION BY HASH (id) PARTITIONS 2;
INSERT INTO encp VALUES (1), (2), (3);
ALTER INSTANCE ROTATE INNODB MASTER KEY;
EOF

enc_sum=$(checksum_table db1 enc)
encp_sum=$(checksum_table db1 encp)

xtrabackup --backup --target-dir=$topdir/backup $keyring_args
xtrabackup --prepare --target-dir=$topdir/backup \
  --xtrabackup-plugin-dir=${plugin_dir} $keyring_args

vlog "--dry-run reads the encrypted tables with the backup's keyring"
set +e
$XB_BIN $XB_ARGS --restore --target-dir=$topdir/backup --restore-include=db1 \
  --dry-run --xtrabackup-plugin-dir=${plugin_dir} $keyring_args \
  > $topdir/restore.log 2>&1
rc=$?
set -e
cat $topdir/restore.log
[ $rc -eq 0 ] || die "--dry-run failed"
grep -q "db1.enc: 1 file(s), encrypted" $topdir/restore.log || \
  die "enc should be listed as encrypted"
grep -q "db1.encp: 2 file(s), partitioned, encrypted" $topdir/restore.log || \
  die "encp should be listed as partitioned and encrypted"

require_import_table_ibd

mysql -e "DROP TABLE db1.enc, db1.encp"

xtrabackup --restore --target-dir=$topdir/backup --restore-include=db1 \
  --xtrabackup-plugin-dir=${plugin_dir} $keyring_args

[ "$(checksum_table db1 enc)" = "$enc_sum" ] || die "enc differs"
[ "$(checksum_table db1 encp)" = "$encp_sum" ] || die "encp differs"
mysql -Ns -e "SHOW CREATE TABLE db1.enc" | grep -q "ENCRYPTION='Y'" || \
  die "enc is not encrypted any more"
if ls $mysql_datadir/db1/*.cfp 2>/dev/null; then
  die "--restore left .cfp files with the tablespace keys in the datadir"
fi

vlog "The tables survive a restart"
stop_server
start_server
[ "$(checksum_table db1 enc)" = "$enc_sum" ] || die "enc differs after restart"
