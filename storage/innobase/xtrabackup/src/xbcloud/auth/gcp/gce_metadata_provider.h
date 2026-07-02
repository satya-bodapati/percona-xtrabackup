/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

GceMetadataProvider — CredentialProvider for GCP Compute Engine
instance metadata service.  When xbcloud runs on a GCE VM with a
service account attached, the local metadata service at
metadata.google.internal returns a short-lived OAuth2 access token
directly, with no client-side signing needed.

Refresh policy: delegates to TokenCache (proactive at expires-5min +
single-flight guard); invalidate() drops the cache for reactive
recovery.

Endpoint:
  GET http://metadata.google.internal/computeMetadata/v1/instance/
      service-accounts/default/token
  Header: Metadata-Flavor: Google

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

#ifndef XBCLOUD_AUTH_GCP_GCE_METADATA_PROVIDER_H
#define XBCLOUD_AUTH_GCP_GCE_METADATA_PROVIDER_H

#include <string>

#include "../credential_provider.h"
#include "../token_cache.h"

namespace xbcloud {
namespace auth {
namespace gcp {

class GceMetadataProvider : public CredentialProvider {
 public:
  GceMetadataProvider();

  WireMode wire_mode() const override { return WireMode::BEARER; }
  HmacCredentials get_hmac() override { return {}; }
  std::string get_bearer() override;
  void invalidate() override { cache_.invalidate(); }
  std::string source_description() const override {
    return "gcp:gce-metadata:default";
  }

  // Cheap probe: HEAD/GET the metadata endpoint with a 1s timeout.
  // Returns true if the metadata service answered — indicates we're
  // actually on GCE.  Used by xbcloud's credential-source detection
  // to decide whether to install this provider vs falling through.
  static bool probe_reachable();

 private:
  TokenCache cache_;
};

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_GCP_GCE_METADATA_PROVIDER_H
