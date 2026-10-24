/*
 * File:        naplps_code_env.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The NAPLPS 7-bit code environment: G-set designation and
 *              invocation, and the C0/C1 control repertoires
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NAPLPS_CODE_ENV_H
#define ORC_NAPLPS_CODE_ENV_H

#include <cstddef>
#include <cstdint>

namespace orc {

/**
 * @file
 * @brief Which repertoire a byte means, before anything is done with it
 *
 * NAPLPS is an ISO 2022 code environment (ANSI X3.110-1983 §4.3), so a byte in
 * columns 2 to 7 means whatever set is currently invoked into the in-use table
 * — and that set is chosen by escape sequences and shift characters that arrive
 * in the same stream. Nothing above this can read a byte until this has said
 * what it is.
 *
 * CEA-516 §6.1 requires the 7-bit environment, so only that is implemented: the
 * C1 set is reached by the two-character escape form of X3.110 Figure 65 with
 * its A = 4 / B = 5 convention, and the right-hand locking shifts of Table 2
 * (LS1R, LS2R, LS3R), which exist only in the 8-bit environment, are absent.
 */

/// Sets that may be designated into a G-set slot (X3.110 Table 1).
enum class NaplpsGSet : uint8_t {
  /// The 94 graphic characters of §7.1, in ASCII positions.
  kPrimary,
  /// The 94 accents and symbols of §7.2.
  kSupplementary,
  /// The 96-position picture description instruction set of §5.3.
  kPdi,
  /// The 96-position six-element block graphics of §5.4.
  kMosaic,
  /// The 96 macro names of §5.5.
  kMacro,
  /// The 96 dynamically redefinable characters of §5.6.
  kDrcs,
  /// §4.3.2: "All other designation sequences shall designate either a null G-
  /// or a null C1-set. ... A null set is a set in which all code positions are
  /// executed as null operations."
  kNull,
};

/// The four G-set slots (X3.110 §4.3.1.3).
enum class NaplpsGSlot : uint8_t { kG0 = 0, kG1 = 1, kG2 = 2, kG3 = 3 };

// C0 control characters this needs to name (X3.110 §6.1). Held as constants
// rather than an enum because the parser compares raw bytes.
constexpr uint8_t kNaplpsNul = 0x00;  // §6.1.6.1, no presentation effect
constexpr uint8_t kNaplpsBel = 0x07;  // 0/7 §6.1.6.2
constexpr uint8_t kNaplpsApb = 0x08;  // 0/8  backspace  §6.1.2.1
constexpr uint8_t kNaplpsApf = 0x09;  // 0/9  horizontal tab §6.1.2.2
constexpr uint8_t kNaplpsApd = 0x0A;  // 0/10 line feed  §6.1.2.3
constexpr uint8_t kNaplpsApu = 0x0B;  // 0/11 vertical tab §6.1.2.5
constexpr uint8_t kNaplpsCs = 0x0C;   // 0/12 clear screen §6.1.2.6
constexpr uint8_t kNaplpsApr = 0x0D;  // 0/13 carriage return §6.1.2.7
constexpr uint8_t kNaplpsSo = 0x0E;   // 0/14 §6.1.3.1
constexpr uint8_t kNaplpsSi = 0x0F;   // 0/15 §6.1.3.2
constexpr uint8_t kNaplpsSs2 = 0x19;  // 1/9  §6.1.3.3
constexpr uint8_t kNaplpsSdc = 0x1A;  // 1/10 §6.1.6.4, a null operation
constexpr uint8_t kNaplpsEsc = 0x1B;  // 1/11 §6.1.3.5
constexpr uint8_t kNaplpsAps = 0x1C;  // 1/12 §6.1.2.4
constexpr uint8_t kNaplpsSs3 = 0x1D;  // 1/13 §6.1.3.4
constexpr uint8_t kNaplpsAph = 0x1E;  // 1/14 §6.1.2.8
constexpr uint8_t kNaplpsNsr = 0x1F;  // 1/15 §6.1.6.5
constexpr uint8_t kNaplpsCan = 0x18;  // 1/8  §6.1.6.3
// 7/15, DELETE in a 94-position set (§4.4): executed as a null operation.
constexpr uint8_t kNaplpsDelete = 0x7F;

