/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

RetryPolicy — thin wrapper around the existing exponential-backoff
helper (get_exponential_backoff() in xbcloud/util.h, PXB-2477) so that
credential-provider refresh calls can share the same retry semantics
as the HTTP request layer without either side duplicating the loop.

The primitive stays put (get_exponential_backoff in util.h); this
header adds a callable-driven retry loop on top of it so provider
refresh code can be written as:

    RetryPolicy policy{max_retries, max_backoff_ms};
    retry_with_backoff(policy, [&](int attempt, std::string *err) {
      if (call_sts_or_imds_or_oauth2()) return RetryDecision::Success;
      if (permanent()) { *err = "..."; return RetryDecision::PermanentFailure; }
      *err = "transient network error";
      return RetryDecision::RetryableFailure;
    });

Behaviour-neutral to the existing HTTP retry path in http.cc:631-646:
that code continues to call get_exponential_backoff() directly and is
not touched by this commit.

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

#ifndef XBCLOUD_AUTH_RETRY_BACKOFF_H
#define XBCLOUD_AUTH_RETRY_BACKOFF_H

#include <cstdint>
#include <functional>
#include <string>

namespace xbcloud {
namespace auth {

/**
  Outcome of one attempt inside retry_with_backoff.  Callables must
  distinguish between "done, worked", "done, will never work" (skip
  the remaining retries), and "worked around a transient hiccup, try
  again" so we don't waste retries on permanent failures like
  401 / invalid-credential.
*/
enum class RetryDecision {
  Success,           // Stop; report success.
  PermanentFailure,  // Stop; report failure.  Do not retry.
  RetryableFailure,  // Sleep+retry (unless attempts are exhausted).
};

/**
  Retry parameters.  Defaults mirror xbcloud's existing HTTP retry
  configuration so provider refresh calls behave the same way HTTP
  requests already do: up to 5 attempts, exponential 2^n backoff, cap
  at 32 s per sleep, plus 0-1 s jitter (from get_exponential_backoff).
*/
struct RetryPolicy {
  int max_retries = 5;
  uint64_t max_backoff_ms = 32000;
};

/**
  Callback signature.  Receives the 1-based attempt number (for
  logging) and an out-parameter for a human-readable error string.
*/
using AttemptFn = std::function<RetryDecision(int attempt, std::string *error)>;

/**
  Drive `attempt` up to `policy.max_retries + 1` times, sleeping
  between attempts according to get_exponential_backoff().  Returns
  true iff any invocation returned RetryDecision::Success.

  Populates `*last_error` (if non-null) with the error string from
  the last failing attempt.  A PermanentFailure return from the
  callback short-circuits and returns false immediately, no more
  retries.
*/
bool retry_with_backoff(const RetryPolicy &policy, const AttemptFn &attempt,
                        std::string *last_error = nullptr);

}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_RETRY_BACKOFF_H
