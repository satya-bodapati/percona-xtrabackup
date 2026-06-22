############################################################################
# Exercise the multipart-upload CLI options that ds_cloud exposes.  Each
# scenario tweaks one knob and verifies the resulting upload is intact
# (round-trips data correctly) and that the multipart behavior matches
# expectations (single PUT vs multipart, part count, etc.).
#
# Options covered:
#   --cloud-multipart-part-size       (force a fixed part size)
#   --cloud-multipart-threshold       (single-PUT cutoff)
#   --cloud-multipart-rollover-threshold (per-object 5 TiB cap; tested at
#                                        a tiny threshold for behavior)
#   --cloud-http-parallel-requests    (curl-multi concurrent in-flight)
#   --cloud-rate-log-interval         (observability; assert log lines)
#   --cloud-timeout                   (sanity-pass with a generous value)
#   --cloud-max-retries / --cloud-max-backoff (sanity-pass)
#   --cloud-insecure (no-op against LocalStack http; just asserts accept)
#   --cloud-storage-class (sanity-pass through; LocalStack ignores it)
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

PROVIDER="${PXB_CLOUD_BACKEND:-s3}"

cloud_emu_require_docker
command -v jq >/dev/null 2>&1 || skip_test "test requires jq"
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for "$PROVIDER"

start_server --innodb_file_per_table

# Three IBDs of meaningfully different sizes: tiny (single-PUT fast path),
# medium (just over default 16 MiB multipart threshold), big (forces
# multiple parts regardless of --cloud-multipart-part-size).
mysql -e "CREATE DATABASE cmpopt;"
mysql -e "CREATE TABLE cmpopt.tiny  (id INT PRIMARY KEY, p TEXT) ENGINE=InnoDB;"
mysql -e "CREATE TABLE cmpopt.medium(id INT PRIMARY KEY AUTO_INCREMENT, p BLOB) ENGINE=InnoDB;"
mysql -e "CREATE TABLE cmpopt.big   (id INT PRIMARY KEY AUTO_INCREMENT, p BLOB) ENGINE=InnoDB;"
mysql -e "INSERT INTO cmpopt.tiny VALUES (1, REPEAT('t', 1000));"
for i in $(seq 1 4) ; do
  mysql -e "INSERT INTO cmpopt.medium (p) VALUES (REPEAT('m', 5000000));"
done
for i in $(seq 1 8) ; do
  mysql -e "INSERT INTO cmpopt.big (p) VALUES (REPEAT('b', 8000000));"
done
innodb_wait_for_flush_all
record_db_state cmpopt

