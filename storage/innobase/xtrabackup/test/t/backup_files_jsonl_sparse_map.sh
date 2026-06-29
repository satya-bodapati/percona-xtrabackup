#
# Verify that backup_files.jsonl carries a per-file `sparse_map`
# array for IBDs that get sparse writes during backup (page-compressed
# tables on a filesystem with PUNCH HOLE support), and does NOT carry
# the array for dense tables.
#
# `sparse_map` entries are {offset, length} pairs describing holes in
# the logical (unpacked) file space, recorded by
# xb_files_jsonl::record_sparse_chunks from datasink.cc's ds_write_sparse
# hook.  The data is what drives the --download / --copy-back sparse
# restore path; this test just verifies it lands in the JSONL.
#

. inc/common.sh

command -v jq >/dev/null 2>&1 || skip_test "test requires jq on PATH"

if grep -q 'PUNCH HOLE support not available' "$MYSQLD_ERRFILE" 2>/dev/null ; then
  skip_test "test requires PUNCH HOLE support on the filesystem"
fi

start_server --innodb_file_per_table

mysql -e "CREATE DATABASE sparse_jsonl;"
mysql -e "CREATE TABLE sparse_jsonl.dense (id INT PRIMARY KEY AUTO_INCREMENT,
                                            payload TEXT) ENGINE=InnoDB;"
mysql -e "INSERT INTO sparse_jsonl.dense (payload) VALUES (REPEAT('d', 100));"

mysql -e "CREATE TABLE sparse_jsonl.t_sparse (c1 INT AUTO_INCREMENT PRIMARY KEY,
                                               c2 BLOB)
          COMPRESSION='zlib' ENGINE=InnoDB;"
mysql -e "INSERT INTO sparse_jsonl.t_sparse (c2) VALUES (REPEAT('x', 5000));"
for i in $(seq 1 6) ; do
  mysql -e "INSERT INTO sparse_jsonl.t_sparse (c2)
            SELECT c2 FROM sparse_jsonl.t_sparse;"
done
innodb_wait_for_flush_all

mkdir -p $topdir/backup
xtrabackup --backup --target-dir=$topdir/backup

JSONL=$topdir/backup/backup_files.jsonl
test -f "$JSONL" || die "backup_files.jsonl missing in $topdir/backup"

# t_sparse.ibd must have a non-empty sparse_map.  Filter out the
# header line (no "path" field) before testing endswith().
SPARSE_COUNT=$(jq -r 'select(has("path") and
                              (.path | endswith("sparse_jsonl/t_sparse.ibd")))
                       | (.sparse_map | length)' \
                "$JSONL" | head -1)
if [ -z "$SPARSE_COUNT" ] || [ "$SPARSE_COUNT" -le 0 ] ; then
  cat "$JSONL"
  die "sparse_jsonl/t_sparse.ibd has no sparse_map entries in backup_files.jsonl"
fi
vlog "t_sparse.ibd sparse_map has $SPARSE_COUNT hole entries"

# Each entry must have integer offset+length, length > 0.
# Filter down to the one matching line then validate.
jq -e --slurp --arg p "sparse_jsonl/t_sparse.ibd" '
  map(select(.path != null and (.path | endswith($p))))
  | .[0].sparse_map
  | all(.[]; (.offset|type) == "number" and (.length|type) == "number"
              and .length > 0)
' "$JSONL" >/dev/null \
  || die "sparse_map entries malformed for t_sparse.ibd"

# dense.ibd must NOT have a sparse_map (dense pages -> no holes).
DENSE_HAS=$(jq -r 'select(has("path") and
                            (.path | endswith("sparse_jsonl/dense.ibd")))
                    | has("sparse_map")' "$JSONL" | head -1)
if [ "$DENSE_HAS" = "true" ] ; then
  die "dense.ibd unexpectedly has sparse_map in backup_files.jsonl"
fi
vlog "dense.ibd has no sparse_map (as expected)"

stop_server
