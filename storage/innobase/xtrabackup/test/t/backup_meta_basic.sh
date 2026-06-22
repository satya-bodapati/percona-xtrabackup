############################################################################
# Test that backup_meta.json is written and well-formed for plain target-dir
# and xbstream backups, including encrypted variants.  Today the manifest
# carries name + logical_size per file; later commits extend it with
# sparse_map and other per-file fields (PXB-3754 / PXB-3671).
#
# Assertions:
#   - backup_meta.json exists in the target dir (or extracts cleanly from
#     the xbstream archive).
#   - The JSON is valid and has "version": 1 and a non-empty "files" array.
#   - Every file entry has a non-empty "name" and a non-negative
#     "logical_size" (logical_size is omitted when zero).
#   - The set of names covers the InnoDB datafiles we copied.
############################################################################

. inc/common.sh

require_jq() {
  command -v jq >/dev/null 2>&1 || die "test requires 'jq' on PATH"
}

require_jq

start_server --innodb_file_per_table

mysql -e "CREATE DATABASE bm_test;"
mysql -e "CREATE TABLE bm_test.t1 (id INT PRIMARY KEY) ENGINE=InnoDB;"
mysql -e "CREATE TABLE bm_test.t2 (id INT PRIMARY KEY, b TEXT) ENGINE=InnoDB;"
mysql -e "INSERT INTO bm_test.t1 VALUES (1),(2),(3);"
mysql -e "INSERT INTO bm_test.t2 SELECT n, REPEAT('x', 100) FROM (SELECT 1 n UNION SELECT 2 UNION SELECT 3) z;"

ENCKEY="percona_xtrabackup_is_awesome___"

verify_manifest() {
  local manifest_path="$1"
  local label="$2"

  [ -s "$manifest_path" ] || die "$label: backup_meta.json missing or empty at $manifest_path"

  # Valid JSON?
  jq empty < "$manifest_path" \
    || die "$label: backup_meta.json is not valid JSON"

  # Schema header
  local version
  version=$(jq -r '.version' < "$manifest_path")
  [ "$version" = "1" ] \
    || die "$label: expected .version=1, got '$version'"

  # Has files array with entries
  local nfiles
  nfiles=$(jq -r '.files | length' < "$manifest_path")
  [ "$nfiles" -gt 0 ] \
    || die "$label: .files array is empty"

  # Every entry has a non-empty name
  local missing_name
  missing_name=$(jq -r '.files[] | select(.name == null or .name == "") | .name' \
                 < "$manifest_path" | wc -l)
  [ "$missing_name" -eq 0 ] \
    || die "$label: $missing_name entries are missing 'name'"

  # logical_size, when present, is non-negative
  local bad_size
  bad_size=$(jq -r '.files[] | select(.logical_size != null and .logical_size < 0) | .name' \
             < "$manifest_path" | wc -l)
  [ "$bad_size" -eq 0 ] \
    || die "$label: $bad_size entries have negative logical_size"

  # The user-table IBDs we created must appear in the manifest.
  jq -e '.files | map(.name) | index("bm_test/t1.ibd") != null' \
     < "$manifest_path" > /dev/null \
    || die "$label: bm_test/t1.ibd missing from manifest"
  jq -e '.files | map(.name) | index("bm_test/t2.ibd") != null' \
     < "$manifest_path" > /dev/null \
    || die "$label: bm_test/t2.ibd missing from manifest"

  vlog "$label: backup_meta.json OK (version=$version, files=$nfiles)"
}

############################################################################
# Scenario 1: Plain --target-dir
############################################################################
vlog "=== Scenario 1: Plain --target-dir ==="

xtrabackup --backup --target-dir=$topdir/bm_backup1

verify_manifest "$topdir/bm_backup1/backup_meta.json" "scen1"

rm -rf $topdir/bm_backup1

############################################################################
# Scenario 2: --stream=xbstream (manifest must extract cleanly)
############################################################################
vlog "=== Scenario 2: --stream=xbstream ==="

mkdir -p $topdir/bm_backup2_extract
xtrabackup --backup --stream=xbstream > $topdir/bm_backup2.xbs

(cd $topdir/bm_backup2_extract && \
   xbstream -xv < $topdir/bm_backup2.xbs)

verify_manifest "$topdir/bm_backup2_extract/backup_meta.json" "scen2"

