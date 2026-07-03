/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See roles_anywhere.h.

Implementation of the AWS Roles Anywhere flow.  Signs a POST to
https://rolesanywhere.<region>.amazonaws.com/sessions with the
operator's private key using AWS4-X509-RSA-SHA256 canonicalisation,
parses the returned JSON credentialSet into HmacCredentials with
session_token and expires_at.

Reference: https://docs.aws.amazon.com/rolesanywhere/latest/userguide/
authentication-sign-process.html

The canonical-request algorithm has the same shape as SigV4 (method,
URI, query, canonical headers, signed headers, payload hash), but:
  * The algorithm string is "AWS4-X509-RSA-SHA256" (or -ECDSA-SHA256).
  * The signing operation is RSA-PKCS#1-v1_5 over SHA256 of the
    string-to-sign, done with the operator's private key — not an
    HMAC-derived signing key.
  * The Credential field's "access key" component is the X.509
    certificate's serial number in hex (uppercase, no leading 0s).
  * The request must carry an X-Amz-X509 header with the base64-
    encoded DER of the certificate.
  * The service name in the credential scope is "rolesanywhere".

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "roles_anywhere.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <curl/curl.h>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>

#include <cstring>
#include <ctime>
#include <fstream>
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

// Read a whole file into a string.  Used for cert PEM and key PEM.
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

// Load an X.509 cert from PEM and return the serial number as an
// uppercase hex string (no leading zeros).  Also fills *der with
// the DER-encoded bytes of the cert for the X-Amz-X509 header.
bool load_cert(const std::string &pem, std::string *serial_hex,
               std::string *der, std::string *err) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
  if (!bio) {
    *err = "OpenSSL: BIO_new_mem_buf failed";
    return false;
  }
  std::unique_ptr<X509, decltype(&X509_free)> cert(
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
  if (!cert) {
    *err = "OpenSSL: PEM_read_bio_X509 failed";
    return false;
  }
  const ASN1_INTEGER *sn_asn1 = X509_get_serialNumber(cert.get());
  std::unique_ptr<BIGNUM, decltype(&BN_free)> sn_bn(
      ASN1_INTEGER_to_BN(sn_asn1, nullptr), &BN_free);
  if (!sn_bn) {
    *err = "OpenSSL: ASN1_INTEGER_to_BN failed";
    return false;
  }
  char *hex = BN_bn2hex(sn_bn.get());
  if (!hex) {
    *err = "OpenSSL: BN_bn2hex failed";
    return false;
  }
  // OpenSSL returns uppercase-hex already; trim any leading zeros
  // because AWS Roles Anywhere expects no leading zeros.
  std::string s(hex);
  OPENSSL_free(hex);
  size_t first_nz = s.find_first_not_of('0');
  if (first_nz == std::string::npos) first_nz = s.size() - 1;
  *serial_hex = s.substr(first_nz);

  // DER-encode the cert.
  unsigned char *der_buf = nullptr;
  const int der_len = i2d_X509(cert.get(), &der_buf);
  if (der_len <= 0 || der_buf == nullptr) {
    *err = "OpenSSL: i2d_X509 failed";
    return false;
  }
  der->assign(reinterpret_cast<char *>(der_buf), der_len);
  OPENSSL_free(der_buf);
  return true;
}

// Base64-encode (with '+' and '/', with '=' padding — standard, not
// URL-safe).  Used for the X-Amz-X509 header.
std::string base64_std(const std::string &in) {
  static const char *alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) | c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(alphabet[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(alphabet[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

// RSA-SHA256-sign the given message with the caller's PEM private key.
// Returns raw signature bytes (hex-encode at the call site).
bool rsa_sign_sha256(const std::string &message,
                     const std::string &private_key_pem,
                     std::string *signature, std::string *err) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(private_key_pem.data(),
                      static_cast<int>(private_key_pem.size())),
      &BIO_free);
  if (!bio) {
    *err = "OpenSSL: BIO_new_mem_buf failed";
    return false;
  }
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
      &EVP_PKEY_free);
  if (!pkey) {
    *err = "OpenSSL: PEM_read_bio_PrivateKey failed";
    return false;
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                              &EVP_MD_CTX_free);
  if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                                 pkey.get()) != 1) {
    *err = "OpenSSL: EVP_DigestSignInit failed";
    return false;
  }
  if (EVP_DigestSignUpdate(ctx.get(), message.data(), message.size()) != 1) {
    *err = "OpenSSL: EVP_DigestSignUpdate failed";
    return false;
  }
  size_t sig_len = 0;
  if (EVP_DigestSignFinal(ctx.get(), nullptr, &sig_len) != 1) {
    *err = "OpenSSL: EVP_DigestSignFinal (size) failed";
    return false;
  }
  signature->resize(sig_len);
  if (EVP_DigestSignFinal(ctx.get(),
                          reinterpret_cast<unsigned char *>(signature->data()),
                          &sig_len) != 1) {
    *err = "OpenSSL: EVP_DigestSignFinal (sign) failed";
    return false;
  }
  signature->resize(sig_len);
  return true;
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

