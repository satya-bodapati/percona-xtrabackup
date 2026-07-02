/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

CredentialProvider — uniform abstraction for cloud-storage credentials.

The interface has two axes.  Axis 1, `WireMode`, tells the request-layer
whether this provider expects to *sign* the outgoing request (HMAC-SigV4
for AWS S3 / GCS interop / GCS-XML; Azure Shared Key for Azure Blob) or
*attach a Bearer token* (GCS OAuth2, Azure AD, Swift Keystone).  Axis 2
is the credential lifecycle — provider-internal to each implementation:
where the material comes from (env vars, keyfile, IMDS, STS,
RolesAnywhere, OAuth2 token endpoint), how it is refreshed proactively
before expiry, and how it is invalidated when the server rejects a
request with a 401 / ExpiredToken / TokenRefreshRequired code.

Request-signing code (S3_object_store, Azure_object_store, Swift_client)
consults `wire_mode()` before every request and picks the appropriate
credential accessor:

  * HMAC_SIGV4       — sign with the AWS-SigV4 canonical-request
                       algorithm using the credentials from get_hmac().
                       session_token, when present, is emitted as the
                       X-Amz-Security-Token header.
  * HMAC_SHARED_KEY  — sign with the Azure Shared Key algorithm using
                       the credentials from get_hmac().  Azure's
                       canonicalisation is different from SigV4 but the
                       credential lifecycle is uniform.
  * BEARER           — attach the token from get_bearer() as
                       "Authorization: Bearer <token>".

On any 401 / expired-token error the request layer calls invalidate()
and retries once; the provider's next get_hmac() / get_bearer() call
returns fresh material.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*******************************************************/

#ifndef XBCLOUD_AUTH_CREDENTIAL_PROVIDER_H
#define XBCLOUD_AUTH_CREDENTIAL_PROVIDER_H

#include <string>

namespace xbcloud {
namespace auth {

/**
  Credentials produced by an HMAC-style provider.  Consumed by both the
  AWS-SigV4 signer (HMAC_SIGV4) and the Azure Shared Key signer
  (HMAC_SHARED_KEY).  session_token is optional — populated only for
  temporary credentials (IMDS, STS, RolesAnywhere).
*/
struct HmacCredentials {
  std::string access_key;
  std::string secret_key;
  std::string session_token;  // empty for long-lived keys
};

/**
  Wire authentication mode.  Selected by the provider; consumed by the
  request-signing layer to pick which credential accessor to call and
  how to attach the result to outgoing requests.
*/
enum class WireMode {
  HMAC_SIGV4,        // AWS S3, GCS-XML interop keys
  HMAC_SHARED_KEY,   // Azure Shared Key
  BEARER,            // GCS OAuth2, Azure AD, Swift Keystone
};

/**
  Uniform interface for producing credentials for outgoing cloud-storage
  requests.  Implementations own the source-specific acquisition and
  refresh logic; the request layer treats them as opaque credential
  suppliers.
*/
class CredentialProvider {
 public:
  virtual ~CredentialProvider() = default;

  /** Which credential-attachment style this provider produces. */
  virtual WireMode wire_mode() const = 0;

  /**
    Return current HMAC credentials.  Providers refresh transparently
    before returning if the cached material is close to expiry.

    Precondition: wire_mode() is HMAC_SIGV4 or HMAC_SHARED_KEY.
    Callers of BEARER providers must use get_bearer() instead;
    implementations may return default-constructed values here.
  */
  virtual HmacCredentials get_hmac() = 0;

  /**
    Return the current Bearer token string.  Providers refresh
    transparently before returning if the cached token is close to
    expiry.

    Precondition: wire_mode() is BEARER.  HMAC providers may return
    the empty string.
  */
  virtual std::string get_bearer() = 0;

  /**
    Mark cached credentials stale.  Called by the request layer on
    401 / ExpiredToken / TokenRefreshRequired responses.  The next
    get_hmac() / get_bearer() call must refresh from source.
  */
  virtual void invalidate() = 0;

  /**
    Human-readable description of where these credentials came from,
    for provenance logging.  Examples:
      - "aws:hmac:cli-flags"
      - "aws:ec2-instance-profile:my-backup-role"
      - "gcp:adc:service_account:/etc/pxb/sa.json"
      - "gcp:gce-metadata:default"
      - "azure:managed-identity:system-assigned"
    Each provider is responsible for keeping the format stable-ish so
    operators can grep / distinguish sources in logs.
  */
  virtual std::string source_description() const = 0;
};

}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_CREDENTIAL_PROVIDER_H
