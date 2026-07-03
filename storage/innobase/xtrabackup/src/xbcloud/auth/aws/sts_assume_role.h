/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

StsAssumeRoleProvider — CredentialProvider that mints AWS temporary
credentials by calling sts:AssumeRole under a parent identity.

Flow:
  1. Parent CredentialProvider produces long-lived HMAC credentials
     (from --s3-access-key/--s3-secret-key, from --s3-profile, or
     from an already-fetched EC2 instance profile).
  2. On the first get_hmac() / after invalidate(), we sign a POST
     to https://sts.<region>.amazonaws.com/ with the parent creds
     using SigV4 (service="sts").  Request body:
       Action=AssumeRole&Version=2011-06-15
       &RoleArn=<arn>&RoleSessionName=<name>
       &DurationSeconds=<int, default 3600>
       [&ExternalId=<opt>]
  3. Response is XML: <AssumeRoleResult><Credentials>
       <AccessKeyId/><SecretAccessKey/><SessionToken/><Expiration/>
     </Credentials></AssumeRoleResult>
  4. Cache the returned temp creds with expires_at from the
     Expiration field.  Serve them via get_hmac() until near expiry.

Refresh policy: TokenCache-shaped, but the cached unit is
HmacCredentials rather than a Bearer string.  Rather than embedding
TokenCache (which is Bearer-specialized), this provider owns its
own {creds, expires_at, mutex, single-flight guard} state.

Retry policy: mint step is wrapped in retry_with_backoff so a
transient sts.amazonaws.com outage doesn't propagate as an auth
failure to the operator.

------------------------------------------------------------------
STATUS AT LANDING: interface + provider skeleton only.  The SigV4
signing of the STS request itself is TODO — the algorithm is
identical to S3's S3_signerV4 but keyed on service="sts", so the
implementation naturally sits in a follow-up alongside a small
extraction of the SigV4 canonical-request helper.  Until that
follows, get_hmac() logs and returns empty credentials.

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

#ifndef XBCLOUD_AUTH_AWS_STS_ASSUME_ROLE_H
#define XBCLOUD_AUTH_AWS_STS_ASSUME_ROLE_H

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace aws {

struct StsAssumeRoleConfig {
  // Parent credentials source.  StsAssumeRoleProvider consumes its
  // get_hmac() to sign the STS request.
  std::unique_ptr<CredentialProvider> parent;

  std::string region = "us-east-1";  // STS regional endpoint
  std::string role_arn;               // arn:aws:iam::<acct>:role/<name>
  std::string role_session_name = "xbcloud-backup";
  std::string external_id;           // optional
  int duration_seconds = 3600;
};

class StsAssumeRoleProvider : public CredentialProvider {
 public:
  explicit StsAssumeRoleProvider(StsAssumeRoleConfig cfg);

  WireMode wire_mode() const override { return WireMode::HMAC_SIGV4; }

  HmacCredentials get_hmac() override;

  std::string get_bearer() override { return {}; }

  void invalidate() override;

  std::string source_description() const override {
    return "aws:sts-assume-role:" + cfg_.role_arn;
  }

 private:
  StsAssumeRoleConfig cfg_;

  // Cached temp creds + expiry.  Guarded by mu_.  On expiry-near
  // OR after invalidate(), mint_locked_() re-runs the STS call.
  mutable std::mutex mu_;
  HmacCredentials cached_;
  std::chrono::system_clock::time_point expires_at_{};
  bool valid_{false};

  bool mint_locked_(std::string *err);
};

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_STS_ASSUME_ROLE_H
