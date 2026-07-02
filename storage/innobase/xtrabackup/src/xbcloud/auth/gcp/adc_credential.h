/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Application Default Credentials (ADC) JSON parser for GCP.

Reads a JSON file matching one of Google's documented shapes:

  { "type": "service_account", "client_email": "...", "private_key":
    "-----BEGIN PRIVATE KEY-----\n...", "token_uri": "...", ... }

  { "type": "authorized_user", "client_id": "...", "client_secret":
    "...", "refresh_token": "...", ... }

Any other "type" value (external_account, impersonated_service_account,
etc.) is rejected with an explicit error — v1 scope of PXB-3592
covers just service_account + authorized_user; other flows land as
separate follow-ups.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*******************************************************/

#ifndef XBCLOUD_AUTH_GCP_ADC_CREDENTIAL_H
#define XBCLOUD_AUTH_GCP_ADC_CREDENTIAL_H

#include <string>

namespace xbcloud {
namespace auth {
namespace gcp {

enum class AdcType {
  kServiceAccount,
  kAuthorizedUser,
};

struct AdcCredential {
  AdcType type;

  // Service-account fields.
  std::string client_email;
  std::string private_key;   // PEM
  std::string token_uri;

  // Authorized-user fields.
  std::string client_id;
  std::string client_secret;
  std::string refresh_token;
};

// Parse ADC JSON from a file path.  Returns true on success.
// On failure fills *err.
bool load_adc_from_file(const std::string &path, AdcCredential *out,
                        std::string *err);

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_GCP_ADC_CREDENTIAL_H
