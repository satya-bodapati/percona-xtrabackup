# OpenID Connect authentication in Percona XtraBackup

This document explains how PXB authenticates to a source MySQL/Percona Server
via OpenID Connect (OIDC), how the client-side plugin is integrated,
what the tests exercise, and what future work is planned.

Written for engineers who need to touch this area later. Read the first
two sections before making changes; skim the rest as reference.

## TL;DR

- xtrabackup can now authenticate to a source server as an OIDC-identified
  user by presenting a signed ID-token (JWT) — no password.
- The client plugin (`authentication_openid_connect_client`) is shipped
  as a loadable `.so` alongside xtrabackup, in `${INSTALL_PLUGINDIR}`.
- xtrabackup calls `mysql_options(MYSQL_PLUGIN_DIR, opt_plugin_dir)` in
  `xb_mysql_connect()` before contacting the plugin, so libmysqlclient
  dlopens it transparently — no operator action needed when PXB is
  installed normally. Override via `--xtrabackup-plugin-dir`.
- New CLI option:
  `--authentication-openid-connect-client-id-token-file=PATH`.
- Mutually exclusive with `--password`.
- xtrabackup logs the plugin dir and the resolved `.so` path so
  ambiguity between multiple installed OIDC client plugins is visible.
- Server side is Percona Server ≥ 8.4.10 with the `auth_openid_connect`
  server plugin installed (from PS PR #5941).

## How xtrabackup finds the client plugin

Every external client auth plugin in the MySQL tree (LDAP, Kerberos,
OCI, FIDO/WebAuthn, and now OIDC in PXB) is built as a loadable `.so`
and installed under the compile-time `PLUGINDIR`. libmysqlclient's
`mysql_load_plugin_v()` resolves the dir it dlopens from as:

1. `mysql_options(mysql, MYSQL_PLUGIN_DIR, ...)` if set, else
2. `LIBMYSQL_PLUGIN_DIR` env var, else
3. Compile-time `PLUGINDIR` (`${DEFAULT_MYSQL_HOME}/${INSTALL_PLUGINDIR}`).

Historically PXB has done none of the above — it never called
`mysql_options(MYSQL_PLUGIN_DIR, ...)` — so plugins that libmysqlclient
links **statically** (`caching_sha2_password`, `mysql_native_password`,
`sha256_password`, `clear_password`) work but any `.so`-based plugin
would silently fall back to whatever the compile-time `PLUGINDIR`
happened to be.

For OIDC we ship the client plugin as a `.so` under `${INSTALL_PLUGINDIR}`
(same location as PS's client-side auth plugins) and explicitly point
libmysqlclient at it. Inside `xb_mysql_connect()`:

```c
if (opt_plugin_dir != nullptr && *opt_plugin_dir != '\0') {
  mysql_options(connection, MYSQL_PLUGIN_DIR, opt_plugin_dir);
}
```

`opt_plugin_dir` is the server-side global filled by `xb_set_plugin_dir()`
from `--xtrabackup-plugin-dir` (user override) or the compile-time
`PLUGINDIR` default. So xtrabackup uses one plugin-dir setting for both
its keyring plugins and its OIDC client plugin. If the operator wants
to test against a specific `.so` (e.g. a PS-shipped copy), passing
`--xtrabackup-plugin-dir=…` moves both.

Two info lines are logged before the connect call so the operator sees
exactly which `.so` will be dlopen'd:

```
[Note] OIDC client plugin dir (--xtrabackup-plugin-dir): '/usr/lib/mysql/plugin'
[Note] Loading OIDC client plugin from '/usr/lib/mysql/plugin/authentication_openid_connect_client.so'
```

This is important because a co-installed Percona Server also ships an
`authentication_openid_connect_client.so` in its own plugin dir — the
log line disambiguates which one xtrabackup is loading.

## Files changed / added

### 1. Ported from Percona Server PR #5941

Everything under `libmysql/authentication_openid_connect_client/`:

```
libmysql/authentication_openid_connect_client/
├── CMakeLists.txt                                (new)
└── authentication_openid_connect_client_plugin.cc  (new, ~250 lines)
```

The `.cc` is a verbatim port from PS's PR — same
`mysql_declare_client_plugin(AUTHENTICATION) … mysql_end_client_plugin;`
tail, same everything.

The `CMakeLists.txt` uses `MYSQL_ADD_PLUGIN(authentication_openid_connect_client
authentication_openid_connect_client_plugin.cc CLIENT_ONLY MODULE_ONLY
MODULE_OUTPUT_NAME "authentication_openid_connect_client")` — mirrors
`libmysql/authentication_oci_client/CMakeLists.txt`. Produces a
`.so` installed to `${INSTALL_PLUGINDIR}` under the `Client` component,
the same location where LDAP / OCI / FIDO / Kerberos client plugins live.

### 2. Wired into libmysqlclient

- `libmysql/CMakeLists.txt` — under `IF(WITH_XTRABACKUP)`,
  `ADD_SUBDIRECTORY(authentication_openid_connect_client)`. Nothing
  else — the plugin is a standalone `.so`, not merged into
  `libmysqlclient.a`.
- `sql-common/client.cc` — single `#ifdef XTRABACKUP` block:
  `info->is_tls_established = true;` inside `mpvio_info()`'s
  `VIO_TYPE_SSL` branch (so the OIDC client plugin can confirm TLS is
  up before releasing the token).
- `include/mysql/plugin_auth_common.h` — `#ifdef XTRABACKUP` blocks
  adding `#include <stdbool.h>` and the `bool is_tls_established;`
  field to `MYSQL_PLUGIN_VIO_INFO`. The `.h.pp` ABI-check files are
  intentionally **not** updated: the ABI check runs the preprocessor
  without `-DXTRABACKUP`, so the field is invisible there and the
  ABI stays clean for external consumers of libmysqlclient.

### 3. PXB-side changes

- `storage/innobase/xtrabackup/src/xtrabackup.cc` — a new global
  `opt_openid_connect_id_token_file`, a new enum id
  `OPT_AUTHENTICATION_OPENID_CONNECT_CLIENT_ID_TOKEN_FILE`, and the
  `--authentication-openid-connect-client-id-token-file` entry in the
  option table.
- `storage/innobase/xtrabackup/src/xtrabackup.h` — extern for the new global.
- `storage/innobase/xtrabackup/src/backup_mysql.cc` — inside
  `xb_mysql_connect()`, before `mysql_real_connect`:

  1. Reject `--password` + OIDC option together — mutually exclusive.
  2. Rewrite the "Connecting to MySQL server host: …" log line to say
     `password: not set (using OIDC id-token-file)` when OIDC is used.
  3. Emit `Using OpenID Connect id-token-file '…' for authentication
     to the server.` (which token file).
  4. Emit `OIDC client plugin dir (--xtrabackup-plugin-dir): '…'`
     (which dir libmysqlclient will search).
  5. Emit `Loading OIDC client plugin from '…/authentication_openid_connect_client.so'`
     (which `.so` will be dlopen'd — disambiguates from a co-installed
     PS-shipped copy).
  6. Call `mysql_options(connection, MYSQL_PLUGIN_DIR, opt_plugin_dir)`
     so libmysqlclient looks in xtrabackup's plugin dir.
  7. Call `mysql_client_find_plugin(conn,
     "authentication_openid_connect_client",
     MYSQL_CLIENT_AUTHENTICATION_PLUGIN)` — this triggers the dlopen —
     then hand the plugin the token path via `mysql_plugin_options(p,
     "id-token-file", …)`. Bail on either error.

  The plugin's own `option()` callback opens the token file at this
  point and rejects invalid paths, so a bad path fails **before** any
  server contact.

### 4. Test framework

`storage/innobase/xtrabackup/test/inc/oidc_common.sh` (~200 lines) —
sourced by all three OIDC tests. Exposes:

| Function | Purpose |
|---|---|
| `oidc_require_server_plugin` | Skip unless `AUTH_OIDC_SERVER_SO` is a real file. Sets `AUTH_OIDC_PLUGIN_DIR` / `AUTH_OIDC_SO_NAME`. |
| `oidc_install_and_configure_server_plugin CFG` | `INSTALL PLUGIN auth_openid_connect SONAME …` + `SET GLOBAL auth_openid_connect_configuration = 'CFG'`. |
| `oidc_create_backup_user USER SUB [IDP]` | `CREATE USER … IDENTIFIED WITH 'auth_openid_connect' AS '{…}';` + all grants xtrabackup `--backup` needs. |
| `oidc_extract_sub TOKEN_FILE` | Base64-decodes the JWT payload, echoes `.sub`. Needed because Keycloak issues UUIDs there. |
| `oidc_fetch_ropc_token URL CID USER PW OUT` | POSTs an ROPC password grant, writes the ID token to `OUT`. |
| `oidc_backup_prepare_restore_verify USER TOKEN DIR PLUGIN_DIR [TABLE]` | Runs `--backup` + `--prepare` + `--copy-back`, restarts the server, compares the row count of `TABLE` (default `sakila.actor`). Real "backup is valid" proof. |

`storage/innobase/xtrabackup/test/inc/oidc_keycloak_docker.sh` — Docker
Keycloak lifecycle. Skips if `docker` isn't on PATH; when the test sets
`OIDC_BOOTSTRAP_KEYCLOAK=1`, sourcing this file:

1. Starts `quay.io/keycloak/keycloak:26.0` in dev mode on port 8080.
2. Waits until the OIDC discovery endpoint responds.
3. Runs `kcadm.sh` inside the container to create realm `pxb`, public
   client `pxb-xtrabackup` (direct grants on), user `xtrabackup-oidc`,
   and an audience mapper so ID tokens carry `aud=pxb-xtrabackup`.
4. Fetches a fresh ID token via ROPC to
   `${TEST_VAR_ROOT}/keycloak_id_token.txt`.
5. Exports `KEYCLOAK_ISSUER`, `KEYCLOAK_AUDIENCE`, `KEYCLOAK_JWKS_URL`,
   `KEYCLOAK_ID_TOKEN`, `KEYCLOAK_MYSQL_USER`.
6. Installs an `EXIT` trap so the container is torn down on script exit.

### 5. Tests

The three tests live in their own suite,
`storage/innobase/xtrabackup/test/suites/oidc/`:

| Test | IdP | Runs any time? |
|---|---|---|
| `oidc_jwt.sh` | PS `std_data/oidc/` dummy in-memory JWKS + `create_id_token` locally-signed JWT | Yes (fast, deterministic) |
| `oidc_percona.sh` | Live `keycloak.int.percona.com` (Percona VPN) | Only from Percona network / VPN |
| `oidc_keycloak.sh` | Dev-provisioned Keycloak in Docker (auto-bootstrap opt-in) OR any externally-provisioned Keycloak | Only when Docker or a Keycloak is available |

Each test:
- Prints its required env vars via `vlog` at the top, so anyone reading
  `results/<test>` can see what to set even before any skip fires.
- Skips cleanly with a specific reason when its prerequisites aren't met.
- Runs a full backup + prepare + copy-back + row-count-verify cycle so
  a green result means the round-trip actually preserved data (not just
  that xtrabackup exited zero).

Run the full suite with `./run.sh -f -s oidc`, or a single test with
`./run.sh -f -t suites/oidc/<name>.sh`.

## Environment variables reference

### Required for all three tests

| Variable | Meaning |
|---|---|
| `AUTH_OIDC_SERVER_SO` | Absolute path to `auth_openid_connect.so`, the **server-side** plugin. Comes from a PS 8.4.10+ build (PR #5941 merged). Example: `/home/you/WORK/ps-84/bld/plugin_output_directory/auth_openid_connect.so`. |

### Required for `suites/oidc/oidc_jwt.sh` (dummy-JWKS fast path)

| Variable | Meaning |
|---|---|
| `CREATE_ID_TOKEN` | Absolute path to the `create_id_token` binary built alongside the PS OIDC plugin. Example: `/home/you/WORK/ps-84/bld/runtime_output_directory/create_id_token`. |
| `AUTH_OIDC_STD_DATA` | Directory containing `idp_private.pem` and `dummy_oidc_conf.json`. Example: `/home/you/WORK/ps-84/mysql-test/std_data/oidc`. |

### Required for `suites/oidc/oidc_percona.sh` (live Percona Keycloak)

None beyond `AUTH_OIDC_SERVER_SO`. IdP URL/credentials are hardcoded to
mirror PS's own `idp.test`. Requires VPN reachability to
`keycloak.int.percona.com`. Also needs `curl` and `jq` on PATH.

### For `suites/oidc/oidc_keycloak.sh` (dev Keycloak)

**Auto-bootstrap mode** — just set `OIDC_BOOTSTRAP_KEYCLOAK=1` and needs
`docker` on PATH. Everything else is bootstrapped.

**External-Keycloak mode:**

| Variable | Meaning |
|---|---|
| `KEYCLOAK_ISSUER` | The `iss` claim value the plugin will trust. Also the base URL for the realm. Example: `http://localhost:8080/realms/pxb`. |
| `KEYCLOAK_AUDIENCE` | The `aud` value tokens are expected to carry. Usually the client_id. |
| `KEYCLOAK_JWKS_URL` | JWKS endpoint. Typically `${KEYCLOAK_ISSUER}/protocol/openid-connect/certs`. |
| `KEYCLOAK_ID_TOKEN` | Path to a file containing a pre-fetched ID token. |
| `KEYCLOAK_MYSQL_USER` | Optional — mysql-side user name; defaults to `mysql_oidc_user`. |

## How to build

Assuming Percona Server 8.4.10+ tree at `/home/you/WORK/ps-84`:

```bash
# PS side — the server plugin and helper
cd /home/you/WORK/ps-84
git submodule update --init --recursive extra/jwt-cpp
cd bld && cmake .. && make -j16 auth_openid_connect create_id_token

# Artifacts you'll need:
#   bld/plugin_output_directory/auth_openid_connect.so
#   bld/runtime_output_directory/create_id_token
#   mysql-test/std_data/oidc/{idp_private.pem,dummy_oidc_conf.json}

# PXB side — everything is built by the standard PXB build.
cd /home/you/WORK/pxb-8.4/bld_rel && make -j16
```

Verify the OIDC option is exposed:

```bash
./runtime_output_directory/xtrabackup --help | grep openid
#   --authentication-openid-connect-client-id-token-file=name
#                       Path to the OpenID Connect ID token file used to ...
```

## How to run the tests

Whichever test you're running, always `make -j16` in `bld_rel/` first so
edits to `test/t/*.sh` and `test/inc/*.sh` get copied into
`bld_rel/storage/innobase/xtrabackup/test/{t,inc}/` (that's the copy
`run.sh` executes from).

### Whole suite

```bash
export AUTH_OIDC_SERVER_SO=/home/you/WORK/ps-84/bld/plugin_output_directory/auth_openid_connect.so
export CREATE_ID_TOKEN=/home/you/WORK/ps-84/bld/runtime_output_directory/create_id_token
export AUTH_OIDC_STD_DATA=/home/you/WORK/ps-84/mysql-test/std_data/oidc
cd /home/you/WORK/pxb-8.4/bld_rel/storage/innobase/xtrabackup/test
./run.sh -f -s oidc
```

Individual tests:

### `oidc_jwt.sh` — dummy JWKS (fast, deterministic)

```bash
export AUTH_OIDC_SERVER_SO=/home/you/WORK/ps-84/bld/plugin_output_directory/auth_openid_connect.so
export CREATE_ID_TOKEN=/home/you/WORK/ps-84/bld/runtime_output_directory/create_id_token
export AUTH_OIDC_STD_DATA=/home/you/WORK/ps-84/mysql-test/std_data/oidc
cd /home/you/WORK/pxb-8.4/bld_rel/storage/innobase/xtrabackup/test
./run.sh -f -t suites/oidc/oidc_jwt.sh
```

### `oidc_percona.sh` — live Percona Keycloak (VPN)

```bash
export AUTH_OIDC_SERVER_SO=/home/you/WORK/ps-84/bld/plugin_output_directory/auth_openid_connect.so
cd /home/you/WORK/pxb-8.4/bld_rel/storage/innobase/xtrabackup/test
./run.sh -f -t suites/oidc/oidc_percona.sh
```

Skips cleanly if `keycloak.int.percona.com` isn't reachable.

### `oidc_keycloak.sh` — Docker-bootstrapped Keycloak

```bash
export AUTH_OIDC_SERVER_SO=/home/you/WORK/ps-84/bld/plugin_output_directory/auth_openid_connect.so
export OIDC_BOOTSTRAP_KEYCLOAK=1
cd /home/you/WORK/pxb-8.4/bld_rel/storage/innobase/xtrabackup/test
./run.sh -f -t suites/oidc/oidc_keycloak.sh
```

Test starts / provisions / tears down a Keycloak container automatically.

### `oidc_keycloak.sh` — external Keycloak

Provision a Keycloak yourself (see `inc/oidc_keycloak_docker.sh` for the
kcadm calls), then:

```bash
export AUTH_OIDC_SERVER_SO=...
export KEYCLOAK_ISSUER=http://your-host:8080/realms/pxb
export KEYCLOAK_AUDIENCE=pxb-xtrabackup
export KEYCLOAK_JWKS_URL=http://your-host:8080/realms/pxb/protocol/openid-connect/certs
export KEYCLOAK_ID_TOKEN=/path/to/your/token
cd /home/you/WORK/pxb-8.4/bld_rel/storage/innobase/xtrabackup/test
./run.sh -f -t suites/oidc/oidc_keycloak.sh
```

## Common pitfalls

### "Access denied … (using password: NO)" against a real IdP

Symptom: xtrabackup connects, transmits the ID token, mysqld rejects
auth. `Access denied for user '…'@'…' (using password: NO)` in the
xtrabackup log.

Cause: Real IdPs (Keycloak, Okta, Auth0, Azure AD) issue **UUIDs** in the
JWT `sub` claim — not the human username. The plugin matches
`CREATE USER … IDENTIFIED WITH 'auth_openid_connect' AS '{…,"user":"X"}'`
against the token's `sub`. If X is the login name, this won't match.

Fix: extract `sub` from the token and use it in the `AS` clause. Our
helper `oidc_extract_sub` does this. PS's `idp.test` works around it by
hardcoding the UUID; ours extracts it dynamically so the test survives
Keycloak re-provisioning.

### "Failed to set id-token-file … on authentication_openid_connect_client plugin"

Symptom: xtrabackup rejects a token path before any server contact.

Cause: the client plugin's `option()` callback opens the file to
sanity-check it before storing the path (see the `std::ifstream file(value);
if (file.good())` block in `authentication_openid_connect_client_plugin.cc`).
It rejects paths it can't open.

Fix: verify the token file exists and is readable by the user running
xtrabackup.

### "The client-server connection is insecure. …"

Symptom: client plugin refuses to send the token, xtrabackup fails.

Cause: The plugin refuses plain TCP transport. Only TLS, unix socket,
or (Windows) shared memory are considered secure enough to release an
ID token.

Fix: connect via socket (default for `localhost`) or use `--ssl-mode=REQUIRED`.

### ABI check fails after editing `plugin_auth_common.h`

Symptom: `make -j16` fails with `ABI check found difference between
include/mysql.h.pp and bld/abi_check.out`.

Cause: You added a field to a struct in a `.h` that's ABI-tracked, but
outside the existing `#ifdef XTRABACKUP` guards.

Fix: either wrap the change in `#ifdef XTRABACKUP` (the ABI check runs
with `-DMYSQL_ABI_CHECK` and **without** `-DXTRABACKUP`, so guarded
changes are invisible to it), or update the corresponding `.h.pp`
file(s) to match.

## Design notes for future readers

### The `is_tls_established` field addition

The client plugin's TLS check reads
`vio_info.is_tls_established || vio_info.protocol == MYSQL_VIO_SOCKET ||
vio_info.protocol == MYSQL_VIO_MEMORY`. That field is new in MySQL 9.x
and was backported into PS 8.4.10 by PR #5941 alongside the OIDC plugin.
PXB 8.4 is currently based on 8.4.0, so we ported just the two-line
struct field addition plus the one-line `info->is_tls_established = true`
setter in `sql-common/client.cc:mpvio_info()`. All three edits are
`#ifdef XTRABACKUP`-guarded.

### Why ship as `.so` rather than static-link into `libmysqlclient`?

Two designs were on the table.  We picked (b).

(a) Static builtin: compile the plugin into `libmysqlclient.a` as a
convenience library and add an entry to `mysql_client_builtins[]` in
`sql-common/client.cc`, so `mysql_client_plugin_init()` pre-registers
it and `find_plugin` never touches `dlopen`.  This is how
`caching_sha2_password_client_plugin` works and matches the shape of
`authentication_win`.  Zero runtime plugin-dir handling in
xtrabackup.

(b) Loadable `.so`: build the plugin with
`MYSQL_ADD_PLUGIN(... CLIENT_ONLY MODULE_ONLY)`, install to
`${INSTALL_PLUGINDIR}`, and have `xb_mysql_connect()` call
`mysql_options(MYSQL_PLUGIN_DIR, opt_plugin_dir)` so libmysqlclient
dlopens it.  This is how LDAP, Kerberos, OCI, and FIDO/WebAuthn
client plugins ship, and how PS's own build produces the plugin
today.

We chose (b) for three reasons:

1. Consistency with the rest of the MySQL ecosystem.  Every other
   external client auth plugin ships as a `.so`; static-linking
   OIDC would have been the odd one out and would have created a
   "why is OIDC special?" question every time someone new looks at
   the tree.
2. Security-fix substitutability.  A `.so` can be swapped in-place
   without relinking `xtrabackup`.  A static builtin can't.
3. Room to grow.  We can add third-party client auth plugins to the
   same directory later without touching `libmysqlclient` or the
   builtin table.

An earlier commit series (`pxb-oidc` branch) implements design (a)
and is kept as a reference for anyone considering the trade-off.

### Percona Server vs. PXB source base skew

PXB 8.4 is based on MySQL 8.4.0; PS PR #5941 was cut against MySQL
8.4.10. Files that PS's PR touched but that we don't need to sync:

- The whole `plugin/auth_openid_connect/` server-side plugin — not our
  problem, PS installs it into `plugin_output_directory/`.
- `extra/jwt-cpp` submodule — server-side dependency; the client plugin
  does no crypto beyond base64URL-parsing the token structure.
- `libmysql/authentication_oci_client/authentication_oci_client_plugin.cc`
  refactor (`base64_encode.h` move) — unrelated to OIDC integration.
- `client/mysql.cc` OIDC option — that's for the stock `mysql` CLI,
  which PXB doesn't build.

## Future work

### Better test coverage

- Group / role mapping (PS PR #5941 supports mapping JWT `groups` claim
  to MySQL roles). Not tested yet on PXB side.
- Proxy accounts (also supported by PS).
- Token expiry / refresh behavior during long backups.
- Multiple IdPs configured on the server; xtrabackup should pick the
  right one via the token's `iss`.

### Certificate-verified JWKS URL (HTTPS)

Our dev Keycloak runs in `start-dev` mode over HTTP. A production PS
config with `jwks-url=https://…` will exercise the plugin's TLS
verification of the JWKS endpoint. We should add a test variant that
runs Keycloak with a self-signed cert and confirms the plugin either
trusts it (via a CA bundle) or refuses safely.

## Reference

- PS PR #5941: [PS 10999 8.4 OIDC authentication](https://github.com/percona/percona-server/pull/5941)
- OpenID Connect Core 1.0: https://openid.net/specs/openid-connect-core-1_0.html
- Keycloak: https://www.keycloak.org
- JWT (RFC 7519): https://datatracker.ietf.org/doc/html/rfc7519
