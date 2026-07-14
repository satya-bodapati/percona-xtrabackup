/******************************************************
Copyright (c) 2021-2022 Percona LLC and/or its affiliates.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*******************************************************/
#ifndef XB_KEYRING_COMPONENTS_H
#define XB_KEYRING_COMPONENTS_H

#include <mysql.h>
namespace xtrabackup {
namespace components {
/** Data types */
extern bool keyring_component_initialized;
extern std::string component_config_path;
extern const char *XTRABACKUP_KEYRING_FILE_CONFIG;
extern const char *XTRABACKUP_KEYRING_KMIP_CONFIG;
extern const char *XTRABACKUP_KEYRING_KMS_CONFIG;
extern const char *XTRABACKUP_KEYRING_VAULT_CONFIG;

/** @Return name of component config file */
const char *xb_component_config_file();

/**
  Initialize Keyring component by querying config from a running server
  This is used at --backup

  @param [in]  connection       MYSQL connection to running server

  @return false in case of error, true otherwise
  @keyring_component_initialized false keyring component not initialized
  @keyring_component_initialized true  keyring component initialized
*/
bool keyring_init_online(MYSQL *connection);

/**
  Initialize Keyring component by reading xtrabackup_keyring_component.cnf file
  This is used at --prepare, --move-back, --copy-back

  @return false in case of error, true otherwise
  @keyring_component_initialized false keyring component not initialized
  @keyring_component_initialized true  keyring component initialized
*/
bool keyring_init_offline();

/**
  Reload the keyring backend so keys added by the server after xtrabackup
  initialized its keyring become visible (e.g. a master key created by
  ALTER INSTANCE ROTATE INNODB MASTER KEY during the backup). Safe no-op when
  no keyring component is in use.

  @return true on success (including the no-component case), false on error
*/
bool reload_keyring();

/** Initialize component service handles */
bool inititialize_service_handles();

/** Deinitialize component service handles */
void deinitialize_service_handles();
}  // namespace components
}  // namespace xtrabackup
#endif  // XB_KEYRING_COMPONENTS_H
