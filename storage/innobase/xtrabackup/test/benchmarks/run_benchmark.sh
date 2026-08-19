#!/bin/bash
############################################################################
# PXB-3862 one-command benchmark driver.
#
# Your job:   build xtrabackup, then run this script from anywhere:
#
#     cmake -S . -B build -DBUILD_CONFIG=xtrabackup_release -DWITH_MAN_PAGES=OFF
#     cmake --build build --target xtrabackup -j"$(nproc)"
#     ./storage/innobase/xtrabackup/test/benchmarks/run_benchmark.sh
#
# The script does the rest: finds the binary, provisions a dbdeployer
# sandbox (or reuses one), installs the mysqlbackup component, loads
# test.sbtest1 with sysbench if missing, unpacks the bundled pxb-bench,
# generates its config.toml, picks the right run mode for the table
# size, runs the sweep and collects one results tarball.
#
# Everything is overridable via environment variables:
#   XTRABACKUP=/abs/path      binary (default: newest of build/, bld_rel/,
#                             bld/ runtime_output_directory/xtrabackup)
#   SANDBOX=/path/to/sandbox  reuse an existing dbdeployer sandbox
#                             (default: deploy ~/sandboxes/pxb3862_bench)
#   MYSQL_TARBALL=/path.tar.xz  Percona Server 8.x tarball to unpack when
#                             dbdeployer has no version yet (component
#                             mysqlbackup requires Percona Server)
#   MYSQL_VERSION=ps8.4.8    which unpacked version to deploy (default:
#                             newest 8.x that `dbdeployer versions` shows;
#                             when NOTHING suitable is unpacked the script
#                             downloads Percona Server PS_VERSION itself,
#                             bootstrap.sh-style, from downloads.percona.com)
#   PS_VERSION=8.4.8-8        full PS version for that auto-download
#   SANDBOX_BINARY=/opt/percona_server   dbdeployer binary dir when the
#                             unpacked servers are not in ~/opt/mysql
#   ROWS=10000000             sbtest1 rows loaded when the table is
#                             missing (10M ~ 2.4GB; the original chart
#                             used 100000000)
#   PERCENTS=5:100:5          sweep densities (start:stop:step, stop INCLUSIVE, or list)
#   GAPS=none,0,auto,160K     gap modes per density (byte distances, K/M suffix)
#   WORKLOADS=scattered       scattered and/or contiguous
#
# Requirements on the host: bash, python3 >= 3.9, wget; mysql client
# libs come with the sandbox; sysbench only if sbtest1 must be loaded;
# curl only if dbdeployer must be installed.
############################################################################
set -euo pipefail

say() { echo; echo "=== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

KIT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$KIT_DIR/../../../../.." && pwd)
PS_VERSION=${PS_VERSION:-8.4.8-8}

# glibc variant of the Percona Server tarball, as in test/bootstrap.sh
tarball_glibc() {
  local glibc
  glibc=$(ldd --version | head -1 | awk '{print $NF}')
  case $glibc in
    2.12 | 2.17 | 2.27 | 2.28 | 2.31 | 2.34 | 2.35) echo "$glibc" ;;
    2.36 | 2.39) echo 2.35 ;;
    *) die "no Percona Server tarball for glibc $glibc - set MYSQL_TARBALL=" ;;
  esac
}

# download PS $PS_VERSION from downloads.percona.com (bootstrap.sh URLs)
download_ps_tarball() {
  local arch tarball url fallback cache
  arch=$(uname -m)
  tarball="Percona-Server-${PS_VERSION}-Linux.${arch}.glibc$(tarball_glibc)-minimal.tar.gz"
  url="https://downloads.percona.com/downloads/Percona-Server-8.4/Percona-Server-${PS_VERSION}/binary/tarball"
  fallback="https://downloads.percona.com/downloads/TESTING/ps-${PS_VERSION}"
  cache=$HOME/.cache/pxb3862
  mkdir -p "$cache"
  if [ ! -s "$cache/$tarball" ]; then
    say "downloading $tarball"
    if wget -q --spider "$url/$tarball"; then
      wget -qc -O "$cache/$tarball" "$url/$tarball"
    elif wget -q --spider "$fallback/$tarball"; then
      wget -qc -O "$cache/$tarball" "$fallback/$tarball"
    else
      die "PS $PS_VERSION tarball not found at percona.com - set MYSQL_TARBALL="
    fi
  fi
  MYSQL_TARBALL=$cache/$tarball
}

ROWS=${ROWS:-10000000}
PERCENTS=${PERCENTS:-5:100:5}
GAPS=${GAPS:-none,0,auto,160K}
WORKLOADS=${WORKLOADS:-scattered}

