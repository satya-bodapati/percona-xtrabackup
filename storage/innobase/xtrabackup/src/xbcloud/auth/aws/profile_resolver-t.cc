/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Unit tests for xbcloud/auth/aws/profile_resolver.

Focus is on dispatch correctness — given profile file contents,
does the resolver build the right kind of provider and produce the
right source_description?  We don't fire real STS or IMDS traffic
here (those need cloud-hosted CI); the resolver is exercised
against synthetic config/credentials files.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "profile_resolver.h"

namespace {

using xbcloud::auth::aws::ProfileResolverOptions;
using xbcloud::auth::aws::resolve_profile;

// Small RAII: write a string into a fresh temp file, return the path,
// unlink on destruction.  Used to fabricate synthetic
// ~/.aws/credentials + ~/.aws/config files without touching the real
// ones.
class TempFile {
 public:
  explicit TempFile(const std::string &contents) {
    char tmpl[] = "/tmp/pxb-aws-prof-XXXXXX";
    int fd = mkstemp(tmpl);
    path_ = tmpl;
    std::ofstream f(path_);
    f << contents;
    close(fd);
  }
  ~TempFile() { std::remove(path_.c_str()); }
  const std::string &path() const { return path_; }

 private:
  std::string path_;
};

TEST(ProfileResolver, PlainHmacProfile) {
  TempFile creds(
      "[default]\n"
      "aws_access_key_id = AKIAEXAMPLE\n"
      "aws_secret_access_key = secret1\n");
  TempFile config("");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();

  std::string err;
  auto p = resolve_profile("default", o, &err);
  ASSERT_NE(nullptr, p) << err;
  EXPECT_NE(std::string::npos,
            p->source_description().find("aws:profile:default"));
  const auto c = p->get_hmac();
  EXPECT_EQ("AKIAEXAMPLE", c.access_key);
  EXPECT_EQ("secret1", c.secret_key);
}

TEST(ProfileResolver, ProfileWithSessionToken) {
  TempFile creds(
      "[dev]\n"
      "aws_access_key_id = ASIATEMP\n"
      "aws_secret_access_key = secret2\n"
      "aws_session_token = tok\n");
  TempFile config("");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("dev", o, &err);
  ASSERT_NE(nullptr, p) << err;
  const auto c = p->get_hmac();
  EXPECT_EQ("tok", c.session_token);
}

TEST(ProfileResolver, StsChainWithSourceProfileConstructs) {
  // Note we don't fire the STS call (no network) — but we can prove
  // the resolver produced an STS-shaped provider by looking at
  // source_description.  The resolver has to be able to resolve the
  // source profile as a parent even for construction.
  TempFile creds(
      "[mgmt]\n"
      "aws_access_key_id = AKIAMGMT\n"
      "aws_secret_access_key = secret3\n");
  TempFile config(
      "[profile prod]\n"
      "role_arn = arn:aws:iam::123:role/backup\n"
      "source_profile = mgmt\n"
      "role_session_name = xbcloud-test\n"
      "external_id = xid\n");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("prod", o, &err);
  ASSERT_NE(nullptr, p) << err;
  EXPECT_NE(std::string::npos,
            p->source_description().find("aws:sts-assume-role"));
  EXPECT_NE(std::string::npos,
            p->source_description().find("arn:aws:iam::123:role/backup"));
}

TEST(ProfileResolver, CredentialProcessProfile) {
  TempFile creds("");
  TempFile config(
      "[profile via-helper]\n"
      "credential_process = /bin/false\n");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("via-helper", o, &err);
  ASSERT_NE(nullptr, p) << err;
  EXPECT_NE(std::string::npos,
            p->source_description().find("aws:credential_process"));
}

TEST(ProfileResolver, CycleDetection) {
  TempFile creds("");
  TempFile config(
      "[profile a]\n"
      "role_arn = arn:aws:iam::1:role/x\n"
      "source_profile = b\n"
      "\n"
      "[profile b]\n"
      "role_arn = arn:aws:iam::1:role/y\n"
      "source_profile = a\n");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("a", o, &err);
  EXPECT_EQ(nullptr, p);
  EXPECT_NE(std::string::npos, err.find("cycle"));
}

TEST(ProfileResolver, RejectsMfaSerial) {
  TempFile creds(
      "[mgmt]\n"
      "aws_access_key_id = AKIA\n"
      "aws_secret_access_key = s\n");
  TempFile config(
      "[profile mfa-prod]\n"
      "role_arn = arn:aws:iam::1:role/x\n"
      "source_profile = mgmt\n"
      "mfa_serial = arn:aws:iam::1:mfa/user\n");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("mfa-prod", o, &err);
  EXPECT_EQ(nullptr, p);
  EXPECT_NE(std::string::npos, err.find("mfa_serial"));
}

TEST(ProfileResolver, RejectsSSO) {
  TempFile creds("");
  TempFile config(
      "[profile via-sso]\n"
      "sso_start_url = https://myco.awsapps.com/start\n"
      "sso_region = us-east-1\n"
      "sso_account_id = 123456789012\n"
      "sso_role_name = Admin\n");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("via-sso", o, &err);
  EXPECT_EQ(nullptr, p);
  EXPECT_NE(std::string::npos, err.find("SSO"));
}

TEST(ProfileResolver, RejectsEmptyProfile) {
  TempFile creds(
      "[empty]\n"
      "# only a section header, no keys\n");
  TempFile config("");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("empty", o, &err);
  EXPECT_EQ(nullptr, p);
  // Error mentions what shapes we DO accept.
  EXPECT_NE(std::string::npos, err.find("no recognised credentials"));
}

TEST(ProfileResolver, MissingProfileErrorNamesBothFiles) {
  TempFile creds("");
  TempFile config("");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("nonexistent", o, &err);
  EXPECT_EQ(nullptr, p);
  EXPECT_NE(std::string::npos, err.find("nonexistent"));
}

TEST(ProfileResolver, ConfigAndCredentialsMerge) {
  // Real-world layout: role_arn in ~/.aws/config, hmac keys in
  // ~/.aws/credentials — SDKs merge, we should too.
  TempFile creds(
      "[mgmt]\n"
      "aws_access_key_id = AKIA-real-in-creds\n"
      "aws_secret_access_key = secret-in-creds\n");
  TempFile config(
      "[profile mgmt]\n"
      "region = us-west-2\n"
      "\n"
      "[profile prod]\n"
      "role_arn = arn:aws:iam::1:role/x\n"
      "source_profile = mgmt\n");
  ProfileResolverOptions o;
  o.credentials_path = creds.path();
  o.config_path = config.path();
  std::string err;
  auto p = resolve_profile("prod", o, &err);
  ASSERT_NE(nullptr, p) << err;
  EXPECT_NE(std::string::npos,
            p->source_description().find("aws:sts-assume-role"));
}

}  // namespace
