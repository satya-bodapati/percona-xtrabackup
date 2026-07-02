/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

AwsHmacProvider — CredentialProvider implementation for long-lived
AWS SigV4 HMAC credentials passed via --s3-access-key / --s3-secret-key
(and optional --s3-session-token).

Behaviour is trivial: it holds the three strings the caller supplied
and hands them back on every get_hmac() call.  There is no expiry, no
refresh, and invalidate() is a no-op — long-lived HMAC keys don't go
stale on their own.

Temporary-credential providers (EC2 instance profile, STS AssumeRole,
Roles Anywhere) are separate implementations in this same auth/aws/
directory; they own refresh logic internally.  From the S3 code's
perspective all four provider types look the same: a wire_mode() of
HMAC_SIGV4 and a get_hmac() that returns current credentials.

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

#ifndef XBCLOUD_AUTH_AWS_HMAC_PROVIDER_H
#define XBCLOUD_AUTH_AWS_HMAC_PROVIDER_H

#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace aws {

class HmacProvider : public CredentialProvider {
 public:
  HmacProvider(std::string access_key, std::string secret_key,
               std::string session_token = {},
               std::string source_label = "aws:hmac:cli-flags");

  HmacProvider(const HmacProvider &) = delete;
  HmacProvider &operator=(const HmacProvider &) = delete;

  WireMode wire_mode() const override { return WireMode::HMAC_SIGV4; }

  HmacCredentials get_hmac() override { return creds_; }

  std::string get_bearer() override { return {}; }

  // Long-lived keys never expire; invalidate is a no-op.  Kept as a
  // valid override so a request-layer 401 handler can call
  // invalidate() uniformly across provider types.
  void invalidate() override {}

  std::string source_description() const override { return source_label_; }

 private:
  HmacCredentials creds_;
  std::string source_label_;
};

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_HMAC_PROVIDER_H
