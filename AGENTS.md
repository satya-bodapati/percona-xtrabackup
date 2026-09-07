# Agent instructions — Percona XtraBackup

All build and test instructions for this repository live in **CLAUDE.md**
(repo root). Read it before building or running tests. It covers:

- dependency installation (the in-tree builder script the build team uses)
- **version-line differences** (8.0 / 8.4 / 9.7 / trunk): which lines need
  the boost download, and matching the test server tarball to the branch
- build directory naming (default `build/<type>/` tree: `build/debug`,
  `build/release`, `build/asan`, `build/docker_<distro>/<type>`;
  user-named dirs like `bld/`, `bld_rel/` equally supported)
- cmake flags (release testing is ALWAYS `RelWithDebInfo`, never `Release`;
  `-DWITH_LTO=OFF` is mandatory on newer Ubuntu)
- rebuilding with `make -j$(nproc)` after every change before running tests,
  and `make link_test_dir` to sync tests into the build tree
- the test framework (`bootstrap.sh` server download, `run.sh` — omit `-s`
  to run every suite, `-s default` is the main `t/` suite — debug gating)
- **sizing `-j`**: run.sh caps itself at 16 workers; each worker is two
  InnoDB boots (mysqld + xtrabackup), so kernel `fs.aio-max-nr` is the real
  ceiling. `io_setup() failed with EAGAIN` floods are environmental, not
  code — triage them before investigating any failure, and never run two
  suites concurrently on one host or in one build tree.
- external test services (S3/MinIO, Vault, KMIP, AWS KMS) using the
  `satyapercona/*` CI images on `pxb_network`; each suite skips cleanly
  when its service is absent
- building inside Docker with persistent `build/docker_<distro>/` dirs

A condensed procedural version is available as the `xtrabackup-build-test`
skill under `.claude/skills/`.
