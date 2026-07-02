/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

SharedKeyProvider — CredentialProvider for Azure Shared Key
authentication (--azure-storage-account + --azure-access-key).

Azure Shared Key signing is different from AWS SigV4 canonicalisation
so this provider returns WireMode::HMAC_SHARED_KEY.  Azure_client
will consult it before each request in the same way S3_client does
for HMAC_SIGV4 once Azure gains Bearer support (Managed Identity /
AAD).  Until then the signer holds the same string material directly;
this provider is the seat behind which future Bearer-mode Azure
providers slot in.

Field mapping into HmacCredentials:
  access_key    = storage account name (identity)
  secret_key    = Azure access key (secret material)
  session_token = empty (Azure Shared Key is long-lived)

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

#ifndef XBCLOUD_AUTH_AZURE_SHARED_KEY_PROVIDER_H
#define XBCLOUD_AUTH_AZURE_SHARED_KEY_PROVIDER_H

#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace azure {

class SharedKeyProvider : public CredentialProvider {
 public:
  SharedKeyProvider(std::string storage_account, std::string access_key)
      : creds_{std::move(storage_account), std::move(access_key), {}} {}

  WireMode wire_mode() const override { return WireMode::HMAC_SHARED_KEY; }

  HmacCredentials get_hmac() override { return creds_; }

  std::string get_bearer() override { return {}; }

  void invalidate() override {}  // long-lived; nothing to refresh

  std::string source_description() const override {
    return "azure:shared-key:cli-flags";
  }

 private:
  HmacCredentials creds_;
};

}  // namespace azure
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AZURE_SHARED_KEY_PROVIDER_H
