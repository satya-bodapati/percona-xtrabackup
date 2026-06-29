#
# Verify backup_files.jsonl is produced for every backup, in every
# output mode, and lands plain (no compress/encrypt transforms
# applied) regardless of pipeline configuration.  Each line must be
# valid JSON; the first line carries manifest_version=1; every other
# line must have a "path" field.  Target-dir and --extra-lsndir
# copies are byte-identical.
#

. inc/common.sh

start_server --innodb_file_per_table
load_sakila

ENCKEY="percona_xtrabackup_is_awesome___"

# Validate a backup_files.jsonl file: parses as JSONL, first line
# carries {"manifest_version": 1}, every subsequent line has a
# "path" field.
validate_jsonl()
{
  local path=$1
  local tag=$2
  test -f "$path" || die "$tag: $path missing"
  python3 - <<PY || die "$tag: backup_files.jsonl invalid"
import json, sys
lines = [l for l in open("$path") if l.strip()]
assert lines, "$tag: empty backup_files.jsonl"
hdr = json.loads(lines[0])
assert hdr.get("manifest_version") == 1, \
  "$tag: first line missing manifest_version=1"
for i, line in enumerate(lines[1:], 1):
  obj = json.loads(line)
  assert "path" in obj, "$tag: line %d missing path" % i
print("$tag: backup_files.jsonl ok, %d entries" % (len(lines) - 1))
PY
}

vlog "=== Case 1: plain --target-dir ==="
mkdir -p $topdir/lsn1
xtrabackup --backup --target-dir=$topdir/backup1 --extra-lsndir=$topdir/lsn1
validate_jsonl $topdir/backup1/backup_files.jsonl "case1-target"
validate_jsonl $topdir/lsn1/backup_files.jsonl "case1-lsn"
# At least one InnoDB tablespace entry must carry space_id + page_size.
python3 - <<PY || die "case1: no entry has space_id + page_size"
import json
lines = [l for l in open("$topdir/backup1/backup_files.jsonl") if l.strip()]
ok = any("space_id" in json.loads(l) and "page_size" in json.loads(l)
         for l in lines[1:])
assert ok, "case1: no InnoDB entry has space_id + page_size"
print("case1: space_id / page_size annotation ok")
PY
diff -q $topdir/backup1/backup_files.jsonl $topdir/lsn1/backup_files.jsonl \
  || die "case1: target-dir and extra-lsndir backup_files.jsonl differ"
# Hidden staging file (.backup_files.jsonl.<pid>.staging) must be
# cleaned up.
if ls "$topdir/backup1"/.backup_files.jsonl.*.staging 2>/dev/null | grep -q .; then
  die "case1: staging file leaked into target-dir"
fi

rm -rf $topdir/backup1 $topdir/lsn1

vlog "=== Case 2: --stream=xbstream, then xbstream -x ==="
mkdir -p $topdir/lsn2
xtrabackup --backup --stream=xbstream --extra-lsndir=$topdir/lsn2 \
  > $topdir/backup2.xbs
mkdir -p $topdir/extract2
xbstream -x -C $topdir/extract2 < $topdir/backup2.xbs
validate_jsonl $topdir/extract2/backup_files.jsonl "case2-extract"
validate_jsonl $topdir/lsn2/backup_files.jsonl "case2-lsn"
diff -q $topdir/extract2/backup_files.jsonl $topdir/lsn2/backup_files.jsonl \
  || die "case2: extracted and extra-lsndir backup_files.jsonl differ"

rm -rf $topdir/backup2.xbs $topdir/extract2 $topdir/lsn2

vlog "=== Case 3: encrypted target -- backup_files.jsonl stays plain ==="
mkdir -p $topdir/lsn3
xtrabackup --backup --encrypt=AES256 --encrypt-key="$ENCKEY" \
    --target-dir=$topdir/backup3 --extra-lsndir=$topdir/lsn3
validate_jsonl $topdir/backup3/backup_files.jsonl "case3"
# No .xbcrypt suffix on the manifest file itself.
[ ! -f "$topdir/backup3/backup_files.jsonl.xbcrypt" ] || \
  die "case3: backup_files.jsonl should not have .xbcrypt suffix"
# Per-file encrypt annotation must be recorded.
python3 - <<PY || die "case3: encrypt annotation missing"
import json
lines = [l for l in open("$topdir/backup3/backup_files.jsonl") if l.strip()]
ok = any(json.loads(l).get("encrypt") == "AES256" for l in lines[1:])
assert ok, "case3: no line records encrypt=AES256"
print("case3: encrypt annotation ok")
PY

rm -rf $topdir/backup3 $topdir/lsn3

vlog "=== Case 4: compressed target (zstd) -- backup_files.jsonl stays plain ==="
mkdir -p $topdir/lsn4
xtrabackup --backup --compress=zstd --target-dir=$topdir/backup4 \
    --extra-lsndir=$topdir/lsn4
validate_jsonl $topdir/backup4/backup_files.jsonl "case4"
for ext in .qp .lz4 .zst .xbcrypt ; do
  [ ! -f "$topdir/backup4/backup_files.jsonl$ext" ] || \
    die "case4: backup_files.jsonl should not have $ext suffix"
done
# Per-file compress annotation must be recorded.
python3 - <<PY || die "case4: compress annotation missing"
import json
lines = [l for l in open("$topdir/backup4/backup_files.jsonl") if l.strip()]
ok = any(json.loads(l).get("compress") == "zstd" for l in lines[1:])
assert ok, "case4: no line records compress=zstd"
print("case4: compress annotation ok")
PY

rm -rf $topdir/backup4 $topdir/lsn4

vlog "PASS: backup_files.jsonl produced plain across all output modes"
