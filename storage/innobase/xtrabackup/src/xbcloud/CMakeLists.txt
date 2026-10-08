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
# xbcloud binary
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

MYSQL_ADD_EXECUTABLE(xbcloud
  xbcloud.cc
  ../xbstream_read.cc
  http.cc
  object_store.cc
  azure.cc
  s3.cc
  s3_ec2.cc
  ../xbcrypt_common.cc
  ../file_utils.cc
  swift.cc
  library_version_check.cc)

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

IF(WITH_UNIT_TESTS)
  XB_ADD_UNIT_TEST(xbcloud-t
    SOURCES xbcloud-t.cc http.cc s3.cc s3_ec2.cc azure.cc swift.cc
    LIBRARIES ${GCRYPT_LIBS} ${LIBEV_LIBRARIES} extra::rapidjson ext::curl
      mysys crc)
ENDIF()
