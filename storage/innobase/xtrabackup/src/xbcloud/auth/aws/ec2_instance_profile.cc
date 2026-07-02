/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

See ec2_instance_profile.h for the contract.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "ec2_instance_profile.h"

#include <utility>

namespace xbcloud {
namespace auth {
namespace aws {

Ec2InstanceProfileProvider::Ec2InstanceProfileProvider(
    std::shared_ptr<S3_ec2_instance> instance)
    : instance_(std::move(instance)) {
  // If the caller already fetched (that is the common path today —
  // xbcloud.cc's main() probes IMDS up front to detect an instance
  // profile), the metadata is already resident.  Mark valid so the
  // first get_hmac() serves from cache rather than re-fetching.
  if (instance_ && instance_->get_is_ec2_instance_with_profile()) {
    have_creds_ = true;
  }
}

HmacCredentials Ec2InstanceProfileProvider::get_hmac() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!have_creds_) {
    // fetch_metadata returns false if we're not on an EC2 instance
    // or IMDS is unreachable.  In either case we hand back empty
    // credentials — sign_request will produce a request that the
    // server rejects, which is the correct behaviour (the operator
    // then sees an authentication failure and knows to fix their
    // setup).  We intentionally don't die here so a transient IMDS
    // outage can be retried by the caller via invalidate() + retry.
    if (instance_ && instance_->fetch_metadata()) {
      have_creds_ = true;
    }
  }
  if (!instance_ || !have_creds_) return {};
  return HmacCredentials{instance_->get_access_key(),
                         instance_->get_secret_key(),
                         instance_->get_session_token()};
}

void Ec2InstanceProfileProvider::invalidate() {
  std::lock_guard<std::mutex> lk(mu_);
  have_creds_ = false;
}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
