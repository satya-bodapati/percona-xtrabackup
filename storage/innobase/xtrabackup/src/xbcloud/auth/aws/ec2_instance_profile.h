/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Ec2InstanceProfileProvider — CredentialProvider wrapper around the
existing S3_ec2_instance metadata client (PXB-2856).  Provides
uniform get_hmac() / invalidate() semantics for the request-signing
layer so it doesn't have to special-case EC2 vs long-lived HMAC vs
future temporary-credential providers.

Refresh policy:
  * First get_hmac() call: fetch_metadata() lazily if we haven't yet.
  * invalidate(): sets a "stale" flag; next get_hmac() re-fetches.
    Called by S3_client::retry_error() on the ExpiredToken /
    InvalidToken / TokenRefreshRequired response codes AWS returns
    when the temp creds have rotated out under us.

For xbcloud's parallel-upload model the provider is shared across
all worker threads; get_hmac() is guarded by a mutex so exactly one
thread performs the fetch on a refresh boundary.

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

#ifndef XBCLOUD_AUTH_AWS_EC2_INSTANCE_PROFILE_H
#define XBCLOUD_AUTH_AWS_EC2_INSTANCE_PROFILE_H

#include <memory>
#include <mutex>
#include <string>

#include "../credential_provider.h"
#include "xbcloud/s3_ec2.h"

namespace xbcloud {
namespace auth {
namespace aws {

class Ec2InstanceProfileProvider : public CredentialProvider {
 public:
  explicit Ec2InstanceProfileProvider(
      std::shared_ptr<S3_ec2_instance> instance);

  Ec2InstanceProfileProvider(const Ec2InstanceProfileProvider &) = delete;
  Ec2InstanceProfileProvider &operator=(const Ec2InstanceProfileProvider &) =
      delete;

  WireMode wire_mode() const override { return WireMode::HMAC_SIGV4; }

  HmacCredentials get_hmac() override;

  std::string get_bearer() override { return {}; }

  void invalidate() override;

  std::string source_description() const override {
    return "aws:ec2-instance-profile";
  }

 private:
  std::shared_ptr<S3_ec2_instance> instance_;
  mutable std::mutex mu_;
  bool have_creds_{false};
};

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_EC2_INSTANCE_PROFILE_H
