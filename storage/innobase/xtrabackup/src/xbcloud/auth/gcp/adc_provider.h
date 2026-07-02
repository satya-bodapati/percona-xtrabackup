/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

AdcProvider — CredentialProvider for GCP OAuth2 Bearer authentication
via Application Default Credentials.  Consumed by xbcloud when the
operator passes --google-service-account-file or sets
GOOGLE_APPLICATION_CREDENTIALS.  wire_mode() returns BEARER; the S3
XML API (which is what xbcloud's GOOGLE storage path speaks) accepts
Authorization: Bearer <token> as an alternative to SigV4 signing.

Refresh policy:
  * Delegates to TokenCache: proactive refresh at expires_at - 5min
    with single-flight guarding.
  * On 401 the request layer calls invalidate(), which drops the
    cache and forces a fresh mint on the next get_bearer().

The mint step (JWT-bearer or refresh-token grant) is done by
oauth2_client.cc, wrapped in retry_with_backoff so brief OAuth2
endpoint hiccups don't propagate as auth failures.

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

#ifndef XBCLOUD_AUTH_GCP_ADC_PROVIDER_H
#define XBCLOUD_AUTH_GCP_ADC_PROVIDER_H

#include <memory>
#include <string>

#include "../credential_provider.h"
#include "../token_cache.h"
#include "adc_credential.h"

namespace xbcloud {
namespace auth {
namespace gcp {

class AdcProvider : public CredentialProvider {
 public:
  // Takes ownership of the parsed AdcCredential.  Optional scope
  // defaults to devstorage.read_write, sufficient for xbcloud
  // put/get/delete against GCS.
  explicit AdcProvider(AdcCredential adc,
                       std::string scope =
                           "https://www.googleapis.com/auth/devstorage.read_write",
                       std::string source_path = "");

  WireMode wire_mode() const override { return WireMode::BEARER; }

  HmacCredentials get_hmac() override { return {}; }

  std::string get_bearer() override;

  void invalidate() override { cache_.invalidate(); }

  std::string source_description() const override { return source_label_; }

 private:
  AdcCredential adc_;
  std::string scope_;
  TokenCache cache_;
  std::string source_label_;
};

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_GCP_ADC_PROVIDER_H
