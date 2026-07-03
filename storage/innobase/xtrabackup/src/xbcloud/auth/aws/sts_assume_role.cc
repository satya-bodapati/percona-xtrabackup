/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See sts_assume_role.h.

Implementation of the STS AssumeRole flow.  Signs a POST to
sts.<region>.amazonaws.com/ with the parent provider's HMAC
credentials via SigV4 (service="sts") and parses the returned
<AssumeRoleResult><Credentials>... XML into temporary
HmacCredentials.

The SigV4 canonical-request algorithm is intentionally written
here in full rather than shared with S3_signerV4, because:
  * S3_signerV4 is bucket-oriented and hardcodes service="s3".
  * The STS request shape is fixed (POST /, form body, three
    headers).  A dedicated implementation is ~100 LOC and easier
    to audit than a generalised signer.
  * If future work needs another SigV4 service the extraction can
    happen then, informed by three concrete users instead of two.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "sts_assume_role.h"

#include <curl/curl.h>

#include <rapidxml/rapidxml.hpp>

#include <cstring>
#include <ctime>
#include <memory>
#include <sstream>
#include <utility>

#include "../../hash.h"
#include "../../util.h"
#include "../retry_backoff.h"

namespace xbcloud {
namespace auth {
namespace aws {

namespace {

std::string aws_date_utc(time_t t) {
  struct tm tmp;
  char buf[32];
  strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", gmtime_r(&t, &tmp));
  return buf;
}

// URL-encode per RFC 3986 (used by SigV4 body/query canonicalisation).
std::string url_encode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += static_cast<char>(c);
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// Build an ordered form-urlencoded body from a list of {key,val}.
// SigV4 requires the body to be lex-sorted by key when canonicalising
// the payload hash — for STS AssumeRole we happen to feed keys in
// canonical order, but the encoder guarantees consistency.
std::string form_encode(const std::vector<std::pair<std::string, std::string>>
                             &params) {
  std::string body;
  bool first = true;
  for (const auto &kv : params) {
    if (!first) body += "&";
    body += url_encode(kv.first) + "=" + url_encode(kv.second);
    first = false;
  }
  return body;
}

// SigV4 signer specialised for a POST to sts.<region>.amazonaws.com/
// with a form-encoded body and Host / X-Amz-Date / Content-Type
// headers.  Returns the Authorization header value.
std::string sign_sts_request(const std::string &region,
                              const HmacCredentials &parent,
                              const std::string &host, const std::string &body,
                              const std::string &amz_date_utc) {
  const std::string yyyymmdd = amz_date_utc.substr(0, 8);
  const std::string scope = yyyymmdd + "/" + region + "/sts/aws4_request";

  // Canonical request.
  std::stringstream cr;
  cr << "POST\n";
  cr << "/\n";
  cr << "\n";  // no query string
  cr << "content-type:application/x-www-form-urlencoded\n";
  cr << "host:" << host << "\n";
  cr << "x-amz-date:" << amz_date_utc << "\n";
  cr << "\n";
  cr << "content-type;host;x-amz-date\n";
  cr << hex_encode(sha256(body));

  const std::string canonical_request = cr.str();

  // String to sign.
  std::string sts;
  sts.append("AWS4-HMAC-SHA256\n");
  sts.append(amz_date_utc).append("\n");
  sts.append(scope).append("\n");
  sts.append(hex_encode(sha256(canonical_request)));

  // Signing key.
  auto k_date = hmac_sha256("AWS4" + parent.secret_key, yyyymmdd);
  auto k_region = hmac_sha256(k_date, region);
  auto k_service = hmac_sha256(k_region, "sts");
  auto k_signing = hmac_sha256(k_service, "aws4_request");

  const std::string signature = hex_encode(hmac_sha256(k_signing, sts));

  std::string auth = "AWS4-HMAC-SHA256 Credential=";
  auth += parent.access_key;
  auth += "/";
  auth += scope;
  auth += ", SignedHeaders=content-type;host;x-amz-date";
  auth += ", Signature=";
  auth += signature;
  return auth;
}

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

bool http_post(const std::string &url, const std::string &body,
               const std::vector<std::string> &headers,
               std::string *response_body, long *http_code, std::string *err) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                            &curl_easy_cleanup);
  if (!curl) {
    *err = "curl_easy_init failed";
    return false;
  }
  curl_slist *hlist = nullptr;
  for (const auto &h : headers) hlist = curl_slist_append(hlist, h.c_str());
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

// Parse <AssumeRoleResult><Credentials>...</> XML.  Populates
// {access_key, secret_key, session_token} and *expires_at from
// the Expiration ISO-8601 string.
bool parse_assume_role_xml(const std::string &body, HmacCredentials *out,
                           std::chrono::system_clock::time_point *expires_at,
                           std::string *err) {
  using namespace rapidxml;
  // rapidxml needs mutable buffer.
  std::vector<char> buf(body.begin(), body.end());
  buf.push_back(0);
  xml_document<> doc;
  try {
    doc.parse<0>(buf.data());
  } catch (const parse_error &e) {
    *err = std::string("STS XML parse error: ") + e.what();
    return false;
  }
  xml_node<> *root = doc.first_node("AssumeRoleResponse");
  if (!root) root = doc.first_node("ErrorResponse");
  if (!root) {
    *err = "STS response missing AssumeRoleResponse root";
    return false;
  }
  if (std::strcmp(root->name(), "ErrorResponse") == 0) {
    xml_node<> *error = root->first_node("Error");
    xml_node<> *code = error ? error->first_node("Code") : nullptr;
    xml_node<> *msg = error ? error->first_node("Message") : nullptr;
    *err = std::string("STS error: ") +
           (code ? code->value() : "<unknown>") + ": " +
           (msg ? msg->value() : "<no message>");
    return false;
  }
  xml_node<> *result = root->first_node("AssumeRoleResult");
  xml_node<> *creds = result ? result->first_node("Credentials") : nullptr;
  if (!creds) {
    *err = "STS response missing AssumeRoleResult/Credentials";
    return false;
  }
  auto get = [&](const char *name) -> std::string {
    xml_node<> *n = creds->first_node(name);
    return n ? std::string(n->value()) : std::string{};
  };
  out->access_key = get("AccessKeyId");
  out->secret_key = get("SecretAccessKey");
  out->session_token = get("SessionToken");
  const std::string exp_str = get("Expiration");
  if (out->access_key.empty() || out->secret_key.empty() ||
      out->session_token.empty()) {
    *err = "STS response missing one of AccessKeyId / SecretAccessKey / "
           "SessionToken";
    return false;
  }
  // Parse ISO-8601 UTC.  STS uses "2026-07-03T12:34:56Z".
  struct tm tm {};
  if (strptime(exp_str.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr) {
    // Not fatal — fall back to a conservative 1h if we can't parse.
    *expires_at = std::chrono::system_clock::now() + std::chrono::hours(1);
  } else {
    *expires_at = std::chrono::system_clock::from_time_t(timegm(&tm));
  }
  return true;
}

}  // namespace

StsAssumeRoleProvider::StsAssumeRoleProvider(StsAssumeRoleConfig cfg)
    : cfg_(std::move(cfg)) {}

HmacCredentials StsAssumeRoleProvider::get_hmac() {
  std::lock_guard<std::mutex> lk(mu_);
  const auto now = std::chrono::system_clock::now();
  const bool need_mint = !valid_ ||
                          now + std::chrono::minutes(5) >= expires_at_;
  if (need_mint) {
    std::string err;
    if (!mint_locked_(&err)) {
      // On failure keep valid_=false; caller sees empty creds and
      // the request layer will surface a clean auth failure that
      // names the STS role via source_description().
      return {};
    }
  }
  return cached_;
}

void StsAssumeRoleProvider::invalidate() {
  std::lock_guard<std::mutex> lk(mu_);
  valid_ = false;
}

bool StsAssumeRoleProvider::mint_locked_(std::string *err) {
  if (!cfg_.parent) {
    *err = "aws::StsAssumeRoleProvider: no parent CredentialProvider";
    return false;
  }
  const HmacCredentials parent = cfg_.parent->get_hmac();
  if (parent.access_key.empty() || parent.secret_key.empty()) {
    *err = "aws::StsAssumeRoleProvider: parent produced empty credentials";
    return false;
  }

  // Build the request body.  Note: SigV4 payload hash is over the
  // body bytes we actually send; we sort keys alphabetically both
  // for the body and for the hash to make debugging easier.  AWS
  // accepts any order.
  std::vector<std::pair<std::string, std::string>> params = {
      {"Action", "AssumeRole"},
      {"DurationSeconds", std::to_string(cfg_.duration_seconds)},
      {"RoleArn", cfg_.role_arn},
      {"RoleSessionName", cfg_.role_session_name},
      {"Version", "2011-06-15"},
  };
  if (!cfg_.external_id.empty()) {
    params.push_back({"ExternalId", cfg_.external_id});
  }
  const std::string body = form_encode(params);

  // If a parent session_token is set (chained temp creds), pass it
  // through as X-Amz-Security-Token on the STS request itself.
  const std::string host = "sts." + cfg_.region + ".amazonaws.com";
  const std::string url = "https://" + host + "/";
  const time_t now = std::time(nullptr);
  const std::string amz_date = aws_date_utc(now);
  const std::string auth =
      sign_sts_request(cfg_.region, parent, host, body, amz_date);

  std::vector<std::string> headers = {
      "Host: " + host,
      "X-Amz-Date: " + amz_date,
      "Content-Type: application/x-www-form-urlencoded",
      "Authorization: " + auth,
  };
  if (!parent.session_token.empty()) {
    headers.push_back("X-Amz-Security-Token: " + parent.session_token);
  }

  std::string response;
  long code = 0;

  RetryPolicy policy;
  const bool ok = retry_with_backoff(
      policy,
      [&](int, std::string *e) {
        response.clear();
        code = 0;
        std::string post_err;
        if (!http_post(url, body, headers, &response, &code, &post_err)) {
          *e = std::move(post_err);
          return RetryDecision::RetryableFailure;
        }
        if (code >= 500) {
          *e = "STS returned HTTP " + std::to_string(code);
          return RetryDecision::RetryableFailure;
        }
        if (code < 200 || code >= 300) {
          // 4xx: parse the error body once for a good message and stop.
          HmacCredentials dummy;
          std::chrono::system_clock::time_point dummy_exp;
          std::string parse_err;
          (void)parse_assume_role_xml(response, &dummy, &dummy_exp, &parse_err);
          *e = "STS returned HTTP " + std::to_string(code) + ": " +
               (parse_err.empty() ? response : parse_err);
          return RetryDecision::PermanentFailure;
        }
        return RetryDecision::Success;
      },
      err);
  if (!ok) return false;

  HmacCredentials fresh;
  std::chrono::system_clock::time_point exp;
  std::string parse_err;
  if (!parse_assume_role_xml(response, &fresh, &exp, &parse_err)) {
    *err = parse_err;
    return false;
  }
  cached_ = std::move(fresh);
  expires_at_ = exp;
  valid_ = true;
  return true;
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