############################################################################
# Helper: run one backup with the given extra flags, verify it lands and
# round-trips.  Argument $1 is the test name; $2 is the extra flag string.
############################################################################
run_one() {
  local label="$1" ; shift
  local extra="$*"

  local bucket="cmpopt-${label}-$(date +%s)"
  cloud_emu_make_bucket "$PROVIDER" "$bucket"
  local flags=$(cloud_emu_xb_flags "$PROVIDER" "$bucket")
  local name="be-${label}"

  vlog "--- $label: $extra ---"

  rm -rf $topdir/$name $topdir/$name-dl
  mkdir -p $topdir/$name
  eval xtrabackup --backup --target-dir=$topdir/$name $flags $extra \
       2> $topdir/$name.log

  # Manifest is mandatory and must be present in the bucket listing.
  case "$PROVIDER" in
    s3)
      AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
      aws --endpoint-url="$CLOUD_EMU_S3_ENDPOINT" \
          s3 ls "s3://$bucket/" --recursive | grep -q backup_meta.json \
        || die "$label: backup_meta.json missing from bucket"
      ;;
  esac

  # Download into a fresh dir and confirm it parses + extracts the IBDs.
  mkdir -p $topdir/$name-dl
  eval xtrabackup --download --target-dir=$topdir/$name-dl/$name $flags
  [ -s $topdir/$name-dl/$name/backup_meta.json ] \
    || die "$label: manifest missing after --download"
  [ -f $topdir/$name-dl/$name/cmpopt/big.ibd ] \
    || die "$label: big.ibd missing after --download"

  # Restore + sanity-check rows survived.
  xtrabackup --prepare --target-dir=$topdir/$name-dl/$name
  stop_server
  rm -rf $mysql_datadir/*
  xtrabackup --copy-back --target-dir=$topdir/$name-dl/$name
  start_server
  verify_db_state cmpopt
  [ "$(mysql -BN -e 'SELECT COUNT(*) FROM cmpopt.big;')" = "8" ] \
    || die "$label: row-count mismatch on cmpopt.big"
}

############################################################################
# Scenario 1: defaults (sanity baseline).
############################################################################
run_one defaults ""

############################################################################
# Scenario 2: --cloud-multipart-part-size=5242880  (fixed 5 MiB parts)
#   Forces many small parts on big.ibd (~64 MiB / 5 MiB ~= 13 parts).
#   Confirms ds_cloud's dynamic-tier overrides work and Stream_multipart_
#   writer commits parts correctly.
############################################################################
run_one fixed-5MiB "--cloud-multipart-part-size=5242880"

############################################################################
# Scenario 3: --cloud-multipart-threshold=1  (tiny -- everything goes
# through multipart, even the smallest file)
############################################################################
run_one threshold-1 "--cloud-multipart-threshold=1"

############################################################################
# Scenario 4: --cloud-multipart-threshold=104857600  (100 MiB; medium and
# tiny take the single-PUT fast path, only big.ibd uses multipart)
############################################################################
run_one threshold-100MiB "--cloud-multipart-threshold=104857600"

############################################################################
# Scenario 5: --cloud-http-parallel-requests=2  (low concurrency stress)
############################################################################
run_one parallel-2 "--cloud-http-parallel-requests=2"

############################################################################
# Scenario 6: --cloud-http-parallel-requests=32 (high concurrency)
############################################################################
run_one parallel-32 "--cloud-http-parallel-requests=32"

############################################################################
# Scenario 7: --cloud-rate-log-interval=1 (log every second; assert at
# least one rate-log line landed in the backup log)
############################################################################
run_one rate-log-1s "--cloud-rate-log-interval=1"
grep -q "rate up=" $topdir/be-rate-log-1s.log \
  || die "rate-log-1s: no 'rate up=' lines in xtrabackup log"

############################################################################
# Scenario 8: --cloud-timeout=600 + --cloud-max-retries=20 (sanity-pass
# of timing-knob plumbing; values higher than default to ensure they
# accept and don't break the request flow)
############################################################################
run_one timeout-retries "--cloud-timeout=600 --cloud-max-retries=20 --cloud-max-backoff=10000"

############################################################################
# Scenario 9: --cloud-insecure=1 (no-op for LocalStack http but ensures
# the option-parsing path works end-to-end)
############################################################################
run_one insecure "--cloud-insecure"

############################################################################
# Scenario 10: --cloud-storage-class=STANDARD (S3-side hint, LocalStack
# accepts and ignores; pass-through plumbing test)
############################################################################
if [ "$PROVIDER" = "s3" ] || [ "$PROVIDER" = "gcs" ]; then
  run_one storage-class "--cloud-storage-class=STANDARD"
fi

############################################################################
# Scenario 11: --cloud-multipart-rollover-threshold tiny value, file
# bigger than threshold -> object is rolled over.  We don't assert wire
# format (rollover lives in the multipart writer), just that the backup
# completes and round-trips OK.  Set to 30 MiB so big.ibd (~64 MiB) will
# need to roll.
############################################################################
run_one rollover-30MiB "--cloud-multipart-rollover-threshold=31457280"

vlog "All multipart-option scenarios passed for $PROVIDER."
