/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See adc_credential.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "adc_credential.h"

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>

#include <fstream>
#include <sstream>

namespace xbcloud {
namespace auth {
namespace gcp {

namespace {

bool read_file(const std::string &path, std::string *out, std::string *err) {
  std::ifstream f(path);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

const char *maybe_string(const rapidjson::Document &d, const char *key) {
  if (!d.HasMember(key) || !d[key].IsString()) return nullptr;
  return d[key].GetString();
}

}  // namespace

bool load_adc_from_file(const std::string &path, AdcCredential *out,
                        std::string *err) {
  std::string raw;
  if (!read_file(path, &raw, err)) return false;

  rapidjson::Document d;
  if (d.Parse(raw.c_str()).HasParseError()) {
    *err = "malformed JSON at " + path;
    return false;
  }
  const char *type = maybe_string(d, "type");
  if (type == nullptr) {
    *err = path + ": missing \"type\" field";
    return false;
  }
  const std::string type_s(type);
  if (type_s == "service_account") {
    out->type = AdcType::kServiceAccount;
    const char *client_email = maybe_string(d, "client_email");
    const char *private_key = maybe_string(d, "private_key");
    const char *token_uri = maybe_string(d, "token_uri");
    if (!client_email || !private_key) {
      *err = path +
             ": service_account credential missing client_email or private_key";
      return false;
    }
    out->client_email = client_email;
    out->private_key = private_key;
    // token_uri is optional in some issuers; default to Google's endpoint.
    out->token_uri = token_uri ? token_uri : "https://oauth2.googleapis.com/token";
    return true;
  }
  if (type_s == "authorized_user") {
    out->type = AdcType::kAuthorizedUser;
    const char *client_id = maybe_string(d, "client_id");
    const char *client_secret = maybe_string(d, "client_secret");
    const char *refresh_token = maybe_string(d, "refresh_token");
    if (!client_id || !client_secret || !refresh_token) {
      *err = path +
             ": authorized_user credential missing "
             "client_id / client_secret / refresh_token";
      return false;
    }
    out->client_id = client_id;
    out->client_secret = client_secret;
    out->refresh_token = refresh_token;
    out->token_uri = "https://oauth2.googleapis.com/token";
    return true;
  }
  // v1 scope of PXB-3592: only the two flows above.  Reject the rest
  // with a clear message so the operator knows we saw their file and
  // deliberately didn't proceed.
  *err = path + ": unsupported credential type \"" + type_s +
         "\" (expected service_account or authorized_user)";
  return false;
}

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud
