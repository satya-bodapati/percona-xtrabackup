/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

RolesAnywhereProvider — CredentialProvider that mints AWS temporary
credentials via IAM Roles Anywhere.  Unlike EC2 instance profile
(HMAC-based) or STS AssumeRole (HMAC-signed request to STS), Roles
Anywhere authenticates to AWS with a private X.509 certificate:
you sign a POST to https://rolesanywhere.amazonaws.com/sessions
using the private key that pairs with the trust-anchor cert
configured in your AWS account, and receive short-lived HMAC
temporary credentials in return.

Flow:
  1. Operator provides:
       --s3-rolesanywhere-cert=<path to X.509 cert PEM>
       --s3-rolesanywhere-private-key=<path to matching key PEM>
       --s3-rolesanywhere-trust-anchor-arn=<arn>
       --s3-rolesanywhere-profile-arn=<arn>
       --s3-rolesanywhere-role-arn=<arn>
  2. On mint: read cert + private key, construct a request signed
     with the private key over a canonicalised form of the request
     (Roles Anywhere uses its own scheme — AWS4-X509-RSA-SHA256 —
     which is similar in spirit to SigV4 but keyed on the X.509
     signature rather than on an HMAC).
  3. Parse the response JSON:
       { "credentialSet": [ { "credentials": { "accessKeyId": ...,
         "secretAccessKey": ..., "sessionToken": ...,
         "expiration": ... } } ] }
  4. Cache with expires_at, refresh on expiry-near / invalidate().

------------------------------------------------------------------
STATUS AT LANDING: interface + skeleton only.  The X.509-signed
POST to rolesanywhere.amazonaws.com is the biggest single crypto
piece in the framework — writing it in a rushed pass would be
irresponsible.  The provider compiles and exposes its intended
API; the mint step returns a "not yet implemented" error until
the crypto lands as its own dedicated PR (with its own review
attention).

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

#ifndef XBCLOUD_AUTH_AWS_ROLES_ANYWHERE_H
#define XBCLOUD_AUTH_AWS_ROLES_ANYWHERE_H

#include <chrono>
#include <mutex>
#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace aws {

struct RolesAnywhereConfig {
  std::string cert_pem_path;
  std::string private_key_pem_path;
  std::string trust_anchor_arn;
  std::string profile_arn;
  std::string role_arn;
  std::string region = "us-east-1";
  int duration_seconds = 3600;
};

class RolesAnywhereProvider : public CredentialProvider {
 public:
  explicit RolesAnywhereProvider(RolesAnywhereConfig cfg);

  WireMode wire_mode() const override { return WireMode::HMAC_SIGV4; }

  HmacCredentials get_hmac() override;

  std::string get_bearer() override { return {}; }

  void invalidate() override;

  std::string source_description() const override {
    return "aws:roles-anywhere:" + cfg_.role_arn;
  }

 private:
  RolesAnywhereConfig cfg_;

  mutable std::mutex mu_;
  HmacCredentials cached_;
  std::chrono::system_clock::time_point expires_at_{};
  bool valid_{false};

  bool mint_locked_(std::string *err);
};

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_ROLES_ANYWHERE_H
