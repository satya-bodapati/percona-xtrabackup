/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Unit tests for xbcloud/auth/adc_lookup.

Covers the behaviours that a provider assembling a resolver chain
depends on:

  * First-Yielded-wins ordering.
  * Skip advances to the next step.
  * Fatal aborts the chain, doesn't fall through.
  * All-Skip chain returns empty and reports the labels tried.

Header-only helper so this is a pure logic test.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include <gtest/gtest.h>

#include "adc_lookup.h"

namespace {

using xbcloud::auth::AdcLookup;
using xbcloud::auth::StepResult;

// Simple stand-in credential for tests.
struct FakeCred {
  std::string source;
};

TEST(AdcLookup, FirstYieldWins) {
  AdcLookup<FakeCred> lookup;
  int steps_called = 0;
  lookup
      .also(
          [&](FakeCred *out, std::string *) {
            ++steps_called;
            *out = FakeCred{"env"};
            return StepResult::Yielded;
          },
          "env")
      .also(
          [&](FakeCred *, std::string *) {
            ++steps_called;
            return StepResult::Yielded;  // should not be reached
          },
          "keyfile");

  auto got = lookup.resolve();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ("env", got->source);
  EXPECT_EQ(1, steps_called);
}

TEST(AdcLookup, SkipAdvancesToNextStep) {
  AdcLookup<FakeCred> lookup;
  lookup
      .also(
          [](FakeCred *, std::string *) { return StepResult::Skip; }, "env")
      .also(
          [](FakeCred *out, std::string *) {
            *out = FakeCred{"keyfile"};
            return StepResult::Yielded;
          },
          "keyfile");

  auto got = lookup.resolve();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ("keyfile", got->source);
}

TEST(AdcLookup, FatalAborts) {
  AdcLookup<FakeCred> lookup;
  int later_step_calls = 0;
  lookup
      .also(
          [](FakeCred *, std::string *e) {
            *e = "keyfile path unreadable";
            return StepResult::Fatal;
          },
          "keyfile")
      .also(
          [&](FakeCred *out, std::string *) {
            ++later_step_calls;
            *out = FakeCred{"imds"};
            return StepResult::Yielded;
          },
          "imds");

  std::string err;
  auto got = lookup.resolve(&err);
  EXPECT_FALSE(got.has_value());
  EXPECT_EQ("keyfile path unreadable", err);
  EXPECT_EQ(0, later_step_calls);  // Fatal must not fall through
}

TEST(AdcLookup, AllSkipReportsLabels) {
  AdcLookup<FakeCred> lookup;
  lookup
      .also([](FakeCred *, std::string *) { return StepResult::Skip; }, "env")
      .also(
          [](FakeCred *, std::string *) { return StepResult::Skip; }, "keyfile")
      .also(
          [](FakeCred *, std::string *) { return StepResult::Skip; }, "imds");

  std::string err;
  auto got = lookup.resolve(&err);
  EXPECT_FALSE(got.has_value());
  EXPECT_NE(std::string::npos, err.find("env"));
  EXPECT_NE(std::string::npos, err.find("keyfile"));
  EXPECT_NE(std::string::npos, err.find("imds"));
}

TEST(AdcLookup, EmptyChainReturnsEmpty) {
  AdcLookup<FakeCred> lookup;
  std::string err;
  auto got = lookup.resolve(&err);
  EXPECT_FALSE(got.has_value());
  // err should still be populated (though the "tried:" list is empty).
  EXPECT_NE(std::string::npos, err.find("no credential source"));
}

}  // namespace
