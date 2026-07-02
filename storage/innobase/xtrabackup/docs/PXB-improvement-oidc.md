# PXB-XXXX — OpenID Connect authentication support in Percona XtraBackup

*Design document for the JIRA improvement ticket. Companion to the
implementation notes in `oidc.md`.*

---

## 1. HLD / Goal

Percona Server 8.4.10 (PS PR #5941) ships a server-side OpenID Connect
authentication plugin. A MySQL account created with

```sql
CREATE USER alice IDENTIFIED WITH 'auth_openid_connect' AS '{...}';
```

can authenticate to `mysqld` by presenting a signed ID token (a JWT)
instead of a password. Every stock MySQL client — `mysql`, `mysqldump`,
`mysqlbinlog` — already supports this via the corresponding client
plugin.

**xtrabackup does not.** It cannot back up a server whose backup account
is OIDC-identified: libmysqlclient built into PXB has no OIDC client
plugin to negotiate the handshake, and xtrabackup has no CLI option to
supply a token.

This improvement closes that gap. After this change, an operator can run

```
xtrabackup --backup --user=alice \
    --authentication-openid-connect-client-id-token-file=/path/to/token \
    --target-dir=/backup/full
```

against a PS 8.4.10 instance and get a full backup, no password.

**Out of scope.** Server-side OIDC work (owned by PS PR #5941).
Interactive OAuth flows inside xtrabackup — a device-code prompt from a
backup binary makes no operational sense. Shipping a bundled identity
provider or a token-refresh daemon.

---

## 2. User Interface Changes

### 2.1 New option

```
--authentication-openid-connect-client-id-token-file=PATH
```

Path (absolute or relative) to a file containing a signed JWT ID token.
Name matches PS PR #5941's addition to `client/mysql.cc`; operators
learn it once and it applies across the MySQL toolchain.

### 2.2 Interaction with existing options

| Option | Interaction |
|---|---|
| `--password` | **Mutually exclusive.** Both set → hard error at option-processing time, before any server contact. |
| `--xtrabackup-plugin-dir` | **Reused.** xtrabackup already consults this to locate keyring plugins; the OIDC client `.so` is installed under the same directory, so no new option is needed. Compile-time default is `${DEFAULT_MYSQL_HOME}/${INSTALL_PLUGINDIR}`. |
| `--ssl-mode` | Recommended `REQUIRED` or higher. The OIDC client plugin refuses to send the token over plain TCP; a unix socket or TLS is mandatory. This is the plugin's own behavior, not a new xtrabackup check. |

### 2.3 Files shipped

`authentication_openid_connect_client.so` under `${INSTALL_PLUGINDIR}`
in the `Client` component of the RPM/DEB packages.

### 2.4 Log output

Emitted before `mysql_real_connect()`:

```
[Note] Connecting to MySQL server host: <h>, user: <u>, password: not set (using OIDC id-token-file), port: <p>, socket: <s>
[Note] Using OpenID Connect id-token-file '<path>' for authentication to the server.
[Note] OIDC client plugin dir (--xtrabackup-plugin-dir): '<dir>'
[Note] Loading OIDC client plugin from '<dir>/authentication_openid_connect_client.so'
```

Last two lines disambiguate which `.so` was loaded when a co-installed
Percona Server also has a client-plugin dir on the box.

### 2.5 Error surface

| Condition | Message | Fires at |
|---|---|---|
| `--password` + OIDC option both given | `--password and --authentication-openid-connect-client-id-token-file are mutually exclusive; the OIDC ID token is the credential.` | `xb_mysql_connect` entry, before any I/O |
| Token file missing / unreadable / malformed | `Failed to set id-token-file '<path>' on authentication_openid_connect_client plugin.` | Plugin `option()` callback, before server contact |
| `.so` absent from plugin_dir | `authentication_openid_connect_client plugin not found: <dlopen error>` | `mysql_client_find_plugin` |
| Insecure transport | Plugin's own message: `The client-server connection is insecure. …` | During handshake |

---

## 3. Requirements

### 3.1 Functional

FR1 — `xtrabackup --backup` with a valid OIDC user and a valid token
      succeeds against a PS 8.4.10 instance with `auth_openid_connect`
      installed and configured. `--prepare` + `--copy-back` produce a
      restored server whose data matches the source.

FR2 — Invalid token (missing, malformed, expired, signature mismatch)
      causes a clean failure that names the token file. No server
      contact is initiated for the "missing / malformed" cases.

FR3 — `--password` and the OIDC option together cause a hard error at
      option-processing time. Both option names appear in the message.

FR4 — Every backup-time connection site (`xb_mysql_connect` — full
      backup, `LOCK INSTANCE`, MDL, redo-log-consumer) uses the OIDC
      credential when the option is set. No code path silently falls
      back to password authentication mid-backup.

FR5 — `--xtrabackup-plugin-dir` overrides the compile-time default for
      both existing keyring plugins and the new OIDC client plugin.

FR6 — `xtrabackup --help` documents the new option.

### 3.2 Non-functional

NFR1 — **No new binary in `libmysqlclient`.** The OIDC client plugin is
       `MODULE_ONLY` (loadable `.so`), not archived into
       `libmysqlclient.a`. A security fix ships as a `.so` swap; no
       xtrabackup relink required.

NFR2 — **No ABI change visible to external `libmysqlclient` consumers.**
       The one struct-field addition (`is_tls_established`) is
       `#ifdef XTRABACKUP`-guarded. The ABI check runs without
       `-DXTRABACKUP`, so `include/mysql/*.h.pp` stay unchanged.

NFR3 — **No plaintext token over the wire.** Enforced by the plugin
       itself (refuses non-TLS/socket/shm transports). PXB does not
       relax or wrap this check.

NFR4 — **Skip-clean tests.** In every representative environment the
       suite runs with 0 failures — tests missing prerequisites skip,
       don't fail:

       | Environment | Passes | Skips |
       |---|---|---|
       | dev, no VPN, no Docker, no PS build | 0 | 3 |
       | dev, PS build present, no VPN | 1 (`oidc_jwt`) | 2 |
       | dev + VPN, no Docker | 2 (`oidc_jwt`, `oidc_percona`) | 1 |
       | Jenkins + Docker | 3 | 0 |

NFR5 — **Package inclusion.** RPM (`packaging/rpm/*.spec`) and DEB
       (`packaging/deb/*.install`) definitions for
       `percona-xtrabackup-84` include the new `.so`.

NFR6 — **No new dependency in the xtrabackup binary.** The client
       plugin links only `mysys`. It does not depend on `jwt-cpp`,
       `openssl`, or any transitive new library — JWT signature
       verification is entirely server-side.

NFR7 — **Fail-closed on ambiguity.** When multiple copies of the
       client `.so` could plausibly be loaded (PS's install + PXB's
       install), the operator can determine which one is used by
       reading the xtrabackup log (§2.4). No silent selection.

---

## 4. HLS

### 4.1 Component view

```
       operator                                          identity provider
   (Jenkins or human)                                (Keycloak/Okta/Auth0/…)
           │                                                    │
           │ writes ID token to a file                          │ issues signed
           ▼                                                    │  ID token (JWT)
    ┌──────────────┐    MySQL wire (TLS/socket)  ┌──────────────┴──────────┐
    │  xtrabackup  │───────────────────────────► │  Percona Server 8.4.10  │
    │              │                             │  auth_openid_connect    │
    │  --auth-…    │                             │  (verifies signature    │
    │  -id-token-  │                             │   against JWKS; checks  │
    │   file=…     │                             │   iss/aud/exp/sub)      │
    └──────┬───────┘                             └─────────────────────────┘
           │
           │ mysql_options(MYSQL_PLUGIN_DIR, opt_plugin_dir)
           ▼
    ┌──────────────┐   dlopen    ┌────────────────────────────────┐
    │libmysqlclient│───────────► │ authentication_openid_         │
    │find_plugin() │             │ connect_client.so              │
    └──────────────┘             │ (reads token file, writes it   │
                                 │  over the plugin VIO)          │
                                 └────────────────────────────────┘
```

xtrabackup is deliberately dumb about JWT semantics — it moves the
token from disk to the wire; every semantic check lives in the server
plugin or the client plugin, both imported unchanged from PS.

### 4.2 How xtrabackup currently loads client auth plugins

libmysqlclient (archived into `xtrabackup`) resolves a client auth
plugin the server asks for in this order:

1. **In-binary builtins** — `caching_sha2_password`,
   `mysql_native_password`, `sha256_password`, `clear_password`.
   Statically archived into `libmysqlclient.a`, registered at
   library init via `mysql_client_builtins[]`
   (`sql-common/client.cc:4071`).

2. **`.so` dlopen** — precedence:
   `mysql_options(mysql, MYSQL_PLUGIN_DIR, …)` if set →
   `LIBMYSQL_PLUGIN_DIR` env → compile-time `PLUGINDIR`.

Historically PXB has never called `mysql_options(MYSQL_PLUGIN_DIR, …)`
anywhere, and has never shipped a client auth `.so`. Anything beyond
the four builtins has silently relied on the compile-time `PLUGINDIR`
matching whatever a co-installed PS put its `.so` under — brittle and
undocumented. This improvement is the first time PXB owns a client
auth plugin end-to-end.

### 4.3 What is imported from Percona Server

From `libmysql/authentication_openid_connect_client/` in PS PR #5941:

- `authentication_openid_connect_client_plugin.cc` (~250 lines). Reads
  the token file, does a syntactic JWT-shape check (three base64URL
  segments, sane sizes), refuses to release the token on plain TCP,
  sends it over the plugin VIO. No cryptography client-side.
- `CMakeLists.txt` — rewritten to fit PXB's build layout, but
  structurally the same as `libmysql/authentication_oci_client/`:
  `MYSQL_ADD_PLUGIN(... CLIENT_ONLY MODULE_ONLY MODULE_OUTPUT_NAME ...)`.
  Installs to `${INSTALL_PLUGINDIR}`, Client component.

Two small companion changes needed by the plugin's TLS check
(also from PR #5941), `#ifdef XTRABACKUP`-guarded:

- `include/mysql/plugin_auth_common.h` — adds a `bool
  is_tls_established;` field to `MYSQL_PLUGIN_VIO_INFO`.
- `sql-common/client.cc` — sets that field in `mpvio_info()`'s
  `VIO_TYPE_SSL` branch.

Everything else PS's PR touched — the server plugin under
`plugin/auth_openid_connect/`, the `jwt-cpp` submodule, PS's own
MTR suite — is server-side and not imported.

### 4.4 How the plugin is loaded and driven

`xb_mysql_connect()` in `storage/innobase/xtrabackup/src/backup_mysql.cc`
gains the following before `mysql_real_connect`:

1. If `opt_openid_connect_id_token_file != nullptr && opt_password != nullptr`
   → `xb::error` + `mysql_close` + return null. Satisfies FR3.
2. Emit the connection-summary line with the `password: not set
   (using OIDC id-token-file)` variant.
3. Emit the token-file `[Note]` line.
4. `set_client_ssl_options(connection)` (existing).
5. Emit the plugin-dir `[Note]` and the resolved-`.so`-path `[Note]`.
6. `mysql_options(connection, MYSQL_PLUGIN_DIR, opt_plugin_dir)` —
   `opt_plugin_dir` is already resolved by `xb_set_plugin_dir()`
   from `--xtrabackup-plugin-dir` or the compile-time default.
7. `mysql_client_find_plugin(conn, "authentication_openid_connect_client",
   MYSQL_CLIENT_AUTHENTICATION_PLUGIN)` — this triggers `dlopen` the
   first time it's called. Failure → `xb::error` + close + null.
8. `mysql_plugin_options(plugin, "id-token-file",
   opt_openid_connect_id_token_file)`. The plugin's `option()` callback
   opens the file eagerly and returns non-zero on failure, satisfying
   FR2 without any server contact.
9. Proceed to `mysql_real_connect`. During its handshake libmysqlclient
   dispatches to the OIDC plugin's `authenticate_user` callback, which
   reads the token file (once) and writes it over the plugin VIO.

Because every xtrabackup MySQL connection funnels through
`xb_mysql_connect()`, FR4 (every connection uses OIDC when the option
is set) holds trivially.

### 4.5 What is passed to the plugin

At configuration time, exactly one key/value:

- `"id-token-file"` → absolute path to the token file.

At handshake time (plugin → server, over the VIO):

- 2-byte capability field (currently constant `0x0001`).
- Length-prefixed raw JWT (up to 20 KB).

xtrabackup never handles the token bytes.

### 4.6 Design trade-off: static builtin vs loadable `.so`

Two implementations were prototyped.

**(a) Static builtin.** Compile the plugin into `libmysqlclient.a`
via `ADD_CONVENIENCE_LIBRARY`; register in `mysql_client_builtins[]`.
No plugin_dir handling needed at runtime.
Same shape as `caching_sha2_password_client_plugin` and
`authentication_win`.

**(b) Loadable `.so`.** `MYSQL_ADD_PLUGIN(... CLIENT_ONLY MODULE_ONLY)`,
installed to `${INSTALL_PLUGINDIR}`, dlopen'd via
`mysql_options(MYSQL_PLUGIN_DIR)`. Same shape as
`authentication_ldap_sasl_client`, `authentication_oci_client`,
`authentication_kerberos_client`, `authentication_webauthn_client`,
and PS's own build of the OIDC plugin.

We ship **(b)**:

1. Consistency. Every external client auth plugin in the tree is
   already a `.so`; making OIDC the odd builtin creates a "why is
   this special?" question every time.
2. Substitutability. A `.so` can be replaced for a security fix
   without relinking xtrabackup.
3. Extensibility. The same directory can hold third-party client
   auth plugins later without touching `libmysqlclient` or the
   builtin table.

The static-builtin design lives on the `pxb-oidc` branch as a
reference; the target implementation is on `pxb-oidc-shared`.

### 4.7 Testing strategy

Three tests under `storage/innobase/xtrabackup/test/suites/oidc/`,
one per token source. The rest of the flow (install server plugin,
create user, backup, prepare, restore, verify) is shared via
`test/inc/oidc_common.sh`.

| Test | Token source | Where it runs |
|---|---|---|
| `oidc_jwt.sh` | `create_id_token` locally-signed JWT + dummy JWKS from PS `std_data/oidc/` | Any dev host with a PS 8.4.10 build. Deterministic, ~15 s. Also covers the `--password` + OIDC mutex and the missing-token-file rejection. |
| `oidc_percona.sh` | Live Percona Keycloak (`keycloak.int.percona.com`) via ROPC | Percona VPN. Skipped otherwise. |
| `oidc_keycloak.sh` | Dev Keycloak via Docker (`quay.io/keycloak/keycloak:26.0`) — auto-bootstrap on `OIDC_BOOTSTRAP_KEYCLOAK=1`, or external via `KEYCLOAK_*` env vars | Jenkins or dev with Docker. Skipped otherwise. |

Framework additions under `test/inc/`:

- `oidc_common.sh` — `oidc_require_server_plugin`,
  `oidc_install_and_configure_server_plugin`, `oidc_create_backup_user`,
  `oidc_extract_sub` (real IdPs stamp UUIDs in `sub`, not usernames —
  extracted dynamically so tests survive IdP re-provisioning),
  `oidc_fetch_ropc_token`, `oidc_backup_prepare_restore_verify` (full
  backup + prepare + copy-back + row-count verify).
- `oidc_keycloak_docker.sh` — Docker Keycloak lifecycle: `docker run` +
  health-poll + `kcadm.sh` realm/client/user/mapper provisioning + ROPC
  token fetch + `EXIT`-trap teardown.

Each test's first log line lists the env vars it needs, so anyone
reading `results/<test>` sees the setup even before any skip.

### 4.8 Risks and mitigations

| Risk | Mitigation |
|---|---|
| PS 8.4.10 slips or ships without PR #5941 | Runtime path is inert without a server plugin; tests skip cleanly on `AUTH_OIDC_SERVER_SO` unset. No PXB-side release blocker. |
| ABI drift breaks external `libmysqlclient` consumers | The single struct-field addition is `#ifdef XTRABACKUP`-guarded and the `abi_check` target passes (§ NFR2). |
| Two `.so`s installed (PS's + PXB's) → operator confusion | The two provenance `[Note]` lines (§2.4) name the exact resolved path. |
| Real IdP outage in CI | `oidc_percona.sh` skips on unreachable JWKS URL; `oidc_keycloak.sh` runs in-Docker and is unaffected. |
| Keycloak image tag drift | Pinned to `quay.io/keycloak/keycloak:26.0` in the helper. Bump is a one-line change. |
| ROPC used in tests is production-inappropriate | The plugin's public API takes a token file — how it was produced is opaque. Production uses whatever flow the IdP mandates. Documented in test headers. |

### 4.9 Acceptance criteria

1. `pxb-oidc-shared` reviewed and merged to `8.4`.
2. `packaging/rpm/*.spec` and `packaging/deb/*.install` for
   `percona-xtrabackup-84` include the new `.so`.
3. Jenkins runs the 3-test OIDC suite green (all three passes) at
   least once against a Docker-provisioned Keycloak.
4. `xtrabackup --help` output shows the new option.
5. docs.percona.com PXB reference documents the option and links to a
   short "using OIDC with PXB" walkthrough.
6. First release carrying the change calls it out in release notes.

### 4.10 Dependencies

- **Hard.** Percona Server 8.4.10 with PR #5941 merged, and the
  matching `create_id_token` helper built and shipped alongside.
- **Soft.** `jwt-cpp` submodule initialized in the PS build tree
  (server-side dependency; PXB itself does not depend on it).
- **CI.** Docker CLI + egress to `quay.io/keycloak` for the auto-bootstrap
  test, or a persistent Keycloak provisioned by the Jenkins job.
- **Testing (optional).** VPN access to `keycloak.int.percona.com`
  for the live-IdP variant.

### 4.11 Deliverables

- Branch `pxb-oidc-shared` — target implementation, ready for review.
- Branch `pxb-oidc` — reference implementation (static-builtin design),
  kept for review-time comparison.
- `storage/innobase/xtrabackup/docs/oidc.md` — implementation-focused
  notes for future maintainers.
- This document — design + plan for the improvement ticket.

---

## References

- PS PR #5941: [PS-10999 [8.4]: OIDC Authentication for Percona Server](https://github.com/percona/percona-server/pull/5941)
- OpenID Connect Core 1.0: <https://openid.net/specs/openid-connect-core-1_0.html>
- RFC 7519 (JWT): <https://datatracker.ietf.org/doc/html/rfc7519>
- Keycloak (Apache 2.0): <https://www.keycloak.org>
