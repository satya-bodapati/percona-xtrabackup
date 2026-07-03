/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See impersonation_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "impersonation_provider.h"

#include <curl/curl.h>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <utility>

#include "../retry_backoff.h"

namespace xbcloud {
namespace auth {
namespace gcp {

namespace {

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

bool call_iam_credentials(const std::string &url, const std::string &parent_token,
                           const std::string &scope, int lifetime_seconds,
                           std::string *response_body, long *http_code,
                           std::string *err) {
  const std::string body =
      "{\"scope\":[\"" + scope + "\"],\"lifetime\":\"" +
      std::to_string(lifetime_seconds) + "s\"}";

  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                            &curl_easy_cleanup);
  if (!curl) {
    *err = "curl_easy_init failed";
    return false;
  }
  const std::string auth_header = "Authorization: Bearer " + parent_token;
  curl_slist *hlist = nullptr;
  hlist = curl_slist_append(hlist, auth_header.c_str());
  hlist = curl_slist_append(hlist, "Content-Type: application/json");
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> hlist_guard(
      hlist, &curl_slist_free_all);
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, hlist);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, response_body);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
  const CURLcode rc = curl_easy_perform(curl.get());
  if (rc != CURLE_OK) {
    *err = std::string("curl: ") + curl_easy_strerror(rc);
    return false;
  }
  *http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, http_code);
  return true;
}

}  // namespace

ImpersonationProvider::ImpersonationProvider(
    std::unique_ptr<CredentialProvider> parent, std::string impersonation_url,
    std::string scope, int lifetime_seconds, std::string source_label)
    : parent_(std::move(parent)),
      impersonation_url_(std::move(impersonation_url)),
      scope_(std::move(scope)),
      lifetime_seconds_(lifetime_seconds),
      cache_([this]() -> MintResult {
        if (!parent_) {
          return MintResult{false, {}, {},
                            "ImpersonationProvider: no parent"};
        }
        const std::string parent_tok = parent_->get_bearer();
        if (parent_tok.empty()) {
          return MintResult{
              false, {}, {},
              "ImpersonationProvider: parent produced no Bearer token"};
        }

        std::string response;
        long code = 0;
        std::string err;
        RetryPolicy policy;
        const bool ok = retry_with_backoff(
            policy,
            [&](int, std::string *e) {
              response.clear();
              code = 0;
              std::string cerr;
              if (!call_iam_credentials(impersonation_url_, parent_tok, scope_,
                                         lifetime_seconds_, &response, &code,
                                         &cerr)) {
                *e = std::move(cerr);
                return RetryDecision::RetryableFailure;
              }
              if (code >= 500) {
                *e = "IAM Credentials API returned HTTP " +
                     std::to_string(code);
                return RetryDecision::RetryableFailure;
              }
              if (code < 200 || code >= 300) {
                *e = "IAM Credentials API returned HTTP " +
                     std::to_string(code) + ": " + response;
                return RetryDecision::PermanentFailure;
              }
              return RetryDecision::Success;
            },
            &err);
        if (!ok) return MintResult{false, {}, {}, std::move(err)};

        rapidjson::Document d;
        if (d.Parse(response.c_str()).HasParseError() ||
            !d.HasMember("accessToken") || !d["accessToken"].IsString()) {
          return MintResult{false, {}, {},
                            "IAM Credentials API returned malformed JSON"};
        }
        std::chrono::system_clock::time_point exp =
            std::chrono::system_clock::now() + std::chrono::hours(1);
        if (d.HasMember("expireTime") && d["expireTime"].IsString()) {
          struct tm tm {};
          const std::string exp_s = d["expireTime"].GetString();
          if (strptime(exp_s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) != nullptr) {
            exp = std::chrono::system_clock::from_time_t(timegm(&tm));
          }
        }
        return MintResult{true, d["accessToken"].GetString(), exp, {}};
      }),
      source_label_(source_label.empty()
                        ? std::string("gcp:impersonation:") + impersonation_url_
                        : std::move(source_label)) {}

std::string ImpersonationProvider::get_bearer() {
  std::string tok;
  std::string err;
  (void)cache_.get(&tok, &err);
  return tok;
}

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud
