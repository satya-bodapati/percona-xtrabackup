/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Thread-safe Bearer token cache with expiry-aware proactive refresh.
See token_cache.h for the contract.

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

#include "token_cache.h"

#include <utility>

namespace xbcloud {
namespace auth {

TokenCache::TokenCache(MintFn mint, std::chrono::seconds early_refresh)
    : mint_(std::move(mint)), early_refresh_(early_refresh) {}

bool TokenCache::cached_still_valid_locked_() const {
  if (!valid_) return false;
  const auto now = std::chrono::system_clock::now();
  return now + early_refresh_ < expires_at_;
}

bool TokenCache::get(std::string *token, std::string *error) {
  // Single-flight: the mutex is held across the mint call, so
  // concurrent callers on the refresh boundary block here and, when
  // the leading caller finishes, wake up and see the fresh cached
  // value on the next iteration of the check.  Simpler than a
  // dedicated in-flight signal + condvar and fine at xbcloud's
  // concurrency scale (N workers where N is single- or double-digit).
  std::unique_lock<std::mutex> lk(mu_);

  if (cached_still_valid_locked_()) {
    if (token != nullptr) *token = token_;
    return true;
  }

  // Cache miss or nearing expiry — mint under the lock.
  MintResult r = mint_();
  ++mint_count_;
  if (!r.ok) {
    valid_ = false;
    token_.clear();
    if (error != nullptr) *error = std::move(r.error);
    return false;
  }

  token_ = std::move(r.token);
  expires_at_ = r.expires_at;
  valid_ = true;
  if (token != nullptr) *token = token_;
  return true;
}

void TokenCache::invalidate() {
  std::lock_guard<std::mutex> lk(mu_);
  valid_ = false;
  token_.clear();
  // expires_at_ intentionally left as-is: it's meaningless once
  // valid_ is false, and clearing it would be extra work with no
  // observable effect.
}

unsigned long TokenCache::mint_count_for_test() const {
  std::lock_guard<std::mutex> lk(mu_);
  return mint_count_;
}

}  // namespace auth
}  // namespace xbcloud
