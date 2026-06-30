############################################################################
# Tests for the auto-sizing algorithm at cloud_open time.
#
# Verifies that the diagnostic log line emitted per file reflects the
# correct decisions for the three cases:
#   (A) user-set part_size + buffer_size cap
#   (B) auto part_size, unlimited buffer (aws-cli-like default)
#   (C) auto part_size, buffer_size cap (effective_concurrent shrinks)
#
# The diagnostic log line format is:
#   ds_cloud: <object>: filesize=X, part_size=Y (auto|user), concurrent=N
#   ds_cloud: <object>: filesize=X, single-PUT fast path     (small files)
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

PROVIDER="s3"   # algorithm is provider-agnostic; one provider exercises it

cloud_emu_require_docker
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for s3

start_server --innodb_file_per_table

# Build a small + medium + larger-than-default-part-size table set so we
# get coverage of single-PUT fast path AND multipart sizing in one backup.
mysql -e "CREATE DATABASE bs;"
mysql -e "CREATE TABLE bs.tiny (id INT PRIMARY KEY) ENGINE=InnoDB;"
mysql -e "INSERT INTO bs.tiny VALUES (1);"
mysql -e "CREATE TABLE bs.big (id INT PRIMARY KEY AUTO_INCREMENT, p LONGBLOB) ENGINE=InnoDB;"
for i in $(seq 1 8) ; do
  mysql --max_allowed_packet=64M -e "INSERT INTO bs.big (p) VALUES (REPEAT('b', 5000000));"
done

run_bench() {
  local label="$1" ; shift
  local extra="$*"
  # S3 bucket names must be lowercase and use only [a-z0-9.-]; coerce.
  local label_safe="$(echo "$label" | tr 'A-Z' 'a-z' | tr '_.' '-')"
  local bucket="bs-${label_safe}-$(date +%s)"
  cloud_emu_make_bucket s3 "$bucket"
  local flags=$(cloud_emu_xb_flags s3 "$bucket")
  rm -rf $topdir/bs-$label
  eval xtrabackup --backup --target-dir=$topdir/bs-$label $flags $extra \
       2> $topdir/bs-$label.log
}

# A: Default behavior (no upload-buffer-size; aws-cli style).
vlog "=== Scenario A: default, unlimited buffer ==="
run_bench A ""
# Expect: bs/big.ibd > 16 MiB -> shows part_size + concurrent=16
grep -q "bs/big.ibd:.*part_size=16.00 MiB (auto), concurrent=16$" \
     $topdir/bs-A.log \
  || die "scenA: expected default 16 MiB part_size / concurrent=16 on big.ibd"
# Expect: bs/tiny.ibd < threshold -> single-PUT
grep -q "bs/tiny.ibd:.*single-PUT fast path" $topdir/bs-A.log \
  || die "scenA: expected single-PUT log for bs/tiny.ibd"

# B: --cloud-upload-buffer-size=256MiB (loose enough not to shrink).
vlog "=== Scenario B: --cloud-upload-buffer-size=256MiB ==="
run_bench B "--cloud-upload-buffer-size=$((256 * 1024 * 1024))"
# 256 MiB / 16 MiB part = 16 -> concurrent stays at user max 16
grep -q "bs/big.ibd:.*part_size=16.00 MiB (auto), concurrent=16$" \
     $topdir/bs-B.log \
  || die "scenB: expected concurrent=16 (buffer fits 16 parts)"
# Must NOT log the shrink message.
if grep -q "shrunk by --cloud-upload-buffer-size" $topdir/bs-B.log ; then
  die "scenB: unexpectedly logged 'shrunk' when buffer was loose enough"
fi

# C: --cloud-upload-buffer-size=64MiB (tight; should shrink concurrent).
vlog "=== Scenario C: --cloud-upload-buffer-size=64MiB ==="
run_bench C "--cloud-upload-buffer-size=$((64 * 1024 * 1024))"
# 64 MiB / 16 MiB = 4 -> concurrent=4, "shrunk" suffix present.
grep -q "bs/big.ibd:.*part_size=16.00 MiB (auto), concurrent=4 (shrunk" \
     $topdir/bs-C.log \
  || die "scenC: expected concurrent shrunk to 4 by 64 MiB buffer"

# D: User-set part-size=32 MiB.
vlog "=== Scenario D: user-set --cloud-multipart-part-size=32 MiB ==="
run_bench D "--cloud-multipart-part-size=$((32 * 1024 * 1024))"
grep -q "bs/big.ibd:.*part_size=32.00 MiB (user), concurrent=16$" \
     $topdir/bs-D.log \
  || die "scenD: expected user-set 32 MiB part_size + concurrent=16"

# E: User-set part-size AND buffer too tight to keep full concurrency.
vlog "=== Scenario E: user part_size=32MiB + buffer=64MiB ==="
run_bench E "--cloud-multipart-part-size=$((32 * 1024 * 1024)) \
             --cloud-upload-buffer-size=$((64 * 1024 * 1024))"
# 64 / 32 = 2 -> concurrent shrinks to 2.
grep -q "bs/big.ibd:.*part_size=32.00 MiB (user), concurrent=2 (shrunk" \
     $topdir/bs-E.log \
  || die "scenE: expected user-set 32 MiB part_size + concurrent=2 (shrunk)"

vlog "All cloud-upload-buffer-size auto-sizing scenarios passed."
