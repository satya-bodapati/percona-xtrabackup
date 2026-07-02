/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

InteropHmacProvider — CredentialProvider for GCS "interoperability"
HMAC keys (--google-access-key / --google-secret-key).  GCS ships an
S3-compatible XML API that accepts either AWS SigV4 HMAC signatures
(with a GCS-issued key pair) or OAuth2 Bearer tokens; this provider
covers the HMAC path.

Functionally identical to aws::HmacProvider — GCS interop signing uses
the same SigV4 canonicalisation AWS does, and xbcloud today
instantiates an S3_object_store pointed at storage.googleapis.com to
serve the GOOGLE storage type.  The only reason for a separate class
is so source_description() reports "gcp:..." rather than "aws:..." in
log lines, keeping operator-facing provenance honest.

The OAuth2 Bearer path lands in a follow-up commit as adc_provider —
that's the "gcloud auth" story from PXB-3592.

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

#ifndef XBCLOUD_AUTH_GCP_INTEROP_HMAC_PROVIDER_H
#define XBCLOUD_AUTH_GCP_INTEROP_HMAC_PROVIDER_H

#include <string>

#include "../aws/hmac_provider.h"

namespace xbcloud {
namespace auth {
namespace gcp {

// Reuse aws::HmacProvider's storage/signing shape verbatim; only the
// source description differs.
class InteropHmacProvider : public aws::HmacProvider {
 public:
  InteropHmacProvider(std::string access_key, std::string secret_key,
                      std::string session_token = {})
      : aws::HmacProvider(std::move(access_key), std::move(secret_key),
                          std::move(session_token),
                          "gcp:hmac-interop:cli-flags") {}
};

}  // namespace gcp
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_GCP_INTEROP_HMAC_PROVIDER_H
