/*
 * File:        eia608_service_demux.cpp
 * Module:      orc-sdk-support
 * Purpose:     EIA-608 service demultiplexer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/eia608_service_demux.h>

namespace orc {
namespace {

// Miscellaneous control codes: first byte 0x14/0x15 (data channel 1) or
// 0x1C/0x1D (data channel 2), second byte 0x20-0x2F [CTA-608-E Table 52]. The
// 0x15/0x1D spellings belong to field 2; accepting them on either field costs
// nothing and keeps a mislabelled capture readable.
bool is_misc_control(uint8_t byte1, uint8_t byte2) {
  const bool first =
      byte1 == 0x14 || byte1 == 0x15 || byte1 == 0x1C || byte1 == 0x1D;
  return first && byte2 >= 0x20 && byte2 <= 0x2F;
}

// Misc control codes that hand the channel to its captioning service.
bool selects_captioning(uint8_t byte2) {
  return byte2 == 0x20 ||  // RCL - Resume Caption Loading (pop-on)
         byte2 == 0x25 ||  // RU2 - Roll-Up, 2 rows
         byte2 == 0x26 ||  // RU3 - Roll-Up, 3 rows
         byte2 == 0x27 ||  // RU4 - Roll-Up, 4 rows
         byte2 == 0x29;    // RDC - Resume Direct Captioning (paint-on)
}

// Misc control codes that act on the channel's caption memories wherever
// they come, the midst of a Text Mode stream included [CTA-608-E 7.7, C.16]:
// they are the captioning service's whichever service the channel has
// selected, and the selection stays. Routed to the text service, an End of
// Caption sent between two rows of text never put its caption up, and an
// erasure cleared the text page instead of the caption (and cut the text row
// it came in the middle of in two).
//
// CTA-608-E has End of Caption end the Text Mode stream as well; here the
// text service keeps the channel until a code selects a caption style. A
// text service of the 1980s clears the caption memories inside its stream —
// EDM, EOC, EDM, then RTD — and where the RTD is lost the rows after it are
// still its own: on one recording ending the stream at the End of Caption
// sent six rows of TEXT2 into CC2's memory, where they wrote over one
// another.
bool acts_on_caption_memory(uint8_t byte2) {
  return byte2 == 0x2C ||  // EDM - Erase Displayed Memory
         byte2 == 0x2E ||  // ENM - Erase Non-Displayed Memory
         byte2 == 0x2F;    // EOC - End of Caption (flip memories)
}

// Misc control codes that hand the channel to its text service.
bool selects_text(uint8_t byte2) {
  return byte2 == 0x2A ||  // TR  - Text Restart
         byte2 == 0x2B;    // RTD - Resume Text Display
}

EIA608Service service_for(int field_index, int channel, bool text_mode) {
  if (field_index == 0) {
    if (channel == 0) {
      return text_mode ? EIA608Service::T1 : EIA608Service::CC1;
    }
    return text_mode ? EIA608Service::T2 : EIA608Service::CC2;
  }
  if (channel == 0) {
    return text_mode ? EIA608Service::T3 : EIA608Service::CC3;
  }
  return text_mode ? EIA608Service::T4 : EIA608Service::CC4;
}

}  // namespace

const char* eia608_service_name(EIA608Service service) {
  switch (service) {
    case EIA608Service::CC1:
      return "CC1";
    case EIA608Service::CC2:
      return "CC2";
    case EIA608Service::T1:
      return "T1";
    case EIA608Service::T2:
      return "T2";
    case EIA608Service::CC3:
      return "CC3";
    case EIA608Service::CC4:
      return "CC4";
    case EIA608Service::T3:
      return "T3";
    case EIA608Service::T4:
      return "T4";
  }
  return "CC1";
}

std::optional<EIA608Service> eia608_service_from_name(const std::string& name) {
  if (name == "CC1") return EIA608Service::CC1;
  if (name == "CC2") return EIA608Service::CC2;
  if (name == "CC3") return EIA608Service::CC3;
  if (name == "CC4") return EIA608Service::CC4;
  if (name == "T1" || name == "TEXT1") return EIA608Service::T1;
  if (name == "T2" || name == "TEXT2") return EIA608Service::T2;
  if (name == "T3" || name == "TEXT3") return EIA608Service::T3;
  if (name == "T4" || name == "TEXT4") return EIA608Service::T4;
  return std::nullopt;
}

int eia608_service_field(EIA608Service service) {
  switch (service) {
    case EIA608Service::CC1:
    case EIA608Service::CC2:
    case EIA608Service::T1:
    case EIA608Service::T2:
      return 0;
    case EIA608Service::CC3:
    case EIA608Service::CC4:
    case EIA608Service::T3:
    case EIA608Service::T4:
      return 1;
  }
  return 0;
}

bool eia608_service_is_text(EIA608Service service) {
  switch (service) {
    case EIA608Service::T1:
    case EIA608Service::T2:
    case EIA608Service::T3:
    case EIA608Service::T4:
      return true;
    default:
      return false;
  }
}

EIA608ServiceDemux::EIA608ServiceDemux(EIA608Service target,
                                       bool suppress_repeated_controls)
    : target_(target),
      suppress_repeated_controls_(suppress_repeated_controls) {}

void EIA608ServiceDemux::reset() {
  fields_[0] = FieldState{};
  fields_[1] = FieldState{};
  last_service_.reset();
  have_previous_ = false;
}

bool EIA608ServiceDemux::accept(int field_index, uint8_t byte1, uint8_t byte2) {
  const int field = (field_index == 0) ? 0 : 1;
  // The observer strips the parity bit, but a caller reading bytes from
  // elsewhere may not have; routing must never see it.
  byte1 = static_cast<uint8_t>(byte1 & 0x7F);
  byte2 = static_cast<uint8_t>(byte2 & 0x7F);

  // Null padding: transmitted on every field the services have nothing to say
  // on, and belonging to none of them.
  if (byte1 == 0x00 && byte2 == 0x00) {
    last_service_.reset();
    return false;
  }

  // XDS (Extended Data Service) packets are addressed by a first byte of
  // 0x01-0x0F on field 2 [CTA-608-E §8]. They are not a caption service and
  // do not disturb the channel selection the caption stream left behind.
  if (byte1 >= 0x01 && byte1 <= 0x0F) {
    last_service_.reset();
    return false;
  }

  FieldState& state = fields_[field];

  if (byte1 >= 0x10 && byte1 <= 0x1F) {
    // A control pair names its own data channel in bit 3, and this is what
    // subsequent character pairs inherit.
    state.channel = (byte1 & 0x08) ? 1 : 0;
    if (is_misc_control(byte1, byte2)) {
      if (selects_captioning(byte2)) {
        state.text_mode[state.channel] = false;
      } else if (selects_text(byte2)) {
        state.text_mode[state.channel] = true;
      }
    }
  }

  const bool text_mode =
      state.text_mode[state.channel] &&
      !(is_misc_control(byte1, byte2) && acts_on_caption_memory(byte2));
  const EIA608Service service = service_for(field, state.channel, text_mode);
  last_service_ = service;

  if (service != target_) {
    return false;
  }

  if (suppress_repeated_controls_ && byte1 >= 0x10 && byte1 <= 0x1F) {
    // Second of two identical consecutive control pairs: the encoder's
    // redundant copy, which must not be acted on twice.
    if (have_previous_ && previous_byte1_ == byte1 &&
        previous_byte2_ == byte2) {
      have_previous_ = false;
      return false;
    }
    have_previous_ = true;
    previous_byte1_ = byte1;
    previous_byte2_ = byte2;
    return true;
  }

  // A character pair breaks the adjacency a duplicate control pair needs.
  have_previous_ = false;
  return true;
}

}  // namespace orc
