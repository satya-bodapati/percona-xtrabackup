/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See sts_assume_role.h.

Landing state: interface + skeleton; the SigV4-signed POST to
sts.amazonaws.com + XML response parsing are the follow-up work.
get_hmac() currently returns empty credentials and logs the
skeleton status so operators aren't left guessing.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "sts_assume_role.h"

#include <utility>

namespace xbcloud {
namespace auth {
namespace aws {

StsAssumeRoleProvider::StsAssumeRoleProvider(StsAssumeRoleConfig cfg)
    : cfg_(std::move(cfg)) {}

HmacCredentials StsAssumeRoleProvider::get_hmac() {
  std::lock_guard<std::mutex> lk(mu_);
  const auto now = std::chrono::system_clock::now();
  const bool need_mint = !valid_ ||
                          now + std::chrono::minutes(5) >= expires_at_;
  if (need_mint) {
    std::string err;
    if (!mint_locked_(&err)) {
      // Return empty credentials.  Consumer's sign() will then
      // produce an unsigned request, the server will reject with
      // 401/403, and retry_error() will call our invalidate() —
      // giving the operator a clear failure surface.  Once
      // mint_locked_ is implemented the caller sees the same
      // signature as the current session_token flow.
      return {};
    }
  }
  return cached_;
}

void StsAssumeRoleProvider::invalidate() {
  std::lock_guard<std::mutex> lk(mu_);
  valid_ = false;
}

bool StsAssumeRoleProvider::mint_locked_(std::string *err) {
  // Follow-up: implement SigV4-signed POST to
  // https://sts.<region>.amazonaws.com/ with body
  //   Action=AssumeRole&Version=2011-06-15&RoleArn=...&RoleSessionName=...
  //   &DurationSeconds=...[&ExternalId=...]
  // and parse the returned <AssumeRoleResult><Credentials>...</> XML.
  //
  // The SigV4 canonical-request algorithm is already in
  // xbcloud/s3.cc's S3_signerV4; extracting a service-agnostic
  // helper is what unblocks this provider.  Same helper will serve
  // the RolesAnywhere provider (sts_role_anywhere.{h,cc}) which
  // signs its POST with an X.509 cert instead of an HMAC key but
  // otherwise looks identical.
  *err = "aws::StsAssumeRoleProvider: mint_locked_ not yet implemented";
  return false;
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
