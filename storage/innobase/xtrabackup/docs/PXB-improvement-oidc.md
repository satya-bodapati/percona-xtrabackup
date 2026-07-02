# PXB Improvement: Support OIDC authentication in Percona Server for MySQL

Ticket-shaped writeup for the JIRA improvement. Companion to the
implementation notes in `oidc.md`.

## Goal

Support users who enable OpenID Connect (OIDC) authentication in
Percona Server for MySQL 8.4.10+. Such users should be able to perform
xtrabackup backups **without configuring a MySQL password** — the
signed ID token issued by their identity provider is the credential.

Concretely, an operator who already has an OIDC-identified MySQL user
should be able to:

```
xtrabackup --backup --user=my-oidc-user \
    --authentication-openid-connect-client-id-token-file=/path/to/token \
    --target-dir=/backup/full
```

with no `--password`, no `--plugin-dir` override, no `.my.cnf` mapping,
and no other one-off setup.

## User Interface

### New option

`--authentication-openid-connect-client-id-token-file=PATH`

Points at a file containing a valid ID token (a JWT) issued by the
OIDC identity provider (Keycloak, Okta, Auth0, Azure AD, Google Cloud
Identity, etc.). The file is read at connection time — no environment
variables, no in-memory tokens on the command line.

### Interaction with existing options

- **Mutually exclusive with `--password`.** Passing both is a hard
  error before any server contact. The token IS the credential; there
  is no reason to also pass a password, and doing so hints at operator
  confusion.
- **Reuses `--xtrabackup-plugin-dir`.** xtrabackup tells libmysqlclient
  where to find the OIDC client plugin via
  `mysql_options(MYSQL_PLUGIN_DIR, opt_plugin_dir)`, and
  `opt_plugin_dir` is the same value `--xtrabackup-plugin-dir`
  populates (defaulting to the compile-time `PLUGINDIR`). Operators
  who already override the plugin dir for keyring will find it
  automatically applies here too.

### Files shipped

- `authentication_openid_connect_client.so` — the client-side OIDC
  plugin, built from PS PR #5941's source. Installed under
  `${INSTALL_PLUGINDIR}` in the Client component. Same directory as
  the keyring components PXB already ships.

### Log output

Before the connection is opened, xtrabackup logs three lines that
together disambiguate every credential-related decision:

```
[Note] Connecting to MySQL server host: localhost, user: my-oidc-user, password: not set (using OIDC id-token-file), port: not set, socket: /tmp/mysql.sock
[Note] Using OpenID Connect id-token-file '/path/to/token' for authentication to the server.
[Note] OIDC client plugin dir (--xtrabackup-plugin-dir): '/usr/lib/mysql/plugin'
[Note] Loading OIDC client plugin from '/usr/lib/mysql/plugin/authentication_openid_connect_client.so'
```

The last two lines are important when a co-installed Percona Server
also has an OIDC client plugin under its own plugin dir — the log
tells the operator exactly which `.so` is being dlopen'd.

### Error cases

| Condition | Error emitted |
|---|---|
| `--password` + `--authentication-openid-connect-client-id-token-file` both given | `--password and --authentication-openid-connect-client-id-token-file are mutually exclusive; the OIDC ID token is the credential.` |
| Token file missing / unreadable / malformed | `Failed to set id-token-file '<path>' on authentication_openid_connect_client plugin.` |
| Plugin `.so` not present in plugin dir | `authentication_openid_connect_client plugin not found: <libmysqlclient error>` |
| Transport is plain TCP (not TLS/socket/shared-mem) | `The client-server connection is insecure. …` (from the plugin itself) |

## High-Level Design

### How xtrabackup currently handles client authentication plugins

libmysqlclient (statically archived into `xtrabackup`) resolves an
auth plugin the server asks for by looking, in order, at:

1. **In-binary builtins.** `caching_sha2_password`,
   `mysql_native_password`, `sha256_password`, and `clear_password`
   are compiled directly into `libmysqlclient.a` and registered
   in `mysql_client_builtins[]` at library init time. No filesystem
   access.