rm -rf $topdir/bm_backup2.xbs $topdir/bm_backup2_extract

############################################################################
# Scenario 3: Encrypted --target-dir
#   The manifest goes through ds_meta which is the no-transform pipeline,
#   so backup_meta.json must remain plaintext-readable even when --encrypt
#   is in effect for the data pipeline.
############################################################################
vlog "=== Scenario 3: Encrypted --target-dir (manifest must stay plaintext) ==="

xtrabackup --backup --encrypt=AES256 --encrypt-key="$ENCKEY" \
    --target-dir=$topdir/bm_backup3

verify_manifest "$topdir/bm_backup3/backup_meta.json" "scen3"

# Belt-and-suspenders: there should NOT be a backup_meta.json.xbcrypt
# next to it -- the manifest must be readable as JSON without decryption.
if [ -f "$topdir/bm_backup3/backup_meta.json.xbcrypt" ]; then
  die "scen3: backup_meta.json was encrypted but must stay plaintext"
fi

rm -rf $topdir/bm_backup3

############################################################################
# Scenario 4: Sparse IBD picks up a sparse_map in the manifest
############################################################################
if grep -q 'PUNCH HOLE support not available' $MYSQLD_ERRFILE ; then
  vlog "=== Scenario 4: SKIPPED (no PUNCH HOLE support) ==="
else
  vlog "=== Scenario 4: Sparse IBD records sparse_map in manifest ==="

  mysql -e "CREATE TABLE bm_test.t_sparse (c1 INT AUTO_INCREMENT PRIMARY KEY, c2 BLOB)
            COMPRESSION='zlib' ENGINE=InnoDB;"
  mysql -e "INSERT INTO bm_test.t_sparse (c2) VALUES (REPEAT('x', 5000));"
  for i in $(seq 1 6) ; do
    mysql -e "INSERT INTO bm_test.t_sparse (c2) SELECT c2 FROM bm_test.t_sparse;"
  done
  innodb_wait_for_flush_all

  is_sparse_file $mysql_datadir/bm_test/t_sparse.ibd \
    || die "scen4: t_sparse.ibd is not sparse on disk"

  xtrabackup --backup --target-dir=$topdir/bm_backup4

  verify_manifest "$topdir/bm_backup4/backup_meta.json" "scen4"

  # Sparse table MUST have a sparse_map in the manifest.
  local_sparse_map_len=$(jq -r '.files[]
                          | select(.name == "bm_test/t_sparse.ibd")
                          | .sparse_map // [] | length' \
                         < $topdir/bm_backup4/backup_meta.json)
  [ "$local_sparse_map_len" -gt 0 ] \
    || die "scen4: bm_test/t_sparse.ibd should have a non-empty sparse_map"

  # Each region must have non-negative offset and positive length.
  bad_regions=$(jq -r '.files[]
                  | select(.name == "bm_test/t_sparse.ibd")
                  | .sparse_map[]
                  | select(.offset < 0 or .length <= 0)' \
                < $topdir/bm_backup4/backup_meta.json | wc -l)
  [ "$bad_regions" -eq 0 ] \
    || die "scen4: $bad_regions sparse_map entries have bad offset/length"

  # Sum of region lengths must not exceed the file's logical_size.
  sum_data=$(jq -r '.files[]
                | select(.name == "bm_test/t_sparse.ibd")
                | [.sparse_map[].length] | add' \
             < $topdir/bm_backup4/backup_meta.json)
  logical=$(jq -r '.files[]
                | select(.name == "bm_test/t_sparse.ibd")
                | .logical_size' \
             < $topdir/bm_backup4/backup_meta.json)
  [ "$sum_data" -le "$logical" ] \
    || die "scen4: sum of region lengths ($sum_data) exceeds logical_size ($logical)"

  vlog "scen4: sparse_map has $local_sparse_map_len regions, $sum_data data bytes (logical=$logical)"

  # Dense files MUST NOT have a sparse_map.
  jq -e '.files[]
          | select(.name == "bm_test/t1.ibd")
          | has("sparse_map") | not' \
     < $topdir/bm_backup4/backup_meta.json > /dev/null \
    || die "scen4: dense bm_test/t1.ibd should not have sparse_map"

  rm -rf $topdir/bm_backup4
fi

vlog "All backup_meta.json scenarios passed."
