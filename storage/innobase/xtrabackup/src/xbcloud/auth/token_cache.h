/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

TokenCache — thread-safe access-token cache with expiry-aware proactive
refresh and single-flight guarding.

Wraps a mint callback that produces { token, expires_at } and serves
subsequent get() calls from cache until the token approaches expiry,
at which point exactly one thread performs the mint and the rest block
on the shared mutex until it completes.  On invalidate() the cache
drops its value and the next get() re-mints unconditionally, so 401
recovery is a simple invalidate + retry.

Sizing note for callers with many upload workers: xbcloud runs an
N-way parallel upload pool.  All N workers share one CredentialProvider
and therefore one TokenCache.  Single-flight is what prevents the
"thundering herd" that would otherwise hit the mint endpoint N times
in a burst on the expires_at - 5 min boundary.

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

#ifndef XBCLOUD_AUTH_TOKEN_CACHE_H
#define XBCLOUD_AUTH_TOKEN_CACHE_H

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace xbcloud {
namespace auth {

/**
  Result of a mint attempt.  Mint callbacks report success by returning
  MintResult{true, token, expires_at}; on failure they return
  MintResult{false, {}, {}, "why"} so the cache can bubble the error
  up to the caller without swallowing it.
*/
struct MintResult {
  bool ok{false};
  std::string token;
  std::chrono::system_clock::time_point expires_at{};
  std::string error;  // human-readable, populated iff !ok
};

/**
  Callback signature for producing a fresh token.  Implementations do
  the source-specific work (POST to sts.amazonaws.com, POST to
  oauth2.googleapis.com, GET from IMDS, …).  Must be thread-safe if the
  same TokenCache instance is shared across threads (it is — cache-
  wide serialisation is provided by TokenCache's own mutex, so mint
  callbacks are invoked with the mutex held and thus effectively
  single-threaded from the callback's own POV).
*/
using MintFn = std::function<MintResult()>;

/**
  Cache for a single Bearer-shaped credential.  One instance per
  CredentialProvider that returns WireMode::BEARER — do NOT share a
  cache across providers (each provider mints from its own source).
*/
class TokenCache {
 public:
  /**
    early_refresh: how long before expires_at the cache treats a token
    as due for refresh.  Default 5 minutes — enough to absorb typical
    NTP clock skew between the local host and the mint endpoint.
  */
  explicit TokenCache(
      MintFn mint,
      std::chrono::seconds early_refresh = std::chrono::minutes(5));

  TokenCache(const TokenCache &) = delete;
  TokenCache &operator=(const TokenCache &) = delete;

  /**
    Return a currently-valid token.  Refreshes transparently if the
    cache is empty or the cached token is within early_refresh of
    expiry.  Single-flight: multiple concurrent callers see exactly one
    mint call; the rest block on the mutex and receive the freshly-
    minted value.

    On mint failure, `token` is left untouched and `error` is populated
    with the mint callback's error string.  Callers that care about
    the reason should check the error first; callers that just want
    the token can ignore it and check for empty.
  */
  bool get(std::string *token, std::string *error = nullptr);

  /**
    Drop the cached value.  Called by the request layer after a 401 /
    ExpiredToken / TokenRefreshRequired response so the next get()
    forces a fresh mint.  Idempotent.
  */
  void invalidate();

  /**
    Test-only accessor: how many times the mint callback has been
    invoked.  Used by the unit test to prove single-flight under
    concurrent load.
  */
  unsigned long mint_count_for_test() const;

 private:
  MintFn mint_;
  std::chrono::seconds early_refresh_;

  mutable std::mutex mu_;
  std::string token_;
  std::chrono::system_clock::time_point expires_at_{};
  bool valid_{false};
  unsigned long mint_count_{0};

  // Returns true iff (valid_ && now + early_refresh_ < expires_at_).
  // Caller must hold mu_.
  bool cached_still_valid_locked_() const;
};

}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_TOKEN_CACHE_H
