/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Azure Storage connection-string parser.

An Azure Storage connection string is a single semicolon-separated
key=value list documented at
https://docs.microsoft.com/azure/storage/common/storage-configure-connection-string

Common shapes:

  * Standard, key-based access:
      DefaultEndpointsProtocol=https;
      AccountName=<account>;
      AccountKey=<base64-key>;
      EndpointSuffix=core.windows.net

  * Azurite dev:
      DefaultEndpointsProtocol=http;
      AccountName=devstoreaccount1;
      AccountKey=Eby8v...;
      BlobEndpoint=http://127.0.0.1:10000/devstoreaccount1

  * Explicit BlobEndpoint (override for sovereign clouds):
      DefaultEndpointsProtocol=https;
      AccountName=<account>;
      AccountKey=<base64-key>;
      BlobEndpoint=https://<account>.blob.<sovereign-domain>/

  * SAS-only (no key):
      BlobEndpoint=https://<account>.blob.core.windows.net;
      SharedAccessSignature=?sv=...

Every SDK (azure-sdk-for-*, az CLI, azcopy, all languages) reads the
same format; consuming it in xbcloud closes the "why does xbcloud
want three separate flags when I already have the connection string
sitting in an env var" gap.

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

#ifndef XBCLOUD_AUTH_AZURE_CONNECTION_STRING_H
#define XBCLOUD_AUTH_AZURE_CONNECTION_STRING_H

#include <string>

namespace xbcloud {
namespace auth {
namespace azure {

// Fields recognised in an Azure Storage connection string.  Empty
// strings mean "not present in the connection string".
struct ConnectionString {
  std::string default_endpoints_protocol;  // "http" | "https"
  std::string account_name;
  std::string account_key;
  std::string endpoint_suffix;      // e.g. "core.windows.net"
  std::string blob_endpoint;        // explicit; wins over suffix-derived
  std::string shared_access_signature;  // SAS token, "?sv=..." — v1 unused
  std::string development_storage_proxy_uri;  // Azurite alt form
};

// Parse a connection string into fields.  Returns true on success;
// on failure fills *err.  Case-insensitive on keys per SDK
// convention; case-sensitive on values.
bool parse_connection_string(const std::string &input, ConnectionString *out,
                             std::string *err);

// Convenience: assemble the blob endpoint URL given a parsed
// connection string.  Uses BlobEndpoint if present; else constructs
// https|http://<account>.blob.<endpoint_suffix>/ using
// DefaultEndpointsProtocol.
std::string blob_endpoint_from(const ConnectionString &cs);

}  // namespace azure
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_AZURE_CONNECTION_STRING_H
