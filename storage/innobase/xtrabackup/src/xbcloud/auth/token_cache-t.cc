/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Unit tests for xbcloud/auth/token_cache.

Compiled into xbcloud-t when WITH_UNIT_TESTS=ON.  Focuses on the two
behaviours that are hard to get right and would be silently wrong
without tests:

  1. Single-flight under concurrent access — N threads calling get()
     across the refresh boundary produce exactly one mint invocation.
  2. invalidate() forces a re-mint on the next call, so 401 recovery
     works correctly.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "token_cache.h"

namespace {

using xbcloud::auth::MintFn;
using xbcloud::auth::MintResult;
using xbcloud::auth::TokenCache;

TEST(TokenCache, ServesCachedTokenWithoutRefresh) {
  std::atomic<int> calls{0};
  MintFn mint = [&]() -> MintResult {
    ++calls;
    return {true, "tok-" + std::to_string(calls.load()),
            std::chrono::system_clock::now() + std::chrono::hours(1), ""};
  };
  TokenCache c(mint);

  std::string t1, t2, t3;
  ASSERT_TRUE(c.get(&t1));
  ASSERT_TRUE(c.get(&t2));
  ASSERT_TRUE(c.get(&t3));

  EXPECT_EQ("tok-1", t1);
  EXPECT_EQ("tok-1", t2);
  EXPECT_EQ("tok-1", t3);
  EXPECT_EQ(1u, c.mint_count_for_test());
}

TEST(TokenCache, RefreshesWhenWithinEarlyRefreshWindow) {
  std::atomic<int> calls{0};
  MintFn mint = [&]() -> MintResult {
    ++calls;
    // First mint expires in 1 s (well inside the default 5-min
    // early_refresh window, so already treated as due-for-refresh).
    return {true, "tok-" + std::to_string(calls.load()),
            std::chrono::system_clock::now() + std::chrono::seconds(1), ""};
  };
  TokenCache c(mint);

  std::string t1, t2;
  ASSERT_TRUE(c.get(&t1));
  ASSERT_TRUE(c.get(&t2));  // still stale → new mint

  EXPECT_EQ("tok-1", t1);
  EXPECT_EQ("tok-2", t2);
  EXPECT_EQ(2u, c.mint_count_for_test());
}

TEST(TokenCache, InvalidateForcesReMintOnNextGet) {
  std::atomic<int> calls{0};
  MintFn mint = [&]() -> MintResult {
    ++calls;
    return {true, "tok-" + std::to_string(calls.load()),
            std::chrono::system_clock::now() + std::chrono::hours(1), ""};
  };
  TokenCache c(mint);

  std::string t1, t2, t3;
  ASSERT_TRUE(c.get(&t1));
  ASSERT_TRUE(c.get(&t2));  // cache hit
  c.invalidate();
  ASSERT_TRUE(c.get(&t3));  // forced re-mint

  EXPECT_EQ("tok-1", t1);
  EXPECT_EQ("tok-1", t2);
  EXPECT_EQ("tok-2", t3);
  EXPECT_EQ(2u, c.mint_count_for_test());
}

TEST(TokenCache, MintFailurePropagatesError) {
  MintFn mint = [](void) -> MintResult {
    return {false, {}, {}, "simulated IdP outage"};
  };
  TokenCache c(mint);

  std::string tok, err;
  EXPECT_FALSE(c.get(&tok, &err));
  EXPECT_EQ("simulated IdP outage", err);
  EXPECT_TRUE(tok.empty());
}

TEST(TokenCache, MintFailureLeavesCacheInvalidForRetries) {
  std::atomic<int> calls{0};
  MintFn mint = [&]() -> MintResult {
    ++calls;
    if (calls.load() == 1) return {false, {}, {}, "first mint fails"};
    return {true, "tok-recovered",
            std::chrono::system_clock::now() + std::chrono::hours(1), ""};
  };
  TokenCache c(mint);

  std::string tok, err;
  EXPECT_FALSE(c.get(&tok, &err));  // first attempt fails
  EXPECT_EQ("first mint fails", err);

  tok.clear();
  err.clear();
  EXPECT_TRUE(c.get(&tok, &err));  // second attempt succeeds
  EXPECT_EQ("tok-recovered", tok);
  EXPECT_TRUE(err.empty());
  EXPECT_EQ(2u, c.mint_count_for_test());
}

// The load-bearing test for the whole framework.  32 threads hammer
// get() concurrently on a fresh cache.  Because the mint callback
// takes a small but non-zero amount of time (to simulate an actual
// STS / OAuth2 roundtrip), a broken single-flight guard would produce
// multiple mint calls.  Correct single-flight produces exactly one.
TEST(TokenCache, SingleFlightUnderConcurrentAccess) {
  std::atomic<int> calls{0};
  MintFn mint = [&]() -> MintResult {
    ++calls;
    // Sleep to keep the "in flight" state visible long enough that
    // any parallel callers would race the guard if it were broken.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return {true, "tok-single-flight",
            std::chrono::system_clock::now() + std::chrono::hours(1), ""};
  };
  TokenCache c(mint);

  constexpr int kThreads = 32;
  std::vector<std::thread> threads;
  std::vector<std::string> results(kThreads);
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      ASSERT_TRUE(c.get(&results[i]));
    });
  }
  for (auto &t : threads) t.join();

  for (int i = 0; i < kThreads; ++i) {
    EXPECT_EQ("tok-single-flight", results[i])
        << "thread " << i << " got unexpected token";
  }
  EXPECT_EQ(1u, c.mint_count_for_test())
      << "single-flight guard failed — mint was called more than once";
}

}  // namespace
