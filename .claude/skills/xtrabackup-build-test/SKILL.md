---
name: xtrabackup-build-test
description: Build Percona XtraBackup and run its test framework — per-version-line differences (8.0/8.4/9.7/trunk boost and server-tarball requirements), cmake configure (Debug/RelWithDebInfo/ASAN/Docker), rebuild-before-test discipline, bootstrap.sh server download, run.sh usage and worker sizing (kernel AIO limits), xbcloud MinIO setup. Use whenever asked to build xtrabackup, run its tests, or test changes on a specific version line or distro.
---

# Build and test Percona XtraBackup

Full reference: `CLAUDE.md` at the repo root. This skill is the procedure.

## 1. One-time setup

```bash
git submodule update --init --recursive   # ALWAYS first in fresh clones/worktrees
```

Dependencies (root): `sudo bash storage/innobase/xtrabackup/utils/percona-xtrabackup-8.0_builder.sh --builddir=/tmp/pxb-deps --install_deps=1`
(manual apt list in CLAUDE.md if the script is unsuitable).

## 2. Know which version line you are on

Branches: `8.0`, `8.4`, `9.7`, `trunk`. Ask/check `git rev-parse
--abbrev-ref HEAD` first — the differences below bite silently.

| | 8.0 | 8.4 | 9.7 | trunk |
|---|---|---|---|---|
| bundled `extra/boost` | 1.84 | 1.84 | 1.87 | 1.87 |
| version cmake wants | **1.77** | 1.84 | 1.87 | 1.87 |
| needs `-DDOWNLOAD_BOOST=1 -DWITH_BOOST=<dir>` | **YES** | no | no | no |
| `bootstrap.sh` default server | 8.0.35-27 | 8.4.4-4 | 9.7.0-1 | 9.7.0-1 |

- **Only 8.0 needs the boost download** — its bundled `extra/boost` is 1.84
  while `cmake/boost.cmake` asks for `boost_1_77_0`, so the in-tree copy does
  not satisfy it. On 8.4/9.7/trunk the bundled version matches what cmake
  asks for; configure with no boost flags at all (verified on 9.7).
  Passing the download flags anyway is harmless, just slower.
- **The test server tarball must match the branch's major version.** A 9.7
  `xtrabackup` against an 8.4 server (or vice versa) fails in confusing ways.
  Reuse an existing `test/server` from another build tree of the SAME line
  instead of re-running bootstrap:
  `cp -a <other-bld>/storage/innobase/xtrabackup/test/server <bld>/storage/innobase/xtrabackup/test/`
- Backporting one fix across lines means one worktree + branch per line, each
  based on that line's own upstream head — never cherry-pick across without
  rebuilding and re-testing per line.

## 3. Configure — pick the right build dir and type

| ask | dir | cmake type |
|---|---|---|
| release-equivalent test/benchmark | `build/release/` | `RelWithDebInfo` (NEVER plain `Release`) |
| debug / `require_debug_pxb_version` tests | `build/debug/` | `Debug` |
| sanitizer run | `build/asan/` | `Debug` + `-DWITH_ASAN=ON` |
| "test on Ubuntu X in docker" | `build/docker_<distro>/<type>/` | as requested |

User-named dirs (e.g. `bld/`, `bld_rel/`) override the defaults; reuse an
existing configured build dir instead of configuring a new one.

```bash
cmake -S . -B build/release \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_LTO=OFF \
    -DDOWNLOAD_BOOST=1 -DWITH_BOOST=$HOME/boost \
    -DWITH_ROUTER=OFF -DWITH_NDB=OFF -DWITH_MAN_PAGES=OFF -DWITH_UNIT_TESTS=ON
```

`-DWITH_LTO=OFF` is mandatory on newer Ubuntu (default `-flto=auto` breaks
the link). For a build-team-faithful release build add
`-DBUILD_CONFIG=xtrabackup_release -DMINIMAL_RELWITHDEBINFO=OFF
-DWITH_ZLIB=bundled -DWITH_ZSTD=bundled` (see CLAUDE.md).

## 4. Build — and REBUILD before every test run

```bash
make -C build/release -j$(nproc)     # NO target name - build everything
```

**Pass no target.** `make ... xtrabackup` builds only the binary: it leaves
`<bld>/plugin_output_directory` empty (which silently breaks every keyring
test, see step 6) and skips the `link_test_dir` ALL target that syncs tests
into the build tree. Name the `xtrabackup` target only for a quick compile
check you will not test with; building all afterwards is cheap since the
objects are shared.

After ANY source or test change: rebuild (which re-runs `link_test_dir`), or
copy the single changed test in by hand
(`cp storage/innobase/xtrabackup/test/t/y.sh <bld>/storage/innobase/xtrabackup/test/t/`).
Verify binary freshness by mtime — never by `--version` revision id (that is
configure-time).

