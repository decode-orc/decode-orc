/*
 * File:        teletext_slicer.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     WST (System B) teletext data-line slicer producing T42 packets,
 *              on 625-line and 525-line television systems
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SLICER_H
#define ORC_TELETEXT_SLICER_H

// Shared plugin-side library, NOT part of the SDK contract: it compiles
// against the public SDK headers only and is linked privately into the stage
// plugins that need it. Nothing here crosses the plugin boundary, so changes
// never force an ABI bump.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orc {

// ETSI EN 300 706 §5.3: bit rate = 444 × nominal fH = 6,9375 Mbit/s ± 25 ppm.
constexpr double kTeletextBitRate = 6'937'500.0;

// ETSI EN 300 706 §7.1: a teletext packet comprises 360 bits organized as 45
// bytes; removing the clock run-in (2 bytes, §6.1) and framing code (1 byte,
// §6.2) leaves the 42-byte MRAG + data payload — the T42 packet.
//
// This is also the size of the packet buffer everywhere in the SDK: the
// 525-line variant below is shorter and occupies the leading bytes of the same
// array (see TeletextLineResult::packet_bytes).
constexpr size_t kTeletextPacketBytes = 42;

// ITU-R BT.653 Table 1b, Teletext System B on 525-line television systems:
// bit rate = 364 × fH = 5,727272 Mbit/s ± 25 ppm. At 4FSC NTSC that is exactly
// 2,5 samples per bit.
constexpr double kTeletext525BitRate = 5'727'272.0;

// ITU-R BT.653 Table 1b: the 525-line data line is 296 bits = 37 bytes;
// removing the clock run-in (2 bytes) and the framing code (1 byte) leaves 34
// bytes — the 2-byte prefix (MRAG) plus a 32-byte data block. The framing code
// and the clock run-in are those of the 625-line service; the bit rate and the
// packet length are not, which is why a 625-line slicer pointed at a 525-line
// line reads noise rather than a short packet.
constexpr size_t kTeletext525PacketBytes = 34;

// CEA-516-S-2013 §2.1 and §3.1, Teletext System C (NABTS) on 525-line
// television systems: the data line is 288 bits, of which the first 24 are the
// synchronization sequence (§2.2), leaving a 264-bit data packet organized as
// 33 bytes. The bit rate is the 525-line System B one (§1.3) and so is the
// clock run-in; the framing code (§2.2.3) and this length are what differ.
constexpr size_t kNabtsPacketBytes = 33;

// Payload bits of one 625-line packet: the 42 T42 bytes, transmitted LSB first
// per byte (ETSI EN 300 706 §7.1). The detectors index the packet by bit as
// well as by byte, and the per-bit diagnostics are sized from here — a
// 525-line packet fills the leading kTeletext525PacketBytes * 8 of them.
constexpr size_t kTeletextPayloadBits = kTeletextPacketBytes * 8;

// Teletext service, and the television system it is carried on. All three
// share the 16-bit clock run-in and the LSB-first byte order, and differ in
// bit rate, framing code, packet length and the position of the data in the
// line.
enum class TeletextSystem {
  // ETSI EN 300 706 (System B on 625 lines): 6,9375 Mbit/s, 42-byte packet,
  // framing code 0xE4.
  kWst625,

  // ITU-R BT.653 Table 1b (System B on 525 lines): 5,727272 Mbit/s, 34-byte
  // packet, framing code 0xE4. The service US broadcasters carried as "WST".
  kWst525,

  // CEA-516-S-2013, ITU-R BT.653 System C (NABTS) on 525 lines: the same
  // 5,727272 Mbit/s (§1.3) and the same clock run-in (§2.2.2) as the line
  // above, with framing code 0xE7 (§2.2.3) and a 33-byte packet (§3.1). The
  // framing code is the only thing that separates the two on a capture.
  kNabts525,
};

// Transmitted bit rate of |system|, in Hz.
constexpr double teletext_bit_rate(TeletextSystem system) {
  return system == TeletextSystem::kWst625 ? kTeletextBitRate
                                           : kTeletext525BitRate;
}

// Packet length of |system|, in bytes (framing code excluded).
constexpr size_t teletext_packet_bytes(TeletextSystem system) {
  switch (system) {
    case TeletextSystem::kWst525:
      return kTeletext525PacketBytes;
    case TeletextSystem::kNabts525:
      return kNabtsPacketBytes;
    case TeletextSystem::kWst625:
      break;
  }
  return kTeletextPacketBytes;
}

// Whether |system| gives its data bytes byte-wise odd parity in a way a slicer
// may gate on.
//
// True for both System B services: ETSI EN 300 706 §9.3.1 and ITU-R BT.653
// Table 1b give the display bytes of rows 0-25 odd parity, and the row number
// is recoverable from the packet's own addressing. False for NABTS, where
// CEA-516 §3.3 makes byte parity conditional on the data group type — a
// property of the group the packet belongs to, not of the packet, and so not
// knowable from one line.
constexpr bool teletext_has_parity_coded_rows(TeletextSystem system) {
  return system != TeletextSystem::kNabts525;
}

// Bytes at the head of a packet that carry Hamming 8/4 protected addressing,
// and which a slicer may therefore test for plausibility.
//
//   System B: the two MRAG bytes (ETSI EN 300 706 §7.1.2).
//   System C: the five packet prefix bytes P1-P3, CI and PS (CEA-516 §3.2.1),
//             which use the same Hamming 8/4 code (§3.2.2).
constexpr size_t teletext_hamming_prefix_bytes(TeletextSystem system) {
  return system == TeletextSystem::kNabts525 ? 5 : 2;
}

// Encode a 4-bit value as a Hamming 8/4 protected byte.
// ETSI EN 300 706 §8.2: bits 1, 3, 5, 7 (LSB numbering, transmission order)
// carry the protection bits P1-P4 and bits 2, 4, 6, 8 the data bits D1-D4.
// Only the low nibble of |value| is used.
uint8_t teletext_hamming84_encode(uint8_t value);

// Decode a Hamming 8/4 protected byte.
// ETSI EN 300 706 §8.2: single-bit errors are identified and corrected;
// double-bit errors are detected. Returns the decoded 4-bit value (0-15), or
// -1 when the byte is uncorrectable (double-bit error).
int teletext_hamming84_decode(uint8_t byte);

// Whether a Hamming 8/4 byte arrived as a codeword rather than being corrected
// into one.
//
// ETSI EN 300 706 §8.2 gives the code minimum distance 4, so a byte one error
// from a codeword is corrected and a byte two errors away is detected — but a
// byte three errors away lands in a *neighbouring* codeword's correction sphere
// and is silently resolved to the wrong value. This is what separates a value
// that was received from one that was inferred, and it is the only warning a
// caller ever gets that a mis-correction was possible.
//
// It matters most where the value names something rather than describing it: a
// mis-corrected display byte damages one character, where a mis-corrected
// address invents a page. A false here does not mean the byte is wrong — most
// corrections are right — only that the code was asked to guess.
bool teletext_hamming84_clean(uint8_t byte);

// Encode 18 data bits as a Hamming 24/18 protected triplet.
// ETSI EN 300 706 §8.3: over three consecutive bytes, transmission-order bits
// 1, 2, 4, 8, 16 and 24 are the protection bits P1-P6 and the remaining
// eighteen carry D1-D18. |value| supplies D1 (its bit 0) to D18 (its bit 17);
// bits above that are ignored. |out_bytes| receives the three bytes in
// transmission order.
void teletext_hamming2418_encode(uint32_t value, uint8_t out_bytes[3]);

// Decode a Hamming 24/18 protected triplet (ETSI EN 300 706 §8.3), given the
// three bytes in transmission order. Single-bit errors are identified and
// corrected; double-bit errors are detected. Returns the 18-bit value with D1
// in bit 0, or -1 when the triplet is uncorrectable.
//
// The returned value is what the standard's packet tables address as "triplet
// bits 1 to 18", so a field at bits m-n is read as (value >> (m - 1)) masked
// to n - m + 1 bits.
int32_t teletext_hamming2418_decode(uint8_t byte_n, uint8_t byte_n1,
                                    uint8_t byte_n2);

// Confidence quantisation of an observation string: one hex digit per byte, so
// 16 levels from 0 (the detector could as well have decided otherwise) to 15
// (as sure as an undamaged signal makes it). A recovered packet is 42 bytes and
// the vote it feeds is a weighting, not an arithmetic; a nibble apiece is
// ample, and it keeps the suffix half the length of the packet itself.
constexpr int kTeletextConfidenceLevels = 16;

// How sure the recovery chain was of each byte of a packet, 0 … 1 (see
// TeletextLineResult::byte_confidence).
using TeletextPacketConfidence = std::array<float, kTeletextPacketBytes>;

// Encode the leading |byte_count| bytes of a packet as lowercase hex, two
// characters per byte — 84 for a 625-line T42 packet, 68 for a 525-line one.
// Shared between the teletext observer (producer) and the teletext sink
// (consumer) so the observation-string representation has a single definition.
//
// The length of the string is what carries the packet length: the two are the
// only ones a WST service transmits and they are unambiguous (see
// teletext_hex_to_observed_packet).
std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes,
    size_t byte_count = kTeletextPacketBytes);

// As above, with the detector's per-byte confidence (see
// TeletextLineResult::byte_confidence) appended as a further |byte_count| hex
// digits, one per byte — 126 characters for a 625-line packet, 102 for a
// 525-line one.
//
// The suffix is optional by design: an observation written before it existed is
// 84 characters and decodes as full confidence, so a stored sweep of a
// recording stays usable and nothing has to be re-observed to read it.
std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes,
    const TeletextPacketConfidence& confidence,
    size_t byte_count = kTeletextPacketBytes);

// Decode a 625-line observation string (either case, with or without the
// confidence suffix) back to its 42 packet bytes. Returns std::nullopt when
// the length or any character is invalid.
//
// Deliberately 625-line only: the returned array carries no length, so
// accepting a 34-byte 525-line packet here would hand the caller eight bytes
// that were never transmitted with nothing to mark them as such. A caller that
// handles both lengths uses teletext_hex_to_observed_packet() below, whose
// result states the length it decoded.
std::optional<std::array<uint8_t, kTeletextPacketBytes>> teletext_hex_to_packet(
    std::string_view hex);

// A packet decoded from an observation string, with whatever confidence the
// string carried.
struct TeletextObservedPacket {
  std::array<uint8_t, kTeletextPacketBytes> bytes{};
  // Bytes of |bytes| the string actually carried: kTeletextPacketBytes for a
  // 625-line packet, kTeletext525PacketBytes for a 525-line one. Bytes past it
  // are zero and were never transmitted.
  size_t byte_count = kTeletextPacketBytes;
  // False when the string carried no suffix (an observation from a build
  // before confidences existed); |confidence| is then 1,0 throughout — a copy
  // of unknown quality must not be weighted below one that measured itself.
  bool has_confidence = false;
  TeletextPacketConfidence confidence{};
};

// Decode an observation string of either packet length, with its confidence
// suffix if it has one. The four accepted lengths — 68, 84, 102 and 126
// characters — are distinct, so the string decodes without the caller having
// to say which system produced it. Returns std::nullopt when the length or any
// character is invalid.
std::optional<TeletextObservedPacket> teletext_hex_to_observed_packet(
    std::string_view hex);

// Catmull-Rom cubic interpolation of |line| at fractional sample position |t|.
//
// The MLSE detector reads each bit at several positions within its bit period,
// so the interpolation kernel is in the signal path rather than a convenience.
// Linear interpolation is a low-pass in its own right: at the half-sample
// point it attenuates by 2/π² per octave of the band, taking out precisely the
// transition energy a band-limited channel has already eaten into. The
// Catmull-Rom kernel — the four-point cubic (-1, 9, 9, -1)/16 at the half
// sample — is interpolating (it reproduces the input samples exactly at
// integer positions), has a far flatter passband, and costs a handful of
// multiplies. Positions outside the line clamp to its end samples.
//
// Exposed rather than kept private to the slicer so its amplitude accuracy can
// be measured directly; it is not otherwise part of the slicer contract.
double teletext_interpolate_sample(const int16_t* line, size_t sample_count,
                                   double t);

// Detector used to recover the payload bits from a candidate line.
enum class TeletextDetector {
  // Threshold slicing at interpolated bit centres after clock run-in
  // correlation. Assumes the channel passes the 3,47 MHz run-in fundamental
  // (ETSI EN 300 706 §6.1) more or less intact, which broadcast, LaserDisc
  // and direct CVBS captures do. Cheapest, and exact on a clean signal.
  kThreshold,

  // Maximum-likelihood sequence estimation against a channel response fitted
  // per line to the known 24-bit preamble, then refitted to the whole packet
  // and run again. Tolerates the intersymbol interference of a band-limited
  // channel — consumer VHS luma rolls off around 3 MHz, which is below the
  // run-in fundamental, so the run-in is attenuated far more than the payload
  // and threshold slicing cannot lock. Costs roughly an order of magnitude
  // more work per line.
  kMlse,

  // Try kThreshold first and fall back to kMlse when it fails to lock, or when
  // it locked with an eye margin too small to trust. A clean source therefore
  // pays nothing extra, because the fallback never runs on a line the
  // threshold detector read with a clear eye.
  kAuto,
};

// Why a candidate line yielded no packet.
//
// Diagnostics only: the reason never changes what the slicer accepts, it
// records which gate discarded the line. Recovery tuning needs that split —
// "no data on this line" and "data that failed the payload-residual gate" are
// the same empty result but opposite problems.
enum class TeletextRejectReason {
  // Not rejected: the result is valid.
  kNone,

  // The line pointer was null, the line too short to hold a whole packet
  // (ETSI EN 300 706 §7.1), or the caller's black/white pair degenerate.
  kInsufficientSamples,

  // Nothing in the line rose meaningfully above black level: an empty VBI
  // line, which is most of them.
  kAmplitudeGate,

  // Threshold detector: the clock run-in correlation (§6.1) never went
  // positive anywhere in the §6.3 timing window.
  kNoRunInLock,

  // Threshold detector: the 1 and 0 levels recovered at the locked phase are
  // too close together to slice between. This is the signature of a channel
  // that has attenuated the 3,47 MHz run-in fundamental (§6.1) away — a tape,
  // where the MLSE detector is the remedy.
  kRunInAmplitude,

  // Threshold detector: the bits at the locked phase are not the alternating
  // run-in pattern (§6.1).
  kRunInPattern,

  // Threshold detector: no framing code (§6.2) at any alignment in the search
  // window around the run-in lock.
  kFramingCodeMiss,

  // Either detector: the MRAG bytes (§7.1.2) did not survive Hamming 8/4
  // correction (§8.2) and require_valid_mrag is set.
  kInvalidMrag,

  // MLSE detector: no bit phase produced a channel fit with usable gain, so
  // there was nothing to detect against.
  kNoPreambleLock,

  // MLSE detector: the fitted channel could not explain the known 24 preamble
  // bits well enough to be believed a preamble.
  kPreambleResidual,

  // MLSE detector: the recovered bits pushed back through the fitted channel
  // do not reproduce the payload samples.
  kPayloadResidual,

  // MLSE detector: too few of the 40 data bytes of a parity-coded row carried
  // odd parity (§9.3.1) for the packet to be a real one.
  kParityFraction,
};

// Number of TeletextRejectReason values, for histogram sizing.
constexpr size_t kTeletextRejectReasonCount = 12;

// Short human-readable name of |reason| (e.g. "payload residual"), for
// diagnostics summaries and log messages. Names the reason in the terms of
// |system|: the addressing gate rejects a System B packet whose MRAG did not
// decode (ETSI EN 300 706 §7.1.2) and a System C packet whose five-byte packet
// prefix did not (CEA-516 §3.2.1), and a report that called the latter an MRAG
// would be naming a field the service does not have.
std::string_view teletext_reject_reason_name(
    TeletextRejectReason reason,
    TeletextSystem system = TeletextSystem::kWst625);

/**
 * @brief A narrowed acquisition window, learned from lines that already locked
 *
 * Both detectors acquire the bit phase by sweeping the whole §6.3 data-timing
 * window, which is where most of the cost of slicing a line sits: 284 candidate
 * positions for the threshold detector on PAL and 355 for MLSE. On a
 * time-base-corrected source every data line of the recording starts at
 * essentially the same sample, so once a few lines have locked there is no
 * reason to keep searching the whole window.
 *
 * A hint narrows the sweep to |centre| ± |radius| samples. It cannot lose a
 * packet: when a hinted attempt yields nothing the slicer repeats the attempt
 * over the full window, which costs the few percent the narrow sweep took. It
 * can, on a line where both windows would recover a packet, return the lock the
 * narrow window liked best rather than the global one — so a hint must be
 * centred on locks actually observed rather than on an assumption, and must be
 * wide enough to hold the spread of them.
 */