2. **`.so` dlopen from `MYSQL_PLUGIN_DIR`.** Precedence:
   `mysql_options(mysql, MYSQL_PLUGIN_DIR, …)` if set, else
   `LIBMYSQL_PLUGIN_DIR` env var, else the compile-time `PLUGINDIR`
   constant.

Historically PXB **never** called `mysql_options(MYSQL_PLUGIN_DIR, …)`
and never shipped its own auth `.so`. So anything beyond the four
builtins silently relied on the compile-time `PLUGINDIR` happening to
match the runtime path where a co-installed Percona Server had put
the corresponding `.so`. This is why LDAP / Kerberos / OCI / FIDO
xtrabackup connections work "on my machine" but are brittle across
distributions and install layouts.

### What we import from Percona Server

Percona Server PR #5941 (PS-10999) adds the client-side OIDC plugin
at `libmysql/authentication_openid_connect_client/`. It's a ~250-line
plugin that does exactly four things:

1. Reads the ID-token file from disk.
2. Verifies the file is a well-formed JWT (three base64URL segments,
   sane sizes). No cryptographic verification client-side — the
   signature is verified by the server against its JWKS.
3. Refuses to send the token unless the transport is TLS, unix
   socket, or shared memory. Refuses plain TCP.
4. Sends the token over the plugin VIO.

PXB imports this file verbatim (with a `SYNCED-FROM: percona-server
PR #5941` header at the top) into
`libmysql/authentication_openid_connect_client/`. The CMake setup
mirrors `authentication_oci_client`:

```cmake
MYSQL_ADD_PLUGIN(
  authentication_openid_connect_client
  authentication_openid_connect_client_plugin.cc
  LINK_LIBRARIES mysys
  CLIENT_ONLY
  MODULE_ONLY MODULE_OUTPUT_NAME "authentication_openid_connect_client"
)
```

The `.so` is installed under `${INSTALL_PLUGINDIR}` in the Client
component. Same shape and shipping location as PS's other client
auth plugins.

We also import two small companion changes from the same PR, both
`#ifdef XTRABACKUP`-guarded so the ABI check preprocessor (which
runs without `-DXTRABACKUP`) doesn't see them:

- A `bool is_tls_established;` field on `MYSQL_PLUGIN_VIO_INFO`
  (`include/mysql/plugin_auth_common.h`). The OIDC plugin's
  insecure-transport check reads it.
- The setter for that field in `sql-common/client.cc:mpvio_info()`
  under the `VIO_TYPE_SSL` case.

Nothing else from the PR is needed — the server-side plugin, jwt-cpp,
JWKS fetch, group→role mapping all stay in Percona Server.

### How the plugin gets loaded and driven

Inside `xb_mysql_connect()` in `storage/innobase/xtrabackup/src/backup_mysql.cc`,
before `mysql_real_connect`:

1. If `--password` was also set, hard-error out (mutex).
2. Emit the reformatted `Connecting to MySQL server host: …` line
   with `password: not set (using OIDC id-token-file)`.
3. Emit the `Using OpenID Connect id-token-file '…'` line.
4. Call `mysql_options(connection, MYSQL_PLUGIN_DIR, opt_plugin_dir)`.
5. Emit the `OIDC client plugin dir (--xtrabackup-plugin-dir): '…'` +
   `Loading OIDC client plugin from '…/authentication_openid_connect_client.so'`
   pair.
6. Call `mysql_client_find_plugin("authentication_openid_connect_client",
   MYSQL_CLIENT_AUTHENTICATION_PLUGIN)`. On the first call for a
   given process this triggers `dlopen`.
7. Call `mysql_plugin_options(plugin, "id-token-file", opt_openid_connect_id_token_file)`.
   The plugin's `option()` callback opens the file eagerly and
   rejects invalid paths — so a bad path fails before any server
   contact.
8. Proceed to `mysql_real_connect`. During its authentication
   handshake libmysqlclient will call the OIDC plugin's
   `authenticate_user` callback, which reads the token and hands it
   to the server over the plugin VIO.

### What is passed to the plugin at runtime

Only one option key: `"id-token-file"` → the absolute path.

The plugin reads and validates the file itself; xtrabackup never
touches the token bytes. Over the wire (during the MySQL protocol
handshake) the plugin sends:

