/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

ManagedIdentityProvider — CredentialProvider for Azure Managed
Identity (system-assigned or user-assigned).  When xbcloud runs on
an Azure VM, App Service, or similar with a Managed Identity
attached, the local IMDS at 169.254.169.254 returns a Bearer token
for the requested resource (Azure Blob Storage in our case).

Endpoint:
  GET http://169.254.169.254/metadata/identity/oauth2/token
      ?api-version=2018-02-01
      &resource=https://storage.azure.com/
      [&client_id=<uuid>]  ← user-assigned identity selector
  Header: Metadata: true

The 'resource' parameter is Azure-specific: unlike AWS/GCP which
issue a general-purpose access token, Azure requires the caller to
declare which service the token is for.  For xbcloud that's
storage.azure.com.

Refresh policy: same as other Bearer providers — TokenCache with
proactive refresh + single-flight guard.

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

#ifndef XBCLOUD_AUTH_AZURE_MANAGED_IDENTITY_PROVIDER_H
#define XBCLOUD_AUTH_AZURE_MANAGED_IDENTITY_PROVIDER_H

#include <string>

#include "../credential_provider.h"
#include "../token_cache.h"

namespace xbcloud {
namespace auth {
namespace azure {

class ManagedIdentityProvider : public CredentialProvider {
 public:
  // client_id optional; empty selects the system-assigned identity.
  explicit ManagedIdentityProvider(
      std::string resource = "https://storage.azure.com/",
      std::string client_id = "");

  WireMode wire_mode() const override { return WireMode::BEARER; }
  HmacCredentials get_hmac() override { return {}; }
  std::string get_bearer() override;
  void invalidate() override { cache_.invalidate(); }
  std::string source_description() const override { return source_label_; }

  static bool probe_reachable();

 private:
  std::string resource_;
  std::string client_id_;
  TokenCache cache_;
  std::string source_label_;
};

}  // namespace azure
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AZURE_MANAGED_IDENTITY_PROVIDER_H
