/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

xbcloud::log_{info,warn,error} backed by xtrabackup's xb::info /
xb::warn / xb::error.  Linked into the xtrabackup binary.

The corresponding standalone-binary impl is log_msg.cc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#include "xbcloud/log.h"

#include "srv0srv.h"
#include "ut0log.h"

namespace xbcloud {

log_info::~log_info() { xb::info() << m_oss.str(); }
log_warn::~log_warn() { xb::warn() << m_oss.str(); }
log_error::~log_error() { xb::error() << m_oss.str(); }

}  // namespace xbcloud
