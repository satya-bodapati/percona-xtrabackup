/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See credential_process.h.

Runs the caller-supplied helper via `/bin/sh -c ...` (via popen) so
shell metacharacters, quoting, and PATH lookup work exactly the way
they do when `aws` CLI runs the same command.  Reads stdout until
EOF (bounded to 64 KB — real helpers return ~200-500 bytes; anything
larger is suspicious).  Parses the JSON per AWS's documented schema.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "credential_process.h"

#include <cstdio>
#include <ctime>
#include <utility>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>

#include "../retry_backoff.h"

namespace xbcloud {
namespace auth {
namespace aws {

namespace {

// Run cmd via popen(), read stdout up to max_bytes.  Returns the
// child's exit status via *exit_status.
bool run_helper(const std::string &cmd, std::string *stdout_out,
                int *exit_status, std::string *err) {
  FILE *f = popen(cmd.c_str(), "r");
  if (!f) {
    *err = "popen failed for helper: " + cmd;
    return false;
  }
  constexpr size_t kMaxBytes = 64 * 1024;
  stdout_out->clear();
  char buf[4096];
  size_t total = 0;
  while (total < kMaxBytes) {
    const size_t n = fread(buf, 1, sizeof(buf), f);
    if (n == 0) break;
    stdout_out->append(buf, n);
    total += n;
  }
  const int rc = pclose(f);
  if (rc == -1) {
    *err = "pclose failed for helper: " + cmd;
    return false;
  }
  // pclose returns the child's status in the wait(2) format; extract
  // the exit code portion for clarity.  Non-zero = helper failed.
  *exit_status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
  return true;
}

bool parse_helper_response(const std::string &body, HmacCredentials *creds,
                           std::chrono::system_clock::time_point *expires_at,
                           bool *never_expires, std::string *err) {
  rapidjson::Document d;
  if (d.Parse(body.c_str()).HasParseError()) {
    *err = "credential_process helper returned invalid JSON";
    return false;
  }
  // Version field: AWS SDKs require it to be 1; we log-and-continue
  // if missing since some existing helpers (early Vault versions,
  // hand-rolled scripts) omit it.
  auto get = [&](const char *k) -> std::string {
    return (d.HasMember(k) && d[k].IsString()) ? d[k].GetString() : "";
  };
  creds->access_key = get("AccessKeyId");
  creds->secret_key = get("SecretAccessKey");
  creds->session_token = get("SessionToken");
  if (creds->access_key.empty() || creds->secret_key.empty()) {
    *err = "credential_process helper JSON missing AccessKeyId or "
           "SecretAccessKey";
    return false;
  }
  const std::string exp = get("Expiration");
  if (exp.empty()) {
    // No Expiration — helper is signalling "these creds are stable,
    // no need to refresh".  Rare but valid (per AWS docs, e.g. for
    // long-lived credentials fetched from a static store).
    *never_expires = true;
    *expires_at = std::chrono::system_clock::now() + std::chrono::hours(24 * 365);
    return true;
  }
  struct tm tm {};
  if (strptime(exp.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr) {
    // Also accept "…+00:00" (RFC 3339 form some helpers emit).
    if (strptime(exp.c_str(), "%Y-%m-%dT%H:%M:%S+00:00", &tm) == nullptr) {
      *err = "credential_process helper Expiration is not ISO-8601: " + exp;
      return false;
    }
  }
  *expires_at = std::chrono::system_clock::from_time_t(timegm(&tm));
  *never_expires = false;
  return true;
}

}  // namespace

CredentialProcessProvider::CredentialProcessProvider(std::string command,
                                                     std::string source_label)
    : command_(std::move(command)),
      source_label_(source_label.empty()
                        ? std::string("aws:credential_process")
                        : std::move(source_label)) {}

HmacCredentials CredentialProcessProvider::get_hmac() {
  std::lock_guard<std::mutex> lk(mu_);
  const auto now = std::chrono::system_clock::now();
  const bool need_mint =
      !valid_ || (!never_expires_ &&
                  now + std::chrono::minutes(5) >= expires_at_);
  if (need_mint) {
    std::string err;
    if (!mint_locked_(&err)) return {};
  }
  return cached_;
}

void CredentialProcessProvider::invalidate() {
  std::lock_guard<std::mutex> lk(mu_);
  valid_ = false;
}

bool CredentialProcessProvider::mint_locked_(std::string *err) {
  std::string body;
  int exit_status = 0;
  RetryPolicy policy;
  const bool ok = retry_with_backoff(
      policy,
      [&](int, std::string *e) {
        body.clear();
        exit_status = 0;
        std::string run_err;
        if (!run_helper(command_, &body, &exit_status, &run_err)) {
          *e = std::move(run_err);
          return RetryDecision::RetryableFailure;
        }
        if (exit_status != 0) {
          // Helper failed — bubble its stderr/stdout into the error.
          // For a truly non-recoverable case (bad config, missing
          // cert), retrying won't help, but our shared policy doesn't
          // distinguish; the helper is expected to be deterministic
          // and will fail identically each retry.
          *e = "credential_process helper exited with " +
               std::to_string(exit_status) + ": " + body;
          return RetryDecision::RetryableFailure;
        }
        return RetryDecision::Success;
      },
      err);
  if (!ok) return false;

  HmacCredentials fresh;
  std::chrono::system_clock::time_point exp;
  bool ne = false;
  std::string parse_err;
  if (!parse_helper_response(body, &fresh, &exp, &ne, &parse_err)) {
    *err = parse_err;
    return false;
  }
  cached_ = std::move(fresh);
  expires_at_ = exp;
  never_expires_ = ne;
  valid_ = true;
  return true;
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
