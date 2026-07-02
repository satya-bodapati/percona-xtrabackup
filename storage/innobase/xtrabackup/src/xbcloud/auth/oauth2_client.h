/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

OAuth2 mint helpers: JWT-bearer (RFC 7523) and refresh-token grants.

Used by gcp::AdcProvider (and future gcp / azure Bearer providers)
to exchange either a signed JWT assertion or a stored refresh_token
for a short-lived access_token + expires_in.

RS256 signing is done directly against OpenSSL (already available
transitively via libcurl; auth/oauth2_client.cc links OpenSSL::Crypto
explicitly).  HTTP POST is done against libcurl directly to keep
this module self-contained — the existing xbcloud Http_client is
designed for cloud-storage requests and would drag in more shape
than we need here.

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

#ifndef XBCLOUD_AUTH_OAUTH2_CLIENT_H
#define XBCLOUD_AUTH_OAUTH2_CLIENT_H

#include <chrono>
#include <string>

namespace xbcloud {
namespace auth {

struct MintOutput {
  std::string access_token;
  std::chrono::system_clock::time_point expires_at{};
};

// RFC 7523 JWT-bearer grant.
// Signs a JWT assertion { iss, sub, aud=token_uri, scope, iat, exp }
// with the caller's PEM-encoded RSA private key (RS256) and POSTs
// it to token_uri.  Populates *out and returns true on success; on
// failure returns false and fills *err with a human-readable message.
bool mint_from_service_account(const std::string &client_email,
                               const std::string &private_key_pem,
                               const std::string &token_uri,
                               const std::string &scope, MintOutput *out,
                               std::string *err);

// OAuth2 refresh-token grant.  POSTs refresh_token + client_id +
// client_secret to token_uri and returns the new access_token /
// expires_in.  Used when the ADC file is type "authorized_user"
// (from `gcloud auth login`).
bool mint_from_refresh_token(const std::string &client_id,
                             const std::string &client_secret,
                             const std::string &refresh_token,
                             const std::string &token_uri, MintOutput *out,
                             std::string *err);

}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_OAUTH2_CLIENT_H
