# PXB-3862 read-request-cost validation — from a bare machine

Everything needed is in this checkout: the xtrabackup source (this
repo, branch PXB-3862-8.4) and the benchmark tool (pxb-test.tar.gz in
this directory — jinyou's pxb-bench with numeric gap modes enabled;
17 unit tests included). This file is the complete runbook.

## Quick start — build, then one command

```bash
cmake -S . -B build -DBUILD_CONFIG=xtrabackup_release -DWITH_MAN_PAGES=OFF
cmake --build build --target xtrabackup -j"$(nproc)"
./storage/innobase/xtrabackup/test/benchmarks/run_benchmark.sh
```

run_benchmark.sh does everything below automatically: finds the built
binary, deploys (or reuses) a dbdeployer sandbox named pxb3862_bench,
installs the mysqlbackup component, loads test.sbtest1 with sysbench
when missing (reused when present — reruns after a rebuild skip
straight to the sweep), unpacks pxb-bench, generates config.toml,
picks run-vs-restart mode from table size vs buffer pool, runs the
sweep and prints the path of one results tarball to send back.
Defaults: ROWS=10000000, PERCENTS=5:100:5, GAPS=none,0,auto,10,
WORKLOADS=scattered. Override anything per invocation, e.g. on
jinyou's rig:

```bash
SANDBOX=~/sandboxes/msb_8_4_8 ./run_benchmark.sh          # reuse 100M-row sandbox
# or provision fresh from the JIRA layout:
SANDBOX_BINARY=/opt/percona_server MYSQL_VERSION=8.4.8 ROWS=100000000 ./run_benchmark.sh
```

Only the packages in step 0 are prerequisites. The manual steps below
remain the reference for what the script does (and for debugging it).

Path variables used below — set them to taste:

    SRC=~/WORK/pxb-3862-8.4        # this repo checkout
    SB=~/sandboxes/msb_ps8_4_x     # the sandbox (created in step 3)

## 0. Packages (once, ~5 min)

```bash
# Debian/Ubuntu (adjust for RHEL):
sudo apt install -y build-essential cmake bison libssl-dev \
    libncurses-dev libaio-dev libcurl4-openssl-dev libudev-dev \
    libgcrypt-dev libev-dev libproc2-dev \
    python3 python3-pip sysbench wget
```

dbdeployer (single static Go binary; assets are named
dbdeployer-<ver>.linux.tar.gz):

```bash
VER=1.73.0    # or check https://github.com/datacharmer/dbdeployer/releases
mkdir -p ~/bin
wget -q https://github.com/datacharmer/dbdeployer/releases/download/v$VER/dbdeployer-$VER.linux.tar.gz
tar xzf dbdeployer-$VER.linux.tar.gz
mv dbdeployer-$VER.linux ~/bin/dbdeployer && chmod +x ~/bin/dbdeployer
export PATH=$PATH:~/bin        # add to ~/.bashrc for permanence

# one-time init: creates ~/opt/mysql (binaries), ~/sandboxes, and
# fetches the tarball download index
dbdeployer init
```

## 1. Source (one clone brings everything)

```bash
git clone -b PXB-3862-8.4 https://github.com/satya-bodapati/percona-xtrabackup.git $SRC
```

## 2. Build xtrabackup (~15 min)

```bash
cd $SRC
cmake -S . -B build -DBUILD_CONFIG=xtrabackup_release -DWITH_MAN_PAGES=OFF
cmake --build build --target xtrabackup -j"$(nproc)"
# sanity - MUST print the option:
build/runtime_output_directory/xtrabackup --help | grep page-tracking-merge-gap
```

## 3. Server sandbox (~5 min)

```bash
# download a Percona Server 8.4 tarball via dbdeployer's index
# (or wget one from percona.com/downloads and skip this line):
dbdeployer downloads get-by-version 8.4 --flavor percona --newest

# make it deployable, then deploy one sandbox:
dbdeployer unpack Percona-Server-8.4*.tar.gz --prefix ps
dbdeployer deploy single ps8.4.<x>       # <x> from: dbdeployer versions
SB=~/sandboxes/msb_ps8_4_<x>             # dbdeployer prints this path;
                                         # port: grep ^port $SB/my.sandbox.cnf
# recommended for large tables:
printf 'innodb_buffer_pool_size=4G\ninnodb_redo_log_capacity=4G\n' >> $SB/my.sandbox.cnf
$SB/restart
```

## 4. Load the table (100M rows ~ 24GB, ~1h; scale down freely)

```bash
PORT=$(awk -F= '/^port/{gsub(/ /,"");print $2;exit}' $SB/my.sandbox.cnf)
$SB/use -e "CREATE DATABASE IF NOT EXISTS test"
sysbench oltp_insert --tables=1 --table-size=100000000 \
    --create_secondary=off --mysql-db=test --db-driver=mysql \
    --mysql-host=127.0.0.1 --mysql-port=$PORT \
    --mysql-user=msandbox --mysql-password=msandbox prepare
```

## 5. Install the benchmark tool (~2 min)

```bash
tar xzf $SRC/storage/innobase/xtrabackup/test/benchmarks/pxb-test.tar.gz -C $SB
cd $SB/pxb-test && python3 -m pip install --user -e .
```

## 6. config.toml — edit ONLY what has no CLI flag (~1 min)

In `$SB/pxb-test/config.toml` (marked CHANGE-ME):

```toml
[sandbox]    port = <the $PORT from step 4>
[xtrabackup] full_binary        = "<absolute $SRC>/build/runtime_output_directory/xtrabackup"
             incremental_binary = "<absolute $SRC>/build/runtime_output_directory/xtrabackup"
[workload]   total_rows = <rows loaded in step 4>
```

Sweep parameters (percents/gaps/workloads) are NEVER edited here — they
are command-line flags.

## 7. Run the sweep (overnight at 100M rows)

```bash
pxb-bench --workdir $SB run \
    --percents 5:100:5 --gaps none,0,auto,10 --workloads scattered
```

Same densities as the original PXB-3862 chart, so cells overlay it
directly; the `10` column measures what bridging buys wherever auto
declines.

SMALL-TABLE caveat: if the table fits in the buffer pool, dirty pages
never flush by eviction and the tracked set stays empty. Then drive the
individual steps with a restart per density instead of `run`:

```bash
pxb-bench --workdir $SB full-backup
for pct in 5 10 15 20; do
  pxb-bench --workdir $SB change-data $pct --workload scattered
  pxb-bench --workdir $SB restart
  for gap in none 0 auto 10; do
    pxb-bench --workdir $SB inc-backup $pct $gap --workload scattered
  done
done
```

## 8. Send back one tarball

```bash
cd $SB
grep -h "calibrated storage\|typical gap\|request reduction" \
    pxb-test/logs/scattered_*_gap_auto.log > pxb-test/results/decision_lines.txt
tar czf /tmp/pxb3862_results.tgz pxb-test/results/ pxb-test/logs/
```

## 9. Predictions (split-disk, ~165us / ~1.5GB/s class storage)

| density | old build (÷2, budget ~125KB) | this build (÷1.5, ~160KB) |
|---|---|---|
| 5%      | refused (correct, beats full scan) | identical |
| **10%** | **refused -> 28.4s**               | **bridged -> ~17s = pin-10** |
| 15-100% | bridged (wins/ties)                | identical |

On fast shared-disk NVMe the 10% verdict is expected to be the
OPPOSITE (refusal, with pin-10 measurably slower) — each machine
correctly for its own storage; the calibration and refusal log lines
state each decision's numbers.