############################################################################
say "1/6 locate the xtrabackup binary"
############################################################################
if [ -z "${XTRABACKUP:-}" ]; then
  for c in "$REPO"/build/runtime_output_directory/xtrabackup \
           "$REPO"/bld_rel/runtime_output_directory/xtrabackup \
           "$REPO"/bld/runtime_output_directory/xtrabackup; do
    [ -x "$c" ] && XTRABACKUP=$c && break
  done
fi
[ -n "${XTRABACKUP:-}" ] && [ -x "${XTRABACKUP}" ] \
    || die "no xtrabackup binary found under $REPO; build it or set XTRABACKUP="
# capture --help first: grep -q would SIGPIPE the binary and trip pipefail
HELP=$("$XTRABACKUP" --help 2>/dev/null || true)
echo "$HELP" | grep -q page-tracking-combine-distance \
    || die "$XTRABACKUP has no --page-tracking-combine-distance (wrong branch?)"
echo "using $XTRABACKUP"

############################################################################
say "2/6 sandbox"
############################################################################
if [ -z "${SANDBOX:-}" ]; then
  if ! command -v dbdeployer >/dev/null; then
    say "installing dbdeployer 1.76.0 into ~/.local/bin"
    mkdir -p "$HOME/.local/bin"
    curl -sL https://github.com/datacharmer/dbdeployer/releases/download/v1.76.0/dbdeployer-1.76.0.linux.tar.gz \
        | tar xz -C "$HOME/.local/bin"
    mv "$HOME/.local/bin/dbdeployer-1.76.0.linux" "$HOME/.local/bin/dbdeployer"
    chmod +x "$HOME/.local/bin/dbdeployer"
    export PATH="$HOME/.local/bin:$PATH"
    dbdeployer init --skip-all-downloads --skip-shell-completion 2>/dev/null || true
  fi
  DBD=(dbdeployer)
  [ -n "${SANDBOX_BINARY:-}" ] && DBD+=(--sandbox-binary "$SANDBOX_BINARY")
  SANDBOX=$HOME/sandboxes/pxb3862_bench
  if [ ! -x "$SANDBOX/use" ]; then
    if [ -n "${MYSQL_TARBALL:-}" ]; then
      "${DBD[@]}" unpack "$MYSQL_TARBALL" 2>/dev/null || true
    fi
    VERSION=${MYSQL_VERSION:-$("${DBD[@]}" versions | tr ' ' '\n' | grep -E '8\.[0-9]' | tail -1 || true)}
    if [ -z "$VERSION" ] || ! "${DBD[@]}" versions | grep -qw "$VERSION"; then
      # bare machine (or the requested version is not unpacked yet):
      # fetch Percona Server ourselves and unpack it
      download_ps_tarball
      "${DBD[@]}" unpack "$MYSQL_TARBALL" 2>/dev/null || true
      # dbdeployer names the unpacked dir 8.4.8 or ps8.4.8 depending on
      # how it detected the flavor - accept either
      VERSION=$("${DBD[@]}" versions | tr ' ' '\n' \
          | grep -wE "(ps)?${PS_VERSION%%-*}" | tail -1 || true)
      [ -n "$VERSION" ] || die "unpacking $MYSQL_TARBALL did not yield ${PS_VERSION%%-*}"
    fi
    say "deploying sandbox $SANDBOX from $VERSION"
    "${DBD[@]}" deploy single "$VERSION" --sandbox-directory pxb3862_bench
  fi
fi
[ -x "$SANDBOX/use" ] || die "$SANDBOX is not a dbdeployer sandbox (no ./use)"
"$SANDBOX/use" -N -e "SELECT 1" >/dev/null 2>&1 || "$SANDBOX/start"
PORT=$(grep -E '^port' "$SANDBOX/my.sandbox.cnf" | head -1 | tr -dc 0-9)
echo "sandbox $SANDBOX (port $PORT)"

HAS_COMPONENT=$("$SANDBOX/use" -N -e \
  "SELECT COUNT(*) FROM mysql.component WHERE component_urn='file://component_mysqlbackup'")
if [ "$HAS_COMPONENT" -eq 0 ]; then
  "$SANDBOX/use" -e 'INSTALL COMPONENT "file://component_mysqlbackup"' \
      || die "cannot install component_mysqlbackup - is this Percona Server?"
fi

