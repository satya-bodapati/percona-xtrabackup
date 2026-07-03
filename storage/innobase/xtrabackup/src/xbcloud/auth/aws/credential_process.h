/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

CredentialProcessProvider — runs an external helper process
(configured via a profile's `credential_process = ...` line) and
consumes its JSON stdout to obtain HMAC credentials.

Follows the AWS SDK convention documented at
https://docs.aws.amazon.com/sdkref/latest/guide/feature-process-credentials.html
Every AWS SDK — Python (boto3), C++, Go, Ruby, JavaScript — reads the
same JSON schema, so any helper already working with `aws` CLI will
work with xbcloud unchanged.

Nice side effect: this one small provider unlocks a lot of shapes
that would otherwise each need their own bespoke code:

  * AWS Roles Anywhere (via the official aws_signing_helper binary)
  * HashiCorp Vault (via vault CLI / vault-agent template)
  * CyberArk (via ccp-cli or the equivalent helper)
  * HSM-backed credentials (via any custom binary that outputs the
    documented JSON)
  * SPIFFE/SPIRE, Keycloak SPIRE integrations, Kerberos-to-AWS
    bridge helpers, and every enterprise-internal secrets tool
    anyone has ever written.

Expected helper stdout:

    {
      "Version": 1,
      "AccessKeyId": "AKIA...",
      "SecretAccessKey": "...",
      "SessionToken": "...",           // optional
      "Expiration": "2026-07-03T..."   // ISO-8601, optional
    }

Missing Expiration → cached forever (until invalidate()).

Refresh policy: same shape as StsAssumeRoleProvider — cache with
5-minute early-refresh guard, mutex-protected mint, retry via
retry_with_backoff on helper failure.

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

#ifndef XBCLOUD_AUTH_AWS_CREDENTIAL_PROCESS_H
#define XBCLOUD_AUTH_AWS_CREDENTIAL_PROCESS_H

#include <chrono>
#include <mutex>
#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace aws {

class CredentialProcessProvider : public CredentialProvider {
 public:
  // command is the raw string from the profile's credential_process
  // line — passed to /bin/sh -c "…" so shell metacharacters (quoting,
  // env vars, argument lists) work exactly as they do with aws CLI.
  explicit CredentialProcessProvider(std::string command,
                                     std::string source_label = {});

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
  bool never_expires_{false};  // set true when helper omits Expiration

  bool mint_locked_(std::string *err);
};

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_CREDENTIAL_PROCESS_H