struct TeletextPhaseHint {
  // False means "no opinion": the slicer sweeps the full window as it always
  // has. This is the default, so an unhinted caller is unaffected.
  bool valid = false;
  // Sample position the observed locks agree on.
  double centre = 0.0;
  // Half-width of the window to sweep around |centre|, in samples.
  double radius = 0.0;
};

// Result of slicing one candidate VBI line.
struct TeletextLineResult {
  // True when the clock run-in and framing code were found and the payload
  // was extracted (subject to the optional MRAG plausibility filter).
  bool valid = false;

  // MRAG + data bytes in transmission coding (Hamming 8/4 on addressing
  // bytes, odd parity on display bytes). No error correction is applied to
  // the payload: the T42 contract preserves transmission coding.
  //
  // Only the leading |packet_bytes| are transmitted; on a 525-line service the
  // remaining eight are left zero.
  std::array<uint8_t, kTeletextPacketBytes> bytes{};

  // Bytes of |bytes| the service transmits: kTeletextPacketBytes on 625 lines,
  // kTeletext525PacketBytes on 525 (ITU-R BT.653 Table 1a/1b),
  // kNabtsPacketBytes for System C (CEA-516 §3.1). Set whether or not the line
  // yielded a packet, so a caller reading a rejected result still knows what it
  // was looking for.
  size_t packet_bytes = kTeletextPacketBytes;

