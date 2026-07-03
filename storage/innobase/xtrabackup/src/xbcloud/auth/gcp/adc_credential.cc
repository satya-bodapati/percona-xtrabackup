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
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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

namespace {

bool parse_adc_doc(const rapidjson::Document &d, const std::string &origin,
                   AdcCredential *out, std::string *err) {
  const char *type = maybe_string(d, "type");
  if (type == nullptr) {
    *err = origin + ": missing \"type\" field";
    return false;
  }
  const std::string type_s(type);
  if (type_s == "service_account") {
    out->type = AdcType::kServiceAccount;
    const char *client_email = maybe_string(d, "client_email");
    const char *private_key = maybe_string(d, "private_key");
    const char *token_uri = maybe_string(d, "token_uri");
    if (!client_email || !private_key) {
      *err = origin +
             ": service_account credential missing client_email or private_key";
      return false;
    }
    out->client_email = client_email;
    out->private_key = private_key;
    out->token_uri =
        token_uri ? token_uri : "https://oauth2.googleapis.com/token";
    return true;
  }
  if (type_s == "authorized_user") {
    out->type = AdcType::kAuthorizedUser;
    const char *client_id = maybe_string(d, "client_id");
    const char *client_secret = maybe_string(d, "client_secret");
    const char *refresh_token = maybe_string(d, "refresh_token");
    if (!client_id || !client_secret || !refresh_token) {
      *err = origin +
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
  if (type_s == "impersonated_service_account") {
    out->type = AdcType::kImpersonatedServiceAccount;
    const char *url = maybe_string(d, "service_account_impersonation_url");
    if (!url) {
      *err = origin +
             ": impersonated_service_account missing "
             "service_account_impersonation_url";
      return false;
    }
    out->service_account_impersonation_url = url;
    // Serialize the nested source_credentials back to JSON so callers
    // can re-parse it as a plain AdcCredential (recursion happens in
    // ImpersonatedServiceAccountProvider, not here).
    if (!d.HasMember("source_credentials") ||
        !d["source_credentials"].IsObject()) {
      *err = origin +
             ": impersonated_service_account missing source_credentials";
      return false;
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    d["source_credentials"].Accept(writer);
    out->source_credentials_json = buf.GetString();
    return true;
  }
  if (type_s == "external_account") {
    out->type = AdcType::kExternalAccount;
    *err = origin +
           ": credential type \"external_account\" (Workload Identity "
           "Federation) is recognised but not yet supported.  Workaround: "
           "use gcloud to exchange your external token for a Google "
           "access_token and export it via credential_process, or file a "
           "PXB ticket for prioritisation";
    return false;
  }
  *err = origin + ": unsupported credential type \"" + type_s +
         "\" (expected service_account, authorized_user, or "
         "impersonated_service_account)";
  return false;
}

}  // namespace

bool load_adc_from_file(const std::string &path, AdcCredential *out,
                        std::string *err) {
  std::string raw;
  if (!read_file(path, &raw, err)) return false;
  return load_adc_from_string(raw, out, err);
}

bool load_adc_from_string(const std::string &json, AdcCredential *out,
                          std::string *err) {
  rapidjson::Document d;
  if (d.Parse(json.c_str()).HasParseError()) {
    *err = "malformed ADC JSON";
    return false;
  }
  return parse_adc_doc(d, "<adc>", out, err);
}

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud
