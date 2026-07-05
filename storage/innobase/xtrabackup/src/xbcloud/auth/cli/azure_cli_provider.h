/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

AzureCliProvider — delegates Azure Bearer-token acquisition to the
`az` CLI via `az account get-access-token`.

Resolves the same sources every Microsoft SDK does:
  * az login (developer laptop)
  * Managed Identity (when running on an Azure VM / App Service)
  * Service principal login (az login --service-principal)
  * Workload identity federation (via az login --federated-token)

Contract:
  * Default command: "az account get-access-token
    --resource=https://storage.azure.com/".
  * Expected stdout: JSON with accessToken + expires_on (unix seconds)
    plus a few other fields we ignore.

Refresh policy: TokenCache with the expires_on-derived expiry.

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

#ifndef XBCLOUD_AUTH_CLI_AZURE_CLI_PROVIDER_H
#define XBCLOUD_AUTH_CLI_AZURE_CLI_PROVIDER_H

#include <string>

#include "../credential_provider.h"
#include "../token_cache.h"

namespace xbcloud {
namespace auth {
namespace cli {

class AzureCliProvider : public CredentialProvider {
 public:
  explicit AzureCliProvider(
      std::string command =
          "az account get-access-token --resource=https://storage.azure.com/",
      std::string source_label = "azure:cli");

  AzureCliProvider(const AzureCliProvider &) = delete;
  AzureCliProvider &operator=(const AzureCliProvider &) = delete;

  WireMode wire_mode() const override { return WireMode::BEARER; }
  HmacCredentials get_hmac() override { return {}; }
  std::string get_bearer() override;
  void invalidate() override { cache_.invalidate(); }
  std::string source_description() const override { return source_label_; }

 private:
  std::string command_;
  std::string source_label_;
  TokenCache cache_;
};

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_CLI_AZURE_CLI_PROVIDER_H
