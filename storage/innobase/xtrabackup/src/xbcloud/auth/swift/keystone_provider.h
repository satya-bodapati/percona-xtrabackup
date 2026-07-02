/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

KeystoneProvider — CredentialProvider for OpenStack Swift.  Wraps a
Keystone-issued auth token (X-Auth-Token header for every subsequent
Swift request).  wire_mode() returns BEARER; get_bearer() returns the
stashed token.

For today's xbcloud shape the token is fetched once by Swift_auth at
startup and stays valid for the life of the backup.  invalidate()
would need to trigger a Swift_auth re-run — full refresh support
follows the pattern established by the AWS Ec2InstanceProfileProvider
(cache flag + on-invalidate flip + on-get lazy refresh) and lands as
a follow-up if Swift Keystone token expiry becomes an operational
problem for long backups.

Field mapping into HmacCredentials is unused for Swift — bearer-only.
This provider deliberately does not participate in HMAC_* signing.

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

#ifndef XBCLOUD_AUTH_SWIFT_KEYSTONE_PROVIDER_H
#define XBCLOUD_AUTH_SWIFT_KEYSTONE_PROVIDER_H

#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace swift {

class KeystoneProvider : public CredentialProvider {
 public:
  explicit KeystoneProvider(std::string token) : token_(std::move(token)) {}

  WireMode wire_mode() const override { return WireMode::BEARER; }

  HmacCredentials get_hmac() override { return {}; }

  std::string get_bearer() override { return token_; }

  // Full refresh support (rerun Swift_auth on invalidate) lands as a
  // follow-up if Keystone token expiry causes operational issues on
  // long backups.  For now Swift's existing single-fetch semantics
  // are preserved.
  void invalidate() override {}

  std::string source_description() const override {
    return "swift:keystone:cli-flags";
  }

 private:
  std::string token_;
};

}  // namespace swift
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_SWIFT_KEYSTONE_PROVIDER_H
