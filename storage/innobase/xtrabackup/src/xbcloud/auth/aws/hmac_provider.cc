/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Trivial implementation — see hmac_provider.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "hmac_provider.h"

#include <utility>

namespace xbcloud {
namespace auth {
namespace aws {

HmacProvider::HmacProvider(std::string access_key, std::string secret_key,
                           std::string session_token, std::string source_label)
    : creds_{std::move(access_key), std::move(secret_key),
             std::move(session_token)},
      source_label_(std::move(source_label)) {}

}  // namespace aws
}  // namespace auth
}  // namespace xbcloud