- A 2-byte capability field (currently always `0x0001`).
- A length-encoded string carrying the raw JWT (up to 20 KB).

The server-side `auth_openid_connect` plugin then verifies the JWT
signature against its configured JWKS, checks `iss` / `aud` / `exp`
claims, and matches `sub` against the `CREATE USER … IDENTIFIED WITH
'auth_openid_connect' AS '{…,"user":"…"}'` clause.

### Ownership boundary

| Concern | Owner |
|---|---|
| ID-token issuance | Identity provider (Keycloak / Okta / Auth0 / …) |
| Token file placement + rotation | Operator |
| JWKS fetch, signature verify, claim check | Percona Server `auth_openid_connect` plugin |
| Group→role mapping, proxy accounts | Percona Server (already in PR #5941) |
| Reading the file, TLS-safety check, wire transport | PXB client plugin (imported from PS) |
| Wiring plugin_dir, option handoff, `--password` mutex, logs | PXB `xb_mysql_connect()` |

xtrabackup is deliberately dumb about JWT semantics — it just moves
the token from disk to the wire.

## Testing

### Test suite

A new suite `storage/innobase/xtrabackup/test/suites/oidc/` with three
tests, each covering a different IdP scenario:

1. **`oidc_jwt.sh` — locally-signed JWT (fast, deterministic).**
   Uses PS's dummy in-memory JWKS (`mysql-test/std_data/oidc/`) and
   the `create_id_token` helper to sign a JWT that the server plugin
   verifies against the baked-in public key. No live IdP. Also
   covers the two negative paths in `xb_mysql_connect()`: the
   `--password` + OIDC mutex, and the missing-token-file rejection.

2. **`oidc_percona.sh` — live Percona-hosted Keycloak.**
   Hits `https://keycloak.int.percona.com` (mirrors PS's own
   `mysql-test/suite/auth_openid_connect/t/idp.test`), fetches a
   real ID token via ROPC password grant, drives the full backup
   round-trip against it. Skips cleanly if unreachable (no VPN,
   IdP maintenance).

3. **`oidc_keycloak.sh` — dev-provisioned Keycloak.**
   Two modes:
   - **Auto-bootstrap:** `OIDC_BOOTSTRAP_KEYCLOAK=1` → the test
     starts `quay.io/keycloak/keycloak:26.0` (Apache 2.0) in dev
     mode, provisions realm/client/user + audience mapper via
     `kcadm.sh`, fetches a token, installs an `EXIT` trap so the
     container is torn down at end of run.
   - **External Keycloak:** operator provisions the container out of
     band and hands the test its coordinates via `KEYCLOAK_*` env
     vars (same shape as `XBCLOUD_CREDENTIALS`).

Every test uses the same core flow — INSTALL PLUGIN + configure +
CREATE USER + `xtrabackup --backup` + `--prepare` + restore + verify
row count of `sakila.actor` matches the source. Green means the
OIDC-authenticated round-trip actually preserved data, not just that
xtrabackup exited zero.

### Test framework changes

New helpers under `storage/innobase/xtrabackup/test/inc/`:

- **`oidc_common.sh`** — reusable across the three tests:
  - `oidc_require_server_plugin` — env-var gate + plugin-dir discovery.
  - `oidc_install_and_configure_server_plugin` — INSTALL PLUGIN +
    SET GLOBAL `auth_openid_connect_configuration`.
  - `oidc_create_backup_user` — CREATE USER + grants xtrabackup needs.
  - `oidc_extract_sub` — decode JWT, echo `.sub` claim (needed
    because real IdPs stamp UUIDs there rather than login names).
  - `oidc_fetch_ropc_token` — POST an ROPC grant, write ID token
    to a file.
  - `oidc_backup_prepare_restore_verify` — full backup +
    prepare + copy-back + row-count-verify cycle.
- **`oidc_keycloak_docker.sh`** — Docker Keycloak lifecycle:
  container start with health-poll wait, `kcadm.sh` provisioning
  of realm/client/user/mapper, ROPC token fetch, `EXIT`-trap
  teardown. Auto-runs when the caller exports
  `OIDC_BOOTSTRAP_KEYCLOAK=1`.

Each test's log starts with a `vlog` line summarising its required
env vars, so anyone reading `results/<test>` can see the expected
setup even if the test skipped or failed before any real action.

### Environment variables required for testing

**All three tests:**

| Variable | Meaning |
|---|---|
| `AUTH_OIDC_SERVER_SO` | Absolute path to `auth_openid_connect.so` (from a Percona Server 8.4.10+ build). |

**`oidc_jwt.sh` also needs:**

| Variable | Meaning |
|---|---|
| `CREATE_ID_TOKEN` | Path to PS's `create_id_token` helper. |
| `AUTH_OIDC_STD_DATA` | Directory containing `idp_private.pem` + `dummy_oidc_conf.json` (PS `mysql-test/std_data/oidc`). |

**`oidc_percona.sh` also needs:**

| Requirement | Meaning |
|---|---|
| VPN reachability to `keycloak.int.percona.com` | Skipped cleanly if unreachable. |
| `curl`, `jq` on PATH | For ROPC token fetch + sub extraction. |

**`oidc_keycloak.sh` also needs one of:**

Auto-bootstrap:

| Variable | Meaning |
|---|---|
| `OIDC_BOOTSTRAP_KEYCLOAK=1` | Opt into container lifecycle inside the test. |
| `docker`, `curl`, `jq` on PATH | Prerequisites for the bootstrap. |

Or external Keycloak:

| Variable | Meaning |
|---|---|
| `KEYCLOAK_ISSUER` | OIDC issuer URL, must match token's `iss`. |
| `KEYCLOAK_AUDIENCE` | Expected `aud` claim value. |
| `KEYCLOAK_JWKS_URL` | JWKS endpoint URL. |
| `KEYCLOAK_ID_TOKEN` | Path to a valid ID-token file. |
| `KEYCLOAK_MYSQL_USER` | Optional; mysql-side user name (default `mysql_oidc_user`). |

### CI / Jenkins considerations

Following the pattern already used for `xbcloud` tests (external
MinIO / Azurite / fake-gcs / etc.): Jenkins either provisions a
Keycloak container up-front and passes `KEYCLOAK_*` vars to the
test, or sets `OIDC_BOOTSTRAP_KEYCLOAK=1` for isolated per-run
containers. Either shape is a small addition to the existing PXB
Jenkins jobs.

### Skip behavior — clean by design

Every test skips (not fails) when its prerequisites aren't available.
This is important because the suite must run cleanly in a variety of
environments:

| Environment | Expected outcome |
|---|---|
| Dev laptop, no VPN, no Docker, no PS build handy | 3 skipped, 0 failed |
| Dev laptop with PS build, no VPN | 1 pass (`oidc_jwt`) + 2 skipped |
| Percona VPN, no Docker | 2 passes (`oidc_jwt`, `oidc_percona`) + 1 skipped |
| Jenkins with Docker | 3 passes |

Concretely observed today on a laptop with PS 8.4.10 build + VPN:
`./run.sh -f -s oidc` → 2 passed + 1 skipped, in ~34s.

## Deliverables

The work lives on two branches so reviewers can compare the design
choice for the client plugin:

- **`pxb-oidc`** — first-cut with the client plugin compiled as a
  static builtin merged into `libmysqlclient.a` (`ADD_CONVENIENCE_LIBRARY`
  pattern; entry in `mysql_client_builtins[]`). No `.so` shipped.
  Kept as reference and for its clean 4-commit split (source /
  helpers / tests / docs).
- **`pxb-oidc-shared`** — the target design. Plugin ships as a
  loadable `.so` under `${INSTALL_PLUGINDIR}`; xtrabackup calls
  `mysql_options(MYSQL_PLUGIN_DIR, opt_plugin_dir)` to point
  libmysqlclient at it. Same test suite; docs updated to match.

The `.so` shape (`pxb-oidc-shared`) is what should ship.

## References

- PS PR #5941: [PS 10999 8.4 OIDC authentication](https://github.com/percona/percona-server/pull/5941)
- Implementation notes: `storage/innobase/xtrabackup/docs/oidc.md`
- OpenID Connect Core 1.0: https://openid.net/specs/openid-connect-core-1_0.html
- Keycloak: https://www.keycloak.org