  // The service the slicer was configured for, and therefore the one these
  // bytes are coded under. Carried alongside the length because a consumer
  // needs it to know how to read them — the transmission coding of a System C
  // packet is not that of a System B one — and deriving it from the length
  // would put that knowledge in every consumer instead of here.
  TeletextSystem system = TeletextSystem::kWst625;

  // Number of bit errors accepted in the framing code: 0, or 1 when the
  // slicer runs in tolerant-framing mode.
  int framing_bit_errors = 0;

  // Sample position (fractional) where the framing code ended and the payload
  // began. Diagnostics only.
  double data_start_sample = 0.0;

  // Sample position (fractional) the detector acquired the bit phase at, i.e.
  // the centre of run-in bit 0. Negative when acquisition failed.
  //
  // Unlike data_start_sample this is free of the framing-code alignment shift,
  // so it is directly comparable between lines: it is what a caller pinning the
  // acquisition window accumulates (see TeletextPhaseHint). The MLSE detector
  // may have read the packet with its tap window up to two bits from here;
  // the bits come out the same, and this stays the timing reference.
  double lock_sample = -1.0;

  // Which detector recovered these bytes — or, when valid is false, which one
  // rejected the line (the detector-independent gates of slice() report
  // kThreshold). Diagnostics only.
  TeletextDetector detector = TeletextDetector::kThreshold;

