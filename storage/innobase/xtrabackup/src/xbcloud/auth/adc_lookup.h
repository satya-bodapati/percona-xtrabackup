/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

AdcLookup — a small helper that runs a resolver chain in order until
one of them yields a credential.  Modelled after Google's Application
Default Credentials lookup ("ADC") — env var → well-known keyfile →
IMDS probe → fail — and generalised so every cloud provider can use
the same shape.

Deliberately minimal.  The helper does not know anything about IMDS
protocols, JSON parsing, HTTP, or credential formats.  Each step in
the chain is a caller-supplied lambda that either produces a value or
signals "not my job, try the next one".  The provider assembles the
chain relevant to its own precedence rules.

Typical use in a provider:

    auto cred = AdcLookup<GcpAdcCredential>()
        .also(from_explicit_cli_option)
        .also(from_env_var, "GOOGLE_APPLICATION_CREDENTIALS")
        .also(from_default_gcloud_config_path)
        .also(from_gce_metadata_probe)
        .resolve(&err);
    if (!cred) { report(err); return nullptr; }

Rationale for the chain-of-lambdas shape rather than a fixed list of
step types: (a) providers differ in which steps they support and in
what order, (b) tests can inject arbitrary steps, and (c) the header
stays free of AWS / GCP / Azure specifics.

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

#ifndef XBCLOUD_AUTH_ADC_LOOKUP_H
#define XBCLOUD_AUTH_ADC_LOOKUP_H

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xbcloud {
namespace auth {

/**
  Outcome of one step in the lookup chain.  A step either yields a
  credential (Yielded), reports "I have nothing to say" and defers to
  the next step (Skip), or reports a fatal error that should abort
  the whole chain (Fatal — e.g. the user passed a keyfile path that
  points at a file we can't read; we shouldn't silently fall through
  to IMDS and hide their misconfiguration).
*/
enum class StepResult {
  Yielded,  // step populated its output; return this value
  Skip,     // step has nothing; try the next step
  Fatal,    // step failed in a way that must not be masked; abort
};

/**
  Templated on the concrete credential type each provider uses
  (GcpAdcCredential, AwsHmacCredentials, AzureServicePrincipal, …).
  Each step lambda gets an out-parameter for the value and for an
  error string (used for Fatal / for the final "we tried these steps
  and none yielded" message).
*/
template <typename CredentialT>
class AdcLookup {
 public:
  using StepFn = std::function<StepResult(CredentialT *out, std::string *err)>;

  /**
    Append a step to the chain.  Steps are attempted in insertion order.
    Provider assembles the chain in its own precedence order.
  */
  AdcLookup &also(StepFn fn, std::string label = "") {
    steps_.push_back({std::move(fn), std::move(label)});
    return *this;
  }

  /**
    Walk the chain.  Returns:
      * populated optional on Yielded — the first step that yielded
        wins; remaining steps are not called.
      * empty optional otherwise.  If any step returned Fatal, *err
        holds that step's error.  If every step returned Skip, *err
        contains a summary of the labels tried so the operator can
        see what we looked at.
  */
  std::optional<CredentialT> resolve(std::string *err = nullptr) const {
    std::vector<std::string> skipped;
    for (const auto &step : steps_) {
      CredentialT candidate{};
      std::string step_err;
      const StepResult r = step.fn(&candidate, &step_err);
      if (r == StepResult::Yielded) return candidate;
      if (r == StepResult::Fatal) {
        if (err != nullptr) *err = step_err;
        return std::nullopt;
      }
      // Skip.
      skipped.push_back(step.label.empty() ? "<unlabeled step>" : step.label);
    }
    if (err != nullptr) {
      std::string msg = "no credential source yielded; tried:";
      for (const auto &s : skipped) {
        msg += " ";
        msg += s;
      }
      *err = std::move(msg);
    }
    return std::nullopt;
  }

 private:
  struct Step {
    StepFn fn;
    std::string label;
  };
  std::vector<Step> steps_;
};

}  // namespace auth
}  // namespace xbcloud

#endif  // XBCLOUD_AUTH_ADC_LOOKUP_H
