/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

AwsCliProvider — delegates AWS credential acquisition to the `aws` CLI
(or any drop-in replacement) via `aws configure export-credentials
--format process-credentials`.

Rationale: the AWS CLI already knows how to resolve every AWS
credential source we might care about — static keys, ~/.aws/credentials
profiles, ~/.aws/config chains with role_arn / source_profile /
credential_source / credential_process, EC2 instance profile, ECS
container credentials, STS AssumeRole, RolesAnywhere (via
aws_signing_helper), AssumeRoleWithWebIdentity (EKS IRSA, GitHub
Actions), IAM Identity Center (SSO), and anything AWS ships next.
Shelling out to it, once per credential mint or refresh, gives xbcloud
all of those modes without maintaining a native implementation for
each.

Contract:
  * Command: default "aws configure export-credentials --format
    process-credentials".  Overridable via CLI option so operators can
    point at a specific aws binary or add flags (--profile, --region,
    etc.).
  * Expected stdout: the standard AWS credential_process JSON schema.
    {
      "Version":         1,
      "AccessKeyId":     "AKIA... | ASIA...",
      "SecretAccessKey": "...",
      "SessionToken":    "...",           // optional, omitted for long-lived keys
      "Expiration":      "2026-07-05T18:30:00Z"  // ISO-8601, optional
    }
  * Missing Expiration is treated as "never expires" (matches
    CredentialProcessProvider's behaviour for long-lived keys).

Refresh policy:
  * First get_hmac() call runs the command lazily.
  * Subsequent calls serve from cache until (now + 5 min) >= Expiration.
  * invalidate() drops the cache; next get_hmac() re-runs the CLI.
  * S3_client::retry_error() calls invalidate() on ExpiredToken /
    InvalidToken / TokenRefreshRequired — same flow as every other
    CredentialProvider on the interface.

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

#ifndef XBCLOUD_AUTH_CLI_AWS_CLI_PROVIDER_H
#define XBCLOUD_AUTH_CLI_AWS_CLI_PROVIDER_H

#include <chrono>
#include <mutex>
#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace cli {

class AwsCliProvider : public CredentialProvider {
 public:
  // command defaults to `aws configure export-credentials --format
  // process-credentials`.  If profile is non-empty, ` --profile <name>`
  // is appended.  source_label is used only for logging.
  explicit AwsCliProvider(
      std::string command =
          "aws configure export-credentials --format process-credentials",
      std::string profile = {},
      std::string source_label = "aws:cli");

  AwsCliProvider(const AwsCliProvider &) = delete;
  AwsCliProvider &operator=(const AwsCliProvider &) = delete;

  WireMode wire_mode() const override { return WireMode::HMAC_SIGV4; }
  HmacCredentials get_hmac() override;
  std::string get_bearer() override { return {}; }
  void invalidate() override;
  std::string source_description() const override { return source_label_; }

 private:
  std::string command_;
  std::string source_label_;

  mutable std::mutex mu_;
  HmacCredentials cached_;
  std::chrono::system_clock::time_point expires_at_{};
  bool valid_{false};
  bool never_expires_{false};

  bool mint_locked_(std::string *err);
};

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_CLI_AWS_CLI_PROVIDER_H
