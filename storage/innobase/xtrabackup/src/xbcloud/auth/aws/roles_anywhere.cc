/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See roles_anywhere.h.

Landing state: interface + skeleton; the X.509-signed POST to
rolesanywhere.amazonaws.com/sessions is deferred to its own PR
alongside the SigV4 extraction work.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "roles_anywhere.h"

#include <utility>

namespace xbcloud {
namespace auth {
namespace aws {

RolesAnywhereProvider::RolesAnywhereProvider(RolesAnywhereConfig cfg)
    : cfg_(std::move(cfg)) {}

HmacCredentials RolesAnywhereProvider::get_hmac() {
  std::lock_guard<std::mutex> lk(mu_);
  const auto now = std::chrono::system_clock::now();
  const bool need_mint = !valid_ ||
                          now + std::chrono::minutes(5) >= expires_at_;
  if (need_mint) {
    std::string err;
    if (!mint_locked_(&err)) return {};
  }
  return cached_;
}

void RolesAnywhereProvider::invalidate() {
  std::lock_guard<std::mutex> lk(mu_);
  valid_ = false;
}

bool RolesAnywhereProvider::mint_locked_(std::string *err) {
  // Follow-up: implement AWS4-X509-RSA-SHA256-signed POST to
  //   https://rolesanywhere.<region>.amazonaws.com/sessions
  // with body
  //   { "trustAnchorArn": ..., "profileArn": ..., "roleArn": ...,
  //     "durationSeconds": ..., "cert": <base64-DER of X509> }
  // Signing uses the private key (RSA-SHA256 over the canonicalised
  // request) rather than an HMAC.  Response is JSON:
  //   { "credentialSet": [ { "credentials": { "accessKeyId": ...,
  //     "secretAccessKey": ..., "sessionToken": ...,
  //     "expiration": "2026-07-03T..." } } ] }
  //
  // The OpenSSL EVP_DigestSign path used by oauth2_client.cc's
  // RS256 signer already covers 80% of the crypto; the remaining
  // work is the canonical-request formation (which differs from
  // both SigV4 and the JWT flow — Roles Anywhere has its own
  // canonicalisation spec).
  *err = "aws::RolesAnywhereProvider: mint_locked_ not yet implemented";
  return false;
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
