/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See gcp_cli_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "gcp_cli_provider.h"

#include <chrono>

#include "run_json.h"

namespace xbcloud {
namespace auth {
namespace cli {

namespace {

// gcloud tokens are 1 h.  Cache slightly under that (55 m) so we mint
// a fresh one before expiry rather than after it.  The 5-min proactive
// guard baked into TokenCache handles the last 5 min of that window.
constexpr auto kAssumedTtl = std::chrono::minutes(55);

// Strip trailing whitespace / newline from the CLI's raw output.
std::string rtrim(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ||
          s.back() == '\t')) {
    s.pop_back();
  }
  return s;
}

}  // namespace

GcpCliProvider::GcpCliProvider(std::string command, std::string source_label)
    : command_(std::move(command)),
      source_label_(std::move(source_label)),
      cache_([this]() -> MintResult {
        std::string body;
        std::string err;
        if (!run_json_command(command_, &body, &err)) {
          return MintResult{false, {}, {}, std::move(err)};
        }
        const std::string tok = rtrim(std::move(body));
        if (tok.empty()) {
          return MintResult{false, {}, {},
                            "gcloud CLI returned empty access token"};
        }
        const auto exp = std::chrono::system_clock::now() + kAssumedTtl;
        return MintResult{true, tok, exp, {}};
      }) {}

std::string GcpCliProvider::get_bearer() {
  std::string tok;
  std::string err;
  (void)cache_.get(&tok, &err);
  return tok;
}

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud
