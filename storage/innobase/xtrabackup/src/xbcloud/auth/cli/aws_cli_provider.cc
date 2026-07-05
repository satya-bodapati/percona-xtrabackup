/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See aws_cli_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "aws_cli_provider.h"

#include <ctime>

#include "my_rapidjson_size_t.h"
#include <rapidjson/document.h>

#include "run_json.h"

namespace xbcloud {
namespace auth {
namespace cli {

namespace {

// Parse an ISO-8601 timestamp like "2026-07-05T18:30:00Z" into a
// system_clock::time_point.  Returns true on success.
bool parse_iso8601_utc(const std::string &s,
                       std::chrono::system_clock::time_point *out) {
  struct tm tm {};
  if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr) return false;
  *out = std::chrono::system_clock::from_time_t(timegm(&tm));
  return true;
}

}  // namespace

AwsCliProvider::AwsCliProvider(std::string command, std::string profile,
                               std::string source_label)
    : command_(std::move(command)),
      source_label_(std::move(source_label)) {
  if (!profile.empty()) {
    command_ += " --profile ";
    command_ += profile;
  }
}

bool AwsCliProvider::mint_locked_(std::string *err) {
  std::string body;
  if (!run_json_command(command_, &body, err)) return false;

  rapidjson::Document d;
  if (d.Parse(body.c_str()).HasParseError() || !d.IsObject()) {
    *err = "aws CLI did not return a JSON object";
    return false;
  }
  if (!d.HasMember("AccessKeyId") || !d["AccessKeyId"].IsString() ||
      !d.HasMember("SecretAccessKey") || !d["SecretAccessKey"].IsString()) {
    *err = "aws CLI response missing AccessKeyId / SecretAccessKey";
    return false;
  }
  HmacCredentials c;
  c.access_key = d["AccessKeyId"].GetString();
  c.secret_key = d["SecretAccessKey"].GetString();
  if (d.HasMember("SessionToken") && d["SessionToken"].IsString()) {
    c.session_token = d["SessionToken"].GetString();
  }
  bool never_expires = true;
  std::chrono::system_clock::time_point exp{};
  if (d.HasMember("Expiration") && d["Expiration"].IsString()) {
    if (!parse_iso8601_utc(d["Expiration"].GetString(), &exp)) {
      *err = std::string("aws CLI returned unparseable Expiration: ") +
             d["Expiration"].GetString();
      return false;
    }
    never_expires = false;
  }

  cached_ = std::move(c);
  expires_at_ = exp;
  valid_ = true;
  never_expires_ = never_expires;
  return true;
}

HmacCredentials AwsCliProvider::get_hmac() {
  std::lock_guard<std::mutex> g(mu_);
  const auto now = std::chrono::system_clock::now();
  const bool need_refresh =
      !valid_ ||
      (!never_expires_ &&
       now + std::chrono::minutes(5) >= expires_at_);
  if (need_refresh) {
    std::string err;
    if (!mint_locked_(&err)) {
      // Return whatever we last had (empty if we never succeeded);
      // callers deal with the resulting 401 / missing-signature the
      // same way they do for every other provider on this interface.
      return HmacCredentials{};
    }
  }
  return cached_;
}

void AwsCliProvider::invalidate() {
  std::lock_guard<std::mutex> g(mu_);
  valid_ = false;
}

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud
