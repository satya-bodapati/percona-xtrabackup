/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

OAuth2 mint helpers.  See oauth2_client.h.

RS256 signing goes through OpenSSL EVP_DigestSign — the same code
path OpenSSL exposes for any TLS signing operation.  We do NOT
depend on jwt-cpp here to avoid pulling in another submodule for a
single algorithm.  Everything crypto-adjacent stays within OpenSSL's
existing API surface.

HTTP POST is done via libcurl direct, not xbcloud's Http_client.
Http_client is designed around cloud-storage semantics (retry loops,
per-bucket state, event handlers); for a one-shot token-endpoint
POST that would be a lot of scaffolding for very little.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "oauth2_client.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <curl/curl.h>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>

#include <cstring>
#include <memory>

namespace xbcloud {
namespace auth {

namespace {

// Base64URL-encode without padding (RFC 4648 §5).  Small helper —
// mysys has base64 encoders but no URL-safe variant.
std::string base64url_encode(const std::string &in) {
  static const char *alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  int val = 0;
  int valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) | c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(alphabet[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(alphabet[((val << 8) >> (valb + 8)) & 0x3F]);
  return out;
}

// RS256-sign the given message with the caller's PEM private key.
// Returns raw signature bytes (base64url-encode at the call site).
bool rs256_sign(const std::string &message, const std::string &private_key_pem,
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
    *err = "OpenSSL: PEM_read_bio_PrivateKey failed — is this an RSA key?";
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

// libcurl body write callback that appends to a std::string.
size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

// POST url-encoded form to url; append response body to *body.
bool http_post_form(const std::string &url, const std::string &body_in,
                    std::string *body_out, std::string *err) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                            &curl_easy_cleanup);
  if (!curl) {
    *err = "curl_easy_init failed";
    return false;
  }
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body_in.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body_in.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, body_out);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
  const CURLcode rc = curl_easy_perform(curl.get());
  if (rc != CURLE_OK) {
    *err = std::string("curl: ") + curl_easy_strerror(rc);
    return false;
  }
  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code < 200 || http_code >= 300) {
    *err = "token endpoint returned HTTP " + std::to_string(http_code) +
           " body=" + *body_out;
    return false;
  }
  return true;
}

// Parse { "access_token": "...", "expires_in": N } into MintOutput.
bool parse_mint_response(const std::string &body, MintOutput *out,
                         std::string *err) {
  rapidjson::Document d;
  if (d.Parse(body.c_str()).HasParseError()) {
    *err = "token response is not valid JSON";
    return false;
  }
  if (!d.HasMember("access_token") || !d["access_token"].IsString()) {
    *err = "token response missing access_token";
    return false;
  }
  int expires_in = 3600;  // spec default when omitted
  if (d.HasMember("expires_in") && d["expires_in"].IsInt()) {
    expires_in = d["expires_in"].GetInt();
  }
  out->access_token = d["access_token"].GetString();
  out->expires_at =
      std::chrono::system_clock::now() + std::chrono::seconds(expires_in);
  return true;
}

}  // namespace

bool mint_from_service_account(const std::string &client_email,
                               const std::string &private_key_pem,
                               const std::string &token_uri,
                               const std::string &scope, MintOutput *out,
                               std::string *err) {
  const auto now = std::chrono::system_clock::now();
  const auto iat =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  const auto exp = iat + 3600;

  std::string header_json = R"({"alg":"RS256","typ":"JWT"})";
  std::string payload_json = "{\"iss\":\"" + client_email +
                              "\",\"sub\":\"" + client_email + "\",\"aud\":\"" +
                              token_uri + "\",\"scope\":\"" + scope +
                              "\",\"iat\":" + std::to_string(iat) +
                              ",\"exp\":" + std::to_string(exp) + "}";
  std::string header_b64 = base64url_encode(header_json);
  std::string payload_b64 = base64url_encode(payload_json);
  std::string signing_input = header_b64 + "." + payload_b64;

  std::string sig;
  if (!rs256_sign(signing_input, private_key_pem, &sig, err)) return false;
  std::string assertion = signing_input + "." + base64url_encode(sig);

  const std::string body =
      "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&"
      "assertion=" +
      assertion;

  std::string response_body;
  if (!http_post_form(token_uri, body, &response_body, err)) return false;
  return parse_mint_response(response_body, out, err);
}

bool mint_from_refresh_token(const std::string &client_id,
                             const std::string &client_secret,
                             const std::string &refresh_token,
                             const std::string &token_uri, MintOutput *out,
                             std::string *err) {
  const std::string body = "grant_type=refresh_token&client_id=" + client_id +
                           "&client_secret=" + client_secret +
                           "&refresh_token=" + refresh_token;
  std::string response_body;
  if (!http_post_form(token_uri, body, &response_body, err)) return false;
  return parse_mint_response(response_body, out, err);
}

}  // namespace auth
}  // namespace xbcloud
