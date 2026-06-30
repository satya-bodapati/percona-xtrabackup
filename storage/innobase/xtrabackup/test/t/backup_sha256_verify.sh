#
# --sha256 records per-file SHA-256 in backup_files.jsonl over the
# LOGICAL bytes; --verify=sha re-hashes the on-disk file in
# --target-dir and compares against the recorded digest.
#
# Covers:
#  * plain backup -> presence + sha pass after restore
#  * tamper after backup -> sha mismatch detected
#  * --verify=presence stops after existence check (no rehash)
#

. inc/common.sh

start_server --innodb_file_per_table
load_sakila

vlog "=== Case 1: --sha256 backup, --verify=sha passes ==="
xtrabackup --backup --sha256 --target-dir=$topdir/backup1
test -f $topdir/backup1/backup_files.jsonl || \
  die "case1: backup_files.jsonl missing"
# At least one entry must carry a 64-char hex sha256.
python3 - <<PY || die "case1: no sha256 entries recorded"
import json, re
lines = [l for l in open("$topdir/backup1/backup_files.jsonl") if l.strip()]
sha_re = re.compile(r"^[0-9a-f]{64}$")
n_sha = n_skip = 0
for l in lines[1:]:
  d = json.loads(l)
  if "sha256" not in d: continue
  v = d["sha256"]
  if sha_re.match(v): n_sha += 1
  elif v.startswith("skipped:"): n_skip += 1
  else:
    raise SystemExit("case1: unexpected sha256 value %r" % v)
assert n_sha > 0, "case1: no real sha256 digests recorded"
print("case1: %d real digests, %d skipped" % (n_sha, n_skip))
PY
# --verify=sha should pass on the just-produced backup.
xtrabackup --verify=sha --target-dir=$topdir/backup1
# --verify=presence should also pass.
xtrabackup --verify=presence --target-dir=$topdir/backup1

vlog "=== Case 2: tamper a file, --verify=sha must fail ==="
# Pick one InnoDB file that has a real sha256 and flip a byte at its end.
vlog "case2: locating tamper target..."
set +e
target=$(jq -r 'select(.sha256? // "" | test("^[0-9a-f]{64}$")) | .path' \
  $topdir/backup1/backup_files.jsonl 2>/dev/null | head -1)
set -e
vlog "case2: target=${target:-<empty>}"
test -n "$target" || die "case2: no tamper target found"
full="$topdir/backup1/$target"
vlog "case2: full=$full"
ls -la "$full" || die "case2: target file missing or unreadable: $full"
# Append one byte at the end of the file.
echo -n "X" >> "$full" || die "case2: cannot append to $full"
vlog "case2: tampered ok"
# run_cmd_expect_failure runs the binary directly (bypassing the
# xtrabackup() wrapper that die()s on non-zero exit).
run_cmd_expect_failure $XB_BIN $XB_ARGS \
    --verify=sha --target-dir=$topdir/backup1 \
    2>$topdir/verify.err
grep -q "sha256 mismatch" $topdir/verify.err || \
  die "case2: --verify=sha did not report sha256 mismatch"
vlog "case2: tamper detected as expected"

vlog "=== Case 3: --verify=presence ignores sha tamper (presence only) ==="
xtrabackup --verify=presence --target-dir=$topdir/backup1 || \
  die "case3: --verify=presence must pass (file still exists)"

rm -rf $topdir/backup1 $topdir/verify.err

vlog "=== Case 4: no --sha256 means no digests recorded ==="
xtrabackup --backup --target-dir=$topdir/backup4
python3 - <<PY || die "case4: digest leaked into manifest"
import json
for l in open("$topdir/backup4/backup_files.jsonl"):
  l = l.strip()
  if not l: continue
  d = json.loads(l)
  assert "sha256" not in d, "case4: unexpected sha256 in entry %r" % d
print("case4: no sha256 fields, as expected")
PY
# --verify=presence on a non-sha backup must still pass.
xtrabackup --verify=presence --target-dir=$topdir/backup4
# --verify=sha on a non-sha backup also passes (no digests to check).
xtrabackup --verify=sha --target-dir=$topdir/backup4

rm -rf $topdir/backup4

vlog "PASS: --sha256 + --verify behave correctly across modes"
