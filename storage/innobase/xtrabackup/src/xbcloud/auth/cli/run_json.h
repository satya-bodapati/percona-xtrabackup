/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Shared helper for the four CLI-backed CredentialProvider implementations
(auth/cli/aws_cli_provider, gcp_cli_provider, azure_cli_provider,
swift_cli_provider).

Runs an external command via /bin/sh -c and captures stdout into a
string.  The command is expected to produce JSON on stdout (each
provider parses the shape it expects — this helper is JSON-agnostic).
Wraps the invocation in the shared retry_with_backoff policy so a
transient CLI hiccup (network blip during `aws configure
export-credentials`, gcloud metadata timeout, etc.) doesn't propagate
as an auth failure to the operator.

This helper deliberately does not sanitise the command string —
callers are trusted to construct it from CLI options that xbcloud's
own operator supplied.  The command runs with the operator's uid, in
the operator's PATH, exactly like `aws ...` would if the operator
typed it themselves.

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

#ifndef XBCLOUD_AUTH_CLI_RUN_JSON_H
#define XBCLOUD_AUTH_CLI_RUN_JSON_H

#include <string>

namespace xbcloud {
namespace auth {
namespace cli {

// Run `command` under /bin/sh -c, capture stdout into *out.  Returns
// true on success (child exited 0 and stdout was captured); on failure
// fills *err with a human-readable message.
//
// Retries with exponential backoff via the shared retry_with_backoff
// helper, so a transient failure isn't fatal.  Non-zero child exit is
// treated as retryable (up to the policy's max attempts) because the
// vendor CLIs generally return non-zero on transient network problems.
bool run_json_command(const std::string &command, std::string *out,
                      std::string *err);

}  // namespace cli
}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_CLI_RUN_JSON_H
