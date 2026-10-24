/*
 * File:        vbi_identity_attestation_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the identity-attestation neighbour test and its
 *              reported summary
 *
 * Covers: what counts as a single-digit misreading, when several explanations
 * make one unusable, the width guard, and whether the summary says anything.
 * Pure value logic; no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi-services/vbi_identity_attestation.h"

#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::VbiIdentityDigits;
using orc::VbiIdentityReconciliation;

TEST(VbiSingleDigitNeighbour, FindTheOneIdentityADigitAway) {
  const std::vector<VbiIdentityDigits> attested = {
      {0, 0, 0, 0, 0, 9},
      {0, 0, 0, 0, 2, 1},
  };
  // 0x7 in place of 0x0, which is the mis-correction Hamming 8/4 makes of an
  // alternating error run over codeword 0.
  const auto found =
      orc::vbi_single_digit_neighbour({0, 0, 0, 7, 0, 9}, attested);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 0u);
}

TEST(VbiSingleDigitNeighbour, RefuseWhenTwoIdentitiesFitEqually) {
  // A service carrying both 003 and 007 explains a misread 005 twice over, and
  // neither explanation is evidence of anything.
  const std::vector<VbiIdentityDigits> attested = {
      {0, 0, 3},
      {0, 0, 7},
  };
  EXPECT_FALSE(
      orc::vbi_single_digit_neighbour({0, 0, 5}, attested).has_value());
}

TEST(VbiSingleDigitNeighbour, RefuseWhenNothingIsClose) {
  const std::vector<VbiIdentityDigits> attested = {{0, 0, 9}};
  // Two digits apart is two independent bursts inside one header, which is
  // possible but no longer points anywhere in particular.
  EXPECT_FALSE(
      orc::vbi_single_digit_neighbour({7, 0, 7}, attested).has_value());
}

TEST(VbiSingleDigitNeighbour, RefuseAnIdenticalIdentity) {
  const std::vector<VbiIdentityDigits> attested = {{0, 0, 9}};
  EXPECT_FALSE(
      orc::vbi_single_digit_neighbour({0, 0, 9}, attested).has_value());
}

TEST(VbiSingleDigitNeighbour, IgnoreIdentitiesOfAnotherWidth) {
  // A short and a long NABTS address are different shapes, not near misses.
  const std::vector<VbiIdentityDigits> attested = {{0, 0, 9}, {0, 0, 9, 0}};
  const auto found = orc::vbi_single_digit_neighbour({0, 7, 9}, attested);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 0u);
}

TEST(VbiSingleDigitNeighbour, AcceptAnEmptyAttestedSet) {
  EXPECT_FALSE(orc::vbi_single_digit_neighbour({0, 0, 9}, {}).has_value());
}

TEST(VbiIdentityReconciliationApplies, NeedsOneAttestedIdentity) {
  EXPECT_FALSE(orc::vbi_identity_reconciliation_applies(0));
  EXPECT_TRUE(orc::vbi_identity_reconciliation_applies(1));
}

TEST(VbiIdentityReconciliationSummary, SaysNothingWhenNothingHappened) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 12;
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_TRUE(reconciliation.summary("page").empty());
}

TEST(VbiIdentityReconciliationSummary, SaysSoWhenTheRuleStoodAside) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 4;
  reconciliation.identities_unattested = 4;
  reconciliation.withheld = true;
  // Not acted() — nothing was removed — but a reader still has to be told the
  // catalogue was left as recovered rather than judged and passed.
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_NE(reconciliation.summary("record").find("left exactly as recovered"),
            std::string::npos);
}

TEST(VbiIdentityReconciliationSummary, ReportsBothOutcomes) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 600;
  reconciliation.identities_unattested = 480;
  reconciliation.identities_folded = 200;
  reconciliation.identities_dropped = 280;
  reconciliation.appearances_folded = 900;
  reconciliation.appearances_dropped = 310;

  const std::string summary = reconciliation.summary("record");
  EXPECT_NE(summary.find("480 of 600 record identities"), std::string::npos);
  EXPECT_NE(summary.find("200"), std::string::npos);
  EXPECT_NE(summary.find("280"), std::string::npos);
}

TEST(VbiIdentityReconciliationSummary, ReportsKeptIdentities) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 70;
  reconciliation.identities_unattested = 30;
  reconciliation.identities_kept = 4;
  reconciliation.appearances_kept = 251;

  EXPECT_TRUE(reconciliation.acted());
  EXPECT_NE(reconciliation.summary("record").find("4 (251 appearance(s)) "
                                                  "recurred"),
            std::string::npos);
}

// Records removed as a page's packets misaddressed to another channel were
// named as transmitted, so they are not among the identities that never were,
// and are said apart.
TEST(VbiIdentityReconciliationSummary, ReportsMisaddressedRecordsApart) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 180;
  reconciliation.identities_unattested = 19;
  reconciliation.identities_folded = 7;
  reconciliation.appearances_folded = 7;
  reconciliation.identities_misaddressed = 2;
  reconciliation.appearances_misaddressed = 2;

  EXPECT_TRUE(reconciliation.acted());
  const std::string summary = reconciliation.summary("record");
  EXPECT_NE(summary.find("19 of 180 record identities"), std::string::npos);
  EXPECT_NE(summary.find("2 more (2 appearance(s)), named on a data channel "
                         "the service never used"),
            std::string::npos);
}

// Content agreement between identities, indexed as the identities are.
std::function<double(std::size_t, std::size_t)> agreement_table(
    std::vector<std::vector<double>> table) {
  return [table](std::size_t a, std::size_t b) { return table[a][b]; };
}

constexpr double kNone = std::numeric_limits<double>::quiet_NaN();

using Action = orc::VbiIdentityVerdict::Action;

TEST(VbiReconcileIdentities, FoldTheMisreadingIntoTheNeighbourItMatches) {
  const std::vector<orc::VbiIdentity> identities = {
      {{0, 0, 3}, true, 20}, {{0, 0, 7}, true, 20}, {{0, 0, 5}, false, 2}};
  const auto verdicts = orc::vbi_reconcile_identities(
      identities,
      agreement_table({{1.0, 0.1, 0.1}, {0.1, 1.0, 0.9}, {0.1, 0.9, 1.0}}));
  EXPECT_EQ(verdicts[2].action, Action::kFold);
  EXPECT_EQ(verdicts[2].into, 1u);
}

TEST(VbiReconcileIdentities, KeepARecurringIdentityOfItsOwn) {
  const std::vector<orc::VbiIdentity> identities = {{{0, 5, 0}, true, 20},
                                                    {{0, 6, 0}, false, 70}};
  const auto verdicts = orc::vbi_reconcile_identities(
      identities, agreement_table({{1.0, 0.2}, {0.2, 1.0}}));
  EXPECT_EQ(verdicts[0].action, Action::kKeep);
  EXPECT_EQ(verdicts[1].action, Action::kKeep);
}

// A header can lose a page digit and a sub-code digit at once; the content
// still says whose page it was.
TEST(VbiReconcileIdentities, FoldAMisreadingSeveralDigitsAway) {
  const std::vector<orc::VbiIdentity> identities = {
      {{1, 2, 8, 0, 0}, true, 150}, {{1, 12, 8, 0, 1}, false, 3}};
  const auto verdicts = orc::vbi_reconcile_identities(
      identities, agreement_table({{1.0, 0.8}, {0.8, 1.0}}));
  EXPECT_EQ(verdicts[1].action, Action::kFold);
  EXPECT_EQ(verdicts[1].into, 0u);
}

// A misreading of a kept page folds into it like one of an attested page.
TEST(VbiReconcileIdentities, FoldIntoAKeptIdentity) {
  const std::vector<orc::VbiIdentity> identities = {
      {{0, 5, 0}, true, 20}, {{0, 6, 0}, false, 70}, {{0, 6, 8}, false, 1}};
  const auto verdicts = orc::vbi_reconcile_identities(
      identities,
      agreement_table({{1.0, 0.2, 0.2}, {0.2, 1.0, 0.95}, {0.2, 0.95, 1.0}}));
  EXPECT_EQ(verdicts[1].action, Action::kKeep);
  EXPECT_EQ(verdicts[2].action, Action::kFold);
  EXPECT_EQ(verdicts[2].into, 1u);
}

// Recurring is not enough without content to tell it from its neighbour.
TEST(VbiReconcileIdentities, KeepNothingThatCannotBeCompared) {
  const std::vector<orc::VbiIdentity> identities = {{{0, 5, 0}, true, 20},
                                                    {{0, 6, 0}, false, 5}};
  const auto verdicts = orc::vbi_reconcile_identities(
      identities, agreement_table({{kNone, kNone}, {kNone, kNone}}));
  EXPECT_EQ(verdicts[1].action, Action::kFold);
  EXPECT_EQ(verdicts[1].into, 0u);
}

TEST(VbiReconcileIdentities, DropASingleSightingWithNoNeighbour) {
  const std::vector<orc::VbiIdentity> identities = {{{0, 0, 9}, true, 20},
                                                    {{7, 0, 7}, false, 1}};
  const auto verdicts = orc::vbi_reconcile_identities(
      identities, agreement_table({{1.0, 0.1}, {0.1, 1.0}}));
  EXPECT_EQ(verdicts[1].action, Action::kDrop);
}

}  // namespace
}  // namespace orc_unit_test