/**
 * @brief Whether |byte| is a control with no effect on the presentation layer
 *
 * X3.110 §6.1.4, §6.1.5 and §6.1.6.1: the transmission control characters
 * (0/1-0/6, 1/0, 1/5-1/7), the device control characters (1/1-1/4) and NUL
 * (0/0) "have no effect on the presentation layer ... They may be embedded
 * within any presentation layer sequence without affecting that sequence."
 *
 * That last clause is why this exists as a test of its own rather than as a
 * default case: one of these inside an escape sequence or an operand run
 * leaves it whole (§5.3.1 has one *not* terminate an open PDI), so the
 * interpreter drops them as it reads a record, before anything is executed,
 * as both receivers of the day drop them at fetch.
 */
constexpr bool naplps_is_transparent_control(uint8_t byte) {
  // NUL, SOH to ACK; DLE, DC1 to DC4, NAK to ETB.
  return byte <= 0x06 || (byte >= 0x10 && byte <= 0x17);
}

/**
 * @brief The C1 control set of X3.110 Figure 65
 *
 * Reached in the 7-bit environment as ESC 4/x for the figure's column A and
 * ESC 5/x for its column B, which is what its "Definition of Columns A and B"
 * note means by A = 4 and B = 5.
 */
enum class NaplpsC1 : uint8_t {
  kDefMacro = 0x40,      // 4/0  §6.2.2.1
  kDefpMacro = 0x41,     // 4/1  §6.2.2.2, defines and executes at once
  kDeftMacro = 0x42,     // 4/2  §6.2.2.3, a transmit macro
  kDefDrcs = 0x43,       // 4/3  §6.2.3
  kDefTexture = 0x44,    // 4/4  §6.2.4
  kEnd = 0x45,           // 4/5  §6.2.5
  kRepeat = 0x46,        // 4/6  §6.2.7.2
  kRepeatToEol = 0x47,   // 4/7  §6.2.7.3
  kReverseVideo = 0x48,  // 4/8  §6.2.7.4
  kNormalVideo = 0x49,   // 4/9  §6.2.7.5
  kSmallText = 0x4A,     // 4/10 §6.2.7.6
  kMediumText = 0x4B,    // 4/11 §6.2.7.7
  kNormalText = 0x4C,    // 4/12 §6.2.7.8
  kDoubleHeight = 0x4D,  // 4/13 §6.2.7.9
  kBlinkStart = 0x4E,    // 4/14 §6.2.8.1
  kDoubleSize = 0x4F,    // 4/15 §6.2.7.10

  // PROTECT and UNPROTECT (§6.2.6) are teletext-optional by Table D1.
  kProtect = 0x50,         // 5/0  §6.2.6
  kEdc1 = 0x51,            // 5/1  §6.2.8.3
  kEdc2 = 0x52,            // 5/2
  kEdc3 = 0x53,            // 5/3
  kEdc4 = 0x54,            // 5/4
  kWordWrapOn = 0x55,      // 5/5  §6.2.7.11
  kWordWrapOff = 0x56,     // 5/6  §6.2.7.12
  kScrollOn = 0x57,        // 5/7  §6.2.7.13
  kScrollOff = 0x58,       // 5/8  §6.2.7.14
  kUnderlineStart = 0x59,  // 5/9  §6.2.7.15
  kUnderlineStop = 0x5A,   // 5/10 §6.2.7.16
  kFlashCursor = 0x5B,     // 5/11 §6.2.7.17
  kSteadyCursor = 0x5C,    // 5/12 §6.2.7.18
  kCursorOff = 0x5D,       // 5/13 §6.2.7.19
  kBlinkStop = 0x5E,       // 5/14 §6.2.8.2
  kUnprotect = 0x5F,       // 5/15 §6.2.6
};

/// Whether |final_byte| of an ESC sequence is a C1 control (columns 4 and 5).
constexpr bool naplps_is_c1_final(uint8_t final_byte) {
  return final_byte >= 0x40 && final_byte <= 0x5F;
}

/// Whether |control| terminates a definition in progress — DEF MACRO, DEFP
/// MACRO, DEFT MACRO, DEF DRCS, DEF TEXTURE or END, which §6.2.2.1, §6.2.3 and
/// §6.2.4 all name as the terminators of each other.
constexpr bool naplps_terminates_definition(NaplpsC1 control) {
  return control == NaplpsC1::kDefMacro || control == NaplpsC1::kDefpMacro ||
         control == NaplpsC1::kDeftMacro || control == NaplpsC1::kDefDrcs ||
         control == NaplpsC1::kDefTexture || control == NaplpsC1::kEnd;
}

