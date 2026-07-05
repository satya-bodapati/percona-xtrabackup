/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

SwiftCliProvider — delegates OpenStack Keystone-token acquisition to
the `openstack` CLI via `openstack token issue -f json`.

Resolves the same sources every OpenStack tool does — an openrc file
sourced into the shell, OS_* env vars, clouds.yaml, or explicit
--os-* flags baked into the command string.

Contract:
  * Default command: "openstack token issue -f json".
  * Expected stdout: JSON with `id` (the token) and `expires` (ISO-8601
    UTC).  These are the field names openstack CLI uses when the
    resource is a Token object.

Refresh policy: TokenCache with the `expires`-derived expiry.

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

#ifndef XBCLOUD_AUTH_CLI_SWIFT_CLI_PROVIDER_H
#define XBCLOUD_AUTH_CLI_SWIFT_CLI_PROVIDER_H

#include <string>

#include "../credential_provider.h"
#include "../token_cache.h"

namespace xbcloud {
namespace auth {
namespace cli {

class SwiftCliProvider : public CredentialProvider {
 public:
  explicit SwiftCliProvider(
      std::string command = "openstack token issue -f json",
      std::string source_label = "swift:cli");

  SwiftCliProvider(const SwiftCliProvider &) = delete;
  SwiftCliProvider &operator=(const SwiftCliProvider &) = delete;

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

#endif  // XBCLOUD_AUTH_CLI_SWIFT_CLI_PROVIDER_H
