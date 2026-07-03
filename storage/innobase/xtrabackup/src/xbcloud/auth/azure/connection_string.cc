/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See connection_string.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "connection_string.h"

#include <algorithm>
#include <cctype>

namespace xbcloud {
namespace auth {
namespace azure {

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, e - b + 1);
}

}  // namespace

bool parse_connection_string(const std::string &input, ConnectionString *out,
                             std::string *err) {
  size_t pos = 0;
  while (pos < input.size()) {
    // Find next ";" (unquoted — connection strings don't quote).
    const size_t semi = input.find(';', pos);
    const std::string kv =
        trim(input.substr(pos, semi == std::string::npos ? std::string::npos
                                                          : semi - pos));
    pos = (semi == std::string::npos) ? input.size() : semi + 1;
    if (kv.empty()) continue;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) {
      *err = "Azure connection string: token without '=': '" + kv + "'";
      return false;
    }
    const std::string key_lc = to_lower(trim(kv.substr(0, eq)));
    const std::string val = trim(kv.substr(eq + 1));
    if (key_lc == "defaultendpointsprotocol") {
      out->default_endpoints_protocol = val;
    } else if (key_lc == "accountname") {
      out->account_name = val;
    } else if (key_lc == "accountkey") {
      out->account_key = val;
    } else if (key_lc == "endpointsuffix") {
      out->endpoint_suffix = val;
    } else if (key_lc == "blobendpoint") {
      out->blob_endpoint = val;
    } else if (key_lc == "sharedaccesssignature") {
      out->shared_access_signature = val;
    } else if (key_lc == "developmentstorageproxyuri") {
      out->development_storage_proxy_uri = val;
    }
    // Other keys (TableEndpoint, QueueEndpoint, FileEndpoint) are
    // silently ignored — xbcloud only speaks Blob.
  }

  // Sanity: without SAS, we need at least an account name for
  // xbcloud to construct URLs and (if signing) an account key.
  if (out->shared_access_signature.empty()) {
    if (out->account_name.empty()) {
      *err = "Azure connection string: missing AccountName";
      return false;
    }
  }
  return true;
}

std::string blob_endpoint_from(const ConnectionString &cs) {
  if (!cs.blob_endpoint.empty()) return cs.blob_endpoint;
  const std::string protocol =
      cs.default_endpoints_protocol.empty() ? std::string("https")
                                             : cs.default_endpoints_protocol;
  const std::string suffix =
      cs.endpoint_suffix.empty() ? std::string("core.windows.net")
                                  : cs.endpoint_suffix;
  return protocol + "://" + cs.account_name + ".blob." + suffix + "/";
}

}  // namespace azure
}  // namespace auth
}  // namespace xbcloud