/**
 * @brief The designation and invocation state of the in-use table
 *
 * Holds which set is designated into each of G0-G3, which of them is invoked,
 * and whether a single shift is pending. §4.3.1.3 gives the defaults: "In the
 * default state G0 contains the primary character set, G1 the PDI set, G2 the
 * supplementary character set, and G3 the mosaic set", with G0 invoked.
 */
class NaplpsCodeEnvironment {
 public:
  NaplpsCodeEnvironment() { reset(); }

  /// Back to the default state of §4.3.1.3, which is what NSR (§6.1.6.5) and
  /// the RESET PDI both require.
  void reset();

  /// The set a byte in columns 2-7 currently means, single shift included.
  NaplpsGSet in_use() const {
    return designated_[static_cast<size_t>(invoked_)];
  }

  /// The slot currently invoked, single shift included.
  NaplpsGSlot invoked_slot() const { return invoked_; }

  /// Set designated into |slot|.
  NaplpsGSet designated(NaplpsGSlot slot) const {
    return designated_[static_cast<size_t>(slot)];
  }

  /// Note that one character has been consumed from the in-use table, which is
  /// what makes a single shift non-locking (§4.3.2).
  void consume_character();

  void designate(NaplpsGSlot slot, NaplpsGSet set) {
    designated_[static_cast<size_t>(slot)] = set;
  }

  /// Locking invocation: SI, SO, LS2, LS3.
  void invoke_locking(NaplpsGSlot slot) {
    locked_ = slot;
    invoked_ = slot;
    single_shift_ = false;
  }

  /// Non-locking invocation: SS2, SS3. Reverts after one character.
  void invoke_single_shift(NaplpsGSlot slot) {
    invoked_ = slot;
    single_shift_ = true;
  }

  bool single_shift_pending() const { return single_shift_; }

 private:
  NaplpsGSet designated_[4]{};
  /// Slot a locking invocation left in force, returned to after a single shift.
  NaplpsGSlot locked_ = NaplpsGSlot::kG0;
  /// Slot in force for the next character.
  NaplpsGSlot invoked_ = NaplpsGSlot::kG0;
  bool single_shift_ = false;
};

/// What an escape sequence turned out to be.
enum class NaplpsEscapeKind : uint8_t {
  /// A G-set designation; |slot| and |set| say which.
  kDesignation,
  /// A locking shift; |slot| says which.
  kLockingShift,
  /// A C1 control; |c1| says which.
  kControl,
  /// Syntactically a valid escape sequence, but not one this implements — a C0
  /// or C1 redesignation, or a final character naming a set that is not in
  /// Table 1. §4.3.2 makes the latter a null set. Skipped whole.
  kUnsupported,
  /// §4.3.2: "The occurrence of any other bit combination in an escape sequence
  /// shall cause the partial escape sequence to be terminated and ignored, and
  /// that bit combination shall be executed." So the offending byte is *not*
  /// consumed — |length| stops before it.
  kMalformed,
  /// The record ended inside the sequence.
  kTruncated,
};

/// One parsed escape sequence.
struct NaplpsEscape {
  NaplpsEscapeKind kind = NaplpsEscapeKind::kTruncated;
  /// Bytes to advance past, the ESC included. For kMalformed this stops before
  /// the byte that broke the sequence, which must then be executed in its own
  /// right.
  size_t length = 1;
  NaplpsGSlot slot = NaplpsGSlot::kG0;
  NaplpsGSet set = NaplpsGSet::kNull;
  NaplpsC1 c1 = NaplpsC1::kEnd;
};

/**
 * @brief Parse the escape sequence starting at |bytes|
 *
 * @param bytes First byte after the ESC
 * @param length Bytes available after the ESC
 *
 * X3.110 §4.3.2 gives the syntax as ESC I...I F, with intermediates in 2/0 to
 * 2/15 and one final in 3/0 to 7/14. Table 1 gives the designation pairs and
 * Table 2 the locking shifts; a final in columns 4 or 5 with no intermediate is
 * the two-character C1 form of Figure 65.
 *
 * The returned |length| always includes the ESC, so a caller advances by it
 * without arithmetic of its own.
 */
NaplpsEscape naplps_parse_escape(const uint8_t* bytes, size_t length);

}  // namespace orc

#endif  // ORC_NAPLPS_CODE_ENV_H
