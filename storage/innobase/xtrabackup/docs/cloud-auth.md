# xbcloud CredentialProvider framework

Design + implementation notes for the multi-provider credential
abstraction introduced on the `pxb-cloud-auth` branch.

## Goal

Consolidate cloud-storage credential handling in `xbcloud` (and, on
9.7, `xtrabackup --cloud-storage=…`) behind a single interface so
that:

- Each cloud provider (AWS S3, GCS via XML API, Azure Blob, Swift)
  gets uniform lifecycle handling — proactive refresh, single-flight
  guarding, 401 recovery, provenance logging — without duplicating
  the loop across every provider's signing code path.
- New credential-source types (STS AssumeRole, RolesAnywhere, GCE
  metadata, Azure Managed Identity, GCP ADC / gcloud auth) plug in
  as small provider implementations without touching the request-
  signing layer.
- Existing behaviour is preserved verbatim during the transition —
  every phase-2 commit is `behavior-neutral` on the pre-existing
  auth modes.

## Two-axis model

Cloud storage auth splits along two independent axes:

**Axis 1 — Wire-authentication mode.** How auth material is attached
to each outgoing HTTPS request.

| Provider | Modes |
|---|---|
| AWS S3 | HMAC_SIGV4 only |
| GCS via XML API | HMAC_SIGV4 (interop keys) **or** BEARER (OAuth2) |
| Azure Blob | HMAC_SHARED_KEY **or** BEARER (Azure AD / Managed Identity) |
| Swift | BEARER only (Keystone X-Auth-Token) |

**Axis 2 — Credential lifecycle.** How the material for Axis 1 is
obtained, cached, and refreshed. Uniform across every provider:

```
source lookup (env → keyfile → IMDS → …)
        ↓
credential struct (HmacCredentials or Bearer token)
        ↓
TokenCache (expiry-aware; single-flight; invalidate())
        ↓
consumed by request layer via provider->get_hmac() / get_bearer()
```

Every `CredentialProvider` implementation declares its wire_mode()
(Axis 1) and owns its Axis-2 lifecycle internally.

## Core types

- `xbcloud/auth/credential_provider.h`
  Abstract `CredentialProvider` with `wire_mode()`, `get_hmac()`,
  `get_bearer()`, `invalidate()`, `source_description()`.
- `xbcloud/auth/token_cache.{h,cc}`
  Thread-safe Bearer cache. Proactive refresh at `expires_at - 5min`;
  single-flight guard across N upload workers; `invalidate()` for
  reactive 401 recovery.
- `xbcloud/auth/retry_backoff.{h,cc}`
  `retry_with_backoff(policy, attempt_fn)` wrapping the existing
  PXB-2477 exponential-backoff helper. Used by every provider's
  refresh mint step.
- `xbcloud/auth/adc_lookup.h`
  Templated env-var → keyfile → IMDS resolver ladder. Not yet used
  by any provider (each provider today does its own lookup); the
  helper is available for future providers with more complex source
  chains.

## Providers

