/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See swift_cli_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "swift_cli_provider.h"

#include <chrono>
#include <ctime>

#include "my_rapidjson_size_t.h"
#include <rapidjson/document.h>

#include "run_json.h"

namespace xbcloud {
namespace auth {
namespace cli {

namespace {

// openstack CLI emits expires in ISO-8601 without a trailing Z:
// "2026-07-05T18:30:00+0000".  Handle both that and the Z-terminated
// form for robustness.
bool parse_openstack_expires(const std::string &s,
                             std::chrono::system_clock::time_point *out) {
  struct tm tm {};
  if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S%z", &tm) != nullptr) {
    *out = std::chrono::system_clock::from_time_t(timegm(&tm));
    return true;
  }
  if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) != nullptr) {
    *out = std::chrono::system_clock::from_time_t(timegm(&tm));
    return true;
  }
  return false;
}

}  // namespace

SwiftCliProvider::SwiftCliProvider(std::string command,
                                    std::string source_label)
    : command_(std::move(command)),
      source_label_(std::move(source_label)),
      cache_([this]() -> MintResult {
        std::string body;
        std::string err;
        if (!run_json_command(command_, &body, &err)) {
          return MintResult{false, {}, {}, std::move(err)};
        }
        rapidjson::Document d;
        if (d.Parse(body.c_str()).HasParseError() || !d.IsObject()) {
          return MintResult{false, {}, {},
                            "openstack CLI did not return a JSON object"};
        }
        if (!d.HasMember("id") || !d["id"].IsString()) {
          return MintResult{false, {}, {},
                            "openstack CLI response missing id (token)"};
        }
        std::chrono::system_clock::time_point exp =
            std::chrono::system_clock::now() + std::chrono::hours(1);
        if (d.HasMember("expires") && d["expires"].IsString()) {
          (void)parse_openstack_expires(d["expires"].GetString(), &exp);
        }
        return MintResult{true, d["id"].GetString(), exp, {}};
      }) {}

std::string SwiftCliProvider::get_bearer() {
  std::string tok;
  std::string err;
  (void)cache_.get(&tok, &err);
  return tok;
}

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud
