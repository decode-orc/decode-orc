/*
 * File:        vbi_identity_attestation.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Identity attestation reconciliation implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_identity_attestation.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace orc {

std::optional<std::size_t> vbi_single_digit_neighbour(
    const VbiIdentityDigits& candidate,
    const std::vector<VbiIdentityDigits>& attested) {
  std::optional<std::size_t> found;
  for (std::size_t index = 0; index < attested.size(); ++index) {
    const VbiIdentityDigits& other = attested[index];
    if (other.size() != candidate.size()) {
      continue;
    }
    std::size_t differences = 0;
    for (std::size_t digit = 0; digit < candidate.size(); ++digit) {
      if (candidate[digit] != other[digit] && ++differences > 1) {
        break;
      }
    }
    if (differences != 1) {
      continue;
    }
    if (found.has_value()) {
      return std::nullopt;  // ambiguous: two attested identities fit equally
    }
    found = index;
  }
  return found;
}

namespace {

// Sightings under one name that make a never-attested identity a page rather
// than a misreading: damage lands on a different name each time, a consistent
// corruption on the same one.
constexpr uint64_t kRecurringAppearances = 2;

}  // namespace

std::vector<VbiIdentityVerdict> vbi_reconcile_identities(
    const std::vector<VbiIdentity>& identities,
    const std::function<double(std::size_t, std::size_t)>& agreement) {
  std::vector<VbiIdentityVerdict> verdicts(identities.size());
  // What a misreading may fold into: the attested identities, and the ones
  // kept below, which are decided most-seen first so they can be.
  std::vector<bool> anchor(identities.size(), false);
  std::vector<VbiIdentityDigits> attested_digits;
  std::vector<std::size_t> attested_index;
  std::vector<std::size_t> unattested;
  for (std::size_t i = 0; i < identities.size(); ++i) {
    if (identities[i].attested) {
      anchor[i] = true;
      attested_digits.push_back(identities[i].digits);
      attested_index.push_back(i);
    } else {
      unattested.push_back(i);
    }
  }
  std::stable_sort(unattested.begin(), unattested.end(),
                   [&](std::size_t a, std::size_t b) {
                     return identities[a].times_seen > identities[b].times_seen;
                   });

  for (const std::size_t u : unattested) {
    const VbiIdentity& identity = identities[u];
    // The anchor whose content this matches best, at any distance: a damaged
    // tape corrupts a page digit and a sub-code digit of one header as often
    // as either alone, and the content is what says whose they were.
    std::optional<std::size_t> match;
    double match_agreement = 0.0;
    bool ambiguous = false;
    for (std::size_t a = 0; a < identities.size(); ++a) {
      if (!anchor[a] || a == u) {
        continue;
      }
      const double agree = agreement(u, a);
      if (!(agree >= kVbiSameContent)) {
        continue;
      }
      const bool better =
          !match || agree > match_agreement ||
          (agree == match_agreement &&
           identities[a].times_seen > identities[*match].times_seen);
      if (better) {
        match = a;
        match_agreement = agree;
        ambiguous = false;
      } else if (agree == match_agreement &&
                 identities[a].times_seen == identities[*match].times_seen) {
        ambiguous = true;
      }
    }

    VbiIdentityVerdict& verdict = verdicts[u];
    const auto single =
        vbi_single_digit_neighbour(identity.digits, attested_digits);
    if (match.has_value() && !ambiguous) {
      verdict = {VbiIdentityVerdict::Action::kFold, *match};
    } else if (!match.has_value() &&
               identity.times_seen >= kRecurringAppearances &&
               !std::isnan(agreement(u, u))) {
      verdict = {VbiIdentityVerdict::Action::kKeep, 0};
      anchor[u] = true;
    } else if (single.has_value()) {
      verdict = {VbiIdentityVerdict::Action::kFold, attested_index[*single]};
    } else {
      verdict = {VbiIdentityVerdict::Action::kDrop, 0};
    }
  }
  return verdicts;
}

std::string VbiIdentityReconciliation::summary(const std::string& noun) const {
  if (withheld) {
    return fmt::format(
        "no {} was ever named as transmitted, so the catalogue is left exactly "
        "as recovered.",
        noun);
  }
  if (!acted()) {
    return {};
  }
  std::string out = fmt::format(
      "{} of {} {} identities were never named as transmitted (a Hamming 8/4 "
      "byte of the identity had to be corrected every time); ",
      identities_unattested, identities_seen, noun);
  std::vector<std::string> outcomes;
  if (identities_folded > 0) {
    outcomes.push_back(fmt::format(
        "{} of them ({} appearance(s)) were misreadings of an identity one "
        "digit away and were folded into it",
        identities_folded, appearances_folded));
  }
  if (identities_kept > 0) {
    outcomes.push_back(fmt::format(
        "{} ({} appearance(s)) recurred under the same name with content of "
        "their own and are kept",
        identities_kept, appearances_kept));
  }
  if (identities_dropped > 0) {
    outcomes.push_back(fmt::format(
        "{} ({} appearance(s)) were seen too seldom to tell and had no "
        "identity to fold into",
        identities_dropped, appearances_dropped));
  }
  for (std::size_t i = 0; i < outcomes.size(); ++i) {
    out += (i == 0                     ? ""
            : i + 1 == outcomes.size() ? ", and "
                                       : ", ") +
           outcomes[i];
  }
  out += '.';
  return out;
}

}  // namespace orc