| Path | Implements | Wire mode |
|---|---|---|
| `auth/aws/hmac_provider.{h,cc}` | Long-lived HMAC keys (`--s3-access-key/-secret-key`) | HMAC_SIGV4 |
| `auth/aws/ec2_instance_profile.{h,cc}` | EC2 IAM instance profile via IMDS | HMAC_SIGV4 |
| `auth/aws/profile_file.{h,cc}` | `~/.aws/credentials` INI, `--s3-profile` / `AWS_PROFILE` | (yields HmacCredentials, consumed by HmacProvider) |
| `auth/aws/sts_assume_role.{h,cc}` | AWS STS AssumeRole (real SigV4-signed mint) | HMAC_SIGV4 |
| `auth/aws/roles_anywhere.{h,cc}` | AWS Roles Anywhere (X.509-signed mint) | HMAC_SIGV4 |
| `auth/aws/credential_process.{h,cc}` | External helper (Roles Anywhere via aws_signing_helper, Vault, CyberArk, HSM, ...) | HMAC_SIGV4 |
| `auth/aws/profile_resolver.{h,cc}` | Walks `~/.aws/config` chains: role_arn + source_profile / credential_source / credential_process | (dispatches) |
| `auth/gcp/impersonation_provider.{h,cc}` | GCP impersonated_service_account (IAM Credentials API) | BEARER |
| `auth/azure/connection_string.{h,cc}` | `AZURE_STORAGE_CONNECTION_STRING` parser | (feeds SharedKeyProvider) |
| `auth/gcp/interop_hmac_provider.h` | GCS interop HMAC keys | HMAC_SIGV4 |
| `auth/gcp/adc_provider.{h,cc}` | GCP ADC (service_account, authorized_user) | BEARER |
| `auth/gcp/gce_metadata_provider.{h,cc}` | GCE VM metadata service | BEARER |
| `auth/azure/shared_key_provider.h` | Azure Shared Key | HMAC_SHARED_KEY |
| `auth/azure/managed_identity_provider.{h,cc}` | Azure IMDS Managed Identity | BEARER |
| `auth/swift/keystone_provider.h` | Swift Keystone X-Auth-Token | BEARER |

## Request-signing integration

`S3_client::sign()` (in `s3.h`) is the branch point. Before every
`signer->sign_request()` call:

```cpp
if (provider->wire_mode() == BEARER) {
    req.add_header("Authorization", "Bearer " + provider->get_bearer());
    signer->update_keys("", "", "");   // skip SigV4 signing
} else {
    HmacCredentials c = provider->get_hmac();
    signer->update_keys(c.access_key, c.secret_key, c.session_token);
}
signer->sign_request(...);
```

For HMAC providers this is either a no-op (long-lived keys, same
strings every call) or a live refresh (temp creds — the provider's
`get_hmac()` internally re-fetches from IMDS/STS/etc. when
`invalidate()` has been called).

For BEARER providers signing is skipped; the token is attached
directly.

`Azure_client::sign()` mirrors S3's shape. When the registered
provider's `wire_mode()` is `BEARER` (`ManagedIdentityProvider` or a
future AAD-based provider), the Shared Key signer runs first to
populate the required `x-ms-date` and `x-ms-version` headers, then
the `Authorization` header is replaced with
`Bearer <provider->get_bearer()>`. When the provider is null or
`HMAC_SHARED_KEY`, signing goes through `Azure_signer` unchanged.
Byte-identical to pre-refactor behaviour on the Shared Key path.

## Behaviour preservation

Phase-2 migration commits (C5-C10) are byte-neutral on pre-existing
auth flows:

- AWS S3 long-lived keys → same SigV4 signing, same request bytes.
- AWS EC2 IMDS → same IMDS GET, same `update_keys()` push, same
  `retry_error()` → `*retry = true` semantics. The only difference is
  the mechanics move from an inline branch in `retry_error` to
  `provider->invalidate()` + provider-owned refresh on the next `sign()`.
- GCS interop HMAC → unchanged.
- Azure Shared Key → unchanged (provider stashed but not consulted yet).
- Swift Keystone → unchanged (provider stashed but not consulted yet).
- Exponential backoff (PXB-2477) → preserved. Same `get_exponential_backoff`
  primitive; `retry_with_backoff` is a wrapper, not a replacement.

## Features added on top

**GCS OAuth2 / gcloud auth (PXB-3592).** `--google-service-account-file`
option accepting ADC JSON. Reads `application_default_credentials.json`,
supports both `service_account` (JWT-bearer grant) and `authorized_user`
(refresh-token grant). Also honours `GOOGLE_APPLICATION_CREDENTIALS`
env var. Mutually exclusive with `--google-access-key/-secret-key`.
Token refresh transparent across long multipart uploads.

**AWS profile file.** `--s3-profile` option + `AWS_PROFILE` env var.
Reads `~/.aws/credentials` INI. Supports both `[default]` and
`[profile name]` section shapes.

**GCE metadata + Azure Managed Identity.** BEARER providers ready to
be dispatched — probe helpers (`probe_reachable()`) available for
IMDS-first credential source ordering.

