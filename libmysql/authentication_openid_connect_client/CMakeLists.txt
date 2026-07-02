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
# plugin as a loadable .so (CLIENT_ONLY, MODULE_ONLY).  Ships in
# ${INSTALL_PLUGINDIR} alongside the other CLIENT_ONLY auth plugins so
# libmysqlclient's mysql_load_plugin_v() can dlopen it at connection
# time.  xtrabackup arranges for it to be found via
# mysql_options(MYSQL_PLUGIN_DIR, opt_xtra_plugin_dir) inside
# xb_mysql_connect().
#
# Mirrors libmysql/authentication_oci_client/CMakeLists.txt in shape.
#

MYSQL_ADD_PLUGIN(
  authentication_openid_connect_client

  authentication_openid_connect_client_plugin.cc

  LINK_LIBRARIES mysys

  CLIENT_ONLY
  MODULE_ONLY MODULE_OUTPUT_NAME "authentication_openid_connect_client"
)
