# Copyright (c) 2019-2023 Percona and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

########################################################################
# xbcloud binary  +  xbcloud_internal OBJECT library shared with
# xtrabackup (Phase 2 ds_cloud reuses the multipart upload machinery).
########################################################################

INCLUDE(gcrypt)
INCLUDE(curl)
INCLUDE(libev)

SET(WITH_CURL "system" CACHE STRING "Using system curl by default")

FIND_GCRYPT()
FIND_EV()

# Remove -Wundef from C++ compiler flags since there are several
# undefined identifiers which are evaluated in for example ev.h
STRING(REPLACE "-Wundef " "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/storage/innobase/include
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/storage/innobase/xtrabackup/src
  ${CMAKE_SOURCE_DIR}/storage/innobase/xtrabackup/src/crc
  ${CMAKE_CURRENT_BINARY_DIR}/..
  ${GCRYPT_INCLUDE_DIR}
  ${LIBEV_INCLUDE_DIRS}
  )

# Shared multipart / HTTP / object-store implementation. Both xbcloud
# and (Phase 2) xtrabackup's ds_cloud link against this. Keep all the
# Object_store / Multipart_uploader / Http_client / signer / per-backend
# client sources here -- only the xbcloud binary's main + version check
# stay xbcloud-specific.
ADD_LIBRARY(xbcloud_internal OBJECT
  http.cc
  object_store.cc
  azure.cc
  s3.cc
  s3_ec2.cc
  swift.cc
  multipart.cc
  )

SET_TARGET_PROPERTIES(xbcloud_internal PROPERTIES POSITION_INDEPENDENT_CODE 1)

# OBJECT libraries don't inherit interface include dirs the same way
# regular targets do; pull them in explicitly so rapidjson and curl
# headers resolve when xtrabackup compiles us too.
TARGET_LINK_LIBRARIES(xbcloud_internal PUBLIC extra::rapidjson ext::curl)

MYSQL_ADD_EXECUTABLE(xbcloud
  xbcloud.cc
  ../xbstream_read.cc
  ../xbcrypt_common.cc
  ../file_utils.cc
  library_version_check.cc
  log_msg.cc
  $<TARGET_OBJECTS:xbcloud_internal>
  )

SET_TARGET_PROPERTIES(xbcloud
        PROPERTIES LINKER_LANGUAGE CXX
        )

TARGET_LINK_LIBRARIES(xbcloud
  ${GCRYPT_LIBS}
  ${LIBEV_LIBRARIES}
  extra::rapidjson
  ext::curl
  mysys
  crc
  )

IF (WITH_UNIT_TESTS AND CMAKE_VERSION VERSION_GREATER 3.22.1)
  ENABLE_TESTING()

  FIND_PACKAGE(GTest)

  IF (GTEST_FOUND)

    INCLUDE_DIRECTORIES(
      ${GTEST_INCLUDE_DIRECTORIES}
      )

    ADD_EXECUTABLE(xbcloud-t xbcloud-t.cc
      log_msg.cc
      $<TARGET_OBJECTS:xbcloud_internal>
      )

    TARGET_LINK_LIBRARIES(xbcloud-t
      ${GCRYPT_LIBS}
      ${LIBEV_LIBRARIES}
      ${GTEST_BOTH_LIBRARIES}
      GTest::gtest
      GTest::gmock
      extra::rapidjson
      ext::curl
      mysys
      crc
      )

    ADD_TEST(xbcloud xbcloud-t)

  ENDIF()

ENDIF()
