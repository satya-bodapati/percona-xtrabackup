/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Parser for ~/.aws/credentials + ~/.aws/config (INI format).

Recognises the full AWS SDK / CLI profile schema:

  Plain HMAC keys
    aws_access_key_id
    aws_secret_access_key
    aws_session_token

  STS AssumeRole chaining
    role_arn
    source_profile          — parent profile name (must exist in file)
    credential_source       — Ec2InstanceMetadata | EcsContainer | Environment
    role_session_name
    external_id
    duration_seconds
    mfa_serial              — makes the profile unsupported (interactive)

  STS AssumeRoleWithWebIdentity (EKS IRSA, GitHub Actions, ...)
    web_identity_token_file

  External helper (Roles Anywhere via aws_signing_helper, Vault, HSM, ...)
    credential_process

  SSO (IAM Identity Center)
    sso_start_url
    sso_region
    sso_account_id
    sso_role_name

  Region hint
    region

Unrecognised keys are silently ignored — matches AWS SDK behaviour.

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

#include <map>
#include <string>

#include "../credential_provider.h"

namespace xbcloud {
namespace auth {
namespace aws {

// One section of ~/.aws/credentials or ~/.aws/config.  All fields
// default-constructed empty; caller inspects which ones are populated
// to decide how to build the CredentialProvider chain.
struct ParsedProfile {
  // Plain HMAC.
  std::string aws_access_key_id;
  std::string aws_secret_access_key;
  std::string aws_session_token;

  // STS AssumeRole family.
  std::string role_arn;
  std::string source_profile;
  std::string credential_source;  // "Ec2InstanceMetadata" | "Environment" | "EcsContainer"
  std::string role_session_name;
  std::string external_id;
  int         duration_seconds{0};
  std::string mfa_serial;
  std::string web_identity_token_file;

  // External helper.
  std::string credential_process;

  // SSO.
  std::string sso_start_url;
  std::string sso_region;
  std::string sso_account_id;
  std::string sso_role_name;

  // Region hint (used by STS endpoint construction, etc.).
  std::string region;

  // Convenience: does this look like a plain-HMAC section?
  bool has_hmac_credentials() const {
    return !aws_access_key_id.empty() && !aws_secret_access_key.empty();
  }
};

// Backwards-compatible flat parser — only fills aws_access_key_id /
// aws_secret_access_key / aws_session_token into an HmacCredentials.
// Kept for callers that already do the plain-HMAC-only path (the
// current xbcloud.cc S3 branch, pre-resolver).  Prefer load_profile_v2
// for new code.
bool load_profile(const std::string &path, const std::string &profile,
                  HmacCredentials *out, std::string *err);

// Full-schema parser.  Reads a section (matched as both "[foo]" and
// "[profile foo]") and populates *out with every recognised key.
// Returns true even if the section only has AssumeRole-style keys and
// no HMAC — the resolver handles that.
bool load_profile_v2(const std::string &path, const std::string &profile,
                     ParsedProfile *out, std::string *err);

// Default AWS credentials path: $HOME/.aws/credentials.
std::string default_credentials_path();

// Default AWS config path: $HOME/.aws/config.  Some fields (like
// role_arn / source_profile / region) conventionally live here rather
// than in credentials.
std::string default_config_path();

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_PROFILE_FILE_H
