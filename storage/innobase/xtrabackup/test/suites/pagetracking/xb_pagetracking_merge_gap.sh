############################################################################
# PXB-3862: --page-tracking-merge-gap groups changed pages into read ranges
#
# Plan: dirty a known fraction of one table's pages (15%, 25%, 50%), then
# take three incrementals from the same base at merge-gap = 0 / 65536 / auto
# and verify each against the two log lines the feature prints:
#
#   pagetracking: test/t1.ibd: <N> changed pages in <R> ranges (avg gap
#     <G> pages); merge-gap=<M> [(auto)] combined them into <K> reads: ...
#        -> the PREDICTION, computed from the changed-page set
#   ... issued <K> read batches (end of the same line)
#        -> the ACTUAL read requests performed for the file
#
# Only hardware-independent facts are asserted:
#   per run:    merge-gap=0     -> nothing merged, and actual == predicted
#               merge-gap=65536 -> one logical group (few actual reads)
#               auto          -> reported as (auto)
#   cross-run:  auto never issues more reads than strict; at 50% changed
#               the gaps are ~1-2 pages, far below the 64KB cost FLOOR,
#               so any measured cost combines them -> >= 4x fewer reads.
#               (At 15%/25% the gap distribution's tail crosses the floor
#               bound of 4 pages, so only the weak comparison is portable.)
#   correctness: the restored auto backup equals the source
#   ceiling:    two regions > 1MB apart are never merged (cost CEILING)
# Deliberately NOT asserted: timings, absolute page counts, and auto's
# chosen gap at 15% (gaps ~5-6 pages sit in the zone where the measured
# cost may legitimately combine or refuse).
############################################################################

. inc/common.sh

start_server

############################################################################
# Fixture: ~512k fixed-width rows, ~50 rows per 16KB page => ~10500 data
# pages (~160MB), loaded in PK order so page order follows id order.
# Sized so the sparsest sweep case stays well above the 1000-changed-page
# logging threshold (the changed count has a fixed overhead component and
# does not scale linearly with touched rows).
# Dirtying one row per K ids dirties roughly every (K/50)th page - a
# uniform stride whose gap we control through K. The file exceeds the
# probe's 64MB minimum, so auto runs exercise the measured read request
# cost rather than the fallback.
############################################################################
vlog "load ~10500 pages of fixed-width rows (PK order == page order)"
mysql test <<EOF
CREATE TABLE t1 (id INT AUTO_INCREMENT PRIMARY KEY, pad CHAR(250) NOT NULL)
    ENGINE=InnoDB;
INSERT INTO t1(pad) VALUES (REPEAT('a', 250));
INSTALL COMPONENT "file://component_mysqlbackup";
EOF
for i in $(seq 1 19); do
  mysql test -e "INSERT INTO t1(pad) SELECT pad FROM t1"
done
# id-based strides need the real id-to-page geometry: INSERT..SELECT
# doubling leaves auto-increment holes, so "id % K" cannot be derived
# from an assumed rows-per-page. ANALYZE refreshes the stats the stride
# formula below reads.
mysql test -e "ANALYZE TABLE t1" >/dev/null

t1_space=$(mysql -Ns -e \
  "SELECT space FROM information_schema.innodb_tables WHERE name='test/t1'")
vlog "t1 space id: $t1_space"

############################################################################
# Log parsing helpers
############################################################################

# Extract one number from t1's grouping (prediction) line; $2 is a sed -E
# expression whose capture group selects the wanted field.
grouping_field() { # $1=log $2=sed-expr
  grep -m1 "pagetracking: .*t1.ibd: .* changed pages" "$1" | sed -E "$2"
}

# The read requests actually issued for t1: reported at the end of the
# grouping line, which prints when the file closes - every number in it
# describes what actually happened (counters accumulate during the copy).
issued_batches() { # $1=log
  grouping_field "$1" 's/.*issued ([0-9]+) read batches.*/\1/'
}

############################################################################
# take_and_verify <label> <setting>
#
# Takes one incremental from full_<label> at --page-tracking-merge-gap=
# <setting> and verifies everything that is knowable from this run alone
# (the per-setting invariants). Numbers needed for CROSS-run comparisons
# are left in LAST_GROUPS / LAST_BATCHES; the caller copies them before
# the next call overwrites them.
############################################################################
take_and_verify() {
  local label=$1 setting=$2
  local tgt=$topdir/inc_${label}_${setting}
  local gap_arg=""
  [ "$setting" != "auto" ] && gap_arg="--page-tracking-merge-gap=$setting"

  xtrabackup --backup --page-tracking $gap_arg \
      --incremental-basedir=$topdir/full_$label --target-dir=$tgt \
      2>&1 | tee $tgt.log

  local changed ranges groups batches
  changed=$(grouping_field $tgt.log 's/.*: ([0-9]+) changed pages.*/\1/')
  ranges=$(grouping_field $tgt.log 's/.* in ([0-9]+) ranges.*/\1/')
  groups=$(grouping_field $tgt.log 's/.*into ([0-9]+) reads.*/\1/')
  batches=$(issued_batches $tgt.log)

  vlog "$label merge-gap=$setting: changed=$changed ranges=$ranges" \
       "predicted_reads=$groups issued_reads=$batches"

  # the grouping line only prints for files with >= 1000 changed pages;
  # if it is missing, either the fixture got too small or logging broke
  [ -n "$changed" ] && [ "$changed" -ge 1000 ] \
      || die "$label/$setting: grouping log line missing or table too small"

  case "$setting" in
    0)
      # strict grouping: no gap may be merged (prediction == range count),
      # and the reads actually issued must equal the prediction - one
      # pread per range, nothing spilling past a run
      [ "$groups" -eq "$ranges" ] \
          || die "$label: merge-gap=0 must not group: $groups != $ranges"
      [ "$batches" -eq "$groups" ] \
          || die "$label: strict issued $batches reads, predicted $groups"
      ;;
    65536)
      # everything merges into ONE logical group; more than one actual
      # read is legal only because a group larger than --read-buffer-size
      # (10MB) is read in buffer-sized pieces (~160MB table -> ~17), so
      # allow up to 24. This actual-reads assert is the one that catches
      # a combining regression hiding behind a correct prediction (the
      # sabotage exercise failed exactly here).
      [ "$groups" -eq 1 ] \
          || die "$label: merge-gap=65536 must form one group, got $groups"
      [ -n "$batches" ] && [ "$batches" -le 24 ] \
          || die "$label: one group must be few reads, issued $batches"
      ;;
    auto)
      # auto's numeric choices depend on the measured cost, so per-run
      # we only verify the mode was really in effect. Anchor on the
      # grouping line specifically: the calibration line also matches a
      # loose "t1.ibd" pattern and carries no "(auto)"
      grouping_field $tgt.log 's/.*/&/' | grep -q "(auto)" \
          || die "$label: auto not reported in the grouping log line"
      ;;
  esac

  LAST_GROUPS=$groups
  LAST_BATCHES=$batches
}

