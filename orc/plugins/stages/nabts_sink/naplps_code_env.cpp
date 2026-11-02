/*
 * File:        naplps_code_env.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NAPLPS code environment implementation (X3.110 §4.3, §6.1, §6.2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_code_env.h"

namespace orc {

namespace {

// X3.110 §4.3.2: an escape sequence is ESC I...I F, with intermediates in 2/0
// to 2/15 and one final character in 3/0 to 7/14.
constexpr bool is_intermediate(uint8_t byte) {
  return byte >= 0x20 && byte <= 0x2F;
}
constexpr bool is_final(uint8_t byte) { return byte >= 0x30 && byte <= 0x7E; }

// Table 1's intermediate characters. The 94-character sets take 2/8 to 2/11 for
// G0 to G3; the 96-character sets take 2/9 to 2/11 for G1 to G3 — there is no
// G0 form, because §4.3.1.2 makes G0 a 94-position set — and also 2/13 to 2/15
// for the same three slots. That duplication is the standard's own: "There are
// two I characters that will result in the redesignation of each of the three
// G-sets ... This dual coding may be removed from the standard in a future
// revision."
constexpr uint8_t kIntermediateG0 = 0x28;     // 2/8
constexpr uint8_t kIntermediateG1 = 0x29;     // 2/9
constexpr uint8_t kIntermediateG2 = 0x2A;     // 2/10
constexpr uint8_t kIntermediateG3 = 0x2B;     // 2/11
constexpr uint8_t kIntermediateG1Alt = 0x2D;  // 2/13
constexpr uint8_t kIntermediateG2Alt = 0x2E;  // 2/14
constexpr uint8_t kIntermediateG3Alt = 0x2F;  // 2/15

// Table 1's final characters.
constexpr uint8_t kFinalPrimary = 0x42;        // 4/2
constexpr uint8_t kFinalSupplementary = 0x7C;  // 7/12
constexpr uint8_t kFinalPdi = 0x57;            // 5/7
constexpr uint8_t kFinalMosaic = 0x7D;         // 7/13
constexpr uint8_t kFinalMacro = 0x7A;          // 7/10
constexpr uint8_t kFinalDrcs = 0x7B;           // 7/11

// Table 1's control-set intermediates. §4.3.2 forbids redesignating C0 in this
// context and has its sequences ignored; a C1 redesignation names a set outside
// this standard, which is a null set.
constexpr uint8_t kIntermediateC0 = 0x21;  // 2/1
constexpr uint8_t kIntermediateC1 = 0x22;  // 2/2

// Table 2's locking shifts, in the forms the 7-bit environment has.
constexpr uint8_t kFinalLs2 = 0x6E;  // ESC 6/14
constexpr uint8_t kFinalLs3 = 0x6F;  // ESC 6/15

/// The slot an intermediate names, or no slot.
bool slot_of_intermediate(uint8_t intermediate, NaplpsGSlot& slot,
                          bool& allows_96_sets) {
  switch (intermediate) {
    case kIntermediateG0:
      slot = NaplpsGSlot::kG0;
      // §4.3.1.2: G0 is a 94-position set, so no 96-position set designates
      // into it.
      allows_96_sets = false;
      return true;
    case kIntermediateG1:
    case kIntermediateG1Alt:
      slot = NaplpsGSlot::kG1;
      allows_96_sets = true;
      return true;
    case kIntermediateG2:
    case kIntermediateG2Alt:
      slot = NaplpsGSlot::kG2;
      allows_96_sets = true;
      return true;
    case kIntermediateG3:
    case kIntermediateG3Alt:
      slot = NaplpsGSlot::kG3;
      allows_96_sets = true;
      return true;
    default:
      return false;
  }
}

/// The set a final character names, or kNull for one Table 1 does not list.
NaplpsGSet set_of_final(uint8_t final_byte, bool& needs_96_slot) {
  needs_96_slot = false;
  switch (final_byte) {
    case kFinalPrimary:
      return NaplpsGSet::kPrimary;
    case kFinalSupplementary:
      return NaplpsGSet::kSupplementary;
    case kFinalPdi:
      needs_96_slot = true;
      return NaplpsGSet::kPdi;
    case kFinalMosaic:
      needs_96_slot = true;
      return NaplpsGSet::kMosaic;
    case kFinalMacro:
      needs_96_slot = true;
      return NaplpsGSet::kMacro;
    case kFinalDrcs:
      needs_96_slot = true;
      return NaplpsGSet::kDrcs;
    default:
      return NaplpsGSet::kNull;
  }
}

}  // namespace

void NaplpsCodeEnvironment::reset() {
  // §4.3.1.3's default designations.
  designated_[0] = NaplpsGSet::kPrimary;
  designated_[1] = NaplpsGSet::kPdi;
  designated_[2] = NaplpsGSet::kSupplementary;
  designated_[3] = NaplpsGSet::kMosaic;
  locked_ = NaplpsGSlot::kG0;
  invoked_ = NaplpsGSlot::kG0;
  single_shift_ = false;
}

void NaplpsCodeEnvironment::consume_character() {
  if (single_shift_) {
    // §4.3.2: a single shift is non-locking, so the set the last locking shift
    // invoked comes back for the character after it.
    invoked_ = locked_;
    single_shift_ = false;
  }
}

NaplpsEscape naplps_parse_escape(const uint8_t* bytes, size_t length) {
  NaplpsEscape out;
  if (bytes == nullptr || length == 0) {
    out.kind = NaplpsEscapeKind::kTruncated;
    out.length = 1;  // the ESC itself
    return out;
  }

  // Walk the intermediates. §4.3.2 permits "zero or more", and every sequence
  // this standard defines has at most one, but the syntax is the syntax: a
  // sequence with two intermediates is well-formed and unsupported rather than
  // malformed.
  size_t index = 0;
  size_t intermediates = 0;
  uint8_t first_intermediate = 0;
  while (index < length && is_intermediate(bytes[index])) {
    if (intermediates == 0) {
      first_intermediate = bytes[index];
    }
    ++intermediates;
    ++index;
  }

  if (index >= length) {
    out.kind = NaplpsEscapeKind::kTruncated;
    out.length = 1 + index;
    return out;
  }

  const uint8_t final_byte = bytes[index];
  if (!is_final(final_byte)) {
    // §4.3.2: the partial sequence is terminated and ignored, and this byte is
    // executed. So it is deliberately left unconsumed.
    out.kind = NaplpsEscapeKind::kMalformed;
    out.length = 1 + index;
    return out;
  }

  const size_t consumed = 1 + index + 1;  // ESC + intermediates + final
  out.length = consumed;

  // No intermediate: a locking shift (Table 2) or a C1 control (Figure 65).
  if (intermediates == 0) {
    if (final_byte == kFinalLs2) {
      out.kind = NaplpsEscapeKind::kLockingShift;
      out.slot = NaplpsGSlot::kG2;
      return out;
    }
    if (final_byte == kFinalLs3) {
      out.kind = NaplpsEscapeKind::kLockingShift;
      out.slot = NaplpsGSlot::kG3;
      return out;
    }
    if (naplps_is_c1_final(final_byte)) {
      out.kind = NaplpsEscapeKind::kControl;
      out.c1 = static_cast<NaplpsC1>(final_byte);
      return out;
    }
    // Includes the right-hand locking shifts of Table 2 (ESC 7/12, 7/13, 7/14),
    // which exist only in the 8-bit environment CEA-516 §6.1 does not use.
    out.kind = NaplpsEscapeKind::kUnsupported;
    return out;
  }

  if (intermediates > 1) {
    out.kind = NaplpsEscapeKind::kUnsupported;
    return out;
  }

  // A control-set designation. §4.3.2: C0 redesignation "is not permitted in
  // the context of this standard and such redesignating escape sequences shall
  // be ignored"; a C1 designation naming a set outside this standard is a null
  // set. Either way there is nothing to do but skip it.
  if (first_intermediate == kIntermediateC0 ||
      first_intermediate == kIntermediateC1) {
    out.kind = NaplpsEscapeKind::kUnsupported;
    return out;
  }

  NaplpsGSlot slot = NaplpsGSlot::kG0;
  bool allows_96_sets = false;
  if (!slot_of_intermediate(first_intermediate, slot, allows_96_sets)) {
    out.kind = NaplpsEscapeKind::kUnsupported;
    return out;
  }

  bool needs_96_slot = false;
  const NaplpsGSet set = set_of_final(final_byte, needs_96_slot);
  if (set == NaplpsGSet::kNull || (needs_96_slot && !allows_96_sets)) {
    // §4.3.2: "All other designation sequences shall designate either a null G-
    // or a null C1-set." Reported as a designation of the null set rather than
    // as unsupported, because it *is* a designation: it replaces whatever the
    // slot held, and the slot then executes as null operations.
    out.kind = NaplpsEscapeKind::kDesignation;
    out.slot = slot;
    out.set = NaplpsGSet::kNull;
    return out;
  }

  out.kind = NaplpsEscapeKind::kDesignation;
  out.slot = slot;
  out.set = set;
  return out;
}

}  // namespace orc