############################################################################
say "3/6 test.sbtest1"
############################################################################
"$SANDBOX/use" -e "CREATE DATABASE IF NOT EXISTS test"
COUNT=$("$SANDBOX/use" -N -e "SELECT COUNT(*) FROM test.sbtest1" 2>/dev/null || echo 0)
if [ "$COUNT" -eq 0 ]; then
  command -v sysbench >/dev/null \
      || die "test.sbtest1 missing and sysbench not installed (apt install sysbench)"
  say "loading $ROWS rows with sysbench (dense PK order)"
  sysbench oltp_insert --threads=1 --tables=1 --table-size="$ROWS" \
      --create_secondary=off --mysql-db=test --db-driver=mysql \
      --mysql-storage-engine=InnoDB --mysql-host=127.0.0.1 \
      --mysql-port="$PORT" --mysql-user=msandbox --mysql-password=msandbox prepare
  COUNT=$ROWS
else
  echo "test.sbtest1 already has $COUNT rows - reusing"
fi

############################################################################
say "4/6 bundled pxb-bench + config.toml"
############################################################################
# always refresh the tool from the bundled tarball so a rebuilt kit (new
# option names, fixes) is picked up on reruns; logs/ and results/ live in
# the sandbox itself and config.toml is regenerated below
tar xzf "$KIT_DIR/pxb-test.tar.gz" -C "$SANDBOX"
CFG=$SANDBOX/pxb-test/config.toml
sed -i -E "s|^port = .*|port = $PORT|" "$CFG"
sed -i -E "s|^full_binary = .*|full_binary = \"$XTRABACKUP\"|" "$CFG"
sed -i -E "s|^incremental_binary = .*|incremental_binary = \"$XTRABACKUP\"|" "$CFG"
sed -i -E "s|^total_rows = .*|total_rows = $COUNT|" "$CFG"
PXB="env PYTHONPATH=$SANDBOX/pxb-test python3 -m pxb_bench --workdir $SANDBOX"

############################################################################
say "5/6 run the sweep (percents=$PERCENTS gaps=$GAPS workloads=$WORKLOADS)"
############################################################################
# pxb-bench's `run` loop relies on buffer-pool eviction flushing dirtied
# pages before the incremental. That holds when the table exceeds the
# buffer pool; a smaller table needs an explicit flush+restart between
# dirtying and backing up, so drive the individual steps instead.
"$SANDBOX/use" -N -e "ANALYZE TABLE test.sbtest1" >/dev/null
DATA=$("$SANDBOX/use" -N -e "SELECT data_length FROM information_schema.tables \
    WHERE table_schema='test' AND table_name='sbtest1'")
BP=$("$SANDBOX/use" -N -e "SELECT @@innodb_buffer_pool_size")

if [[ $PERCENTS == *:* ]]; then
  IFS=: read -r P_START P_STOP P_STEP <<<"$PERCENTS"
  PCT_LIST=$(seq "$P_START" "$P_STEP" "$P_STOP")   # pxb-bench treats stop as inclusive
else
  PCT_LIST=${PERCENTS//,/ }
fi

if [ "$DATA" -gt "$BP" ]; then
  echo "table (${DATA} B) > buffer pool (${BP} B): using pxb-bench run"
  $PXB run --percents "$PERCENTS" --gaps "$GAPS" --workloads "$WORKLOADS"
else
  echo "table (${DATA} B) <= buffer pool (${BP} B): stepping with restarts"
  mkdir -p "$SANDBOX/results"
  STEP_CSV=$SANDBOX/results/step_times.csv
  echo "workload,percent,gap,sbtest1_seconds" >"$STEP_CSV"
  for workload in ${WORKLOADS//,/ }; do
    $PXB full-backup
    for pct in $PCT_LIST; do
      $PXB change-data "$pct" --workload "$workload"
      $PXB restart
      for gap in ${GAPS//,/ }; do
        $PXB inc-backup "$pct" "$gap" --workload "$workload"
        LOG=$SANDBOX/logs/${workload}_${pct}_gap_${gap}.log
        SECS=$($PXB parse-log "$LOG" 2>/dev/null | awk '{print $1}' || true)
        echo "$workload,$pct,$gap,$SECS" >>"$STEP_CSV"
      done
    done
  done
  column -t -s, "$STEP_CSV"
fi

############################################################################
say "6/6 collect results"
############################################################################
# pxb-bench resolves logs/ and results/ against --workdir (the sandbox)
grep -h "calibrated storage\|typically\|request reduction" \
    "$SANDBOX"/logs/*_gap_auto.log \
    >"$SANDBOX/results/decision_lines.txt" 2>/dev/null || true
OUT=/tmp/pxb3862_results_$(date +%Y%m%d_%H%M%S).tgz
tar czf "$OUT" -C "$SANDBOX" results logs pxb-test/config.toml
say "done - send back: $OUT"