## 5. Test framework — always from the BUILD tree

```bash
cd <bld>/storage/innobase/xtrabackup/test
./bootstrap.sh                     # once: downloads PS server into ./server
./run.sh -f -j 128                 # EVERYTHING: no -s means all suites
./run.sh -f -t t/<test>.sh         # one test from the main suite
./run.sh -f -t suites/<suite>/<test>.sh   # one test from a named suite
./run.sh -f -j 128 -s default -s pagetracking   # selected suites only
```

- **No `-s` runs every suite** — that is the normal full-regression command.
  Only `xbcloud` and `keyring` need external setup; the rest are self-contained.
- **`-s default` is the main `t/*.sh` suite.** There is NO `-s main`, despite
  what `run.sh -h` prints: `-s X` expands to `suites/X/*.sh`, so `-s main`
  silently becomes a literal unmatched glob and "runs" one bogus failing test.
- `results/` is wiped every run — inspect failure logs immediately.

### Sizing `-j`: cores, but bounded by kernel AIO

run.sh auto-caps at 16 workers no matter how big the box is, so always pass
`-j` explicitly. But cores are not the only limit: **every worker boots
InnoDB twice over — once for its `mysqld` and again inside `xtrabackup`
itself** (backup and prepare both start an InnoDB instance). Each InnoDB boot
calls `io_setup()` for its AIO segments — roughly
`(innodb_read_io_threads + innodb_write_io_threads + 1) x 256` events, so
~2.3k with the framework's defaults (it sets no IO-thread options, so server
defaults of 4+4 apply).

When the kernel runs out, InnoDB cannot start and the test dies with a
failure that looks nothing like a code bug:

```
[Warning] [MY-012582] [InnoDB] io_setup() failed with EAGAIN. Will make 5 attempts...
[ERROR]   [MY-012584] [InnoDB] io_setup() failed with EAGAIN after 5 attempts.
[ERROR]   [MY-012954] [InnoDB] Cannot initialize AIO sub-system
```

**Always triage a big failure count this way first:**

```bash
grep -l "io_setup() failed" results/* | wc -l    # vs total failures
```

If that is most of them, the run is environmental noise — raise the ceiling
and rerun, do not go bug-hunting. Prefer raising the limit over shrinking `-j`:

```bash
cat /proc/sys/fs/aio-max-nr                 # ceiling (often 1048576)
cat /proc/sys/fs/aio-nr                     # in use right now
sudo sysctl -w fs.aio-max-nr=8388608        # 8x headroom; transient
# persist across reboots:
echo 'fs.aio-max-nr = 8388608' | sudo tee /etc/sysctl.d/99-pxb-aio.conf
```

If you cannot get root, cut `-j` instead (halve it per EAGAIN recurrence).

Also raise the open-file limit — the distro soft limit is often 1024, which
is per-process and too low (xtrabackup logs `open files limit requested 0,
set to 1024`). The hard limit is normally large (524288 on the epyc hosts),
so no root is needed:

```bash
ulimit -n 100000        # in the shell that runs run.sh
```

`fs.file-max`, `kernel.pid_max`, `vm.max_map_count` and `kernel.threads-max`
are ample by default; do not bother tuning them.

- **Never run two suites concurrently on one box.** Two `-j64` runs = 128
  mysqlds + 128 xtrabackups contending for the same global `fs.aio-max-nr`;
  that is what produces a sudden ~190-failure run where nearly every failure
  is `io_setup`. Run one branch to completion, then the next.
- Peak AIO is bursty — many servers initializing at the same instant — so a
  low `aio-nr` sample proves nothing. Trust the `io_setup` grep, not sampling.
- bootstrap glibc gotcha: on very new glibc it refuses; manually download
  the `glibc2.35` tarball (release area or `downloads/TESTING/ps-<ver>`)
  and `tar -xf ... -C server --strip-components=1`.
- Debug-gated tests need the Debug build (`bld/`); `require_debug_server`
  additionally needs a debug mysqld tarball.

## 6. External services: xbcloud / vault / kmip / kms

Only these suites need a server, and each **skips cleanly** without it — a
missing service is a skip, never a failure. So set them up only when the ask
actually covers them.

| suite | service | gate when absent |
|---|---|---|
| `suites/xbcloud/*` | S3 (MinIO) | `XBCLOUD_CREDENTIALS` unset → skip |
| `suites/keyring/*vault*` | Vault | `keyring_vault_ping` → skip |
| `suites/keyring/*kmip*` | PyKMIP | `ping_kmip` → skip |
| `suites/keyring/*kms*` | **real AWS KMS** | `ping_kms` → skip; cannot be faked |

Images are **`satyapercona/*`** — what the functional CI uses. The
authoritative reference is `jenkins-pipelines/pxb/v2/docker/run-test`; read
it when in doubt. (`altmannmarcelo/*` is stale — its Vault cert expired
2024-12-17 and the container then emits an empty token.)