  // Why the line yielded no packet; kNone when valid. Diagnostics only.
  TeletextRejectReason reject_reason = TeletextRejectReason::kNone;

  // kMlse only: RMS residual of the preamble channel fit as a fraction of the
  // fitted channel gain — how well the band-limited channel model explained
  // the known 24 preamble bits. Lower is a better lock. Set whenever a fit was
  // made, including on lines the residual gates then rejected, so the gates
  // can be studied against real data. Diagnostics only.
  double preamble_residual = 0.0;

  // kMlse only: RMS error between the recovered bit sequence pushed back
  // through the fitted channel and the samples actually read, again as a
  // fraction of the channel gain. Measures the whole payload rather than the
  // 24 preamble bits, so it is the stronger indicator of a real lock. Set
  // whenever the detection ran, rejected lines included.
  //
  // Always from the first detection pass, whose channel was fitted to the
  // preamble alone: the second pass refits the channel to the bits the first
  // decided, so its residual measures how well a model explains the data it was
  // fitted to rather than whether the line is a teletext packet. Diagnostics
  // only.
  double payload_residual = 0.0;

  // kMlse only: the same reconstruction error resolved per payload bit rather
  // than summed over the packet — element n is the RMS error of bit n over the
  // sample phases scored for it, as a fraction of the fitted channel gain.
  // Zero throughout when no detection ran.
  //
  // This is the timing diagnostic. A bit clock that is running at slightly the
  // wrong rate reads each bit a little further from its centre than the last,
  // so its reconstruction error grows across the 52 µs of the packet; noise and
  // intersymbol interference cost the same everywhere. The shape of this
  // profile separates the two, which the per-byte parity profile of
  // TeletextRecoveryStats can only hint at.
  //
  // From the first detection pass, for the reason given for payload_residual
  // above. Diagnostics only.
  std::array<float, kTeletextPayloadBits> payload_bit_errors{};

