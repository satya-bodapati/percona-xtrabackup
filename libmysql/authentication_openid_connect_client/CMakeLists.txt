# Copyright (c) 2024, Oracle and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

#
# Configuration for building the OpenID Connect client-side authentication
# plugin as a convenience library merged into libmysqlclient (so that
# xtrabackup links it statically without needing a plugin_dir).
#
# Upstream PS builds this as a MODULE_ONLY .so under
# libmysql/authentication_openid_connect_client/CMakeLists.txt.  In PXB we
# want the plugin to be part of libmysqlclient itself; the ADD_CONVENIENCE_LIBRARY
# pattern below mirrors libmysql/authentication_win/CMakeLists.txt, and the
# resulting object is merged into libmysqlclient via LIBS_TO_MERGE in the
# parent libmysql/CMakeLists.txt.
#

ADD_CONVENIENCE_LIBRARY(auth_openid_connect_client
  authentication_openid_connect_client_plugin.cc
  LINK_LIBRARIES mysys)
