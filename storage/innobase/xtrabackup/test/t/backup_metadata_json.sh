#
# Verify backup_metadata.json is produced for every backup, in
# every output mode, and lands plain (no compress/encrypt transforms
# applied) regardless of pipeline configuration.  Also verifies
# that target-dir and --extra-lsndir copies of xtrabackup_info now
# carry the same backup_size value (consistency fix).
#

. inc/common.sh

start_server --innodb_file_per_table
load_sakila

ENCKEY="percona_xtrabackup_is_awesome___"

vlog "=== Case 1: plain --target-dir ==="
mkdir -p $topdir/lsn1
xtrabackup --backup --target-dir=$topdir/backup1 --extra-lsndir=$topdir/lsn1
test -f $topdir/backup1/backup_metadata.json || \
  die "case1: backup_metadata.json missing from target-dir"
test -f $topdir/lsn1/backup_metadata.json || \
  die "case1: backup_metadata.json missing from extra-lsndir"
# Must parse as JSON and have the expected top-level shape.
python3 -c "import json,sys; \
  d=json.load(open('$topdir/backup1/backup_metadata.json')); \
  assert d['manifest_version']==1, 'wrong manifest_version'; \
  assert 'xtrabackup_info' in d and d['xtrabackup_info'] is not None; \
  assert 'xtrabackup_binlog_info' in d; \
  assert 'xtrabackup_checkpoints' in d and d['xtrabackup_checkpoints'] is not None; \
  print('case1 schema OK')" || die "case1: backup_metadata.json schema invalid"
# target-dir and extra-lsndir copies of xtrabackup_info must agree on backup_size.
bs_target=$(get_field "$topdir/backup1/xtrabackup_info" backup_size)
bs_lsn=$(get_field "$topdir/lsn1/xtrabackup_info" backup_size)
assert_eq "$bs_target" "$bs_lsn" \
  "case1: backup_size matches across xtrabackup_info copies"
# Both backup_metadata.json copies must also be byte-identical.
diff -q $topdir/backup1/backup_metadata.json $topdir/lsn1/backup_metadata.json \
  || die "case1: target-dir and extra-lsndir backup_metadata.json differ"

rm -rf $topdir/backup1 $topdir/lsn1

vlog "=== Case 2: --stream=xbstream, then xbstream -x ==="
mkdir -p $topdir/lsn2
xtrabackup --backup --stream=xbstream --extra-lsndir=$topdir/lsn2 \
  > $topdir/backup2.xbs
mkdir -p $topdir/extract2
xbstream -x -C $topdir/extract2 < $topdir/backup2.xbs
test -f $topdir/extract2/backup_metadata.json || \
  die "case2: backup_metadata.json missing from xbstream extract"
test -f $topdir/lsn2/backup_metadata.json || \
  die "case2: backup_metadata.json missing from extra-lsndir"
python3 -c "import json; \
  d=json.load(open('$topdir/extract2/backup_metadata.json')); \
  assert d['manifest_version']==1, 'wrong manifest_version'; \
  print('case2 schema OK')" || die "case2: extracted backup_metadata.json invalid"
diff -q $topdir/extract2/backup_metadata.json $topdir/lsn2/backup_metadata.json \
  || die "case2: extracted and extra-lsndir backup_metadata.json differ"

rm -rf $topdir/backup2.xbs $topdir/extract2 $topdir/lsn2

vlog "=== Case 3: encrypted target -- backup_metadata.json stays plain ==="
mkdir -p $topdir/lsn3
xtrabackup --backup --encrypt=AES256 --encrypt-key="$ENCKEY" \
    --target-dir=$topdir/backup3 --extra-lsndir=$topdir/lsn3
test -f $topdir/backup3/backup_metadata.json || \
  die "case3: backup_metadata.json missing from encrypted target-dir"
# Other files should be encrypted (.xbcrypt suffix), but backup_metadata.json
# must remain plain JSON.
python3 -c "import json; \
  json.load(open('$topdir/backup3/backup_metadata.json')); \
  print('case3: backup_metadata.json is plain JSON, as expected')" \
  || die "case3: backup_metadata.json was encrypted despite ds_open_plain"

rm -rf $topdir/backup3 $topdir/lsn3

vlog "=== Case 4: compressed target -- backup_metadata.json stays plain ==="
mkdir -p $topdir/lsn4
xtrabackup --backup --compress --target-dir=$topdir/backup4 \
    --extra-lsndir=$topdir/lsn4
test -f $topdir/backup4/backup_metadata.json || \
  die "case4: backup_metadata.json missing from compressed target-dir"
# Should NOT have a compress suffix (.qp/.lz4/.zst) on the manifest.
for ext in .qp .lz4 .zst .xbcrypt ; do
  [ ! -f "$topdir/backup4/backup_metadata.json$ext" ] || \
    die "case4: backup_metadata.json should not have $ext suffix"
done
python3 -c "import json; \
  json.load(open('$topdir/backup4/backup_metadata.json')); \
  print('case4: backup_metadata.json is plain JSON')" \
  || die "case4: backup_metadata.json was compressed despite ds_open_plain"

rm -rf $topdir/backup4 $topdir/lsn4

vlog "PASS: backup_metadata.json produced plain across all output modes"