  // Whether byte_confidence below carries a measurement. True for every
  // threshold-detected packet, and for every MLSE-detected one except the rare
  // line whose channel refit was singular.
  bool has_byte_confidence = false;

  // How sure the detector was of each of the 42 bytes, 0 … 1.
  //
  // The threshold detector reports the eye margin: how far the least certain
  // of a byte's eight bit-centre samples sits from the slicing threshold, as a
  // fraction of half the recovered amplitude.
  //
  // The MLSE detector reports the path-metric margin. The Viterbi picks the
  // most likely bit sequence, and how much more likely it
  // is than the best sequence with a given bit flipped is a measurement of that
  // bit in its own right. Element n is the smallest such margin among the eight
  // bits of byte n — the byte is only as trustworthy as its weakest bit —
  // expressed as a fraction of the margin a noiseless line would give (the
  // energy of the fitted channel's response to one bit), so lines of different
  // amplitude and channels of different bandwidth are on one scale. 1 means the
  // decision is as clear-cut as an undamaged signal makes it; 0 means the
  // opposite bit fitted the samples just as well.
  //
  // This is what lets the odd parity of ETSI EN 300 706 §8.1 be acted on rather
  // than merely observed: parity says a byte is wrong, and this says which bit
  // of it to doubt (see TeletextSlicerOptions::parity_repair). It also weights
  // the vote when repeated copies of a row are combined (see
  // vbi-services/teletext_row_squasher.h).
  TeletextPacketConfidence byte_confidence{};

