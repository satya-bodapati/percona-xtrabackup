/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See gce_metadata_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "gce_metadata_provider.h"

#include <curl/curl.h>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>

#include <chrono>
#include <memory>

#include "../retry_backoff.h"

namespace xbcloud {
namespace auth {
namespace gcp {

namespace {

const char *kTokenUrl =
    "http://metadata.google.internal/computeMetadata/v1/instance/"
    "service-accounts/default/token";

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

bool http_get_metadata(const std::string &url, long timeout_seconds,
                       std::string *body, std::string *err) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                            &curl_easy_cleanup);
  if (!curl) {
    *err = "curl_easy_init failed";
    return false;
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> hdrs(
      curl_slist_append(nullptr, "Metadata-Flavor: Google"),
      &curl_slist_free_all);
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
    *err = "metadata endpoint returned HTTP " + std::to_string(code);
    return false;
  }
  return true;
}

MintResult mint_from_gce_metadata() {
  std::string body;
  std::string err;
  RetryPolicy policy;
  const bool ok = retry_with_backoff(
      policy,
      [&](int, std::string *e) {
        std::string local_body;
        std::string local_err;
        if (http_get_metadata(kTokenUrl, 5, &local_body, &local_err)) {
          body = std::move(local_body);
          return RetryDecision::Success;
        }
        *e = std::move(local_err);
        return RetryDecision::RetryableFailure;
      },
      &err);
  if (!ok) return MintResult{false, {}, {}, std::move(err)};

  rapidjson::Document d;
  if (d.Parse(body.c_str()).HasParseError() || !d.HasMember("access_token") ||
      !d["access_token"].IsString()) {
    return MintResult{false, {}, {}, "malformed metadata token response"};
  }
  int expires_in = 3600;
  if (d.HasMember("expires_in") && d["expires_in"].IsInt())
    expires_in = d["expires_in"].GetInt();
  return MintResult{
      true, d["access_token"].GetString(),
      std::chrono::system_clock::now() + std::chrono::seconds(expires_in), {}};
}

}  // namespace

GceMetadataProvider::GceMetadataProvider()
    : cache_(&mint_from_gce_metadata) {}

std::string GceMetadataProvider::get_bearer() {
  std::string tok;
  std::string err;
  (void)cache_.get(&tok, &err);
  return tok;
}

bool GceMetadataProvider::probe_reachable() {
  std::string body;
  std::string err;
  return http_get_metadata(kTokenUrl, 1, &body, &err);
}

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud
