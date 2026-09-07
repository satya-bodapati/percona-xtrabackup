# Percona XtraBackup — build & test instructions

Canonical instructions for building and testing XtraBackup in this repo.
`AGENTS.md` points here; the `xtrabackup-build-test` skill condenses this
into a procedure.

## Dependencies (Ubuntu/Debian)

The authoritative installer is the in-tree builder script that the Jenkins
packaging pipelines themselves use (root required):

```bash
sudo bash storage/innobase/xtrabackup/utils/percona-xtrabackup-8.0_builder.sh \
    --builddir=/tmp/pxb-deps --install_deps=1
```

Equivalent manual apt list (taken from that script's Debian branch):

```bash
sudo apt install -y build-essential cmake bison ca-certificates \
    libcurl4-openssl-dev libaio-dev libncurses-dev libtool libz-dev \
    libsasl2-dev libgcrypt-dev libev-dev libudev-dev libssl-dev \
    libicu-dev libproc2-dev libnuma1 vim-common patchelf \
    python3 wget curl git pkg-config
```

If cmake reports another missing library, install its `-dev` package and
re-run cmake. Never remove or downgrade system packages to satisfy a build.

Related CI sources (read them when in doubt, both public):
- `Percona-Lab/jenkins-pipelines` — `pxb/jenkins/*.groovy`, the build/test
  pipelines (packaging builds run the builder script above inside Docker).
- `percona/pxb-jenkins-images` — the S3 (MinIO), KMIP and Vault Docker
  images the functional CI uses, with usage READMEs.

**Fresh clone or new git worktree:** initialize submodules FIRST, or cmake
fails with a missing `extra/libkmip` CMakeLists error:

```bash
git submodule update --init --recursive
```

## Version lines: 8.0 / 8.4 / 9.7 / trunk

Check `git rev-parse --abbrev-ref HEAD` before building — these differ:

| | 8.0 | 8.4 | 9.7 | trunk |
|---|---|---|---|---|
| bundled `extra/boost` | 1.84 | 1.84 | 1.87 | 1.87 |
| version `cmake/boost.cmake` wants | **1.77** | 1.84 | 1.87 | 1.87 |
| needs `-DDOWNLOAD_BOOST=1 -DWITH_BOOST=` | **YES** | no | no | no |
| `bootstrap.sh` default server | 8.0.35-27 | 8.4.4-4 | 9.7.0-1 | 9.7.0-1 |

- **Only 8.0 actually needs the boost download.** Its in-tree `extra/boost`
  holds 1.84 while cmake asks for `boost_1_77_0`, so the bundled copy does
  not satisfy it. On 8.4/9.7/trunk the bundled version is exactly what cmake
  wants — configure with no boost flags (verified on 9.7). Passing the
  download flags anyway is harmless, only slower.
- **The test server tarball must match the branch's major version.** Reuse an
  existing `test/server` from another build tree of the same line rather than
  re-running bootstrap:
  `cp -a <other-bld>/storage/innobase/xtrabackup/test/server <bld>/storage/innobase/xtrabackup/test/`
- A fix that spans lines needs one worktree + branch per line, each based on
  that line's own upstream head, each built and tested separately.

## Build directory naming

Builds live inside the source directory, under a top-level `build/` tree
(untracked). When the user names a build directory, use it; otherwise the
default layout is:

| dir | type | use |
|---|---|---|
| `build/debug/` | Debug | day-to-day dev, debug-gated tests, gunit |
| `build/release/` | RelWithDebInfo | release-equivalent testing, benchmarks |
| `build/asan/` | Debug + ASAN | sanitizer runs |
| `build/docker_<distro>/<type>/` | per distro | builds done inside a Docker container, e.g. `build/docker_ubuntu_24_04/debug/` (see below) |

Short flat names like `bld/` (debug) and `bld_rel/` (RelWithDebInfo) are
also in active use and fully supported — if the user names such a dir, or
one already exists in the checkout with a configured CMakeCache, prefer
reusing it over configuring a fresh `build/<type>/` from scratch.

**Release testing always means `RelWithDebInfo`, never plain `Release`.**
Debug and release builds behave differently (assertions, debug_sync,
require_debug_* test gates) — know which one you are testing.

## Configure and build

**How the build team builds** (chain used by Jenkins packaging): the
builder script downloads sources and calls
`storage/innobase/xtrabackup/utils/build-binary.sh`, whose cmake line is
the flag source of truth:
`-DBUILD_CONFIG=xtrabackup_release -DDOWNLOAD_BOOST=1 -DWITH_BOOST=<dir>
-DMINIMAL_RELWITHDEBINFO=OFF -DWITH_ZLIB=bundled -DWITH_ZSTD=bundled`
(in-source; build type defaults to RelWithDebInfo).

Use `cmake -S . -B <builddir>` from the repo root — it works for any
nesting depth of the build tree.

**Build-team-faithful release build** (into `build/release/`):

```bash
cmake -S . -B build/release \
    -DBUILD_CONFIG=xtrabackup_release -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DWITH_LTO=OFF -DDOWNLOAD_BOOST=1 -DWITH_BOOST=$HOME/boost \
    -DMINIMAL_RELWITHDEBINFO=OFF -DWITH_ZLIB=bundled -DWITH_ZSTD=bundled
make -C build/release -j$(nproc) xtrabackup
```

**Fast dev build** (smaller configure, skips server extras):

```bash
cmake -S . -B build/release \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_LTO=OFF \
    -DDOWNLOAD_BOOST=1 -DWITH_BOOST=$HOME/boost \
    -DWITH_ROUTER=OFF -DWITH_NDB=OFF -DWITH_MAN_PAGES=OFF \
    -DWITH_UNIT_TESTS=ON
make -C build/release -j$(nproc) xtrabackup
```

- Debug build: same but `-DCMAKE_BUILD_TYPE=Debug` into `build/debug`.
- ASAN build: Debug plus `-DWITH_ASAN=ON` into `build/asan`.
- Boost downloads once into `WITH_BOOST` and is reused across build dirs.
- `-DWITH_UNIT_TESTS=ON` enables gunit targets (e.g. `make xb_page_group-t`,
  run the binary from `runtime_output_directory/`).

**Linker errors on newer Ubuntu (24.04+/26.04):** the distro default
`-flto=auto` breaks the link. `-DWITH_LTO=OFF` is mandatory.

**Build everything — pass no target.** `make -C <bld> -j$(nproc)` (no
target name) is the default. `make ... xtrabackup` builds only the binary and
leaves `plugin_output_directory` empty, which silently breaks every keyring
test (see the keyring-component trap below), and skips the `link_test_dir`
ALL target that syncs tests into the build tree. Naming the `xtrabackup`
target is a shortcut only for a quick compile check you will not test with.
Building all after a targeted build is cheap — the objects are shared.

**Rebuild before every test run.** Always rebuild
after ANY code change — tests execute the binary from the build tree and a
stale binary silently invalidates results. Verify freshness by comparing
the binary mtime against your newest source file; do NOT trust the
`revision id` in `xtrabackup --version` (it is captured at cmake time, not
at build time).

## Running the test framework

Always run from the BUILD tree's test dir, never from the source tree:

```bash
cd build/release/storage/innobase/xtrabackup/test   # or bld_rel/... etc.
```

**One-time server setup** — `bootstrap.sh` downloads a Percona Server
tarball into `./server`:

```bash
./bootstrap.sh                    # default version pinned in the script
./bootstrap.sh --version=8.4.8-8  # or pick one
```

glibc gotcha: on a very new glibc (e.g. 2.43) bootstrap fails with
"tarball for your glibc version is not available". Download the
`glibc2.35` tarball manually (downloads.percona.com release area, or the
`.../downloads/TESTING/ps-<version>` fallback) and unpack it yourself:

```bash
mkdir -p server && tar -xf Percona-Server-<v>-Linux.x86_64.glibc2.35-minimal.tar.gz \
    -C server --strip-components=1
```

**Running tests:**

```bash
./run.sh -f -j 128                          # full regression: no -s = ALL suites
./run.sh -f -t t/<test>.sh                  # one test from the main suite
./run.sh -f -t suites/<suite>/<test>.sh     # one test from a named suite
./run.sh -f -j 128 -s default -s binlog     # selected suites only
```

- **Omit `-s` to run every suite.** That is the normal full-regression
  invocation. Of the lot, only `xbcloud` and `keyring` need external setup
  (MinIO / keyring servers); everything else is self-contained.
- **The main `t/*.sh` suite is `-s default`, not `-s main`.** `run.sh -h`
  lists "main" as a suite name, but `-s X` just expands to `suites/X/*.sh`
  and there is no `suites/main/`, so `-s main` degrades to an unmatched
  literal glob and reports a single bogus failing test named `*`.
- **Size `-j` yourself, but respect kernel AIO.** With no `-j`, run.sh
  auto-caps at 16 workers regardless of machine size. Cores are not the only
  ceiling though: each worker boots InnoDB **twice** — its `mysqld`, and
  again inside `xtrabackup` (backup and prepare each start an InnoDB
  instance). Every boot calls `io_setup()` for roughly
  `(innodb_read_io_threads + innodb_write_io_threads + 1) x 256` events
  (~2.3k at the framework's defaults; it sets no IO-thread options).
  Exhausting `fs.aio-max-nr` makes InnoDB fail to start, which surfaces as a
  flood of failures that look like code bugs but are not:

  ```
  [ERROR] [MY-012584] [InnoDB] io_setup() failed with EAGAIN after 5 attempts.
  [ERROR] [MY-012954] [InnoDB] Cannot initialize AIO sub-system
  ```

  Triage any large failure count with
  `grep -l "io_setup() failed" results/* | wc -l` before investigating code.
  Prefer raising the ceiling over shrinking `-j`:

  ```bash
  cat /proc/sys/fs/aio-max-nr; cat /proc/sys/fs/aio-nr   # ceiling; in use
  sudo sysctl -w fs.aio-max-nr=8388608                   # transient 8x bump
  echo 'fs.aio-max-nr = 8388608' | sudo tee /etc/sysctl.d/99-pxb-aio.conf
  ```

- **Raise `ulimit -n` too.** The distro soft limit is often 1024, which is
  per-process and too low — xtrabackup logs `open files limit requested 0,
  set to 1024`. The hard limit is usually large (524288 on the epyc hosts),
  so no root is needed: `ulimit -n 100000` in the shell that runs run.sh.
  Other kernel limits (`fs.file-max`, `kernel.pid_max`, `vm.max_map_count`,
  `kernel.threads-max`) are ample by default and need no tuning.
- **Never run two suites concurrently on one host.** Two `-j64` runs put 128
  mysqlds plus 128 xtrabackups against the same global `fs.aio-max-nr` and
  produce a ~190-failure run that is almost entirely `io_setup`. Run one
  branch to completion, then the next. **Two runs in the same build tree is
  worse still** — they share one `var/` and one `results/` and overwrite each
  other's state. When killing a stale run, match the exact `-j` value you
  launched (`pkill -f "run.sh -f -j 64"`), and re-check with
  `pgrep -af "[r]un[.]sh"`; a bracketed pattern avoids matching your own
  command line, which otherwise kills your shell instead of the run. Peak AIO is bursty, so a low
  `aio-nr` sample proves nothing — trust the `io_setup` grep.
- The framework judges a test purely by its exit status (`run.sh` sources it
  and propagates `$?`); it never scans output for `[Warning]`/`[ERROR]`. New
  warning lines therefore cannot fail a test unless the test itself greps
  its own `$OUTFILE`.
- `results/` is WIPED at the start of every run — inspect or copy logs
  immediately after a failure.
- **Tests edited or added in the SOURCE tree must be synced into the build
  tree** — run.sh executes the build tree's copy. The build does this with
  `make -C <bld> link_test_dir` (a `copy_directory` of the whole test dir).
  It is an ALL target, so a plain `make xtrabackup` leaves the build tree's
  `test/` empty or stale; run it explicitly, or copy the single file:
  `cp storage/innobase/xtrabackup/test/t/y.sh <bld>/storage/innobase/xtrabackup/test/t/`
- Debug gating: `require_debug_pxb_version` needs a Debug xtrabackup
  (use `build/debug/`); `require_debug_server` additionally needs a debug mysqld
  in the server tarball.

## Docker preflight — run docker without sudo

Before ANY docker use (xbcloud MinIO, distro builds), verify docker works
unprivileged — do not sudo-prefix docker commands:

```bash
docker ps >/dev/null 2>&1 || {
    sudo usermod -aG docker "$USER"     # one-time fix
    # group membership applies on a NEW login/shell; in the current shell:
    # re-run the docker commands via:  sg docker -c '<command>'
}
```

If `docker ps` still fails after that, the daemon isn't running
(`sudo systemctl enable --now docker`). Solve this once, early — every
docker step below assumes unprivileged docker.

**Already inside a container?** Detect it first:

```bash
[ -f /.dockerenv ] || grep -qE 'docker|containerd|kubepods' /proc/1/cgroup 2>/dev/null
```

Policy when inside a container:
- **Compiling, building and running non-docker tests is fine** — that is
  what a build container is for. Proceed normally.
- **Do NOT attempt docker-in-docker.** For docker-dependent steps
  (MinIO/xbcloud, "test on <distro> in docker"):
  - If `/var/run/docker.sock` is mounted and `docker ps` works, containers
    you start are SIBLINGS on the host: reach them over `pxb_network`
    by container name (attach this container to it), and remember `-v`
    paths in `docker run` are HOST paths — a repo path inside this
    container cannot be volume-mounted unless its host path is known.
    Distro-build requests are therefore still better done from the host.
  - Otherwise, refuse the docker part with a clear message ("already
    inside a container without docker access — run this from the host")
    and offer the native equivalent: a request like "test on centos8 in
    docker" from inside an Ubuntu container becomes a native build/test
    here, with the note that the centos8 run needs to happen on the host.

## External test services (xbcloud / vault / kmip / kms)

Most suites are self-contained. Only these need servers, and each one
**skips cleanly** when its server is absent — so a missing service shows up
as a skip, never a failure:

| suite / test | service | gate |
|---|---|---|
| `suites/xbcloud/*` | S3 (MinIO) | `XBCLOUD_CREDENTIALS` unset → skip |
| `suites/keyring/*vault*` | HashiCorp Vault | `keyring_vault_ping` fails → skip |
| `suites/keyring/*kmip*` | PyKMIP | `ping_kmip` fails → skip |
| `suites/keyring/innodb_keyring_kms_component.sh` | **real AWS KMS** | `ping_kms` fails → skip |

The images are **`satyapercona/*`** — that is what the functional CI runs
(`jenkins-pipelines/pxb/v2/docker/run-test`, the authoritative source for
this whole section; the older `altmannmarcelo/*` images are stale and their
Vault TLS cert expired 2024-12-17). KMS cannot be faked locally: it needs a
real AWS key, so leave those tests skipping unless you have credentials.

Host prerequisites: `socat` (used by `ping_kmip`), `uuidgen`, `curl`.

**The keyring-component trap.** Every `suites/keyring/*` test fails with

```
[ERROR] [MY-013709] [Server] Received an error while processing components from
        manifest file: Failed to load components from manifest file
[ERROR] [MY-011825] [Xtrabackup] failed to init keyring component
```

if the keyring component `.so`s cannot be found — and that happens by default
after a targeted build. `run.sh` picks the plugin dir like this:

```bash
if test -d $PWD/../../../../plugin_output_directory
then  plugin_dir=$PWD/../../../../plugin_output_directory
else  plugin_dir=$PWD/../../lib/plugin/          # the server tarball's
fi
```

`make <bld> xtrabackup` **creates `<bld>/plugin_output_directory` but leaves
it empty**, so the `test -d` succeeds and run.sh uses the empty build dir
instead of falling back to the server tarball's populated `lib/plugin/`. The
failure looks like a keyring bug and is not one.

**The fix is to build everything — `make -C <bld> -j64` with no target.**
That is the rule for any run that includes the keyring suite (so: any full
regression). Verify with `ls <bld>/plugin_output_directory | wc -l` before
trusting a keyring result; it should contain the `component_keyring_*.so`
files.

```bash
docker network create pxb_network             # idempotent
docker run -d --rm --network pxb_network -p 9000:9000 -p 9001:9001 \
    --security-opt seccomp=unconfined --cap-add=NET_ADMIN \
    --env USER=myuser --env PASSWORD=someStrongPWD \
    --name s3 satyapercona/minio:latest
docker run -d --rm --network pxb_network -p 8200:8200 \
    --security-opt seccomp=unconfined --cap-add=NET_ADMIN \
    --name vault satyapercona/vault:latest
docker run -d --rm --network pxb_network -p 5696:5696 \
    --security-opt seccomp=unconfined --cap-add=NET_ADMIN \
    --name kmip satyapercona/kmip:latest
```

Then copy the credentials out of the containers and point the framework at
them. The TLS certs are issued for the names `local.vault.com` /
`local.kmip.com`, so those must resolve — with ports published, map them to
loopback for a host-native `run.sh`:

```bash
E=/bigdisk_kioxia/satya/pxb-testenv; mkdir -p $E     # any stable dir
docker cp vault:/opt/vault/tls/tls.crt $E/vault.crt
docker cp kmip:/opt/certs/root_certificate.pem $E/
docker cp kmip:/opt/certs/client_key_jane_doe.pem $E/
docker cp kmip:/opt/certs/client_certificate_jane_doe.pem $E/
docker cp s3:/usr/bin/mc $E/mc && chmod +x $E/mc     # tests need the mc client
echo "127.0.0.1 local.vault.com" | sudo tee -a /etc/hosts
echo "127.0.0.1 local.kmip.com"  | sudo tee -a /etc/hosts
```

```bash
# xbcloud / S3
export XBCLOUD_CREDENTIALS="--storage=s3 --s3-endpoint=http://localhost:9000 --s3-access-key=myuser --s3-secret-key=someStrongPWD --s3-bucket=newbucket"
export XBCLOUD_MC=$E/mc
# vault - dev root token is regenerated on every container restart, so read it back
export VAULT_URL=https://local.vault.com:8200
export VAULT_CA=$E/vault.crt VAULT_CACERT=$E/vault.crt
export VAULT_TOKEN=$(docker logs vault 2>&1 | grep 'export VAULT_TOKEN' | tail -1 | cut -d= -f2 | tr -d '[:space:]')
# kmip
export KMIP_SERVER_ADDR=local.kmip.com KMIP_SERVER_PORT=5696
export KMIP_SERVER_CA=$E/root_certificate.pem
export KMIP_CLIENT_CA=$E/client_certificate_jane_doe.pem
export KMIP_CLIENT_KEY=$E/client_key_jane_doe.pem
```

Keep that block in a sourceable `env.sh` next to the copied certs rather than
retyping it; on the epyc lab hosts it already lives at
`/bigdisk_kioxia/satya/pxb-testenv/env.sh` and prints a one-line health check
for all three services when sourced.

Rules and gotchas:
- **No inner quotes inside `XBCLOUD_CREDENTIALS`** (only the surrounding
  double quotes) — `common.sh`'s parser breaks on them. Note the CI script
  does quote the endpoint; do not copy that detail for local runs.
- Endpoint: `http://localhost:9000` from the host; `http://s3:9000` from
  another container on `pxb_network`; the container IP works from anywhere.
- `XBCLOUD_MC` matters: several xbcloud tests drive the `mc` client to list
  objects and manage IAM users, and it is not in the image the tests run in
  — that is why CI copies the static binary out of the MinIO image.
- **Verify the image certs have not expired before blaming the tests.**
  `docker exec kmip openssl x509 -in /opt/certs/root_certificate.pem -noout -dates`
  and the same for `vault:/opt/vault/tls/tls.crt`. An expired cert makes the
  vault container fail its own self-init and emit an *empty* token, which
  looks like a test bug and is not one. (Current: KMIP valid to 2026-12-01,
  Vault to 2027-01-16.)
- The MinIO image supports `--env PORT/ADMIN_PORT` overrides and ships awscli
  plus `tc`/`tcpkill` for chaos testing (see its README).
- The `WITH_AZURITE` toggle in the Jenkins pipelines is legacy for the 9.7
  line — the 9.7 test tree contains no azurite references, so no Azure
  emulator is needed there.

## Building/testing inside Docker (e.g. "test my changes on Ubuntu 24.04")

Keep build artifacts persistent across container runs: create a
`build/docker_<distro>/<type>/` dir INSIDE the repo and mount the repo into the
container — re-running reuses the incremental build instead of starting
from scratch.

```bash
docker run -it --name pxb-build-2404 --network pxb_network \
    -v "$PWD":/work -w /work ubuntu:24.04 bash

# inside the container:
apt update && apt install -y <dependency list above>
cmake -S . -B build/docker_ubuntu_24_04/release \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_LTO=OFF \
    -DDOWNLOAD_BOOST=1 -DWITH_BOOST=/work/build/docker_ubuntu_24_04/boost \
    -DWITH_ROUTER=OFF -DWITH_NDB=OFF -DWITH_MAN_PAGES=OFF
make -C build/docker_ubuntu_24_04/release -j$(nproc) xtrabackup
```

Reuse the container with `docker start -ai pxb-build-2404`.

Tests that themselves need Docker (MinIO for xbcloud) must NOT be run as
docker-in-docker from inside the build container. Run MinIO as a sibling
container on `pxb_network` from the host (as above) and attach the build
container to the same network (`--network pxb_network` at run time, or
`docker network connect pxb_network pxb-build-2404`), then point
`--s3-endpoint` at the container name: `http://s3:9000`.
