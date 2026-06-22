############################################################################
# Sparse-file round-trip tests for the local and xbstream transport paths.
#
# Covers the matrix:
#
#                    | local target-dir | xbstream
#   -----------------+------------------+---------------
#   no compress      | scen 1           | scen 3
#   --compress=zstd  | scen 2           | scen 4
#
# Each scenario:
#   1. Creates a page-compressed (sparse) table.
#   2. Backs up via the chosen transport.
#   3. Asserts the backup artifact has the expected on-disk shape.
#   4. Asserts backup_meta.json carries sparse_map for the sparse table.
#   5. Restores via --decompress (if compressed) + --prepare + --copy-back.
#   6. Asserts the restored .ibd is sparse on disk.
#   7. Verifies the database content matches via verify_db_state.
#
# Cloud transport (--cloud-storage) is covered in
# suites/cloud-direct/sparse_cloud.sh (gated on docker availability).
############################################################################

. inc/common.sh

command -v jq >/dev/null 2>&1 || skip_test "test requires jq on PATH"

if grep -q 'PUNCH HOLE support not available' "$MYSQLD_ERRFILE" 2>/dev/null ; then
  skip_test "test requires PUNCH HOLE support on the filesystem"
fi

start_server --innodb_file_per_table

mysql -e "CREATE DATABASE sparse_rt;"
mysql -e "CREATE TABLE sparse_rt.dense_one (id INT PRIMARY KEY AUTO_INCREMENT, payload TEXT)
          ENGINE=InnoDB;"
mysql -e "INSERT INTO sparse_rt.dense_one (payload) VALUES (REPEAT('d', 100));"

mysql -e "CREATE TABLE sparse_rt.t_sparse (c1 INT AUTO_INCREMENT PRIMARY KEY, c2 BLOB)
          COMPRESSION='zlib' ENGINE=InnoDB;"
mysql -e "INSERT INTO sparse_rt.t_sparse (c2) VALUES (REPEAT('x', 5000));"
for i in $(seq 1 6) ; do
  mysql -e "INSERT INTO sparse_rt.t_sparse (c2) SELECT c2 FROM sparse_rt.t_sparse;"
done
innodb_wait_for_flush_all

is_sparse_file $mysql_datadir/sparse_rt/t_sparse.ibd \
  || die "source sparse_rt/t_sparse.ibd is NOT sparse on disk"
SRC_BLOCKS=$(stat -c %b $mysql_datadir/sparse_rt/t_sparse.ibd)
SRC_APPARENT=$(stat -c %s $mysql_datadir/sparse_rt/t_sparse.ibd)
[ "$((SRC_BLOCKS * 512))" -lt "$SRC_APPARENT" ] \
  || die "source IBD allocated=$((SRC_BLOCKS * 512)) is not less than apparent=$SRC_APPARENT (not sparse)"
vlog "source: t_sparse.ibd apparent=$SRC_APPARENT allocated=$((SRC_BLOCKS * 512))"

record_db_state sparse_rt