bool parse_response_json(const std::string &body, HmacCredentials *out,
                         std::chrono::system_clock::time_point *expires_at,
                         std::string *err) {
  rapidjson::Document d;
  if (d.Parse(body.c_str()).HasParseError()) {
    *err = "Roles Anywhere response: malformed JSON";
    return false;
  }
  if (!d.HasMember("credentialSet") || !d["credentialSet"].IsArray() ||
      d["credentialSet"].Empty()) {
    *err = "Roles Anywhere response missing credentialSet";
    return false;
  }
  const auto &cs = d["credentialSet"][0];
  if (!cs.HasMember("credentials") || !cs["credentials"].IsObject()) {
    *err = "Roles Anywhere response missing credentials object";
    return false;
  }
  const auto &c = cs["credentials"];
  auto get = [&](const char *k) -> std::string {
    return (c.HasMember(k) && c[k].IsString()) ? c[k].GetString() : "";
  };
  out->access_key = get("accessKeyId");
  out->secret_key = get("secretAccessKey");
  out->session_token = get("sessionToken");
  const std::string exp = get("expiration");
  if (out->access_key.empty() || out->secret_key.empty() ||
      out->session_token.empty()) {
    *err = "Roles Anywhere: incomplete credentials in response";
    return false;
  }
  struct tm tm {};
  if (strptime(exp.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr) {
    *expires_at = std::chrono::system_clock::now() + std::chrono::hours(1);
  } else {
    *expires_at = std::chrono::system_clock::from_time_t(timegm(&tm));
  }
  return true;
}

}  // namespace

RolesAnywhereProvider::RolesAnywhereProvider(RolesAnywhereConfig cfg)
    : cfg_(std::move(cfg)) {}

HmacCredentials RolesAnywhereProvider::get_hmac() {
  std::lock_guard<std::mutex> lk(mu_);
  const auto now = std::chrono::system_clock::now();
  const bool need_mint = !valid_ ||
                          now + std::chrono::minutes(5) >= expires_at_;
  if (need_mint) {
    std::string err;
    if (!mint_locked_(&err)) return {};
  }
  return cached_;
}

void RolesAnywhereProvider::invalidate() {
  std::lock_guard<std::mutex> lk(mu_);
  valid_ = false;
}

bool RolesAnywhereProvider::mint_locked_(std::string *err) {
  std::string cert_pem;
  std::string key_pem;
  if (!read_file(cfg_.cert_pem_path, &cert_pem, err)) return false;
  if (!read_file(cfg_.private_key_pem_path, &key_pem, err)) return false;

  std::string serial_hex;
  std::string cert_der;
  if (!load_cert(cert_pem, &serial_hex, &cert_der, err)) return false;

  // JSON body.
  std::string body = "{\"trustAnchorArn\":\"" + cfg_.trust_anchor_arn +
                      "\",\"profileArn\":\"" + cfg_.profile_arn +
                      "\",\"roleArn\":\"" + cfg_.role_arn +
                      "\",\"durationSeconds\":" +
                      std::to_string(cfg_.duration_seconds) + "}";

  const std::string host = "rolesanywhere." + cfg_.region + ".amazonaws.com";
  const std::string url = "https://" + host + "/sessions";
  const time_t now = std::time(nullptr);
  const std::string amz_date = aws_date_utc(now);
  const std::string yyyymmdd = amz_date.substr(0, 8);
  const std::string scope =
      yyyymmdd + "/" + cfg_.region + "/rolesanywhere/aws4_request";

  const std::string cert_b64 = base64_std(cert_der);

  // Canonical request.  Note headers are lex-sorted by name.
  std::stringstream cr;
  cr << "POST\n";
  cr << "/sessions\n";
  cr << "\n";  // no query string
  cr << "content-type:application/json\n";
  cr << "host:" << host << "\n";
  cr << "x-amz-date:" << amz_date << "\n";
  cr << "x-amz-x509:" << cert_b64 << "\n";
  cr << "\n";
  cr << "content-type;host;x-amz-date;x-amz-x509\n";
  cr << hex_encode(sha256(body));

  const std::string canonical_request = cr.str();

  std::string sts;
  sts.append("AWS4-X509-RSA-SHA256\n");
  sts.append(amz_date).append("\n");
  sts.append(scope).append("\n");
  sts.append(hex_encode(sha256(canonical_request)));

  std::string raw_sig;
  if (!rsa_sign_sha256(sts, key_pem, &raw_sig, err)) return false;
  const std::string sig_hex = hex_encode(raw_sig);

  const std::string auth = "AWS4-X509-RSA-SHA256 Credential=" + serial_hex +
                            "/" + scope +
                            ", SignedHeaders=content-type;host;x-amz-date;"
                            "x-amz-x509"
                            ", Signature=" +
                            sig_hex;

  const std::vector<std::string> headers = {
      "Host: " + host,
      "Content-Type: application/json",
      "X-Amz-Date: " + amz_date,
      "X-Amz-X509: " + cert_b64,
      "Authorization: " + auth,
  };

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
          *e = "Roles Anywhere returned HTTP " + std::to_string(code);
          return RetryDecision::RetryableFailure;
        }
        if (code < 200 || code >= 300) {
          *e = "Roles Anywhere returned HTTP " + std::to_string(code) + ": " +
               response;
          return RetryDecision::PermanentFailure;
        }
        return RetryDecision::Success;
      },
      err);
  if (!ok) return false;

  HmacCredentials fresh;
  std::chrono::system_clock::time_point exp;
  std::string parse_err;
  if (!parse_response_json(response, &fresh, &exp, &parse_err)) {
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
