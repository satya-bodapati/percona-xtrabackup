/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See managed_identity_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "managed_identity_provider.h"

#include <curl/curl.h>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>

#include <chrono>
#include <memory>

#include "../retry_backoff.h"

namespace xbcloud {
namespace auth {
namespace azure {

namespace {

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string build_url(const std::string &resource,
                      const std::string &client_id) {
  std::string url =
      "http://169.254.169.254/metadata/identity/oauth2/token"
      "?api-version=2018-02-01&resource=" +
      resource;
  if (!client_id.empty()) url += "&client_id=" + client_id;
  return url;
}

bool http_get(const std::string &url, long timeout_seconds, std::string *body,
              std::string *err) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                            &curl_easy_cleanup);
  if (!curl) {
    *err = "curl_easy_init failed";
    return false;
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> hdrs(
      curl_slist_append(nullptr, "Metadata: true"), &curl_slist_free_all);
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, hdrs.get());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout_seconds);
  const CURLcode rc = curl_easy_perform(curl.get());
  if (rc != CURLE_OK) {
    *err = std::string("curl: ") + curl_easy_strerror(rc);
    return false;
  }
  long code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
  if (code < 200 || code >= 300) {
    *err = "Azure IMDS returned HTTP " + std::to_string(code);
    return false;
  }
  return true;
}

}  // namespace

ManagedIdentityProvider::ManagedIdentityProvider(std::string resource,
                                                  std::string client_id)
    : resource_(std::move(resource)),
      client_id_(std::move(client_id)),
      cache_([this]() -> MintResult {
        std::string body;
        std::string err;
        RetryPolicy policy;
        const bool ok = retry_with_backoff(
            policy,
            [&](int, std::string *e) {
              std::string local_body;
              std::string local_err;
              if (http_get(build_url(resource_, client_id_), 5, &local_body,
                           &local_err)) {
                body = std::move(local_body);
                return RetryDecision::Success;
              }
              *e = std::move(local_err);
              return RetryDecision::RetryableFailure;
            },
            &err);
        if (!ok) return MintResult{false, {}, {}, std::move(err)};

        rapidjson::Document d;
        if (d.Parse(body.c_str()).HasParseError() ||
            !d.HasMember("access_token") || !d["access_token"].IsString()) {
          return MintResult{false, {}, {}, "malformed IMDS response"};
        }
        // Azure returns expires_on as a Unix timestamp string OR
        // expires_in seconds — handle both.
        std::chrono::system_clock::time_point exp =
            std::chrono::system_clock::now() + std::chrono::seconds(3600);
        if (d.HasMember("expires_on") && d["expires_on"].IsString()) {
          try {
            const long long secs = std::stoll(d["expires_on"].GetString());
            exp = std::chrono::system_clock::time_point(
                std::chrono::seconds(secs));
          } catch (...) {
          }
        } else if (d.HasMember("expires_in") && d["expires_in"].IsInt()) {
          exp = std::chrono::system_clock::now() +
                std::chrono::seconds(d["expires_in"].GetInt());
        }
        return MintResult{true, d["access_token"].GetString(), exp, {}};
      }),
      source_label_(std::string("azure:managed-identity:") +
                    (client_id_.empty() ? "system" : client_id_)) {}

std::string ManagedIdentityProvider::get_bearer() {
  std::string tok;
  std::string err;
  (void)cache_.get(&tok, &err);
  return tok;
}

bool ManagedIdentityProvider::probe_reachable() {
  std::string body;
  std::string err;
  return http_get(build_url("https://storage.azure.com/", ""), 1, &body, &err);
}

}  // namespace azure
}  // namespace auth
}  // namespace xbcloud