############################################################################
# Helper: assert that a backup_meta.json entry has a non-empty sparse_map
# for the sparse table, and that the dense table has no sparse_map.
############################################################################
assert_manifest_sparse_map() {
  local manifest="$1" label="$2"
  [ -s "$manifest" ] || die "$label: backup_meta.json missing or empty"

  local n_regions
  n_regions=$(jq -r '.files[]
                      | select(.name == "sparse_rt/t_sparse.ibd")
                      | .sparse_map // [] | length' < "$manifest")
  [ "$n_regions" -gt 0 ] \
    || die "$label: t_sparse.ibd has no sparse_map in backup_meta.json"

  jq -e '.files[]
          | select(.name == "sparse_rt/dense_one.ibd")
          | has("sparse_map") | not' < "$manifest" > /dev/null \
    || die "$label: dense_one.ibd unexpectedly has a sparse_map"

  vlog "$label: backup_meta.json has $n_regions regions for t_sparse.ibd, none for dense_one.ibd"
}

############################################################################
# Helper: end-to-end restore + db-state verification on a target-dir backup.
# Argument $1 is the backup directory.
############################################################################
restore_and_verify() {
  local backup_dir="$1" label="$2"

  xtrabackup --prepare --target-dir=$backup_dir

  stop_server
  rm -rf $mysql_datadir/*
  xtrabackup --copy-back --target-dir=$backup_dir
  start_server

  verify_db_state sparse_rt

  # After --copy-back the live datadir's t_sparse.ibd should be sparse again.
  # (--copy-back must preserve sparseness; if not, this assertion catches a
  # regression in copy-back vs the new manifest-driven flow.)
  is_sparse_file $mysql_datadir/sparse_rt/t_sparse.ibd \
    || die "$label: restored t_sparse.ibd is NOT sparse on disk"

  local rb=$(stat -c %b $mysql_datadir/sparse_rt/t_sparse.ibd)
  local ra=$(stat -c %s $mysql_datadir/sparse_rt/t_sparse.ibd)
  [ "$((rb * 512))" -lt "$ra" ] \
    || die "$label: restored IBD not sparse (allocated=$((rb*512)) >= apparent=$ra)"
  vlog "$label: restored sparse: apparent=$ra allocated=$((rb * 512))"
}

############################################################################
# Scenario 1: --backup --target-dir, no compress
#   Sparse layout preserved on disk by ds_local's lseek+write (PXB-3658).
#   Manifest carries sparse_map for downstream consumers.
############################################################################
vlog "=== Scenario 1: local target-dir, no compress ==="

xtrabackup --backup --target-dir=$topdir/srt1

is_sparse_file $topdir/srt1/sparse_rt/t_sparse.ibd \
  || die "scen1: backed-up t_sparse.ibd is not sparse on disk"

assert_manifest_sparse_map $topdir/srt1/backup_meta.json "scen1"

restore_and_verify $topdir/srt1 "scen1"

rm -rf $topdir/srt1

############################################################################
# Scenario 2: --backup --target-dir --compress=zstd
#   Backup file is .ibd.zst (dense compressed bytes).
#   --decompress unpacks and uses manifest's sparse_map to re-create holes
#   via fallocate(PUNCH_HOLE).
############################################################################
vlog "=== Scenario 2: local target-dir, --compress=zstd ==="

xtrabackup --backup --compress=zstd --target-dir=$topdir/srt2

[ -f $topdir/srt2/sparse_rt/t_sparse.ibd.zst ] \
  || die "scen2: t_sparse.ibd.zst missing in compressed backup"
[ ! -f $topdir/srt2/sparse_rt/t_sparse.ibd ] \
  || die "scen2: raw .ibd unexpectedly present alongside .zst"

assert_manifest_sparse_map $topdir/srt2/backup_meta.json "scen2"

xtrabackup --decompress --target-dir=$topdir/srt2

[ -f $topdir/srt2/sparse_rt/t_sparse.ibd ] \
  || die "scen2: t_sparse.ibd missing after --decompress"

is_sparse_file $topdir/srt2/sparse_rt/t_sparse.ibd \
  || die "scen2: t_sparse.ibd is NOT sparse after --decompress"

restore_and_verify $topdir/srt2 "scen2"

rm -rf $topdir/srt2

############################################################################
# Setup for xbstream scenarios: we need the data again after the previous
# scenarios' stop/restart cycles wiped the datadir.
############################################################################
record_db_state sparse_rt   # refresh checksum baseline against current datadir

############################################################################
# Scenario 3: --backup --stream=xbstream (no compress)
#   The stream's wire format uses XB_CHUNK_TYPE_SPARSE frames inline for
#   the sparse IBD.  xbstream -x lseek+writes the data regions, producing
#   a sparse file on disk natively.  Manifest also present in the stream
#   as a separate file.
############################################################################
vlog "=== Scenario 3: --stream=xbstream, no compress ==="

mkdir -p $topdir/srt3
xtrabackup --backup --stream=xbstream > $topdir/srt3.xbs
(cd $topdir/srt3 && xbstream -xv < $topdir/srt3.xbs)

[ -f $topdir/srt3/sparse_rt/t_sparse.ibd ] \
  || die "scen3: t_sparse.ibd missing after xbstream extract"

is_sparse_file $topdir/srt3/sparse_rt/t_sparse.ibd \
  || die "scen3: extracted t_sparse.ibd is NOT sparse (in-stream sparse frame failed)"

assert_manifest_sparse_map $topdir/srt3/backup_meta.json "scen3"

restore_and_verify $topdir/srt3 "scen3"

rm -rf $topdir/srt3 $topdir/srt3.xbs

############################################################################
# Scenario 4: --backup --stream=xbstream --compress=zstd
#   xbstream emits .ibd.zst chunks (dense compressed; no sparse frames
#   used because compression is upstream).  xbstream -x --decompress
#   inflates back to .ibd; the manifest-driven punch (replacing the
#   legacy restore_sparseness page-walk) reconstructs the holes.
############################################################################
vlog "=== Scenario 4: --stream=xbstream --compress=zstd ==="

mkdir -p $topdir/srt4
xtrabackup --backup --stream=xbstream --compress=zstd > $topdir/srt4.xbs
(cd $topdir/srt4 && xbstream --decompress -xv < $topdir/srt4.xbs)

[ -f $topdir/srt4/sparse_rt/t_sparse.ibd ] \
  || die "scen4: t_sparse.ibd missing after xbstream -x --decompress"
[ ! -f $topdir/srt4/sparse_rt/t_sparse.ibd.zst ] \
  || die "scen4: .zst leftover after --decompress"

is_sparse_file $topdir/srt4/sparse_rt/t_sparse.ibd \
  || die "scen4: t_sparse.ibd is NOT sparse after xbstream -x --decompress"

assert_manifest_sparse_map $topdir/srt4/backup_meta.json "scen4"

restore_and_verify $topdir/srt4 "scen4"

rm -rf $topdir/srt4 $topdir/srt4.xbs

vlog "All sparse round-trip scenarios (local/xbstream x compressed/no-compress) passed."
