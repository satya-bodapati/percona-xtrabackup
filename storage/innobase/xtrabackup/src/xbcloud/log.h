/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Thin logger abstraction for code inside xbcloud_internal (multipart.cc,
http.cc, etc.) that needs to log messages but is linked into BOTH the
standalone xbcloud binary (which does not link the innodb logger
framework) AND xtrabackup (which does, via xb::info()).

Usage matches xb::info() stream syntax:

    xbcloud::log_info() << "multipart start " << name << " bytes=" << n;
    xbcloud::log_warn() << "rate logger: clock not moving";
    xbcloud::log_error() << "multipart " << name << " part #" << k
                         << " FAILED";

The destructor emits the accumulated stream on a SINGLE line.  Each
binary picks an implementation (log_xbinfo.cc for xtrabackup,
log_msg.cc for standalone xbcloud) via CMake so the output format
matches xtrabackup's other logs in both cases.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#ifndef XBCLOUD_LOG_H
#define XBCLOUD_LOG_H

#include <sstream>
#include <string>

namespace xbcloud {

class log_info {
 public:
  log_info() = default;
  /* Non-copyable, non-movable -- usage is a temporary lifetime
     expression, the dtor is what emits.  No need to support
     storing instances or chaining across statements. */
  log_info(const log_info &) = delete;
  log_info &operator=(const log_info &) = delete;
  ~log_info();  /* defined per-binary in log_xbinfo.cc or log_msg.cc */

  template <typename T>
  log_info &operator<<(const T &val) {
    m_oss << val;
    return *this;
  }

 private:
  std::ostringstream m_oss;
};

class log_warn {
 public:
  log_warn() = default;
  log_warn(const log_warn &) = delete;
  log_warn &operator=(const log_warn &) = delete;
  ~log_warn();

  template <typename T>
  log_warn &operator<<(const T &val) {
    m_oss << val;
    return *this;
  }

 private:
  std::ostringstream m_oss;
};

class log_error {
 public:
  log_error() = default;
  log_error(const log_error &) = delete;
  log_error &operator=(const log_error &) = delete;
  ~log_error();

  template <typename T>
  log_error &operator<<(const T &val) {
    m_oss << val;
    return *this;
  }

 private:
  std::ostringstream m_oss;
};

}  // namespace xbcloud

#endif  // XBCLOUD_LOG_H
