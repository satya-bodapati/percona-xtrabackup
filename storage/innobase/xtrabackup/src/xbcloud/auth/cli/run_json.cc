/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See run_json.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "run_json.h"

#include <sys/wait.h>
#include <cstdio>
#include <memory>

#include "../retry_backoff.h"

namespace xbcloud {
namespace auth {
namespace cli {

namespace {

// Actual popen invocation.  One attempt.  Returns:
//   RetryDecision::Success on child exit 0
//   RetryDecision::RetryableFailure on non-zero exit / read error
//   RetryDecision::PermanentFailure never — CLI failures are always
//     considered retryable in this helper, because we can't easily
//     distinguish "network blip" from "actually wrong config" from
//     the child's exit code alone.  If the config is actually wrong,
//     retry_with_backoff will burn its budget and surface the last
//     error to the caller.
RetryDecision popen_once(const std::string &command, std::string *out,
                         std::string *err) {
  out->clear();
  FILE *p = ::popen(command.c_str(), "r");
  if (p == nullptr) {
    *err = "popen() failed";
    return RetryDecision::RetryableFailure;
  }
  std::unique_ptr<FILE, int (*)(FILE *)> guard(p, &::pclose);
  char buf[4096];
  while (true) {
    const size_t n = std::fread(buf, 1, sizeof(buf), p);
    if (n > 0) out->append(buf, n);
    if (n < sizeof(buf)) {
      if (std::feof(p)) break;
      if (std::ferror(p)) {
        *err = "read from CLI subprocess failed";
        return RetryDecision::RetryableFailure;
      }
    }
  }
  // pclose() runs when guard destructs.  We can't inspect the exit
  // code with unique_ptr — so release + close manually here.
  FILE *raw = guard.release();
  const int rc = ::pclose(raw);
  if (rc == -1) {
    *err = "pclose() failed";
    return RetryDecision::RetryableFailure;
  }
  if (WIFEXITED(rc)) {
    const int exit_code = WEXITSTATUS(rc);
    if (exit_code == 0) return RetryDecision::Success;
    *err = "CLI subprocess exited with status " + std::to_string(exit_code) +
           ": " + *out;
    return RetryDecision::RetryableFailure;
  }
  if (WIFSIGNALED(rc)) {
    *err = "CLI subprocess killed by signal " +
           std::to_string(WTERMSIG(rc));
    return RetryDecision::RetryableFailure;
  }
  *err = "CLI subprocess terminated abnormally";
  return RetryDecision::RetryableFailure;
}

}  // namespace

bool run_json_command(const std::string &command, std::string *out,
                      std::string *err) {
  RetryPolicy policy;
  return retry_with_backoff(
      policy,
      [&](int /*attempt*/, std::string *e) {
        return popen_once(command, out, e);
      },
      err);
}

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud
