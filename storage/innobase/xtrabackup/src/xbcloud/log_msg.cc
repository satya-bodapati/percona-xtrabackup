/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

xbcloud::log_{info,warn,error} backed by direct stderr writes that
match xtrabackup's xb::info()/xb::warn()/xb::error() output format.
Linked into binaries that don't link the innodb logger framework
(xbcloud, xbstream, xbcrypt).

Output format (matches xb::info()):
  2026-06-23T00:21:59.065368+01:00 0 [Note]    [MY-011825] [Xtrabackup] <msg>
  2026-06-23T00:21:59.065368+01:00 0 [Warning] [MY-011825] [Xtrabackup] <msg>
  2026-06-23T00:21:59.065368+01:00 0 [ERROR]   [MY-011825] [Xtrabackup] <msg>

The corresponding xtrabackup-binary impl is log_xbinfo.cc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#include "xbcloud/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>

namespace xbcloud {

namespace {

/* Single mutex around stderr writes so the timestamp+message lines
   don't interleave when multiple threads (e.g. the libev rate-log
   thread + a worker) log concurrently.  fputs/fprintf are otherwise
   not guaranteed atomic on Linux glibc for arbitrary buffer sizes. */
std::mutex &log_mutex() {
  static std::mutex m;
  return m;
}

/* Format the current local time as ISO 8601 with microsecond
   precision and timezone offset, e.g.
   "2026-06-23T00:21:59.065368+01:00".  Matches xb::info()'s
   leading-timestamp convention. */
std::string iso_now() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto secs = time_point_cast<seconds>(now);
  const auto micros =
      duration_cast<microseconds>(now - secs).count();
  std::time_t t = system_clock::to_time_t(now);
  std::tm tm_buf;
  localtime_r(&t, &tm_buf);

  char date_buf[32];
  std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

  /* Timezone offset in +HH:MM form. */
  char tz_buf[8];
  std::strftime(tz_buf, sizeof(tz_buf), "%z", &tm_buf);
  /* "%z" yields "+0100"; reshape to "+01:00" for ISO 8601. */
  char tz_iso[8] = "+00:00";
  if (tz_buf[0] != '\0') {
    tz_iso[0] = tz_buf[0];
    tz_iso[1] = tz_buf[1];
    tz_iso[2] = tz_buf[2];
    tz_iso[3] = ':';
    tz_iso[4] = tz_buf[3];
    tz_iso[5] = tz_buf[4];
    tz_iso[6] = '\0';
  }

  char out[64];
  snprintf(out, sizeof(out), "%s.%06lld%s", date_buf,
           static_cast<long long>(micros), tz_iso);
  return std::string(out);
}

void emit(const char *level_tag, const std::string &body) {
  std::lock_guard<std::mutex> lock(log_mutex());
  std::fprintf(stderr, "%s 0 %s [MY-011825] [Xtrabackup] %s\n",
               iso_now().c_str(), level_tag, body.c_str());
  std::fflush(stderr);
}

}  // namespace

log_info::~log_info()  { emit("[Note]   ", m_oss.str()); }
log_warn::~log_warn()  { emit("[Warning]", m_oss.str()); }
log_error::~log_error() { emit("[ERROR]  ", m_oss.str()); }

}  // namespace xbcloud
