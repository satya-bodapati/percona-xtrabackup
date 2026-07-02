/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Parser for ~/.aws/credentials + ~/.aws/config (INI format).  Returns
the resolved HMAC key material for a given profile.

Not a general INI parser — we understand only what the AWS SDK
convention needs:

  ~/.aws/credentials
    [default]
    aws_access_key_id = AKIA...
    aws_secret_access_key = ...
    aws_session_token = ... (optional; typically for temp creds)

    [profile-name]
    aws_access_key_id = ...
    aws_secret_access_key = ...

  ~/.aws/config uses [profile foo] section names except the default
  which is [default].  We support both files' shape.

Chained assume-role via source_profile/role_arn is a follow-up.

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

#ifndef XBCLOUD_AUTH_AWS_PROFILE_FILE_H
#define XBCLOUD_AUTH_AWS_PROFILE_FILE_H

#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace aws {

// Reads path (defaults to ~/.aws/credentials), selects [profile],
// populates *out.  profile="" resolves to the [default] section.
// Returns false + fills *err on failure.
bool load_profile(const std::string &path, const std::string &profile,
                  HmacCredentials *out, std::string *err);

// Convenience: resolves the default file path ($HOME/.aws/credentials).
std::string default_credentials_path();

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_PROFILE_FILE_H