  // Data bytes whose least-confident bit was flipped to restore odd parity
  // (TeletextSlicerOptions::parity_repair). Zero when the option is off.
  // Diagnostics only.
  int repaired_bytes = 0;
};

// Slicer tuning options. Defaults match the strictest behaviour: exact
// framing-code match and MRAG plausibility filtering enabled.
struct TeletextSlicerOptions {
  // Television system the service is carried on. It selects the packet length,
  // the position of the data in the line and the nominal data '1' amplitude
  // (ITU-R BT.653 Tables 1a and 1b); the bit rate comes from the constructor,
  // which derives it from this unless a caller states one explicitly.
  TeletextSystem system = TeletextSystem::kWst625;

  // Accept a framing code with one bit error (ETSI EN 300 706 §6.2 defines
  // the exact 8-bit pattern; some receivers tolerate a single error, at the
  // cost of a higher false-positive rate on noisy sources).
  bool tolerant_framing = false;

  // Require both MRAG bytes (ETSI EN 300 706 §7.1.2) to survive Hamming 8/4
  // correction (§8.2) at the chosen byte alignment. Single-bit-damaged
  // packets still pass — Hamming 8/4 corrects those — but noise that happens
  // to spell the framing code does not, which is what keeps the framing-code
  // search window safe. A packet whose addressing is unrecoverable carries no
  // usable payload anyway: TeletextPageDecoder drops it. The MRAG bytes in
  // the output remain uncorrected (transmission coding).
  bool require_valid_mrag = true;

