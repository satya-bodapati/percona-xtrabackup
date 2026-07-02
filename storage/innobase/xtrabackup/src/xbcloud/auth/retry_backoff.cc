/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

retry_with_backoff() — see retry_backoff.h.

Sits on top of get_exponential_backoff() from xbcloud/util.h so the
backoff schedule is byte-identical to what the existing HTTP retry
loop in http.cc:631-646 uses.  Deliberately does not call any curl /
HTTP APIs directly — that keeps the helper trivially unit-testable
by injecting a lambda.

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

#include "retry_backoff.h"

#include <chrono>
#include <thread>

#include "../util.h"  // get_exponential_backoff()

namespace xbcloud {
namespace auth {

bool retry_with_backoff(const RetryPolicy &policy, const AttemptFn &attempt,
                        std::string *last_error) {
  std::string local_error;
  for (int i = 1; i <= policy.max_retries + 1; ++i) {
    local_error.clear();
    const RetryDecision decision = attempt(i, &local_error);

    if (decision == RetryDecision::Success) {
      if (last_error != nullptr) last_error->clear();
      return true;
    }
    if (decision == RetryDecision::PermanentFailure) {
      if (last_error != nullptr) *last_error = local_error;
      return false;
    }
    // RetryableFailure — sleep unless we've exhausted our budget.
    if (i > policy.max_retries) break;

    const ulong delay_ms =
        ::xbcloud::get_exponential_backoff(i, policy.max_backoff_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }

  if (last_error != nullptr) *last_error = local_error;
  return false;
}

}  // namespace auth
}  // namespace xbcloud