## SDK-convention env vars we honour

An operator whose environment is already configured for the AWS
CLI / gcloud / az / openstack CLI gets zero-config xbcloud for
the common shapes:

| Provider | Env var | Purpose |
|---|---|---|
| AWS | `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / `AWS_SESSION_TOKEN` | Long-lived / temporary HMAC keys. |
| AWS | `AWS_PROFILE` | Selects a section from `~/.aws/credentials` + `~/.aws/config`, resolved through the profile chain (role_arn / source_profile / credential_process / credential_source). |
| GCP | `GOOGLE_APPLICATION_CREDENTIALS` | Path to ADC JSON (service_account / authorized_user / impersonated_service_account). |
| Azure | `AZURE_STORAGE_CONNECTION_STRING` | Full connection string parsed for account name + key + endpoint. |
| Azure | `AZURE_STORAGE_ACCOUNT` / `AZURE_STORAGE_KEY` | Discrete equivalents. |
| Swift | `OS_AUTH_URL` / `OS_USERNAME` / `OS_PASSWORD` / `OS_PROJECT_NAME` / `OS_TENANT_NAME` / `OS_REGION_NAME` / `OS_USER_DOMAIN_NAME` / `OS_PROJECT_DOMAIN_NAME` / `OS_IDENTITY_API_VERSION` / ... | Standard openrc file conventions. |

Combined with in-cloud IMDS auto-detection (EC2 / GCE / Azure),
an operator running xbcloud from a properly-configured VM or a
shell that has sourced their credentials file usually needs
`--storage=<x> --<x>-bucket=<name>` and nothing else.

## Planned follow-ups (not on this branch)

- **AWS AssumeRoleWithWebIdentity** (EKS IRSA, GitHub Actions
  federation). Detected in the profile parser via
  `web_identity_token_file` but rejected at resolver time with a
  workaround hint (use `credential_process` with a helper).
- **AWS SSO / IAM Identity Center.** Same — recognised, rejected
  with a workaround hint (`aws sso login` +
  `credential_process` wrapping `aws configure export-credentials`).
- **GCP external_account (Workload Identity Federation).**
  Recognised in `AdcType::kExternalAccount` but the STS token-
  exchange isn't yet implemented. Non-trivial: the source token
  can be a file, URL, or AWS-signed request, and STS token
  exchange has its own request shape.
- **ECS container credentials** (`credential_source =
  EcsContainer`). Same shape as EC2 IMDS but at 169.254.170.2 +
  an env-var-supplied path. Small add.
- **`AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` / `_FULL_URI`** env
  vars — ECS's canonical env-var-driven mode; currently ignored.
- **End-to-end tests** against fake-gcs-server, Azurite, LocalStack
  with the emulator harness in `test/inc/cloud_emu.sh`. Three of
  five are green today; GCS via fake-gcs-server is blocked on
  an upstream compat gap.

## Ownership boundary

`xbcloud` (and, on 9.7, `xtrabackup --cloud-storage=…`) is deliberately
dumb about credential *semantics*. It:

- Reads the credential material from wherever the provider tells it.
- Attaches it to outgoing HTTPS requests.
- Handles 401 by asking the provider to refresh.

Semantic decisions — "is this JWT valid?", "should we chain through
assume-role?", "which resource does this Azure token target?" — live
inside the provider implementations. Adding a new credential source
means writing a new provider class; nothing above `CredentialProvider`
changes.

## References

- `test/inc/cloud_emu.sh` — cherry-picked from `pxb-9.7-PXB-3671`
  (PXB-3843). LocalStack + fake-gcs-server + Azurite + openstackswift
  Docker harness for cloud tests.
- `test/t/xbcloud_gcs_oauth2.sh` — smoke test for PXB-3592 option
  handling.
- PS PR #5941 (PS-10999) — server-side OpenID Connect authentication
  plugin. Unrelated code path (MySQL auth vs cloud auth) but the
  same design pattern (`CredentialProvider`-style interface, expiry-
  aware caching, provenance logging).
