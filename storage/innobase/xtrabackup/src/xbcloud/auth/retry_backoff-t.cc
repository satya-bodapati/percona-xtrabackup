/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Unit tests for xbcloud/auth/retry_backoff.  Focused on the three
behaviours callers actually depend on:

  1. Success short-circuits (no unnecessary sleeps).
  2. PermanentFailure stops retrying immediately.
  3. RetryableFailure loops up to max_retries + 1 attempts.

Sleep durations are not asserted precisely — get_exponential_backoff()
adds up-to-1 s of jitter and caps at max_backoff_ms.  Tests use
max_backoff_ms=0 to make retryable-failure tests fast.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include <gtest/gtest.h>

#include "retry_backoff.h"

namespace {

using xbcloud::auth::RetryDecision;
using xbcloud::auth::RetryPolicy;
using xbcloud::auth::retry_with_backoff;

TEST(RetryBackoff, SuccessOnFirstAttemptDoesNotRetry) {
  int calls = 0;
  RetryPolicy p{5, 0};
  const bool ok = retry_with_backoff(p, [&](int attempt, std::string *) {
    ++calls;
    EXPECT_EQ(1, attempt);
    return RetryDecision::Success;
  });
  EXPECT_TRUE(ok);
  EXPECT_EQ(1, calls);
}

TEST(RetryBackoff, PermanentFailureShortCircuits) {
  int calls = 0;
  RetryPolicy p{5, 0};
  std::string err;
  const bool ok = retry_with_backoff(
      p,
      [&](int, std::string *e) {
        ++calls;
        *e = "invalid credentials";
        return RetryDecision::PermanentFailure;
      },
      &err);
  EXPECT_FALSE(ok);
  EXPECT_EQ(1, calls);  // no retry on a permanent failure
  EXPECT_EQ("invalid credentials", err);
}

TEST(RetryBackoff, RetryableFailureLoopsUpToMaxRetriesPlusOne) {
  int calls = 0;
  // max_backoff_ms=0 → get_exponential_backoff returns 0..1 ms → tests
  // finish quickly.  Sleep resolution below 1 ms doesn't matter.
  RetryPolicy p{3, 0};
  std::string err;
  const bool ok = retry_with_backoff(
      p,
      [&](int, std::string *e) {
        ++calls;
        *e = "transient net error";
        return RetryDecision::RetryableFailure;
      },
      &err);
  EXPECT_FALSE(ok);
  // Attempts: 1 + max_retries = 4 (i.e. up to policy.max_retries+1).
  EXPECT_EQ(4, calls);
  EXPECT_EQ("transient net error", err);
}

TEST(RetryBackoff, RetryUntilEventualSuccess) {
  int calls = 0;
  RetryPolicy p{5, 0};
  const bool ok = retry_with_backoff(p, [&](int attempt, std::string *e) {
    ++calls;
    if (attempt < 3) {
      *e = "keep trying";
      return RetryDecision::RetryableFailure;
    }
    return RetryDecision::Success;
  });
  EXPECT_TRUE(ok);
  EXPECT_EQ(3, calls);
}

TEST(RetryBackoff, LastErrorClearedOnEventualSuccess) {
  RetryPolicy p{3, 0};
  std::string err = "sentinel";
  const bool ok = retry_with_backoff(
      p,
      [](int attempt, std::string *e) {
        if (attempt == 1) {
          *e = "hiccup";
          return RetryDecision::RetryableFailure;
        }
        return RetryDecision::Success;
      },
      &err);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(err.empty()) << "last_error should be cleared on success";
}

}  // namespace