  // Bit detector (see TeletextDetector). The default preserves the behaviour
  // of every caller written before the MLSE detector existed; kAuto is what a
  // caller that may be handed a tape source wants.
  TeletextDetector detector = TeletextDetector::kThreshold;

  // MLSE detector only: sample phases scored per bit period.
  //
  // At the PAL 4FSC sample rate one teletext bit spans ≈ 2.556 samples, and at
  // the NTSC 4FSC rate exactly 2.5 (the ITU-R BT.653 bit rate of the system
  // against the 4FSC sample clock), so the capture already holds more than two
  // independent observations of every bit.
  // The detector fits the channel per phase and accumulates its branch metric
  // over all of them, which is what turns those extra samples into evidence.
  // Three is the most a bit period of 2,5 samples supports before two grid
  // points fall between the same pair of captured samples and stop being
  // independent. A value of 1 reduces the detector to scoring the bit centre
  // alone, which is what the fractionally-spaced metric is measured against;
  // values are clamped to 1 … 3.
  int mlse_samples_per_bit = 3;

  // MLSE detector only: restore odd parity on damaged display bytes by
  // flipping the bit the detector was least sure of.
  //
  // ETSI EN 300 706 §8.1 gives every display byte odd parity, so a byte that
  // fails it carries an odd number of bit errors — at least one. The detector
  // knows which of the eight bits it came closest to deciding the other way
  // (TeletextLineResult::byte_confidence), and flipping that one is the
  // maximum-likelihood repair of a single-bit error. Applied only to the 40
  // data bytes of a parity-coded row (0-25): the MRAG and the Hamming 8/4
  // bytes of the enhancement rows carry their own correction, which downstream
  // decoding already applies, and the rows above 25 are not byte-wise parity
  // coded at all.
  //
  // Off by default, and deliberately so. A repaired byte satisfies parity
  // whether or not the flip was right, so it can no longer be told from a byte
  // that arrived undamaged: it is a decoded best guess in a stream whose
  // contract is transmission coding. Consumers that count on parity to mark
  // damage — the page renderer's damaged-byte readout, and the parity-first
  // vote of the row squasher — see fewer damaged bytes and more silently wrong
  // ones. Where a recording is bad enough that most bytes are damaged, that
  // trade is worth making; where it is not, it is not. The repaired byte does
  // carry the low confidence of the bit that was flipped, so a squasher vote
  // weighted by confidence still prefers a copy that arrived intact.
  bool parity_repair = false;
};

/**
 * @brief WST (System B) teletext data-line slicer.
 *
 * Recovers T42 packets (MRAG + data, transmission coding) from single VBI
 * lines of 4FSC-sampled video: 42 bytes on 625-line systems (ETSI EN 300 706)
 * and 34 on 525-line ones (ITU-R BT.653 Table 1b), selected by
 * TeletextSlicerOptions::system. At the decode-orc sample rates one teletext
 * bit spans ≈ 2.556 samples (PAL) or exactly 2.5 (NTSC), so recovery uses
 * clock run-in correlation and interpolated bit-centre sampling rather than a
 * transition-map approach.
 *
 * Two bit detectors are available, selected by TeletextSlicerOptions::detector:
 * threshold slicing (the default, for channels that pass the data band) and
 * MLSE against a per-line channel fit (for band-limited channels such as
 * consumer VHS, where intersymbol interference makes bit-centre thresholding
 * unusable).
 *
 * The MLSE detector runs two passes. The first detects against a channel
 * fitted to the known 24-bit preamble and supplies the diagnostics and the
 * lock gates; the second refits the channel to all bits the first pass decided
 * and detects again, which is where most of its accuracy on a tape comes from
 * — 20 fit equations per sample phase describe a channel far less well than
 * 336 do.
 *
 * Thread safety: slice() is const and the class holds no mutable state; a
 * single instance may be used concurrently from multiple threads.
 */
