/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See azure_cli_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "azure_cli_provider.h"

#include <chrono>

#include "my_rapidjson_size_t.h"
#include <rapidjson/document.h>

#include "run_json.h"

namespace xbcloud {
namespace auth {
namespace cli {

AzureCliProvider::AzureCliProvider(std::string command,
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
                            "az CLI did not return a JSON object"};
        }
        if (!d.HasMember("accessToken") || !d["accessToken"].IsString()) {
          return MintResult{false, {}, {},
                            "az CLI response missing accessToken"};
        }
        std::chrono::system_clock::time_point exp =
            std::chrono::system_clock::now() + std::chrono::hours(1);
        // Prefer the numeric unix-timestamp `expires_on` if present;
        // fall back to `expiresOn` (a formatted string) if not.
        if (d.HasMember("expires_on")) {
          const auto &v = d["expires_on"];
          if (v.IsInt64() || v.IsUint64()) {
            exp = std::chrono::system_clock::from_time_t(
                static_cast<time_t>(v.GetInt64()));
          } else if (v.IsString()) {
            // Some az versions emit the unix stamp as a string.
            try {
              exp = std::chrono::system_clock::from_time_t(
                  static_cast<time_t>(std::stoll(v.GetString())));
            } catch (...) {
              // fall through — keep the default 1 h
            }
          }
        }
        return MintResult{true, d["accessToken"].GetString(), exp, {}};
      }) {}

std::string AzureCliProvider::get_bearer() {
  std::string tok;
  std::string err;
  (void)cache_.get(&tok, &err);
  return tok;
}

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud
