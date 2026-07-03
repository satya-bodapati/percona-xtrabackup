/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

ImpersonationProvider — CredentialProvider for GCP's "impersonated
service account" flow.  Given a parent CredentialProvider that
produces a Bearer token for an identity with iam.serviceAccountTokenCreator
permission on a target service account, mints a delegated Bearer
for the target by calling the IAM Credentials API:

  POST <service_account_impersonation_url>
  Authorization: Bearer <parent_access_token>
  Content-Type: application/json
  Body: {"scope":["https://www.googleapis.com/auth/devstorage.read_write"],
         "lifetime":"3600s"}

  → 200 { "accessToken": "...", "expireTime": "2026-07-03T..." }

This matches what `gcloud --impersonate-service-account=<target>` and
every Google SDK does when reading an ADC file with
type=impersonated_service_account.

Refresh policy: TokenCache with proactive refresh (5 min guard) +
single-flight.  invalidate() drops the cache.

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

#ifndef XBCLOUD_AUTH_GCP_IMPERSONATION_PROVIDER_H
#define XBCLOUD_AUTH_GCP_IMPERSONATION_PROVIDER_H

#include <memory>
#include <string>

#include "../credential_provider.h"
#include "../token_cache.h"

namespace xbcloud {
namespace auth {
namespace gcp {

class ImpersonationProvider : public CredentialProvider {
 public:
  // parent must produce a Bearer token (its wire_mode() should be
  // BEARER).  impersonation_url is the full IAM Credentials API
  // endpoint from the ADC file's service_account_impersonation_url.
  ImpersonationProvider(std::unique_ptr<CredentialProvider> parent,
                        std::string impersonation_url,
                        std::string scope =
                            "https://www.googleapis.com/auth/devstorage.read_write",
                        int lifetime_seconds = 3600,
                        std::string source_label = {});

  WireMode wire_mode() const override { return WireMode::BEARER; }

  HmacCredentials get_hmac() override { return {}; }

  std::string get_bearer() override;

  void invalidate() override { cache_.invalidate(); }

  std::string source_description() const override { return source_label_; }

 private:
  std::unique_ptr<CredentialProvider> parent_;
  std::string impersonation_url_;
  std::string scope_;
  int lifetime_seconds_;
  TokenCache cache_;
  std::string source_label_;
};

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_GCP_IMPERSONATION_PROVIDER_H
