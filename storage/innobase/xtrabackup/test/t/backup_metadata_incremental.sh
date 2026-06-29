#
# Cross-mode coverage for backup_metadata.json across a full +
# incremental chain.  Confirms:
#   - every backup in the chain produces its own backup_metadata.json
#   - manifest_version, xtrabackup_info, and xtrabackup_checkpoints
#     are present and parseable in each
#   - backup_size is consistent between the target-dir and the
#     --extra-lsndir copies of xtrabackup_info AND
#     backup_metadata.json (all four agree)
#   - the chain restores successfully end-to-end via --apply-redo-only
#     + final --prepare + --copy-back
#

. inc/common.sh

start_server --innodb_file_per_table
load_sakila

# Helper: assert all four "backup_size" values agree -- target-dir
# xtrabackup_info, extra-lsndir xtrabackup_info, target-dir
# backup_metadata.json, extra-lsndir backup_metadata.json.
assert_backup_size_consistent() {
  local label=$1 dir=$2 lsn=$3
  local bs_t_info=$(get_field "$dir/xtrabackup_info" backup_size)
  local bs_l_info=$(get_field "$lsn/xtrabackup_info" backup_size)
  local bs_t_meta=$(python3 -c "import json; \
    print(json.load(open('$dir/backup_metadata.json'))['xtrabackup_info']['backup_size'])")
  local bs_l_meta=$(python3 -c "import json; \
    print(json.load(open('$lsn/backup_metadata.json'))['xtrabackup_info']['backup_size'])")
  if [ "$bs_t_info" != "$bs_l_info" ] || [ "$bs_t_info" != "$bs_t_meta" ] ||
     [ "$bs_t_info" != "$bs_l_meta" ]; then
    die "$label: backup_size disagreement: \
target/xtrabackup_info=$bs_t_info \
lsn/xtrabackup_info=$bs_l_info \
target/backup_metadata.json=$bs_t_meta \
lsn/backup_metadata.json=$bs_l_meta"
  fi
  vlog "$label: backup_size=$bs_t_info (consistent across all 4 copies)"
}

vlog "Full backup"
mkdir -p $topdir/lsn_full
xtrabackup --backup --target-dir=$topdir/backup_full \
           --extra-lsndir=$topdir/lsn_full

test -f $topdir/backup_full/backup_metadata.json || \
  die "full: backup_metadata.json missing"
python3 -c "import json; \
  d=json.load(open('$topdir/backup_full/backup_metadata.json')); \
  assert d['manifest_version']==1; \
  assert d['xtrabackup_info'] is not None; \
  assert d['xtrabackup_checkpoints'] is not None; \
  assert d['xtrabackup_checkpoints']['backup_type']=='full-backuped'" \
  || die "full: backup_metadata.json schema check failed"
assert_backup_size_consistent "full" "$topdir/backup_full" "$topdir/lsn_full"

# Make some changes for inc1
$MYSQL $MYSQL_ARGS -e "INSERT INTO actor (first_name, last_name) VALUES ('A', 'B')" sakila

vlog "Incremental 1"
mkdir -p $topdir/lsn_inc1
xtrabackup --backup --target-dir=$topdir/backup_inc1 \
           --incremental-basedir=$topdir/backup_full \
           --extra-lsndir=$topdir/lsn_inc1

test -f $topdir/backup_inc1/backup_metadata.json || \
  die "inc1: backup_metadata.json missing"
python3 -c "import json; \
  d=json.load(open('$topdir/backup_inc1/backup_metadata.json')); \
  assert d['manifest_version']==1; \
  assert d['xtrabackup_info']['incremental']=='Y'; \
  assert d['xtrabackup_checkpoints']['backup_type']=='incremental'" \
  || die "inc1: backup_metadata.json schema check failed"
assert_backup_size_consistent "inc1" "$topdir/backup_inc1" "$topdir/lsn_inc1"

# More changes for inc2
$MYSQL $MYSQL_ARGS -e "INSERT INTO actor (first_name, last_name) VALUES ('C', 'D')" sakila

vlog "Incremental 2"
mkdir -p $topdir/lsn_inc2
xtrabackup --backup --target-dir=$topdir/backup_inc2 \
           --incremental-basedir=$topdir/backup_inc1 \
           --extra-lsndir=$topdir/lsn_inc2

test -f $topdir/backup_inc2/backup_metadata.json || \
  die "inc2: backup_metadata.json missing"
assert_backup_size_consistent "inc2" "$topdir/backup_inc2" "$topdir/lsn_inc2"

# tool_version should be identical across the chain.
tv_full=$(python3 -c "import json; \
  print(json.load(open('$topdir/backup_full/backup_metadata.json'))['xtrabackup_info']['tool_version'])")
tv_inc1=$(python3 -c "import json; \
  print(json.load(open('$topdir/backup_inc1/backup_metadata.json'))['xtrabackup_info']['tool_version'])")
tv_inc2=$(python3 -c "import json; \
  print(json.load(open('$topdir/backup_inc2/backup_metadata.json'))['xtrabackup_info']['tool_version'])")
if [ "$tv_full" != "$tv_inc1" ] || [ "$tv_full" != "$tv_inc2" ]; then
  die "tool_version mismatch across chain: full=$tv_full inc1=$tv_inc1 inc2=$tv_inc2"
fi
vlog "tool_version consistent across chain: $tv_full"

# End-to-end restore: prepare chain with --apply-redo-only (new alias),
# final --prepare, --copy-back, server starts cleanly, data intact.
record_db_state sakila
xtrabackup --prepare --apply-redo-only --target-dir=$topdir/backup_full
xtrabackup --prepare --apply-redo-only --target-dir=$topdir/backup_full \
           --incremental-dir=$topdir/backup_inc1
xtrabackup --prepare --apply-redo-only --target-dir=$topdir/backup_full \
           --incremental-dir=$topdir/backup_inc2
xtrabackup --prepare --target-dir=$topdir/backup_full

stop_server
rm -rf $mysql_datadir/*
xtrabackup --copy-back --target-dir=$topdir/backup_full
start_server
verify_db_state sakila

vlog "PASS: full+2 incremental chain produces consistent backup_metadata.json files and restores cleanly"
