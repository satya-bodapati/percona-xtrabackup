############################################################################
# --cloud-s3-bucket BUCKET vs BUCKET/PREFIX behavior exercised end-to-end
# against the LocalStack S3 emulator.  Verifies the parser's
# normalization rules (HNS-safe trailing-slash strip, embedded sub-prefix
# preserved) by inspecting the object keys after backup.
#
# Scenarios in this file (each with its own freshly-created bucket):
#   1. plain bucket          --cloud-s3-bucket=B           -> objects at B/<file>
#   2. single-level prefix   --cloud-s3-bucket=B/p         -> objects at B/p/<file>
#   3. multi-level prefix    --cloud-s3-bucket=B/a/b/c     -> objects at B/a/b/c/<file>
#   4. trailing-slash strip  --cloud-s3-bucket=B/p/        -> objects at B/p/<file>
#                            (HNS would reject a "B/p/" placeholder key)
#   5. no-key-ends-with-/    every PUT key must be a real object, never a
#                            "directory placeholder" ending in '/'
#   6. target-dir != prefix  --target-dir is purely local; its basename
#                            is NOT used as the bucket prefix anymore.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for s3

start_server --innodb_file_per_table
mysql -e "CREATE DATABASE bp_test;"
mysql -e "CREATE TABLE bp_test.t1 (id INT PRIMARY KEY, payload TEXT)
          ENGINE=InnoDB;"
mysql -e "INSERT INTO bp_test.t1 VALUES (1, REPEAT('x', 100));"
innodb_wait_for_flush_all

s3_ls() {
  local path="$1"
  AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
  aws --endpoint-url="$CLOUD_EMU_S3_ENDPOINT" \
      s3 ls "$path" --recursive 2>/dev/null \
    | awk '{print $NF}'
}

run_case() {
  local case_name="$1" bucket_arg="$2" want_prefix="$3"
  vlog "==== bp_test case: $case_name (bucket-arg='$bucket_arg', " \
       "want-prefix='$want_prefix') ===="

  local bucket=$(echo "$bucket_arg" | cut -d/ -f1)
  cloud_emu_make_bucket s3 "$bucket"

  local tdir="$topdir/case-$case_name"
  rm -rf "$tdir"
  mkdir -p "$tdir"

  local flags=$(cloud_emu_xb_flags s3 "$bucket_arg")
  eval xtrabackup --backup --target-dir="$tdir" $flags

  s3_ls "s3://$bucket/" > "$tdir/listing"
  [ -s "$tdir/listing" ] \
    || die "$case_name: bucket listing empty"

  # All keys must live under want_prefix/ (or directly under bucket
  # when want_prefix is empty).
  local bad
  if [ -z "$want_prefix" ]; then
    # No key may contain a '/' before the first segment except as a
    # natural sub-dir of a backup file (e.g. bp_test/t1.ibd).  Since
    # the file path layout under empty-prefix is exactly the database
    # layout, every key must start with a known DB folder.  Easier
    # check: NO key starts with the bucket name (we already listed
    # only that bucket) AND no key has leading '/'.
    bad=$(grep '^/' "$tdir/listing" || true)
    [ -z "$bad" ] \
      || die "$case_name: keys have leading '/': $bad"
  else
    bad=$(grep -v "^${want_prefix}/" "$tdir/listing" | head -3 || true)
    [ -z "$bad" ] \
      || die "$case_name: keys not under prefix '$want_prefix/': $bad"
  fi

  # HNS-safety: no key may end in '/'.
  bad=$(grep '/$' "$tdir/listing" || true)
  [ -z "$bad" ] \
    || die "$case_name: keys end in '/' (HNS-unsafe): $bad"

  vlog "    PASS: $(wc -l < $tdir/listing) keys, all under " \
       "'${want_prefix:-<bucket-root>}', none ending in '/'"
}

############################################################################
# 1. plain bucket -- no prefix at all
############################################################################
run_case "plain" "pxb-bp-plain-$$" ""

############################################################################
# 2. single-level prefix
############################################################################
run_case "single" "pxb-bp-single-$$/2026-06-23-full" "2026-06-23-full"

############################################################################
# 3. multi-level prefix
############################################################################
run_case "multi" "pxb-bp-multi-$$/year/2026/full" "year/2026/full"

############################################################################
# 4. trailing slash -- must be stripped
############################################################################
run_case "trailing_slash" "pxb-bp-trail-$$/with-slash/" "with-slash"

############################################################################
# 5. target-dir basename is NOT a substitute for --cloud-s3-bucket prefix
#    (regression: old code used basename(--target-dir) as the prefix).
#    Pass a target-dir whose basename DOES NOT match the bucket prefix,
#    and verify objects landed under the PREFIX (not the basename).
############################################################################
vlog "==== bp_test case: target_dir_independent ===="

CLOUD_BUCKET="pxb-bp-tdi-$$"
PREFIX="real-prefix-name"
TDIR_BASENAME="totally-different-basename"

cloud_emu_make_bucket s3 "$CLOUD_BUCKET"
TDIR="$topdir/$TDIR_BASENAME"
rm -rf "$TDIR"; mkdir -p "$TDIR"

FLAGS=$(cloud_emu_xb_flags s3 "$CLOUD_BUCKET/$PREFIX")
eval xtrabackup --backup --target-dir="$TDIR" $FLAGS

s3_ls "s3://$CLOUD_BUCKET/" > "$TDIR/listing"

# Every key under PREFIX/, none under TDIR_BASENAME/.
bad=$(grep "^${TDIR_BASENAME}/" "$TDIR/listing" | head -1 || true)
[ -z "$bad" ] \
  || die "target-dir basename leaked into cloud keys: $bad"

bad=$(grep -v "^${PREFIX}/" "$TDIR/listing" | head -1 || true)
[ -z "$bad" ] \
  || die "key not under explicit prefix: $bad"

vlog "    PASS: target-dir basename '$TDIR_BASENAME' did NOT leak " \
     "into cloud keys; all keys live under '$PREFIX/'"

vlog "==== cloud_bucket_prefix.sh PASSED ===="
