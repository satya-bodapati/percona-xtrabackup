/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See adc_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "adc_provider.h"

#include <utility>

#include "../oauth2_client.h"
#include "../retry_backoff.h"

namespace xbcloud {
namespace auth {
namespace gcp {

namespace {

// One "attempt" of the mint operation.  Returns Success on a good
// mint, RetryableFailure otherwise so retry_with_backoff will loop.
RetryDecision attempt_mint(const AdcCredential &adc, const std::string &scope,
                           MintOutput *out, std::string *err) {
  bool ok = false;
  if (adc.type == AdcType::kServiceAccount) {
    ok = mint_from_service_account(adc.client_email, adc.private_key,
                                    adc.token_uri, scope, out, err);
  } else {
    ok = mint_from_refresh_token(adc.client_id, adc.client_secret,
                                  adc.refresh_token, adc.token_uri, out, err);
  }
  return ok ? RetryDecision::Success : RetryDecision::RetryableFailure;
}

}  // namespace

AdcProvider::AdcProvider(AdcCredential adc, std::string scope,
                         std::string source_path)
    : adc_(std::move(adc)),
      scope_(std::move(scope)),
      cache_(
          // Lambda captures adc_ + scope_ by reference so mints
          // always use the current stored credential state.  The
          // cache owns single-flight + expiry logic; we own only
          // "what to do when it's time to mint".
          [this]() -> MintResult {
            MintOutput out;
            std::string err;
            RetryPolicy policy;
            const bool ok = retry_with_backoff(
                policy,
                [this, &out](int, std::string *e) {
                  return attempt_mint(adc_, scope_, &out, e);
                },
                &err);
            if (!ok) return MintResult{false, {}, {}, std::move(err)};
            return MintResult{true, std::move(out.access_token),
                              out.expires_at, {}};
          }),
      source_label_(
          std::string("gcp:adc:") +
          (adc_.type == AdcType::kServiceAccount ? "service_account"
                                                  : "authorized_user") +
          (source_path.empty() ? "" : ":" + source_path)) {}

std::string AdcProvider::get_bearer() {
  std::string token;
  std::string err;
  (void)cache_.get(&token, &err);
  return token;
}

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud
