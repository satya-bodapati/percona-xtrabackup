/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

GcpCliProvider — delegates GCP Bearer-token acquisition to the
`gcloud` CLI via `gcloud auth application-default print-access-token`.

The gcloud CLI resolves credentials from the same sources every other
Google-authored tool does:
  * gcloud auth application-default login  (developer laptop)
  * GOOGLE_APPLICATION_CREDENTIALS pointing at a service-account JSON
  * GCE metadata service (when running on a GCE VM)
  * impersonated_service_account chains
  * Workload Identity Federation (external_account ADC files)
Shelling out to gcloud once per mint gives xbcloud all of the above
without a native implementation for each.

Caveat: `gcloud auth ... print-access-token` prints only the token
string — no expiry.  We conservatively treat the token as valid for
55 minutes (gcloud tokens are 1 h) and invalidate on 401.  When
gcloud grows a JSON output mode with expiry, we can tighten this.

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

#ifndef XBCLOUD_AUTH_CLI_GCP_CLI_PROVIDER_H
#define XBCLOUD_AUTH_CLI_GCP_CLI_PROVIDER_H

#include <string>

#include "../credential_provider.h"
#include "../token_cache.h"

namespace xbcloud {
namespace auth {
namespace cli {

class GcpCliProvider : public CredentialProvider {
 public:
  explicit GcpCliProvider(
      std::string command = "gcloud auth application-default print-access-token",
      std::string source_label = "gcp:cli");

  GcpCliProvider(const GcpCliProvider &) = delete;
  GcpCliProvider &operator=(const GcpCliProvider &) = delete;

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

#endif  // XBCLOUD_AUTH_CLI_GCP_CLI_PROVIDER_H
