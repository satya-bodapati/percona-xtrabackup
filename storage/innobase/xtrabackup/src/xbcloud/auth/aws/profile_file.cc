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

}  // namespace

std::string default_credentials_path() {
  const char *home = std::getenv("HOME");
  if (!home) return {};
  return std::string(home) + "/.aws/credentials";
}

bool load_profile(const std::string &path, const std::string &profile,
                  HmacCredentials *out, std::string *err) {
  std::ifstream f(path);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  // AWS-config-style also supports "[profile <name>]" section names.
  const std::string wanted_section =
      profile.empty() ? "default" : profile;
  const std::string wanted_section_with_prefix = "profile " + profile;

  std::string line;
  std::string current_section;
  bool in_wanted = false;
  bool have_access = false;
  bool have_secret = false;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      current_section = trim(line.substr(1, line.size() - 2));
      in_wanted = (current_section == wanted_section) ||
                  (current_section == wanted_section_with_prefix);
      continue;
    }
    if (!in_wanted) continue;

    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (key == "aws_access_key_id") {
      out->access_key = val;
      have_access = true;
    } else if (key == "aws_secret_access_key") {
      out->secret_key = val;
      have_secret = true;
    } else if (key == "aws_session_token") {
      out->session_token = val;
    }
  }
  if (!have_access || !have_secret) {
    *err = path + ": section [" + wanted_section +
           "] missing aws_access_key_id or aws_secret_access_key";
    return false;
  }
  return true;
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