class TeletextSlicer {
 public:
  // |sample_rate| in Hz (e.g. kPalSampleRate = 17,734,475 Hz).
  // |bit_rate| fixed at 444 × fH by ETSI EN 300 706 §5.3; overridable for
  // tests only. Everything else the system decides comes from
  // TeletextSlicerOptions::system, which defaults to the 625-line service.
  explicit TeletextSlicer(double sample_rate,
                          double bit_rate = kTeletextBitRate,
                          TeletextSlicerOptions options = {});

  // As above with the bit rate derived from |system| (ITU-R BT.653 Table 1a /
  // Table 1b), which is what every caller slicing real video wants: the system
  // then decides the bit rate, the packet length and the data timing together
  // rather than in two places. Any |options.system| is overwritten by |system|.
  TeletextSlicer(double sample_rate, TeletextSystem system,
                 TeletextSlicerOptions options = {});

  // Slice one candidate VBI line of |sample_count| samples in the
  // CVBS_U10_4FSC 10-bit level domain. |black_level| and |white_level| locate
  // the data levels (ETSI EN 300 706 §5.2 on 625 lines: 0 = black, 1 = 66 % of
  // black-to-white; ITU-R BT.653 Table 1b on 525: 70 % of the same excursion).
  // Returns a result with valid == false when the line carries no recoverable
  // teletext packet.
  //
  // |phase_hint|, when valid, narrows the bit-phase acquisition sweep of
  // whichever detector runs; a hinted attempt that recovers nothing is repeated
  // over the full window, so a hint can only cost time, never packets.
  TeletextLineResult slice(const int16_t* line, size_t sample_count,
                           int16_t black_level, int16_t white_level,
                           const TeletextPhaseHint& phase_hint = {}) const;

  // Packet length of the configured system, in bytes.
  size_t packet_bytes() const { return packet_bytes_; }

  // Sample positions per transmitted bit, from the sample rate and the
  // system's bit rate. Exposed because a caller pinning the acquisition window
  // sizes its tolerance in bit periods.
  double samples_per_bit() const { return samples_per_bit_; }

 private:
  // The acquisition sweep, as first and last candidate sample position. Full
  // is the §6.3 data-timing window of the configured system; a valid hint
  // narrows it, clamped to stay inside the full one.
  struct AcquisitionWindow {
    double first = 0.0;
    double last = 0.0;
  };
  AcquisitionWindow full_window() const;
  // The hinted window, or std::nullopt when |hint| is unset, or narrows to
  // nothing, or is no narrower than the full sweep and so would only cost a
  // wasted first attempt.
  std::optional<AcquisitionWindow> hinted_window(
      const TeletextPhaseHint& hint) const;

  // Both detectors share the caller's level domain, so slice() computes the
  // nominal '1' amplitude and the empty-line rejection gate once and hands
  // them down. Each runs its detection over one acquisition window; slice()
  // owns the hinted-then-full retry.
  TeletextLineResult slice_threshold(const int16_t* line, size_t sample_count,
                                     double amplitude_gate,
                                     AcquisitionWindow window) const;
  TeletextLineResult slice_mlse(const int16_t* line, size_t sample_count,
                                double nominal_amplitude,
                                AcquisitionWindow window) const;

  // A blank result already stamped with the configured packet length, so no
  // path can return one that misreports what it was looking for.
  TeletextLineResult new_result() const;

  // MLSE detection at one bit phase: the fractionally-spaced channel fit at
  // |t0|, the residual and framing gates, the trellis over the payload and the
  // packet gates. |grid| is scratch the caller lends.
  TeletextLineResult detect_mlse_at(const int16_t* line, size_t sample_count,
                                    double t0, int phases,
                                    std::vector<double>& grid) const;

  double sample_rate_;
  double samples_per_bit_;
  TeletextSlicerOptions options_;

  // System-dependent geometry, resolved once from options_.system.
  size_t packet_bytes_;
  int payload_bits_;
  double data_one_fraction_;
  double search_start_samples_;
  double search_end_samples_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_SLICER_H