**Keyring needs a full build.** `ls <bld>/plugin_output_directory | wc -l`
must be non-empty before you trust any keyring result. `make xtrabackup`
creates that dir empty, and run.sh's `test -d` then prefers it over the
server tarball's populated `lib/plugin/`, so every keyring test dies with
"failed to init keyring component" — environment, not a bug. Fix: build with
no target (`make -C <bld> -j64`).

Docker preflight FIRST: `docker ps` must work without sudo. If it fails,
`sudo usermod -aG docker "$USER"`, then use `sg docker -c '...'` in the
current shell. Never sudo-prefix docker commands. Host also needs `socat`
(for `ping_kmip`), `uuidgen`, `curl`.

Already inside a container (`[ -f /.dockerenv ]` or docker/containerd in
/proc/1/cgroup)? Build/compile/native tests: proceed normally. Docker-
dependent asks: NEVER docker-in-docker — if the host socket is mounted
and `docker ps` works, started containers are host SIBLINGS (reach via
pxb_network by name; `-v` needs HOST paths, so distro builds still belong
on the host); otherwise refuse the docker part with a clear message and
do the native equivalent instead.

```bash
docker network create pxb_network            # idempotent
docker run -d --rm --network pxb_network -p 9000:9000 -p 9001:9001 \
    --security-opt seccomp=unconfined --cap-add=NET_ADMIN \
    --env USER=myuser --env PASSWORD=someStrongPWD --name s3 satyapercona/minio:latest
docker run -d --rm --network pxb_network -p 8200:8200 \
    --security-opt seccomp=unconfined --cap-add=NET_ADMIN --name vault satyapercona/vault:latest
docker run -d --rm --network pxb_network -p 5696:5696 \
    --security-opt seccomp=unconfined --cap-add=NET_ADMIN --name kmip satyapercona/kmip:latest

# certs/creds live inside the containers; copy them out once
E=/bigdisk_kioxia/satya/pxb-testenv; mkdir -p $E
docker cp vault:/opt/vault/tls/tls.crt $E/vault.crt
docker cp kmip:/opt/certs/root_certificate.pem $E/
docker cp kmip:/opt/certs/client_key_jane_doe.pem $E/
docker cp kmip:/opt/certs/client_certificate_jane_doe.pem $E/
docker cp s3:/usr/bin/mc $E/mc && chmod +x $E/mc   # xbcloud tests drive mc
# vault/kmip TLS certs are issued for these names, so they must resolve
echo "127.0.0.1 local.vault.com" | sudo tee -a /etc/hosts
echo "127.0.0.1 local.kmip.com"  | sudo tee -a /etc/hosts
```

On the epyc lab hosts this is already done and sourceable — it also prints a
health line for all three services:

```bash
. /bigdisk_kioxia/satya/pxb-testenv/env.sh && ./run.sh -f -j 128
```

Otherwise export: `XBCLOUD_CREDENTIALS` (no inner quotes — common.sh's parser
breaks on them), `XBCLOUD_MC`, `VAULT_URL=https://local.vault.com:8200`,
`VAULT_CA`/`VAULT_CACERT`, `VAULT_TOKEN`, `KMIP_SERVER_ADDR=local.kmip.com`,
`KMIP_SERVER_PORT=5696`, `KMIP_SERVER_CA`, `KMIP_CLIENT_CA`,
`KMIP_CLIENT_KEY`. Full contract in CLAUDE.md.

**Vault's dev root token is regenerated on every container restart** — never
pin it; read it back with
`docker logs vault | grep 'export VAULT_TOKEN' | cut -d= -f2`.

**Check cert expiry before blaming a test**:
`docker exec kmip openssl x509 -in /opt/certs/root_certificate.pem -noout -dates`
(and `vault:/opt/vault/tls/tls.crt`). An expired cert makes vault fail its own
self-init and hand out an empty token — looks like a test bug, is not one.

`WITH_AZURITE` in the Jenkins pipelines is legacy for 9.7: the 9.7 test tree
has no azurite references, so no Azure emulator is needed.

## 7. Docker distro builds ("test my changes on ubuntu 24.04")

Persistent incremental builds: create `build/docker_<distro>/<type>/` INSIDE the repo,
mount the repo, configure that dir from inside the container:

```bash
docker run -it --name pxb-build-2404 --network pxb_network \
    -v "$PWD":/work -w /work ubuntu:24.04 bash
# inside: install deps (step 1), then
cmake -S . -B build/docker_ubuntu_24_04/release <flags> && make -C build/docker_ubuntu_24_04/release -j$(nproc) xtrabackup
```

Reuse later with `docker start -ai pxb-build-2404`. Never docker-in-docker
for MinIO: run it as a sibling container on `pxb_network` and use
`--s3-endpoint=http://s3:9000` from inside the build container.
