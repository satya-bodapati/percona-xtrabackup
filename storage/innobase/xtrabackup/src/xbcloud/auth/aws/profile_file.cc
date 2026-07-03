/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See profile_file.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "profile_file.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace xbcloud {
namespace auth {
namespace aws {

namespace {

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, e - b + 1);
}

// Read one section's key/value pairs into a map.  Returns true iff
// the requested section was found.  Handles both "[foo]" and
// "[profile foo]" section names — AWS convention is that
// ~/.aws/config uses the latter (except for [default]) while
// ~/.aws/credentials uses plain names.
bool read_section(const std::string &path, const std::string &profile,
                  std::map<std::string, std::string> *out, std::string *err) {
  std::ifstream f(path);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  const std::string wanted = profile.empty() ? "default" : profile;
  const std::string wanted_prefixed = "profile " + wanted;
  std::string line;
  std::string current_section;
  bool in_wanted = false;
  bool found = false;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      current_section = trim(line.substr(1, line.size() - 2));
      in_wanted =
          (current_section == wanted) || (current_section == wanted_prefixed);
      if (in_wanted) found = true;
      continue;
    }
    if (!in_wanted) continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (key.empty()) continue;
    // Last wins if the same key appears twice — matches AWS SDK.
    (*out)[key] = val;
  }
  return found;
}

// Populate ParsedProfile from a key/value map.
void populate(const std::map<std::string, std::string> &kv,
              ParsedProfile *out) {
  auto get = [&](const char *key) -> std::string {
    auto it = kv.find(key);
    return (it == kv.end()) ? std::string{} : it->second;
  };
  out->aws_access_key_id = get("aws_access_key_id");
  out->aws_secret_access_key = get("aws_secret_access_key");
  out->aws_session_token = get("aws_session_token");

  out->role_arn = get("role_arn");
  out->source_profile = get("source_profile");
  out->credential_source = get("credential_source");
  out->role_session_name = get("role_session_name");
  out->external_id = get("external_id");
  const std::string dur = get("duration_seconds");
  if (!dur.empty()) {
    try {
      out->duration_seconds = std::stoi(dur);
    } catch (...) {
      out->duration_seconds = 0;
    }
  }
  out->mfa_serial = get("mfa_serial");
  out->web_identity_token_file = get("web_identity_token_file");

  out->credential_process = get("credential_process");

  out->sso_start_url = get("sso_start_url");
  out->sso_region = get("sso_region");
  out->sso_account_id = get("sso_account_id");
  out->sso_role_name = get("sso_role_name");

  out->region = get("region");
}

}  // namespace

std::string default_credentials_path() {
  const char *home = std::getenv("HOME");
  if (!home) return {};
  return std::string(home) + "/.aws/credentials";
}

std::string default_config_path() {
  const char *home = std::getenv("HOME");
  if (!home) return {};
  return std::string(home) + "/.aws/config";
}

bool load_profile(const std::string &path, const std::string &profile,
                  HmacCredentials *out, std::string *err) {
  ParsedProfile p;
  if (!load_profile_v2(path, profile, &p, err)) return false;
  if (!p.has_hmac_credentials()) {
    const std::string section = profile.empty() ? "default" : profile;
    *err = path + ": section [" + section +
           "] has no plain HMAC credentials (aws_access_key_id / "
           "aws_secret_access_key).  Use load_profile_v2 + profile "
           "resolver for role_arn / credential_process / SSO shapes.";
    return false;
  }
  out->access_key = p.aws_access_key_id;
  out->secret_key = p.aws_secret_access_key;
  out->session_token = p.aws_session_token;
  return true;
}

bool load_profile_v2(const std::string &path, const std::string &profile,
                     ParsedProfile *out, std::string *err) {
  std::map<std::string, std::string> kv;
  if (!read_section(path, profile, &kv, err)) {
    if (err->empty()) {
      const std::string section = profile.empty() ? "default" : profile;
      *err = path + ": section [" + section + "] not found";
    }
    return false;
  }
  populate(kv, out);
  return true;
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