############################################################################
# Density sweep: per density one base backup, the three incrementals with
# their per-run checks inside take_and_verify, then the two cross-run
# comparisons that define auto's contract against strict.
############################################################################
# each dataset writes a DISTINCT pad value: the strides overlap (164
# divides 328), and updating a row to the value it already holds is a
# no-op that dirties no page - with one shared value the 50% dataset
# would silently lose every row the 25% dataset already touched
for spec in 15:b 25:c 50:d; do
  pct=${spec%%:*}
  letter=${spec##*:}
  # stride in ids that touches ~one row per (100/pct) pages, derived from
  # the table's real id density and page count so auto-increment holes
  # and fill factor cancel out (same formula as the PXB-3862 benchmarks):
  # rows-per-page = (AUTO_INCREMENT-1) / pages, stride = (100/pct) * that.
  # Resulting page gaps: 15% -> ~5-6 (above the 64KB cost floor of 4
  # pages: the measured cost decides), 25% -> ~3, 50% -> ~1 (below the
  # floor: combined at any cost).
  mod=$(mysql -Ns test -e "SELECT GREATEST(1, ROUND(100 / $pct * \
      (AUTO_INCREMENT - 1) / (DATA_LENGTH / @@innodb_page_size))) \
      FROM information_schema.tables WHERE table_name = 't1'")
  vlog "=== dataset: ~${pct}% of pages changed (one row per $mod ids)"
  xtrabackup --backup --page-tracking --target-dir=$topdir/full_$pct

  mysql test -e \
    "UPDATE t1 SET pad = REPEAT('$letter', 250) WHERE id % $mod = 0"

  # clean shutdown, NOT stop_server (kill -9): page tracking records pages
  # when they are flushed, and the incremental needs the checkpoint past
  # the base's to_lsn or pagetracking::init skips the component entirely;
  # a clean shutdown guarantees both, recovery after kill guarantees
  # neither
  shutdown_server
  start_server
  if [ $pct -eq 50 ]; then
    record_db_state test
  fi

  take_and_verify $pct 0
  batches_strict=$LAST_BATCHES
  take_and_verify $pct 65536
  take_and_verify $pct auto
  batches_auto=$LAST_BATCHES

  # cross-run floor of auto's contract: whatever cost was measured,
  # auto must never do MORE read requests than strict grouping
  [ "$batches_auto" -le "$batches_strict" ] \
      || die "pct=$pct: auto issued more reads than strict:" \
             "$batches_auto > $batches_strict"

  if [ $pct -ge 50 ]; then
    # at 50% the mean gap is ~1 page (16KB), so even with page-fill
    # variance essentially the whole gap distribution stays under the
    # 64KB cost floor of 4 pages: combined at any measured cost on
    # any hardware, and the request count must collapse. At 15%/25% the
    # distribution's tail crosses the floor bound, so those densities
    # only carry the weak assert above.
    [ $((batches_auto * 4)) -le "$batches_strict" ] \
        || die "pct=$pct: auto must combine sub-floor gaps:" \
               "$batches_auto vs $batches_strict"
  fi
done

############################################################################
# Correctness: filler pages are read but never written (the write filter
# drops them by LSN), so the prepared auto backup must reproduce the
# source exactly.
############################################################################
vlog "restore the 50% auto incremental and verify content"
stop_server
rm -rf $mysql_datadir
xtrabackup --prepare --apply-log-only --target-dir=$topdir/full_50
xtrabackup --prepare --target-dir=$topdir/full_50 \
    --incremental-dir=$topdir/inc_50_auto
xtrabackup --copy-back --target-dir=$topdir/full_50 --datadir=$mysql_datadir
start_server
verify_db_state test

############################################################################
# Budget ceiling: two dense changed regions (~600 + ~560 pages) separated
# by ~1400 untouched pages (~22MB >> the 1MB cost ceiling) must never be
# merged into one read, for any measured cost.
############################################################################
vlog "=== two far-apart regions: a gap > 1MB is never combined"
xtrabackup --backup --page-tracking --target-dir=$topdir/full_far
mysql test -e \
  "UPDATE t1 SET pad = REPEAT('z', 250) WHERE id <= 30000 OR id > 100000"
shutdown_server
start_server

take_and_verify far auto
[ "$LAST_GROUPS" -ge 2 ] \
    || die "a >1MB gap must never be combined, got groups=$LAST_GROUPS"
