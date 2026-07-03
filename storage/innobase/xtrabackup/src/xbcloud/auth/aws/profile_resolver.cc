/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See profile_resolver.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "profile_resolver.h"

#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>

#include "credential_process.h"
#include "ec2_instance_profile.h"
#include "hmac_provider.h"
#include "profile_file.h"
#include "sts_assume_role.h"

namespace xbcloud {
namespace auth {
namespace aws {

namespace {

// Try to load a profile section from either the config or credentials
// file (in that order — AWS SDK precedence).  Fills *dst; leaves
// missing keys empty.  Returns true iff at least one file yielded
// the section.
bool load_profile_from_both(const std::string &config_path,
                            const std::string &creds_path,
                            const std::string &profile,
                            ParsedProfile *dst, std::string *err) {
  bool any = false;
  ParsedProfile from_config;
  ParsedProfile from_creds;
  std::string ec, ec2;

  const bool from_cfg =
      !config_path.empty() &&
      load_profile_v2(config_path, profile, &from_config, &ec);
  const bool from_cr =
      !creds_path.empty() &&
      load_profile_v2(creds_path, profile, &from_creds, &ec2);
  any = from_cfg || from_cr;
  if (!any) {
    *err = "profile [" + profile + "] not found in " + config_path + " or " +
           creds_path;
    return false;
  }

  // Merge: credentials file wins for HMAC material (that's where SDKs
  // conventionally keep it); config file wins for role_arn /
  // source_profile / credential_process (that's where SDKs
  // conventionally keep them).  In practice both files can carry
  // both — a value in either wins over an empty.
  auto pick = [](const std::string &a, const std::string &b) {
    return b.empty() ? a : b;
  };
  dst->aws_access_key_id =
      pick(from_config.aws_access_key_id, from_creds.aws_access_key_id);
  dst->aws_secret_access_key =
      pick(from_config.aws_secret_access_key, from_creds.aws_secret_access_key);
  dst->aws_session_token =
      pick(from_config.aws_session_token, from_creds.aws_session_token);
  dst->role_arn = pick(from_creds.role_arn, from_config.role_arn);
  dst->source_profile =
      pick(from_creds.source_profile, from_config.source_profile);
  dst->credential_source =
      pick(from_creds.credential_source, from_config.credential_source);
  dst->role_session_name =
      pick(from_creds.role_session_name, from_config.role_session_name);
  dst->external_id =
      pick(from_creds.external_id, from_config.external_id);
  dst->duration_seconds =
      from_creds.duration_seconds > 0 ? from_creds.duration_seconds
                                       : from_config.duration_seconds;
  dst->mfa_serial = pick(from_creds.mfa_serial, from_config.mfa_serial);
  dst->web_identity_token_file =
      pick(from_creds.web_identity_token_file,
           from_config.web_identity_token_file);
  dst->credential_process =
      pick(from_creds.credential_process, from_config.credential_process);
  dst->sso_start_url =
      pick(from_creds.sso_start_url, from_config.sso_start_url);
  dst->sso_region = pick(from_creds.sso_region, from_config.sso_region);
  dst->sso_account_id =
      pick(from_creds.sso_account_id, from_config.sso_account_id);
  dst->sso_role_name =
      pick(from_creds.sso_role_name, from_config.sso_role_name);
  dst->region = pick(from_creds.region, from_config.region);
  return true;
}

// Recursive worker that carries a visited-set for cycle detection.
std::unique_ptr<CredentialProvider> resolve_recursive(
    const std::string &profile_name, const ProfileResolverOptions &opts,
    std::set<std::string> *visited, std::string *err) {
  if (!visited->insert(profile_name).second) {
    *err = "AWS profile chain cycle detected at [" + profile_name + "]";
    return nullptr;
  }

  ParsedProfile p;
  if (!load_profile_from_both(opts.config_path, opts.credentials_path,
                              profile_name, &p, err)) {
    return nullptr;
  }

  // Unsupported flows — surface clearly before we try to construct
  // a provider that would silently be wrong.
  if (!p.mfa_serial.empty()) {
    *err = "profile [" + profile_name +
           "] uses mfa_serial — MFA-gated profiles are interactive and "
           "unsupported in unattended tools like xbcloud";
    return nullptr;
  }
  if (!p.web_identity_token_file.empty() && p.credential_process.empty()) {
    *err = "profile [" + profile_name +
           "] uses web_identity_token_file — AssumeRoleWithWebIdentity is "
           "not yet supported; workaround: use credential_process with an "
           "external helper";
    return nullptr;
  }
  if (!p.sso_start_url.empty() || !p.sso_role_name.empty()) {
    *err = "profile [" + profile_name +
           "] uses SSO — IAM Identity Center is not yet supported; "
           "workaround: `aws sso login` + credential_process with "
           "aws configure export-credentials";
    return nullptr;
  }

  // Decide the "parent" (HMAC-producing) provider.  If role_arn is
  // set, we'll wrap this in STS AssumeRole below.
  std::unique_ptr<CredentialProvider> parent;
  std::string parent_kind;

  if (!p.credential_process.empty()) {
    parent = std::make_unique<CredentialProcessProvider>(
        p.credential_process,
        "aws:credential_process:profile=" + profile_name);
    parent_kind = "credential_process";
  } else if (!p.source_profile.empty()) {
    // Recurse into the source profile.  Cycle detection carries the
    // same visited set.
    parent = resolve_recursive(p.source_profile, opts, visited, err);
    if (!parent) return nullptr;
    parent_kind = "source_profile=" + p.source_profile;
  } else if (!p.credential_source.empty()) {
    if (p.credential_source == "Ec2InstanceMetadata") {
      if (opts.http_client_for_ec2 == nullptr) {
        *err = "profile [" + profile_name +
               "] uses credential_source=Ec2InstanceMetadata but the "
               "resolver was not passed an Http_client for the IMDS "
               "probe";
        return nullptr;
      }
      auto ec2 = std::make_shared<S3_ec2_instance>(opts.http_client_for_ec2);
      // Probe once so the initial creds are populated before we hand
      // the provider off — matches the shape xbcloud.cc uses today
      // for the top-level EC2 auto-detect path.
      ec2->fetch_metadata();
      parent = std::make_unique<Ec2InstanceProfileProvider>(std::move(ec2));
      parent_kind = "credential_source=Ec2InstanceMetadata";
    } else if (p.credential_source == "Environment") {
      const char *env_ak = std::getenv("AWS_ACCESS_KEY_ID");
      const char *env_sk = std::getenv("AWS_SECRET_ACCESS_KEY");
      const char *env_tok = std::getenv("AWS_SESSION_TOKEN");
      if (!env_ak || !env_sk) {
        *err = "profile [" + profile_name +
               "] uses credential_source=Environment but AWS_ACCESS_KEY_ID"
               " / AWS_SECRET_ACCESS_KEY are not set in the env";
        return nullptr;
      }
      parent = std::make_unique<HmacProvider>(
          env_ak, env_sk, env_tok ? env_tok : "",
          "aws:env:credential_source-in-profile=" + profile_name);
      parent_kind = "credential_source=Environment";
    } else {
      // EcsContainer would fetch from 169.254.170.2/… — same shape as
      // EC2 IMDS.  Not implemented in v1.
      *err = "profile [" + profile_name + "] uses credential_source=" +
             p.credential_source +
             ", which is not yet supported (only Ec2InstanceMetadata and "
             "Environment are)";
      return nullptr;
    }
  } else if (p.has_hmac_credentials()) {
    parent = std::make_unique<HmacProvider>(
        p.aws_access_key_id, p.aws_secret_access_key, p.aws_session_token,
        "aws:profile:" + profile_name);
    parent_kind = "hmac_keys";
  } else {
    *err = "profile [" + profile_name +
           "] has no recognised credentials — expected some combination "
           "of aws_access_key_id/aws_secret_access_key, role_arn + "
           "source_profile, role_arn + credential_source, or "
           "credential_process";
    return nullptr;
  }

  // If role_arn is set, wrap the parent in STS AssumeRole.
  if (!p.role_arn.empty()) {
    StsAssumeRoleConfig sts;
    sts.parent = std::move(parent);
    sts.region =
        p.region.empty() ? (opts.region.empty() ? "us-east-1" : opts.region)
                          : p.region;
    sts.role_arn = p.role_arn;
    sts.role_session_name =
        p.role_session_name.empty() ? "xbcloud-backup" : p.role_session_name;
    sts.external_id = p.external_id;
    if (p.duration_seconds > 0) sts.duration_seconds = p.duration_seconds;
    return std::make_unique<StsAssumeRoleProvider>(std::move(sts));
  }

  // No role_arn — the parent IS the final provider.  Rewrap only if
  // we want to override the source_description with the profile
  // name; but keeping the internal label is fine for logs.
  (void)parent_kind;  // used only for future logging
  return parent;
}

}  // namespace

std::unique_ptr<CredentialProvider> resolve_profile(
    const std::string &profile_name, const ProfileResolverOptions &opts,
    std::string *err) {
  ProfileResolverOptions o = opts;
  if (o.credentials_path.empty()) o.credentials_path = default_credentials_path();
  if (o.config_path.empty()) o.config_path = default_config_path();
  std::set<std::string> visited;
  return resolve_recursive(profile_name, o, &visited, err);
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
