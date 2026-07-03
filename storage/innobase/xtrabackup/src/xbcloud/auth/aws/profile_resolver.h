/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Profile resolver — turns a profile name into a CredentialProvider by
walking the ~/.aws/config + ~/.aws/credentials chain the same way the
AWS CLI / SDKs do.

Dispatch table on the parsed profile:

  aws_access_key_id + aws_secret_access_key present
      → HmacProvider (plain long-lived keys)

  role_arn + source_profile
      → StsAssumeRoleProvider whose parent is
        resolve_profile(source_profile) (recursion)

  role_arn + credential_source = Ec2InstanceMetadata
      → StsAssumeRoleProvider(parent = Ec2InstanceProfileProvider)

  role_arn + credential_source = Environment
      → StsAssumeRoleProvider(parent = HMAC from AWS_ACCESS_KEY_ID
        + AWS_SECRET_ACCESS_KEY env vars)

  credential_process
      → CredentialProcessProvider running the helper.  Composes with
        role_arn: if both are present, credential_process is the
        parent for the STS AssumeRole call.

Explicit rejections with clear error messages:

  * mfa_serial       — interactive; unattended tool can't prompt.
  * web_identity_token_file — SDK feature not yet implemented.
  * sso_* fields     — SDK feature not yet implemented.
  * Empty profile    — nothing recognisable.

Cycle detection: resolve_profile keeps a visited-set across recursive
calls; A → B → A errors out with a "profile chain cycle detected"
message rather than blowing the stack.

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

#ifndef XBCLOUD_AUTH_AWS_PROFILE_RESOLVER_H
#define XBCLOUD_AUTH_AWS_PROFILE_RESOLVER_H

#include <memory>
#include <string>

#include "../credential_provider.h"
#include "xbcloud/http.h"

namespace xbcloud {
namespace auth {
namespace aws {

// Options passed into the resolver.  Any of these may be empty —
// caller sensible defaults are applied.
struct ProfileResolverOptions {
  std::string credentials_path;   // default: $HOME/.aws/credentials
  std::string config_path;        // default: $HOME/.aws/config
  std::string region;             // default: us-east-1 (used by STS)
  // Http_client is needed to construct an Ec2InstanceProfileProvider
  // when a profile uses credential_source = Ec2InstanceMetadata.
  // May be null; in that case an EC2-metadata-backed profile fails
  // resolution with a clear error.
  const Http_client *http_client_for_ec2{nullptr};
};

// Resolve profile_name from the credentials + config files listed in
// opts.  Returns nullptr and populates *err on failure.  Recursively
// walks role_arn / source_profile chains through the two files
// (credentials + config), matching AWS SDK behaviour where source
// keys can live in either file.
std::unique_ptr<CredentialProvider> resolve_profile(
    const std::string &profile_name, const ProfileResolverOptions &opts,
    std::string *err);

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AWS_PROFILE_RESOLVER_H
