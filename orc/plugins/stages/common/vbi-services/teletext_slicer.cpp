/*
 * File:        teletext_slicer.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     WST (System B) teletext data-line slicer producing T42 packets,
 *              on 625-line and 525-line television systems
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_slicer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace orc {

namespace {

// ETSI EN 300 706 §6.1: 16-bit clock run-in, transmission order 1010…1010
// (first transmitted bit is '1'; even bit indices are ones).
constexpr int kRunInBits = 16;

// Framing code length. Both System B (ETSI EN 300 706 §6.2) and System C
// (CEA-516 §2.2.3) use eight bits; the pattern is per-system and lives in the
// geometry table below.
constexpr int kFramingBits = 8;

// The framing code of a service, in transmission order.
using FramingCode = std::array<int, kFramingBits>;

// ETSI EN 300 706 §6.2: transmission order 11100100 (conventionally written
// 0xE4 MSB-first).
constexpr FramingCode kFramingCodeWst = {1, 1, 1, 0, 0, 1, 0, 0};

// CEA-516 §2.2.3: the broadcast teletext framing code is 11100111, written
// there as b8…b1 with b1 transmitted first. The pattern is symmetric under bit
// reversal, so it reads the same in either convention — unlike the System B
// code, which does not, and which is why the two are never confused.
constexpr FramingCode kFramingCodeNabts = {1, 1, 1, 0, 0, 1, 1, 1};

// The payload of a 625-line packet is 336 bits: ETSI EN 300 706 §7.1's 360-bit
// packet less the run-in (16) and framing code (8), transmitted LSB first per
// byte. A 525-line packet is shorter (ITU-R BT.653 Table 1b) and fills the
// leading bits of the same buffers, which are sized from kTeletextPayloadBits;
// the count the detectors actually work to is TeletextSlicer::payload_bits_.

// Data '1' level as a fraction of the black-to-white difference (the '0' level
// is black in both systems).
//   625 lines: ETSI EN 300 706 §5.2 and ITU-R BT.653 Table 1a — 66 % ± 6 %.
//   525 lines: ITU-R BT.653 Table 1b — 70 % ± 6 %.
constexpr double kDataOneLevelFraction625 = 0.66;
constexpr double kDataOneLevelFraction525 = 0.70;

// Minimum recovered data amplitude, as a fraction of the nominal '1' level,
// below which a line is treated as empty. Implementation choice: half the
// nominal amplitude rejects blank and noise-only lines cheaply while accepting
// the ± 6 % tolerance of either system with wide margin.
constexpr double kAmplitudeGateFraction = 0.5;

// Search window for the centre of the first clock run-in bit, in µs from the
// start of the line (which decode-orc places at 0H).
//
// ETSI EN 300 706 §6.3 fixes the timing reference at the mid point of the
// penultimate '1' of the clock run-in. The run-in is 1010…1010 over 16 bits,
// so its ones are bits 1, 3 … 15 and the penultimate one is bit 13 — the same
// bit ITU-R BT.653 Table 1a/1b names. The first bit centre therefore sits 12
// bit periods before the reference:
//
//   625 lines: reference 12,0 µs (§6.3 note, BT.653 Table 1a) → first bit
//     centre at 12,0 − 12/6,9375 = 10,27 µs, leading edge at 10,20 µs — which
//     is the 10 300 ns libzvbi tabulates and the VBI source stage places to.
//   525 lines: reference 11,7 µs ± 0,175 (BT.653 Table 1b) → first bit centre
//     at 11,7 − 12/5,727272 = 9,61 µs, leading edge at 9,52 µs. The 525-line
//     reference captures measured for the VBI source stage put the leading
//     edge at 9,3 to 9,4 µs, which agrees with this to within a bit period —
//     and disagrees with libzvbi's tabulated 10 500 ns by seven of them.
//
// The §6.3 note allows departures from the nominal for network re-timing, so
// each window spans roughly ± 2 µs around it. The lower bound also has to
// clear the colour burst, which ends ≈ 7,8 µs into the line on both systems;
// that is what makes the 525-line window asymmetric about its nominal.
constexpr double kRunInSearchStartUs625 = 8.0;
constexpr double kRunInSearchEndUs625 = 12.0;
constexpr double kRunInSearchStartUs525 = 8.0;
constexpr double kRunInSearchEndUs525 = 11.5;

// NABTS: CEA-516 §1.3 puts the half-amplitude point of the first 0→1
// transition of the clock synchronization sequence at 10,48 µs ± 0,34 from the
// leading edge of horizontal sync. That transition is the leading edge of
// run-in bit 0, so the first bit centre is half a bit period later at 10,57
// µs. The ExtraVision captures measured for the VBI source stage put the
// leading edge at 10,27 to 10,34 µs, i.e. a bit centre of 10,39 µs — half a
// bit period below nominal, well inside the tabulated tolerance.
//
// The lower bound clears the colour burst, which ends ≈ 7,8 µs into the line.
// The upper bound is what leaves the whole 288-bit line (§2.1) inside the
// 910-sample NTSC line: 12,2 µs of lock plus 50,3 µs of burst ends at 62,5 of
// 63,56 µs. That still gives +1,3 µs of network re-timing headroom above
// nominal, comparable to the 625-line row above.
constexpr double kRunInSearchStartUsNabts = 8.5;
constexpr double kRunInSearchEndUsNabts = 12.2;

// Everything above that the slicer's geometry depends on the system for,
// gathered so that the choice is made once. A further service is a row here
// plus a TeletextSystem enumerator, rather than a ternary to be found in each
// of the places the geometry is read.
struct SystemGeometry {
  double bit_rate;      // Hz (ITU-R BT.653 Tables 1a and 1b, CEA-516 §1.3)
  size_t packet_bytes;  // framing code excluded
  double data_one_fraction;
  double search_start_us;  // first clock run-in bit centre, from 0H
  double search_end_us;
  FramingCode framing;
  // Hamming 8/4 protected addressing bytes at the head of the packet, which
  // the plausibility gates test (see teletext_hamming_prefix_bytes).
  size_t hamming_prefix_bytes;
  // Whether the data bytes are byte-wise odd parity coded in a way the MLSE
  // plausibility gate can act on (see teletext_has_parity_coded_rows).
  bool parity_coded_rows;
};

constexpr SystemGeometry system_geometry(TeletextSystem system) {
  switch (system) {
    case TeletextSystem::kWst525:
      return SystemGeometry{kTeletext525BitRate,
                            kTeletext525PacketBytes,
                            kDataOneLevelFraction525,
                            kRunInSearchStartUs525,
                            kRunInSearchEndUs525,
                            kFramingCodeWst,
                            teletext_hamming_prefix_bytes(system),
                            teletext_has_parity_coded_rows(system)};
    case TeletextSystem::kNabts525:
      // CEA-516 §1.6 puts logic '1' at 70 IRE and logic '0' at blanking, which
      // is what ITU-R BT.653 Table 1b says of the 525-line System B service as
      // well, so the fraction is shared with the row above. It is expressed
      // against the black-to-white excursion while the transmitted '0' sits at
      // blanking, below the 7,5 IRE setup black: measured against the levels
      // the VBI source stage places to (blanking 240, '1' 632, black 282,
      // white 800 in the 10-bit domain) the true fraction is 0,676, so 0,70
      // sets the nominal 3,5 % high and the half-nominal amplitude gate
      // correspondingly strict. A real burst still clears it by 350 counts
      // against 181 — a factor of 1,9.
      return SystemGeometry{kTeletext525BitRate,
                            kNabtsPacketBytes,
                            kDataOneLevelFraction525,
                            kRunInSearchStartUsNabts,
                            kRunInSearchEndUsNabts,
                            kFramingCodeNabts,
                            teletext_hamming_prefix_bytes(system),
                            teletext_has_parity_coded_rows(system)};
    case TeletextSystem::kWst625:
      break;
  }
  return SystemGeometry{
      kTeletextBitRate,
      kTeletextPacketBytes,
      kDataOneLevelFraction625,
      kRunInSearchStartUs625,
      kRunInSearchEndUs625,
      kFramingCodeWst,
      teletext_hamming_prefix_bytes(TeletextSystem::kWst625),
      teletext_has_parity_coded_rows(TeletextSystem::kWst625)};
}

// The public accessors in the header and this table describe the same
// services, so they must agree; they are separate only because the header's
// have to be usable in constant expressions by callers that never see this.
static_assert(system_geometry(TeletextSystem::kWst625).bit_rate ==
                  teletext_bit_rate(TeletextSystem::kWst625) &&
              system_geometry(TeletextSystem::kWst525).bit_rate ==
                  teletext_bit_rate(TeletextSystem::kWst525) &&
              system_geometry(TeletextSystem::kNabts525).bit_rate ==
                  teletext_bit_rate(TeletextSystem::kNabts525) &&
              system_geometry(TeletextSystem::kWst625).packet_bytes ==
                  teletext_packet_bytes(TeletextSystem::kWst625) &&
              system_geometry(TeletextSystem::kWst525).packet_bytes ==
                  teletext_packet_bytes(TeletextSystem::kWst525) &&
              system_geometry(TeletextSystem::kNabts525).packet_bytes ==
                  teletext_packet_bytes(TeletextSystem::kNabts525));

// Correlation phase-search step in samples. At the ≈ 2.556 samples/bit of the
// 625-line service, or the 2.5 of the 525-line one, a quarter sample bounds
// the bit-centre placement error at ≈ 5 % of a bit period.
constexpr double kPhaseSearchStep = 0.25;

// Minimum run-in bits that must match the alternating pattern after
// thresholding. ETSI EN 300 706 §6.1 note: the two leading data ones may be
// absent or reduced in amplitude, so up to two mismatches are allowed.
constexpr int kMinRunInMatches = 14;

// Framing-code search range around the nominal position, in bit periods.
// The run-in correlation is ambiguous to even-bit shifts (the alternating
// kernel re-aligns every 2 bits), and on real recordings the correlation peak
// lands up to four bits from the true run-in start — the §6.3 note allows the
// insertion point to move for network re-timing, and the kernel also
// correlates against the framing code and the leading payload. A ± 2 window
// therefore misses the framing code outright on a third of otherwise perfect
// lines, so the search spans ± 4 bit positions.
constexpr int kFramingSearchBits = 4;

// Weakest-byte eye margin below which kAuto also runs the MLSE detector on a
// line the threshold detector locked. Measured on synthesized lines band
// limited between 2,8 and 6 MHz, every threshold packet with a bit error sat
// below it and the clear ones sat above it once the channel passed 5 MHz.
constexpr double kAutoFallbackEyeMargin = 0.15;

// --- MLSE detector (TeletextDetector::kMlse) ------------------------------
//
// A channel that rolls off below the 3,47 MHz run-in fundamental (§6.1)
// smears each bit across its neighbours, so no per-sample threshold recovers
// the bits: the level at a bit centre depends on the bits around it.  The
// detector therefore models the channel explicitly.  Every line starts with
// the same 24 known bits (run-in then framing code), which is enough to fit a
// short linear response; the payload bits are then whichever sequence that
// fitted channel would most likely have produced.  Nothing is trained ahead
// of time and nothing is assumed about the recorder: each line carries the
// reference needed to fit its own channel.

// ETSI EN 300 706 §6.1 + §6.2: the known preamble the channel is fitted to.
constexpr int kPreambleBits = kRunInBits + kFramingBits;

// Channel response length in bit periods, and the index of its centre tap.
// Five taps span ±2 bit periods around the bit being detected: measured on
// VHS SP captures the response is essentially spent by then, and each extra
// tap doubles the Viterbi state count.
constexpr int kChannelTaps = 5;
constexpr int kChannelCentre = 2;

// Bits either side of the best acquisition fit whose shifts are read: the
// shift the free taps can absorb, which is the reach of the tap window from
// its centre. The candidates are the fit's own phase and every half-bit
// shift within that reach.
constexpr int kAlignmentBits = kChannelCentre;
constexpr int kAlignmentCandidates = 4 * kAlignmentBits + 1;

// Shifts read per line, and how many of them may supply a reading whose
// prefix needed correction. Measured on two EP tapes, the fourth and fifth
// best-fitting shifts still add packets that pass the NABTS longitudinal
// check where the first three do not, and the sixth onward add none.
constexpr int kAlignmentReadings = 5;
constexpr int kClaimedShifts = 2;

// Viterbi state = the kChannelTaps - 1 preceding bits.
constexpr int kMlseStates = 1 << (kChannelTaps - 1);

// Bit patterns a tap window can hold: a state and the bit entering it, indexed
// state * 2 + bit, oldest bit first.
constexpr int kTapPatterns = 2 * kMlseStates;

// Bit periods past the end of the packet the detector reads when the line is
// long enough to hold them. ETSI EN 300 706 §7.1: nothing follows the last
// packet bit, so the line is back at black level there and those samples are
// the known-bit evidence that pins the last payload bits. Filling the whole
// state register takes kChannelTaps - 1 of them.
constexpr int kTrailingBits = kChannelTaps - 1;

// Bits older than the tap window whose response the decision-directed refit
// models as well (see detect_mlse_at). They are not held in the Viterbi state,
// which would double its size for every one: each state reads them off its own
// survivor path instead, which is delayed decision-feedback sequence estimation
// (Duel-Hallen and Heegard, IEEE Trans. Commun. 37(5), 1989). A tape transfer
// through a recorder's own playback electronics and a card can ring for a
// microsecond after each bit: on one capture card dump of a tape the response
// undershoots by 4-6 % of the data step three to six bit periods after its
// peak, five taps leave that tail in the residual, and after the run of ones
// at the end of the framing code it pulls the first prefix byte wrong on 47 %
// of lines. Four taps reach the end of it: on that dump, read with two tail
// refits, three taps leave 6,1 bit errors a packet, four 5,8, five 5,9 and
// six 6,0.
constexpr int kChannelTailTaps = 4;

// Refits of the channel with its tail against the bits the detection before
// decided, at most: a refit fitted to fewer wrong bits describes the channel
// better, so they are repeated while the decisions keep changing. On the same
// card dump one leaves 8,3 bit errors a packet, two 5,8 and three 5,2; on a
// clean line the first decides what the five-tap refit before it did and the
// repetition stops there.
constexpr int kMaxChannelRefits = 3;

// The wide reading of a System C packet (see TeletextSlicer): a channel window
// reaching kWideAhead bits past the bit a sample belongs to, where the tap
// window reaches kChannelCentre, held in a trellis of kWideStates states, and
// kWideTailTaps bits behind the window read off each state's survivor path as
// the tail refits read theirs.
//
// The window is wider because some tape transfers need it. Fitted against the
// true bits of their packets (the pages they share with a tape transferred
// three ways, whose transfers agree on 99.5 % of their bytes), some VHS EP
// recordings respond to a bit three bit periods before its own sample by
// -0,03 to -0,08 of the data step, where the three transfers respond by
// +0,01, and their response dies away over some fifteen bits behind it. Read
// with a channel fitted to the true bits, the window of the tail refits
// leaves 3,4 bit errors in a packet of such a recording, this one 2,2; the
// packets of the three transfers lose a fifth of theirs too.
constexpr int kWideTaps = 6;
constexpr int kWideAhead = kWideTaps - 1 - kChannelCentre;
constexpr int kWideStates = 1 << (kWideTaps - 1);
constexpr int kWidePatterns = 2 * kWideStates;
constexpr int kWideTailTaps = 8;

// Refits of the wide channel against the bits the detection before decided, at
// most, repeated while they change them (see kMaxChannelRefits).
constexpr int kMaxWideRefits = 3;

// Least response to the bit kWideAhead periods on, as a fraction of the gain,
// that the wide channel fitted to a packet's bits must show for the packet to
// be read wide. The EP recordings that need it respond to it by -0,065 at the
// median and beyond -0,04 on nine packets in ten, where cleaner transfers
// respond by -0,013 to +0,012. Read wide wherever their checks failed, three
// transfers of one tape came out with 79 bytes made wrong for 21 put right,
// judged by each other; held to this, with 1 made wrong.
constexpr double kWideMinAheadFraction = 0.03;

// Upper bound on TeletextSlicerOptions::mlse_samples_per_bit, which sets how
// many evenly spaced positions within each bit period the detector scores.
// At the PAL 4FSC rate a bit spans ≈ 2.556 samples and at the NTSC one exactly
// 2.5, so beyond three grid points per bit two of them fall between the same
// pair of captured samples and carry no independent information.
constexpr int kMaxMlseSamplesPerBit = 3;

// Bit-phase search step for the preamble fit, in samples. Finer than the
// threshold detector's quarter sample because the fit residual — not a
// correlation peak — is what discriminates, and it sharpens with phase.
constexpr double kMlsePhaseStep = 0.2;

// Minimum fitted channel gain (sum of taps = the modelled black-to-'1' step)
// as a fraction of the nominal '1' amplitude. A fit that explains the
// preamble with almost no gain has locked onto flat noise.
constexpr double kMlseMinGainFraction = 0.25;

// Maximum RMS preamble-fit residual as a fraction of the fitted gain. A real
// preamble under a five-tap linear model leaves a small residual; picture
// content that happens to sit under the search window does not fit at all.
// Measured over VHS SP and LP captures: locks that yield packets passing
// their parity checks sit below 0.15, and beyond 0.20 the recovered bytes are
// noise. Set at the upper end of that range because the row squasher outvotes
// the occasional bad packet, whereas a packet never recovered is simply lost.
constexpr double kMlseMaxResidualFraction = 0.20;

// Maximum RMS payload reconstruction error, as a fraction of the fitted gain,
// for the recovered bits to be believed. Measured across VHS SP, VHS LP and
// LaserDisc captures, real packets sit below 0.4.
constexpr double kMlseMaxPayloadResidualFraction = 0.5;

// How much better another service's framing code must explain the framing
// samples before an MLSE lock is rejected as being that other service's line,
// in units of the fitted channel's response to one bit (channel_bit_energy).
//
// System B and System C differ in two of the eight framing bits, so a line of
// the wrong service costs about two bit energies; a same-service line whose
// framing bits the recording has damaged costs a fraction of one.
//
// The value is a floor, not a midpoint: a larger margin rejects less, so
// raising it leaks more of the other service's lines through. Measured on the
// reference captures, a 525-line WST recording read as System C yields 126
// packets at 1,25 against 133 at 1,5 and 268 at 4,0. Below 1,25 the exhaustive
// 625-line WST decode starts losing its own marginal packets — 3963 of 3964 at
// 1,0 — so 1,25 is the smallest margin that costs the WST services nothing.
//
// The gate does not close the leak on its own and is not expected to. On the
// same WST recording the threshold detector, which matches the framing code bit
// by bit, produced zero false locks; every one of the 126 came from the MLSE
// detector, which fits the framing code rather than matching it. What stops
// them being mistaken for a service is the five-byte Hamming prefix gate
// (1559 rejections on that recording against this gate's 91) and the resulting
// yield: 126 packets at a mean decision confidence of 0,21, against 2460 at
// 0,56 from a real System C recording over the same number of frames.
constexpr double kFramingDiscriminationBits = 1.25;

// Framing-code bit errors a reading whose prefix needed correction may carry
// when read back (see read_back_sync): CEA-516 §2.2.3 and §1.2.2 of the 1976
// Broadcast Teletext Specification chose framing codes that survive one wrong
// bit. A prefix that arrived as sent is not read back. Its Hamming 8/4
// codewords (§8.2) match a line carrying no packet once in 256 on System B and
// once in a million on System C, while on a VHS EP recording a third of the
// packets whose every byte passed parity read the last five framing bits back
// as 01010, the ringing of the 111 before them.
constexpr int kMlseMaxFramingErrors = 1;

// Index of the packet structure byte PS in a System C payload: the fifth and
// last byte of the packet prefix (CEA-516 §3.2.5).
constexpr size_t kNabtsPacketStructureByte = 4;

// The System C packet prefix in bits — P1-P3, CI and PS, CEA-516 §3.2.1. The
// data block and the suffix follow it, and the packet's own checks cover them
// and nothing else (§3.3, §3.4).
constexpr int kNabtsPrefixBits = (kNabtsPacketStructureByte + 1) * 8;

// CEA-516 §3.4: PS b8,b6 = 0,1 announces one suffix byte, the longitudinal
// parity check, chosen so that the exclusive-or of the data block and the
// suffix is 0xFF. The other suffix codes are not this check on the recordings:
// on services that use them, packets coded 1,0 pass it well under 1 % of the
// time, their data bytes odd about half the time — a forward error correction
// of the encoder's own.
constexpr uint8_t kNabtsLongitudinalCheck = 0xFF;

// Last data byte of a packet with a one-byte suffix: the data block runs from
// the prefix to it (§3.3), and every byte of a type-zero data group's data
// block carries odd parity; the suffix byte after it carries the check.
constexpr int kNabtsLastCheckedDataByte = 31;

// Most extra cost, in units of the fitted channel's response to one bit
// (channel_bit_energy), at which the reading the packet's own checks accept is
// taken over the one the detector decided (see the class comment). A wrong bit
// costs an undamaged line a whole bit's energy, so a reading within this of the
// decided one is one the samples hardly tell from it.
//
// Two limits, because the checks say more about where the error is in one case
// than the other. Where a data byte failed its parity, that byte is where the
// error is, and the reading that mends it is nearly always the right one:
// across a bench of recordings sent with the check, 0,75 put right 9 packets
// in 10 that it changed. Where every data byte passed and only the
// longitudinal check failed, the eight bits of the check say which bits are
// wrong but not in which byte, and an error mended in the wrong byte passes
// every check afterwards — on a damaged tape, 1,6 such readings in 2 taken
// between 0,25 and 0,75 were wrong — so a data byte is changed there only when
// the samples were nearly undecided between the two readings.
constexpr double kMlseMaxCheckedCost = 0.75;
constexpr double kMlseMaxUnlocatedCheckedCost = 0.2;

// How far past kMlseMaxCheckedCost the search for that reading looks. The
// search prices the bits before the tap window (kChannelTailTaps) along the
// decided reading rather than its own, so its costs are a little off for a
// reading that differs; the one it returns is costed exactly before it is
// judged, and the margin keeps a reading just inside the limit from being
// lost to that approximation.
constexpr double kMlseCheckedSearchMargin = 0.25;

// Bits per segment over which the timing step (see detect_mlse_at) measures
// where the line's bits actually fall. A tape's playback speed varies within a
// line: fitted against the packets' own bits on VHS recordings, the
// offset between the line's first and last bits spreads with a standard
// deviation of 0,4 samples (a sixth of a bit) on packets read exactly and 0,7
// on packets read with errors, while what is left about the straight line
// through 16-bit segments, 0,15 samples, is the measurement's own noise — the
// segments' offsets about it are uncorrelated (-0,13 from one to the next) —
// so a straight line is all the warp the data supports.
constexpr int kTimingSegmentBits = 16;

// Gauss-Newton steps per segment, and the largest move one may make, in
// samples: the offsets sought are a fraction of a bit, and a larger step is
// the fit chasing a wrong decision rather than the timing.
constexpr int kTimingSteps = 2;
constexpr double kTimingMaxStep = 0.5;

// Leading clock run-in bits the read-back does not judge. ETSI EN 300 706 §6.1
// notes that the two leading ones may be absent or reduced in amplitude.
constexpr int kRunInUnjudgedBits = 4;

// Bits of the rest of the clock run-in, as read back, that may break its
// alternation (CEA-516 §2.2.2, EN 300 706 §6.1). §1 of the 1976 Broadcast
// Teletext Specification identifies a data line by the run-in followed by the
// framing code, and a reading whose prefix needed correction has only this to
// show it is one. Measured on three VHS EP recordings, 99 % of packets with a
// prefix as sent read back within two bits, and the vertical-interval test
// signal a fit mistakes for a packet reads back at six: its ripple runs at
// half the run-in rate.
constexpr int kMlseMaxRunInErrors = 2;

// Minimum fraction of the data bytes (40 of them on 625 lines, 32 on 525) that
// must carry odd parity for an MLSE-recovered packet in a parity-coded row.
//
// ETSI EN 300 706 §9.3.1 gives the display bytes odd parity — ITU-R BT.653
// Table 1b says the same of the 525-line service — and every Hamming 8/4
// codeword (§8.2) is odd-parity as well, so an undamaged packet
// in rows 0-25 has all its data bytes odd — confirmed at exactly 1,000 across
// 130 000 packets of an independently decoded reference stream. Random bytes
// sit at 0,5, so this rejects a false lock with high confidence while leaving
// room for a genuinely damaged packet the row squasher can still repair.
// The threshold detector needs no equivalent: its exact framing-code match
// (§6.2) already rules out noise.
constexpr double kMlseMinDataParityFraction = 0.6;

// ETSI EN 300 706 §7.1.2: the packet opens with the two Hamming 8/4 coded
// magazine/row address bytes.
constexpr size_t kMragBytes = 2;

// ETSI EN 300 706 §9.3.1: a page header (X/0) spends its first ten bytes on
// addressing and control — the MRAG, the two page-number bytes, the four
// sub-code bytes and the two control-bit bytes — all Hamming 8/4 coded. The
// bytes that follow are the odd-parity header text (32 of them on 625 lines,
// 24 on 525; the addressing is the same either way, ITU-R BT.653 Table 1b
// §3.3).
constexpr size_t kHeaderControlBytes = 10;

// Rows above this carry Hamming 24/18 triplets (§9.6 packets X/26 to X/29) or
// independent data services (§9.8 packet X/31), neither of which is byte-wise
// odd parity, so the parity gate does not apply to them.
constexpr int kMlseLastParityCodedRow = 25;

// Observation-string encoding: two hex characters per packet byte, optionally
// followed by one per byte of quantised confidence.
constexpr char kHexDigits[] = "0123456789abcdef";

// Packet lengths a teletext service transmits, and therefore the only ones an
// observation string may encode. The six resulting string lengths — 66, 68,
// 84, 99, 102 and 126 characters — are distinct, which is what lets a string
// be decoded without being told which system produced it.
constexpr size_t kPacketByteLengths[] = {
    kTeletextPacketBytes, kTeletext525PacketBytes, kNabtsPacketBytes};

// The claim above, checked rather than asserted: no packet length's plain
// encoding collides with another's confidence-suffixed one.
constexpr bool packet_string_lengths_are_distinct() {
  for (size_t i = 0; i < std::size(kPacketByteLengths); ++i) {
    for (size_t j = 0; j < std::size(kPacketByteLengths); ++j) {
      if (i != j && kPacketByteLengths[i] * 2 == kPacketByteLengths[j] * 3) {
        return false;
      }
      if (i < j && kPacketByteLengths[i] == kPacketByteLengths[j]) {
        return false;
      }
    }
  }
  return true;
}
static_assert(packet_string_lengths_are_distinct(),
              "an observation string length would decode as two different "
              "packet lengths");

// Value of one hex character (either case), or -1 when it is not one.
inline int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Mark |result| rejected for |reason| and hand it back to the caller. Every
// early return of both detectors goes through here so no path can leave the
// reason unset.
inline TeletextLineResult reject(TeletextLineResult result,
                                 TeletextRejectReason reason) {
  result.valid = false;
  result.reject_reason = reason;
  return result;
}

// ETSI EN 300 706 §9.3.1: a transmitted display byte carries odd parity over
// all eight bits.
inline bool odd_parity(uint8_t byte) {
  int ones = 0;
  for (int bit = 0; bit < 8; ++bit) {
    ones += (byte >> bit) & 1;
  }
  return (ones & 1) == 1;
}

// The known preamble bit at index |k| (0 … kPreambleBits - 1): the alternating
// clock run-in followed by the service's framing code.
inline int preamble_bit(int k, const FramingCode& framing) {
  return k < kRunInBits ? ((k % 2 == 0) ? 1 : 0)
                        : framing[static_cast<size_t>(k - kRunInBits)];
}

// Linear interpolation between adjacent samples at fractional position |t|.
// Caller guarantees t >= 0 and t + 1 < sample_count.
inline double sample_at(const int16_t* line, double t) {
  const auto i = static_cast<size_t>(t);
  const double frac = t - static_cast<double>(i);
  return static_cast<double>(line[i]) +
         (static_cast<double>(line[i + 1]) - static_cast<double>(line[i])) *
             frac;
}

// Offset of grid phase |p| of |phases| from the bit centre, in bit periods.
// The phases straddle the centre symmetrically, so phases == 1 is the bit
// centre itself and the detector reduces to its pre-fractional form.
inline double phase_offset(int p, int phases) {
  return (static_cast<double>(p) - 0.5 * static_cast<double>(phases - 1)) /
         static_cast<double>(phases);
}

// Index of bit |bit| at phase |p| in a |phases|-per-bit grid.
inline size_t grid_index(int bit, int p, int phases) {
  return static_cast<size_t>(bit) * static_cast<size_t>(phases) +
         static_cast<size_t>(p);
}

// Catmull-Rom cubic through |p0| … |p3| over the interval from |p1| to |p2|,
// as the coefficients a, b, c and p1 of its Horner form. Shared by
// teletext_interpolate_sample and the grid resampler, so both give the same
// value for the same point.
inline std::array<double, 4> catmull_rom_coefficients(double p0, double p1,
                                                      double p2, double p3) {
  return {-0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3,
          p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3, -0.5 * p0 + 0.5 * p2, p1};
}

// The cubic with coefficients |c| at fraction |x| of the way along its
// interval.
inline double catmull_rom_at(const double* c, double x) {
  return ((c[0] * x + c[1]) * x + c[2]) * x + c[3];
}

// Four doubles side by side, on AVX2. The x86-64 baseline the build targets
// has two-lane vectors; AVX2 has four, so on a processor that offers it
// (every x86-64 one sold since 2013-15, asked once: avx2_available()) the
// detector works out four of its numbers at a time — two trellis states'
// pairs of branches, or four candidate phases' preamble fits. Each lane takes
// exactly the operations the scalar or two-lane form would, in the same
// order, and AVX2 alone, without the FMA extension, which nothing here
// enables, has no instruction that fuses a multiply into an add: a line is
// read the same to the bit whichever form ran. GCC and Clang compile the
// functions marked for AVX2 alongside the rest; elsewhere only the other
// forms exist.
#if defined(__GNUC__) && defined(__x86_64__) && defined(__has_builtin)
#if __has_builtin(__builtin_shufflevector) && \
    __has_builtin(__builtin_convertvector)
#define ORC_TELETEXT_AVX2 1
#endif
#endif
#if defined(ORC_TELETEXT_AVX2)

bool avx2_available() {
  static const bool available = __builtin_cpu_supports("avx2") != 0;
  return available;
}

using QuadLanes = double __attribute__((vector_size(4 * sizeof(double))));
using QuadMask = decltype(QuadLanes{} < QuadLanes{});
using HalfLanes = double __attribute__((vector_size(2 * sizeof(double))));

// The bits of |from| as a |To|: the same size, another lane type.
template <typename To, typename From>
[[gnu::target("avx2"), gnu::always_inline]] inline To lanes_as(From from) {
  static_assert(sizeof(To) == sizeof(From), "a cast keeps every bit");
  To to;
  std::memcpy(&to, &from, sizeof(to));
  return to;
}
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_load(
    const double* from) {
  QuadLanes quad;
  std::memcpy(&quad, from, sizeof(quad));
  return quad;
}
// The pair at |low| and the pair at |high| as one quad.
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_join(
    const double* low, const double* high) {
  HalfLanes a;
  HalfLanes b;
  std::memcpy(&a, low, sizeof(a));
  std::memcpy(&b, high, sizeof(b));
  return __builtin_shufflevector(a, b, 0, 1, 2, 3);
}
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_splat(
    double value) {
  return QuadLanes{value, value, value, value};
}
[[gnu::target("avx2"), gnu::always_inline]] inline void quad_store(
    double* to, QuadLanes quad) {
  std::memcpy(to, &quad, sizeof(quad));
}
// Bit i set where lane i of |a| is less than that of |b|.
[[gnu::target("avx2"), gnu::always_inline]] inline unsigned quad_less(
    QuadLanes a, QuadLanes b) {
  return static_cast<unsigned>(
      __builtin_ia32_movmskpd256(lanes_as<QuadLanes>(a < b)));
}
// Lane by lane, |a| where |mask| is set, else |b|: each lane's bits taken
// whole from one or the other.
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_select(
    QuadMask mask, QuadLanes a, QuadLanes b) {
  return lanes_as<QuadLanes>((lanes_as<QuadMask>(a) & mask) |
                             (lanes_as<QuadMask>(b) & ~mask));
}
// Lane by lane, |a| where it is less than |b|, else |b|.
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_min(
    QuadLanes a, QuadLanes b) {
  return quad_select(a < b, a, b);
}
#else
constexpr bool avx2_available() { return false; }
#endif

#if defined(ORC_TELETEXT_AVX2)
// Four samples from |from| on, as doubles.
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_widen(
    const int16_t* from) {
  using SampleLanes = int16_t __attribute__((vector_size(4 * sizeof(int16_t))));
  SampleLanes lanes;
  std::memcpy(&lanes, from, sizeof(lanes));
  return __builtin_convertvector(lanes, QuadLanes);
}

// cubic_coefficients() four intervals at a time, lane i of each sum the
// interval at |first| + i, worked out as catmull_rom_coefficients() works out
// one; the coefficients then stored interval by interval. Returns the first
// interval it left for the caller.
[[gnu::target("avx2")]] size_t cubic_coefficients_avx2(const int16_t* line,
                                                       size_t sample_count,
                                                       size_t first,
                                                       double* out) {
  size_t i = first;
  for (; i + 5 < sample_count; i += 4) {
    const QuadLanes p0 = quad_widen(line + i - 1);
    const QuadLanes p1 = quad_widen(line + i);
    const QuadLanes p2 = quad_widen(line + i + 1);
    const QuadLanes p3 = quad_widen(line + i + 2);
    const QuadLanes a = quad_splat(-0.5) * p0 + quad_splat(1.5) * p1 -
                        quad_splat(1.5) * p2 + quad_splat(0.5) * p3;
    const QuadLanes b =
        p0 - quad_splat(2.5) * p1 + quad_splat(2.0) * p2 - quad_splat(0.5) * p3;
    const QuadLanes c = quad_splat(-0.5) * p0 + quad_splat(0.5) * p2;
    const QuadLanes d = p1;
    const QuadLanes ab_even = __builtin_shufflevector(a, b, 0, 4, 2, 6);
    const QuadLanes ab_odd = __builtin_shufflevector(a, b, 1, 5, 3, 7);
    const QuadLanes cd_even = __builtin_shufflevector(c, d, 0, 4, 2, 6);
    const QuadLanes cd_odd = __builtin_shufflevector(c, d, 1, 5, 3, 7);
    double* at = out + 4 * i;
    quad_store(at, __builtin_shufflevector(ab_even, cd_even, 0, 1, 4, 5));
    quad_store(at + 4, __builtin_shufflevector(ab_odd, cd_odd, 0, 1, 4, 5));
    quad_store(at + 8, __builtin_shufflevector(ab_even, cd_even, 2, 3, 6, 7));
    quad_store(at + 12, __builtin_shufflevector(ab_odd, cd_odd, 2, 3, 6, 7));
  }
  return i;
}
#endif

// The catmull_rom_coefficients of every interval of |line| whose four-point
// window lies inside it, at out[4 * i] for the interval that starts at sample
// i. They depend on the four samples around the interval alone, and the MLSE
// detector reads a line at some ten thousand positions — acquisition resamples
// ~300 candidate phases, and each of up to kAlignmentReadings readings the
// whole packet at three phases — so they are worked out once per interval
// rather than once per read.
void cubic_coefficients(const int16_t* line, size_t sample_count,
                        std::vector<double>& out, bool avx2) {
  out.resize(4 * sample_count);
  size_t first = 1;
#if defined(ORC_TELETEXT_AVX2)
  if (avx2) {
    first = cubic_coefficients_avx2(line, sample_count, first, out.data());
  }
#endif
  for (size_t i = first; i + 2 < sample_count; ++i) {
    const auto c = catmull_rom_coefficients(
        static_cast<double>(line[i - 1]), static_cast<double>(line[i]),
        static_cast<double>(line[i + 1]), static_cast<double>(line[i + 2]));
    std::copy(c.begin(), c.end(), out.begin() + static_cast<ptrdiff_t>(4 * i));
  }
}

#if defined(ORC_TELETEXT_AVX2)
// The grid positions |t| of four samples read from the cubics of the
// intervals they fall in (catmull_rom_at()), a lane each; every position must
// have its four-point window inside the line.
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_cubic_at(
    const std::vector<double>& cubic, QuadLanes t) {
  using Intervals = int32_t __attribute__((vector_size(4 * sizeof(int32_t))));
  const Intervals base = __builtin_convertvector(t, Intervals);
  const QuadLanes x = t - __builtin_convertvector(base, QuadLanes);
  const QuadLanes c0 = quad_load(&cubic[4 * static_cast<size_t>(base[0])]);
  const QuadLanes c1 = quad_load(&cubic[4 * static_cast<size_t>(base[1])]);
  const QuadLanes c2 = quad_load(&cubic[4 * static_cast<size_t>(base[2])]);
  const QuadLanes c3 = quad_load(&cubic[4 * static_cast<size_t>(base[3])]);
  // The coefficients by power, a lane per position.
  const QuadLanes low_even = __builtin_shufflevector(c0, c1, 0, 4, 2, 6);
  const QuadLanes low_odd = __builtin_shufflevector(c0, c1, 1, 5, 3, 7);
  const QuadLanes high_even = __builtin_shufflevector(c2, c3, 0, 4, 2, 6);
  const QuadLanes high_odd = __builtin_shufflevector(c2, c3, 1, 5, 3, 7);
  const QuadLanes a = __builtin_shufflevector(low_even, high_even, 0, 1, 4, 5);
  const QuadLanes b = __builtin_shufflevector(low_odd, high_odd, 0, 1, 4, 5);
  const QuadLanes c = __builtin_shufflevector(low_even, high_even, 2, 3, 6, 7);
  const QuadLanes d = __builtin_shufflevector(low_odd, high_odd, 2, 3, 6, 7);
  return ((a * x + b) * x + c) * x + d;
}
#endif

// Resample |bit_count| bit periods of |line|, starting from the bit whose
// centre is at sample |t0|, onto a fixed |phases|-per-bit grid (at most
// kMaxMlseSamplesPerBit). Output element k * phases + p holds bit k at phase
// p. Written into |out| (resized to fit) so a caller sweeping candidate phases
// can reuse one buffer. |cubic| holds the line's cubic_coefficients.
//
// Doing this once per lock, rather than interpolating inside the trellis,
// keeps the interpolation cost linear in the packet length however many times
// the detector reads a sample.
void resample_bit_grid(const int16_t* line, size_t sample_count,
                       const std::vector<double>& cubic, double t0, double spb,
                       int bit_count, int phases, std::vector<double>& out) {
  out.resize(static_cast<size_t>(bit_count) * static_cast<size_t>(phases));
  std::array<double, kMaxMlseSamplesPerBit> offsets{};
  for (int p = 0; p < phases; ++p) {
    offsets[static_cast<size_t>(p)] = phase_offset(p, phases);
  }
  // Every read of a data burst falls where the whole four-point window is
  // inside the line, and there the position is not negative, so truncation is
  // its floor and the interval's coefficients are at hand. The rest go through
  // teletext_interpolate_sample, which clamps the window.
  const double interior_end = static_cast<double>(sample_count) - 2.0;
  for (int k = 0; k < bit_count; ++k) {
    for (int p = 0; p < phases; ++p) {
      const double t =
          t0 + (static_cast<double>(k) + offsets[static_cast<size_t>(p)]) * spb;
      double value = 0.0;
      if (t >= 1.0 && t < interior_end) {
        const auto base = static_cast<size_t>(t);
        const double x = t - static_cast<double>(base);
        value = catmull_rom_at(&cubic[4 * base], x);
      } else {
        value = teletext_interpolate_sample(line, sample_count, t);
      }
      out[grid_index(k, p, phases)] = value;
    }
  }
}

// Channel model unknowns: kChannelTaps bit-spaced taps for each sample phase
// within the bit period, plus one DC offset shared by all phases (the black
// level the burst sits on, which the caller's nominal levels only approximate
// on tape; it is a property of the line, not of where in a bit it is read).
constexpr int kMaxFitUnknowns = kChannelTaps * kMaxMlseSamplesPerBit + 1;

// Bit positions a fit can take equations from: the whole grid of the longest
// packet and the known black bits after it.
constexpr int kMaxFitPositions =
    kPreambleBits + kTeletextPayloadBits + kTrailingBits;

struct ChannelFit {
  bool ok = false;
  // Sample phases the fit covers; taps[p] is meaningful for p < phases.
  int phases = 1;
  std::array<std::array<double, kChannelTaps>, kMaxMlseSamplesPerBit> taps{};
  double offset = 0.0;
  // Mean over the phases of the summed taps: the modelled black-to-'1' step of
  // the recovered data. The tap sum is the channel's DC response, so it is the
  // same quantity at every phase and averaging simply steadies it.
  double gain = 0.0;
  // RMS fit residual over the preamble, in level-domain counts, taken over
  // every phase — the same per-sample scale as the single-phase fit, so the
  // residual gates carry across unchanged.
  double residual = 0.0;
  // Whether |tail| is part of the model (fit_channel_with_tail): tail[p][d] is
  // the response at phase p to the bit d + 1 periods older than the oldest bit
  // of the tap window (see kChannelTailTaps). Its taps count towards |gain|.
  bool has_tail = false;
  std::array<std::array<double, kChannelTailTaps>, kMaxMlseSamplesPerBit>
      tail{};
};

using FitMatrix =
    std::array<std::array<double, kMaxFitUnknowns>, kMaxFitUnknowns>;
using FitVector = std::array<double, kMaxFitUnknowns>;

// Gaussian elimination with partial pivoting of the leading |n| by |n| block
// of a matrix, recorded so that it can be applied to any right-hand side: the
// pivot row chosen for each column, the multiple of the pivot row taken from
// each row below it, and the upper triangle left behind. None of that reads
// the right-hand side, so eliminating once and solving many times performs on
// each right-hand side exactly the operations, in exactly the order, that
// eliminating alongside it would.
struct Elimination {
  // False when the matrix is singular to working precision.
  bool ok = false;
  int n = 0;
  std::array<int, kMaxFitUnknowns> pivot{};
  // factor[col][row] for row > col.
  FitMatrix factor{};
  FitMatrix upper{};

  // Solve for |b| in place.
  void solve(FitVector& b) const {
    for (int col = 0; col < n; ++col) {
      const auto c = static_cast<size_t>(col);
      std::swap(b[c], b[static_cast<size_t>(pivot[c])]);
      for (int row = col + 1; row < n; ++row) {
        const double f = factor[c][static_cast<size_t>(row)];
        if (f != 0.0) {
          b[static_cast<size_t>(row)] -= f * b[c];
        }
      }
    }
    for (int row = n - 1; row >= 0; --row) {
      const auto r = static_cast<size_t>(row);
      double sum = b[r];
      for (int k = row + 1; k < n; ++k) {
        sum -= upper[r][static_cast<size_t>(k)] * b[static_cast<size_t>(k)];
      }
      b[r] = sum / upper[r][r];
    }
  }
};

Elimination eliminate(FitMatrix a, int n) {
  Elimination e;
  e.n = n;
  for (int col = 0; col < n; ++col) {
    const auto c = static_cast<size_t>(col);
    int pivot = col;
    for (int row = col + 1; row < n; ++row) {
      if (std::abs(a[static_cast<size_t>(row)][c]) >
          std::abs(a[static_cast<size_t>(pivot)][c])) {
        pivot = row;
      }
    }
    if (std::abs(a[static_cast<size_t>(pivot)][c]) < 1e-9) {
      return e;
    }
    e.pivot[c] = pivot;
    std::swap(a[c], a[static_cast<size_t>(pivot)]);
    for (int row = col + 1; row < n; ++row) {
      const auto r = static_cast<size_t>(row);
      const double factor = a[r][c] / a[c][c];
      e.factor[c][r] = factor;
      if (factor == 0.0) {
        continue;
      }
      for (int k = col; k < n; ++k) {
        a[r][static_cast<size_t>(k)] -= factor * a[c][static_cast<size_t>(k)];
      }
    }
  }
  e.upper = a;
  e.ok = true;
  return e;
}

// The equations of a channel fit over known bits (see fit_channel_range):
// which tap-window pattern each contributing bit position holds, how often
// each pattern occurs, and the normal matrix, which counts the equations in
// which two taps both saw a one and so depends on the bits alone.
struct FitSystem {
  bool ok = false;
  int phases = 1;
  int first = 0;
  int last = -1;
  std::array<int, kTapPatterns> occurrences{};
  std::array<uint8_t, kMaxFitPositions> pattern_at{};
  FitMatrix normal{};
  // The positions at which each tap saw a one, in order, as offsets from
  // |first|: the only equations that tap's share of the right-hand side sums.
  std::array<std::array<uint16_t, kMaxFitPositions>, kChannelTaps> ones_at{};
  std::array<int, kChannelTaps> ones{};
  // The patterns that occur at all.
  std::array<uint8_t, kTapPatterns> used_patterns{};
  int used = 0;
};

// Tap j of the window |pattern| (oldest bit first, as fit_channel_range
// builds it).
inline int tap_bit(int pattern, int j) {
  return (pattern >> (kChannelTaps - 1 - j)) & 1;
}

template <typename BitAt>
FitSystem build_fit_system(int phases, BitAt bit_at, int first, int last) {
  FitSystem system;
  system.phases = phases;
  system.first = first;
  system.last = last;
  const int unknowns = kChannelTaps * phases + 1;
  const int offset_index = unknowns - 1;
  const int positions = last - first + 1;
  if (positions <= 0 || positions > kMaxFitPositions) {
    return system;
  }
  // The window slides a bit at a time, so each position reads one new bit.
  int pattern = 0;
  for (int j = 0; j < kChannelTaps - 1; ++j) {
    pattern = (pattern << 1) | bit_at(first - kChannelCentre + j);
  }
  for (int k = first; k <= last; ++k) {
    pattern = ((pattern << 1) | bit_at(k - kChannelCentre + kChannelTaps - 1)) &
              (kTapPatterns - 1);
    ++system.occurrences[static_cast<size_t>(pattern)];
    system.pattern_at[static_cast<size_t>(k - first)] =
        static_cast<uint8_t>(pattern);
    for (int j = 0; j < kChannelTaps; ++j) {
      if (tap_bit(pattern, j) != 0) {
        auto& count = system.ones[static_cast<size_t>(j)];
        system.ones_at[static_cast<size_t>(j)][static_cast<size_t>(count++)] =
            static_cast<uint16_t>(k - first);
      }
    }
  }
  for (int pattern = 0; pattern < kTapPatterns; ++pattern) {
    if (system.occurrences[static_cast<size_t>(pattern)] != 0) {
      system.used_patterns[static_cast<size_t>(system.used++)] =
          static_cast<uint8_t>(pattern);
    }
  }
  // Every phase's block of taps counts the same windows, so the first is
  // counted and the rest are copies of it.
  auto& normal = system.normal;
  const auto offset_at = static_cast<size_t>(offset_index);
  for (int pattern = 0; pattern < kTapPatterns; ++pattern) {
    const int count = system.occurrences[static_cast<size_t>(pattern)];
    if (count == 0) {
      continue;
    }
    for (int i = 0; i < kChannelTaps; ++i) {
      if (tap_bit(pattern, i) == 0) {
        continue;
      }
      const auto row = static_cast<size_t>(i);
      for (int j = 0; j < kChannelTaps; ++j) {
        if (tap_bit(pattern, j) != 0) {
          normal[row][static_cast<size_t>(j)] += static_cast<double>(count);
        }
      }
      normal[row][offset_at] += static_cast<double>(count);
      normal[offset_at][row] += static_cast<double>(count);
    }
  }
  for (int p = 1; p < phases; ++p) {
    const auto block = static_cast<size_t>(p) * kChannelTaps;
    for (size_t i = 0; i < kChannelTaps; ++i) {
      for (size_t j = 0; j < kChannelTaps; ++j) {
        normal[block + i][block + j] = normal[i][j];
      }
      normal[block + i][offset_at] = normal[i][offset_at];
      normal[offset_at][block + i] = normal[offset_at][i];
    }
  }
  normal[static_cast<size_t>(offset_index)][static_cast<size_t>(offset_index)] =
      static_cast<double>(positions * phases);
  system.ok = true;
  return system;
}

// For each of the |kPhases| sample phases, the sum in order of the samples of
// the |count| bits listed at |positions|, each taken |shift| bits earlier:
// sample p of bit n is samples[n * kPhases + p]. The phases' sums are
// independent, so they are added side by side, each in the order it would be
// on its own.
template <int kPhases>
std::array<double, kMaxMlseSamplesPerBit> phase_sums(const double* samples,
                                                     const uint16_t* positions,
                                                     int count, int shift) {
  std::array<double, kMaxMlseSamplesPerBit> sums{};
  double first = 0.0;
  double second = 0.0;
  double third = 0.0;
  for (int j = 0; j < count; ++j) {
    const double* at =
        samples + (static_cast<ptrdiff_t>(positions[j]) - shift) * kPhases;
    first += at[0];
    if constexpr (kPhases > 1) {
      second += at[1];
    }
    if constexpr (kPhases > 2) {
      third += at[2];
    }
  }
  sums[0] = first;
  sums[1] = second;
  sums[2] = third;
  return sums;
}

std::array<double, kMaxMlseSamplesPerBit> phase_sums(int phases,
                                                     const double* samples,
                                                     const uint16_t* positions,
                                                     int count, int shift) {
  static_assert(kMaxMlseSamplesPerBit == 3, "a sum for each phase");
  switch (phases) {
    case 1:
      return phase_sums<1>(samples, positions, count, shift);
    case 2:
      return phase_sums<2>(samples, positions, count, shift);
    default:
      return phase_sums<3>(samples, positions, count, shift);
  }
}

// Least-squares fit of the channel over the equations of |system|, whose
// normal matrix |elimination| has eliminated, to the samples of |grid|. The
// residual is worked out only |with_residual|: the preamble fits are judged by
// it, and the refits to decided bits by the decisions they lead to.
ChannelFit solve_fit_system(const std::vector<double>& grid,
                            const FitSystem& system,
                            const Elimination& elimination,
                            bool with_residual = true) {
  ChannelFit fit;
  const int phases = system.phases;
  fit.phases = phases;
  if (!system.ok || !elimination.ok) {
    return fit;
  }
  const int offset_index = kChannelTaps * phases;
  const int first = system.first;
  const int last = system.last;
  const int positions = last - first + 1;

  // Only the right-hand side reads the samples: each tap's share sums the
  // samples of the equations in which it saw a one, in position order. A tap
  // that saw a zero adds nothing, and each sum starts at +0, so leaving those
  // terms out changes no bit of it.
  FitVector rhs{};
  const double* samples = &grid[grid_index(first, 0, phases)];
  for (int j = 0; j < kChannelTaps; ++j) {
    const std::array<double, kMaxMlseSamplesPerBit> sums = phase_sums(
        phases, samples, system.ones_at[static_cast<size_t>(j)].data(),
        system.ones[static_cast<size_t>(j)], 0);
    for (int p = 0; p < phases; ++p) {
      rhs[static_cast<size_t>(p) * kChannelTaps + static_cast<size_t>(j)] =
          sums[static_cast<size_t>(p)];
    }
  }
  {
    double sum = 0.0;
    const size_t samples_used =
        static_cast<size_t>(positions) * static_cast<size_t>(phases);
    for (size_t i = 0; i < samples_used; ++i) {
      sum += samples[i];
    }
    rhs[static_cast<size_t>(offset_index)] = sum;
  }
  elimination.solve(rhs);

  for (int p = 0; p < phases; ++p) {
    double phase_gain = 0.0;
    for (int j = 0; j < kChannelTaps; ++j) {
      const int tap_index = p * kChannelTaps + j;
      const double tap = rhs[static_cast<size_t>(tap_index)];
      fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] = tap;
      phase_gain += tap;
    }
    fit.gain += phase_gain;
  }
  fit.gain /= static_cast<double>(phases);
  fit.offset = rhs[static_cast<size_t>(offset_index)];
  if (!with_residual) {
    fit.ok = true;
    return fit;
  }

  // The modelled sample likewise depends only on the pattern and the phase,
  // and is read only for the patterns that occur, so only those are filled.
  std::array<std::array<double, kMaxMlseSamplesPerBit>, kTapPatterns> predicted;
  for (int u = 0; u < system.used; ++u) {
    const int pattern = system.used_patterns[static_cast<size_t>(u)];
    for (int p = 0; p < phases; ++p) {
      double value = fit.offset;
      for (int j = 0; j < kChannelTaps; ++j) {
        value += fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] *
                 static_cast<double>(tap_bit(pattern, j));
      }
      predicted[static_cast<size_t>(pattern)][static_cast<size_t>(p)] = value;
    }
  }
  double sum_sq = 0.0;
  for (int k = first; k <= last; ++k) {
    const auto& row =
        predicted[system.pattern_at[static_cast<size_t>(k - first)]];
    for (int p = 0; p < phases; ++p) {
      const double error =
          grid[grid_index(k, p, phases)] - row[static_cast<size_t>(p)];
      sum_sq += error * error;
    }
  }
  fit.residual = std::sqrt(sum_sq / static_cast<double>(positions * phases));
  fit.ok = true;
  return fit;
}

// Least-squares fit of the channel response to the known preamble, read off a
// |phases|-per-bit grid that starts at the first run-in bit (see
// resample_bit_grid). Only preamble positions whose whole tap window falls
// inside the known bits contribute equations; with five taps that is 20 of the
// 24 bits, so the fit has 20 × |phases| equations for kChannelTaps × |phases|
// + 1 unknowns.
//
// Each phase gets its own tap vector — a fractionally-spaced channel model.
// The bit period is not an integer number of samples, so the three positions
// scored within a bit sit at different points on the channel's pulse response
// and a single bit-spaced tap vector cannot describe all of them at once.
//
// |bit_at| supplies the transmitted bit at a grid bit index and |first|/|last|
// bound the positions that contribute equations; the preamble fit passes the
// known preamble over the preamble bits, and the decision-directed refit of the
// experiment passes the decided payload over the whole packet.
//
// The regressors are transmitted bits, so every entry of the normal matrix
// counts the equations in which two taps both saw a one: it depends only on
// how often each pattern of the tap window occurred, and one set of counts
// serves every phase. Only the right-hand side reads the samples.
template <typename BitAt>
ChannelFit fit_channel_range(const std::vector<double>& grid, int phases,
                             BitAt bit_at, int first, int last,
                             bool with_residual) {
  const FitSystem system = build_fit_system(phases, bit_at, first, last);
  if (!system.ok) {
    ChannelFit fit;
    fit.phases = phases;
    return fit;
  }
  return solve_fit_system(grid, system,
                          eliminate(system.normal, kChannelTaps * phases + 1),
                          with_residual);
}

// The preamble fit's equations, and so its elimination, depend on nothing but
// the service's preamble bits and the grid rate. Acquisition fits some 300
// candidate phases of every line and each detection fits once more, so both
// are done once for every service and rate and only the right-hand side is
// solved per fit — to the same bits as a full solve (see Elimination).
struct PreambleFitSystem {
  FitSystem system;
  Elimination elimination;
};

const PreambleFitSystem* preamble_fit_system(int phases,
                                             const FramingCode& framing) {
  static const auto systems = [] {
    std::array<std::array<PreambleFitSystem, kMaxMlseSamplesPerBit>, 2> all{};
    const std::array<FramingCode, 2> framings = {kFramingCodeWst,
                                                 kFramingCodeNabts};
    for (size_t service = 0; service < framings.size(); ++service) {
      const FramingCode& code = framings[service];
      for (int p = 1; p <= kMaxMlseSamplesPerBit; ++p) {
        auto& entry = all[service][static_cast<size_t>(p - 1)];
        entry.system = build_fit_system(
            p, [&code](int k) { return preamble_bit(k, code); }, kChannelCentre,
            kPreambleBits - 1 - (kChannelTaps - 1 - kChannelCentre));
        entry.elimination =
            eliminate(entry.system.normal, kChannelTaps * p + 1);
      }
    }
    return all;
  }();
  if (phases < 1 || phases > kMaxMlseSamplesPerBit) {
    return nullptr;
  }
  const auto rate = static_cast<size_t>(phases - 1);
  if (framing == kFramingCodeWst) {
    return &systems[0][rate];
  }
  if (framing == kFramingCodeNabts) {
    return &systems[1][rate];
  }
  return nullptr;
}

// The channel fit against the known preamble: the acquisition and detection
// model of every line.
ChannelFit fit_preamble_channel(const std::vector<double>& grid, int phases,
                                const FramingCode& framing) {
  if (const PreambleFitSystem* cached = preamble_fit_system(phases, framing)) {
    return solve_fit_system(grid, cached->system, cached->elimination);
  }
  return fit_channel_range(
      grid, phases, [&framing](int k) { return preamble_bit(k, framing); },
      kChannelCentre, kPreambleBits - 1 - (kChannelTaps - 1 - kChannelCentre),
      /*with_residual=*/true);
}

// What acquisition reads of a candidate phase's preamble fit
// (fit_preamble_channel() at one sample a bit).
struct PreambleScore {
  bool ok = false;
  double gain = 0.0;
  double residual = 0.0;
  // The response to the bit a sample belongs to: taps[0][kChannelCentre].
  double centre = 0.0;
};

#if defined(ORC_TELETEXT_AVX2)
// The preamble fits of the four candidate phases |t0|, a lane each: what
// resample_bit_grid() and solve_fit_system() work out for one, the same
// operations in the same order in every lane. |grid| is scratch.
[[gnu::target("avx2")]] void score_preamble_quad(
    const int16_t* line, size_t sample_count, const std::vector<double>& cubic,
    const double* t0, double spb, const PreambleFitSystem& preamble,
    std::vector<double>& grid, PreambleScore* out) {
  const FitSystem& system = preamble.system;
  const Elimination& elimination = preamble.elimination;
  constexpr int kLanes = 4;
  // Bit k of the four candidates' grids, side by side, read as
  // resample_bit_grid() reads one sample a bit: from the cubic of the
  // interval each position falls in, or, for a position whose four-point
  // window leaves the line, as it reads that.
  const QuadLanes start = quad_load(t0);
  const double interior_end = static_cast<double>(sample_count) - 2.0;
  QuadLanes samples[kPreambleBits];
  for (int k = 0; k < kPreambleBits; ++k) {
    const QuadLanes t =
        start + quad_splat((static_cast<double>(k) + phase_offset(0, 1)) * spb);
    if (quad_less(t, quad_splat(1.0)) == 0 &&
        quad_less(t, quad_splat(interior_end)) == 0xF) {
      samples[k] = quad_cubic_at(cubic, t);
      continue;
    }
    double read[kLanes];
    for (int lane = 0; lane < kLanes; ++lane) {
      resample_bit_grid(line, sample_count, cubic, t0[lane], spb, k + 1, 1,
                        grid);
      read[lane] = grid[static_cast<size_t>(k)];
    }
    samples[k] = quad_load(read);
  }
  const int first = system.first;
  const int positions = system.last - first + 1;
  const QuadLanes* at_first = samples + first;
  // The right-hand side: each tap's samples where it saw a one, in position
  // order, then every sample, for the offset.
  constexpr int kUnknowns = kChannelTaps + 1;
  QuadLanes rhs[kUnknowns];
  for (int j = 0; j < kChannelTaps; ++j) {
    const auto& at = system.ones_at[static_cast<size_t>(j)];
    QuadLanes sum = quad_splat(0.0);
    for (int i = 0; i < system.ones[static_cast<size_t>(j)]; ++i) {
      sum = sum + at_first[at[static_cast<size_t>(i)]];
    }
    rhs[j] = sum;
  }
  {
    QuadLanes sum = quad_splat(0.0);
    for (int i = 0; i < positions; ++i) {
      sum = sum + at_first[i];
    }
    rhs[kChannelTaps] = sum;
  }
  // Elimination::solve(), the multiples the same in every lane.
  for (int col = 0; col < kUnknowns; ++col) {
    const auto c = static_cast<size_t>(col);
    std::swap(rhs[col], rhs[elimination.pivot[c]]);
    for (int row = col + 1; row < kUnknowns; ++row) {
      const double f = elimination.factor[c][static_cast<size_t>(row)];
      if (f != 0.0) {
        rhs[row] = rhs[row] - quad_splat(f) * rhs[col];
      }
    }
  }
  for (int row = kUnknowns - 1; row >= 0; --row) {
    const auto r = static_cast<size_t>(row);
    QuadLanes sum = rhs[row];
    for (int k = row + 1; k < kUnknowns; ++k) {
      sum = sum -
            quad_splat(elimination.upper[r][static_cast<size_t>(k)]) * rhs[k];
    }
    rhs[row] = sum / quad_splat(elimination.upper[r][r]);
  }
  QuadLanes phase_gain = quad_splat(0.0);
  for (int j = 0; j < kChannelTaps; ++j) {
    phase_gain = phase_gain + rhs[j];
  }
  const QuadLanes gain = (quad_splat(0.0) + phase_gain) / quad_splat(1.0);
  const QuadLanes offset = rhs[kChannelTaps];
  // The residual: each pattern's prediction, then the squared errors in
  // position order.
  QuadLanes predicted[kTapPatterns];
  for (int u = 0; u < system.used; ++u) {
    const int pattern = system.used_patterns[static_cast<size_t>(u)];
    QuadLanes value = offset;
    for (int j = 0; j < kChannelTaps; ++j) {
      value =
          value + rhs[j] * quad_splat(static_cast<double>(tap_bit(pattern, j)));
    }
    predicted[pattern] = value;
  }
  QuadLanes sum_sq = quad_splat(0.0);
  for (int i = 0; i < positions; ++i) {
    const QuadLanes error =
        at_first[i] - predicted[system.pattern_at[static_cast<size_t>(i)]];
    sum_sq = sum_sq + error * error;
  }
  const QuadLanes mean = sum_sq / quad_splat(static_cast<double>(positions));
  for (int lane = 0; lane < kLanes; ++lane) {
    out[lane] = {true, gain[lane], std::sqrt(mean[lane]),
                 rhs[kChannelCentre][lane]};
  }
}
#endif

// The preamble fits of the candidate phases |t0| (fit_preamble_channel() of
// each at one sample a bit) into |scores|: four at a time on AVX2 (see
// QuadLanes), one at a time otherwise. |grid| is scratch.
void score_preambles(const int16_t* line, size_t sample_count,
                     const std::vector<double>& cubic,
                     const std::vector<double>& t0, double spb,
                     const FramingCode& framing, bool avx2,
                     std::vector<double>& grid,
                     std::vector<PreambleScore>& scores) {
  scores.resize(t0.size());
  size_t done = 0;
#if defined(ORC_TELETEXT_AVX2)
  const PreambleFitSystem* preamble = preamble_fit_system(1, framing);
  if (avx2 && preamble != nullptr && preamble->system.ok &&
      preamble->elimination.ok && preamble->elimination.n == kChannelTaps + 1) {
    for (; done + 4 <= t0.size(); done += 4) {
      score_preamble_quad(line, sample_count, cubic, &t0[done], spb, *preamble,
                          grid, &scores[done]);
    }
  }
#endif
  for (; done < t0.size(); ++done) {
    resample_bit_grid(line, sample_count, cubic, t0[done], spb, kPreambleBits,
                      1, grid);
    const ChannelFit fit = fit_preamble_channel(grid, 1, framing);
    scores[done] = {fit.ok, fit.gain, fit.residual,
                    fit.taps[0][kChannelCentre]};
  }
}

// How well a reading's Hamming 8/4 protected prefix arrived: how many of its
// bytes are past correction, and how many needed it (ETSI EN 300 706 §8.2).
struct PrefixStanding {
  int uncorrectable = 0;
  int corrected = 0;

  bool worse_than(const PrefixStanding& other) const {
    return uncorrectable != other.uncorrectable
               ? uncorrectable > other.uncorrectable
               : corrected > other.corrected;
  }
};

// The taps of a channel fitted over a window of kTaps bits whose centre tap,
// kChannelCentre bits into it, is the bit a sample belongs to, together with
// the kTailTaps bits before the window: taps[p][j] is the response at phase p
// to the bit j - kChannelCentre periods from that bit, tail[p][d] to the bit
// d + 1 periods older than the oldest of the window, and |gain| the mean over
// the phases of all of them summed (see ChannelFit).
template <int kTaps, int kTailTaps>
struct WindowFit {
  bool ok = false;
  int phases = 1;
  std::array<std::array<double, kTaps>, kMaxMlseSamplesPerBit> taps{};
  std::array<std::array<double, kTailTaps>, kMaxMlseSamplesPerBit> tail{};
  double offset = 0.0;
  double gain = 0.0;
};

// Least-squares fit of a channel together with its tail: the kTaps window and
// the kTailTaps bits before it, a tap vector of each per phase, and the shared
// DC offset, over grid positions |first| to |last| of the bits |bit_at|
// supplies. Positions whose tail would reach before the first grid bit are
// left out. No residual is worked out: a refit is judged by the decisions it
// leads to, and nothing reads one.
//
// As in build_fit_system, the regressors are bits, so the normal matrix counts
// the positions at which two taps both saw a one and reads no sample. It is
// also the same for every phase, which couple only through the offset, so the
// system is solved for one block of the window's width and the offset follows
// by elimination (its Schur complement) rather than as one system of every
// phase's taps at once.
template <int kTaps, int kTailTaps, typename BitAt>
WindowFit<kTaps, kTailTaps> fit_window_with_tail(
    const std::vector<double>& grid, int phases, BitAt bit_at, int first,
    int last) {
  // Window index i holds bit k - kChannelCentre - kTailTaps + i, oldest first:
  // the tail, then the tap window.
  constexpr int kWidth = kTailTaps + kTaps;
  // Right-hand sides solved against the block: the offset's column, then one
  // per phase.
  constexpr int kColumns = 1 + kMaxMlseSamplesPerBit;
  WindowFit<kTaps, kTailTaps> fit;
  fit.phases = phases;
  first = std::max(first, kChannelCentre + kTailTaps);
  const int position_count = last - first + 1;
  if (last < first || position_count > kMaxFitPositions || phases < 1 ||
      phases > kMaxMlseSamplesPerBit) {
    return fit;
  }

  // The bits the windows read, each once: window index i of the position n
  // past |first| is bits[n + i].
  std::array<uint8_t, kMaxFitPositions + kWidth - 1> bits{};
  const int oldest = first - kChannelCentre - kTailTaps;
  for (int n = 0; n < position_count + kWidth - 1; ++n) {
    bits[static_cast<size_t>(n)] = static_cast<uint8_t>(bit_at(oldest + n));
  }

  // The block counts the positions at which two window bits are both one,
  // which for window indices i and i + lag is a sum of bits[m] & bits[m + lag]
  // over the run of m from i on, |position_count| long; the run for i + 1 is
  // the run for i moved on by one. Counts are whole numbers, which a double
  // holds exactly however they are added.
  std::array<std::array<double, kWidth>, kWidth> block{};
  std::array<std::array<double, kWidth>, kColumns> columns{};
  for (int lag = 0; lag < kWidth; ++lag) {
    const auto product = [&bits, lag](int m) {
      const auto at = static_cast<size_t>(m);
      return bits[at] & bits[at + static_cast<size_t>(lag)];
    };
    int together = 0;
    for (int m = 0; m < position_count; ++m) {
      together += product(m);
    }
    for (int i = 0; i + lag < kWidth; ++i) {
      if (i > 0) {
        together += product(i - 1 + position_count) - product(i - 1);
      }
      const auto count = static_cast<double>(together);
      const auto row = static_cast<size_t>(i);
      const auto column = row + static_cast<size_t>(lag);
      block[row][column] = count;
      block[column][row] = count;
      if (lag == 0) {
        columns[0][static_cast<size_t>(i)] = count;
      }
    }
  }
  // Each phase's column sums, in position order, the samples at which its tap
  // saw a one: window index i of position n is bit n + i, so every column is
  // read off the one list of the bits that are set.
  std::array<uint16_t, kMaxFitPositions + kWidth - 1> set_bits{};
  int set_count = 0;
  for (int m = 0; m < position_count + kWidth - 1; ++m) {
    if (bits[static_cast<size_t>(m)] != 0) {
      set_bits[static_cast<size_t>(set_count++)] = static_cast<uint16_t>(m);
    }
  }
  const double* samples = &grid[grid_index(first, 0, phases)];
  // The set bits window index i reads are those from bit i to bit
  // i + position_count - 1, a run of the list that moves along it with i.
  int begin = 0;
  int end = 0;
  for (int i = 0; i < kWidth; ++i) {
    while (begin < set_count && set_bits[static_cast<size_t>(begin)] < i) {
      ++begin;
    }
    while (end < set_count &&
           set_bits[static_cast<size_t>(end)] < i + position_count) {
      ++end;
    }
    const std::array<double, kMaxMlseSamplesPerBit> sums =
        phase_sums(phases, samples, set_bits.data() + begin, end - begin, i);
    for (int p = 0; p < phases; ++p) {
      columns[static_cast<size_t>(p) + 1][static_cast<size_t>(i)] =
          sums[static_cast<size_t>(p)];
    }
  }
  double sample_sum = 0.0;
  for (int n = 0; n < position_count * phases; ++n) {
    sample_sum += samples[n];
  }
  // Column 0 is the offset's coupling to every tap, the count of positions at
  // which that tap saw a one; keep it for the Schur complement below.
  const std::array<double, kWidth> ones = columns[0];

  // Gaussian elimination with partial pivoting of the block, applied to every
  // column at once.
  for (int col = 0; col < kWidth; ++col) {
    const auto c = static_cast<size_t>(col);
    int pivot = col;
    for (int row = col + 1; row < kWidth; ++row) {
      if (std::abs(block[static_cast<size_t>(row)][c]) >
          std::abs(block[static_cast<size_t>(pivot)][c])) {
        pivot = row;
      }
    }
    if (std::abs(block[static_cast<size_t>(pivot)][c]) < 1e-9) {
      return fit;
    }
    std::swap(block[c], block[static_cast<size_t>(pivot)]);
    for (auto& column : columns) {
      std::swap(column[c], column[static_cast<size_t>(pivot)]);
    }
    for (int row = col + 1; row < kWidth; ++row) {
      const auto r = static_cast<size_t>(row);
      const double factor = block[r][c] / block[c][c];
      if (factor == 0.0) {
        continue;
      }
      for (int k = col; k < kWidth; ++k) {
        block[r][static_cast<size_t>(k)] -=
            factor * block[c][static_cast<size_t>(k)];
      }
      for (auto& column : columns) {
        column[r] -= factor * column[c];
      }
    }
  }
  for (auto& column : columns) {
    for (int row = kWidth - 1; row >= 0; --row) {
      const auto r = static_cast<size_t>(row);
      double sum = column[r];
      for (int k = row + 1; k < kWidth; ++k) {
        sum -=
            block[r][static_cast<size_t>(k)] * column[static_cast<size_t>(k)];
      }
      column[r] = sum / block[r][r];
    }
  }

  // Each phase's taps are its column less the offset times the offset's
  // column, and the offset makes the equation of its own row hold.
  double coupling = 0.0;
  double explained = 0.0;
  for (int i = 0; i < kWidth; ++i) {
    coupling +=
        ones[static_cast<size_t>(i)] * columns[0][static_cast<size_t>(i)];
    for (int p = 0; p < phases; ++p) {
      explained += ones[static_cast<size_t>(i)] *
                   columns[static_cast<size_t>(p) + 1][static_cast<size_t>(i)];
    }
  }
  const double positions = static_cast<double>((last - first + 1) * phases);
  const double denominator = positions - static_cast<double>(phases) * coupling;
  if (std::abs(denominator) < 1e-9) {
    return fit;
  }
  fit.offset = (sample_sum - explained) / denominator;

  for (int p = 0; p < phases; ++p) {
    const auto& column = columns[static_cast<size_t>(p) + 1];
    const auto tap_at = [&](int i) {
      return column[static_cast<size_t>(i)] -
             fit.offset * columns[0][static_cast<size_t>(i)];
    };
    double phase_gain = 0.0;
    for (int d = 0; d < kTailTaps; ++d) {
      // The bit d + 1 periods older than the window is window index
      // kTailTaps - 1 - d.
      const double tap = tap_at(kTailTaps - 1 - d);
      fit.tail[static_cast<size_t>(p)][static_cast<size_t>(d)] = tap;
      phase_gain += tap;
    }
    for (int j = 0; j < kTaps; ++j) {
      const double tap = tap_at(kTailTaps + j);
      fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] = tap;
      phase_gain += tap;
    }
    fit.gain += phase_gain;
  }
  fit.gain /= static_cast<double>(phases);
  fit.ok = true;
  return fit;
}

// The channel fitted with its tail (kChannelTailTaps) for the tail refits: see
// fit_window_with_tail(). |residual| is left at zero.
template <typename BitAt>
ChannelFit fit_channel_with_tail(const std::vector<double>& grid, int phases,
                                 BitAt bit_at, int first, int last) {
  const WindowFit<kChannelTaps, kChannelTailTaps> window =
      fit_window_with_tail<kChannelTaps, kChannelTailTaps>(grid, phases, bit_at,
                                                           first, last);
  ChannelFit fit;
  fit.phases = phases;
  if (!window.ok) {
    return fit;
  }
  fit.taps = window.taps;
  fit.tail = window.tail;
  fit.offset = window.offset;
  fit.gain = window.gain;
  fit.has_tail = true;
  fit.ok = true;
  return fit;
}

// Energy of the fitted channel's response to one bit: the squared change, over
// every sample the detector scores, that flipping a single bit makes to the
// waveform the channel would have produced. On a noiseless line that is exactly
// the path-metric penalty of getting that bit wrong, which is what makes it the
// natural full-confidence reference for the margins below.
double channel_bit_energy(const ChannelFit& fit) {
  double energy = 0.0;
  for (int p = 0; p < fit.phases; ++p) {
    for (int j = 0; j < kChannelTaps; ++j) {
      const double tap =
          fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)];
      energy += tap * tap;
    }
    if (fit.has_tail) {
      for (const double tap : fit.tail[static_cast<size_t>(p)]) {
        energy += tap * tap;
      }
    }
  }
  return energy;
}

// Whether the framing code the slicer was configured for explains the samples
// at least as well as any other service's does.
//
// The threshold detector matches the framing code bit by bit, which is exactly
// what makes it blind to the other service's lines. The MLSE detector does not
// match it at all: it fits its channel to the whole 24-bit preamble and takes
// the framing code as known. On a 525-line capture System B and System C share
// the bit rate, the clock run-in, the lines and the levels, and differ in
// nothing but those eight bits — two of them, between 0xE4 and 0xE7 — so an
// assumed code that is wrong still fits well enough to clear the residual
// gates, and the payload is then read at the wrong length under the wrong
// coding.
//
// This is deliberately a discrimination test and not a quality test. Asking
// whether the samples support the assumed code in absolute terms would reject
// the band-limited recordings the MLSE detector exists for, where the framing
// bits are smeared into each other and no per-bit decision is reliable. Asking
// which of the two real framing codes explains them best is a far easier
// question — the codes differ in two bits — and it is the only question that
// needs answering, because a line carries one service or the other.
//
// The comparison carries a margin, and needs one. Simply requiring the
// configured code to be the better of the two rejects genuine lines: where the
// recording has damaged the two bits that differ, which code explains them is a
// coin toss, and half of that marginal tail is thrown away — measured at 0,7 %
// of the 625-line reference capture's packets and 1,7 % of the 525-line one's.
// The margin is expressed in units of the fitted channel's response to one bit
// (channel_bit_energy), which is the natural scale: a code wrong in two bits
// costs about two of them, while noise on a marginal line costs a fraction of
// one.
//
// The comparison is also biased in favour of the configured code, whose assumed
// bits the channel was fitted to; that bias costs nothing here, since it can
// only make a wrongly-rejected line rarer.
bool framing_code_fits_best(const std::vector<double>& grid, int phases,
                            const ChannelFit& fit, const FramingCode& framing,
                            bool& other_fits_better) {
  const auto grid_bits =
      static_cast<int>(grid.size() / static_cast<size_t>(phases));

  // Every sample any framing bit reaches: sample k is formed from bits
  // k - kChannelCentre + j for j in [0, kChannelTaps).
  const int first_sample =
      std::max(0, kRunInBits + kChannelCentre - (kChannelTaps - 1));
  const int last_sample =
      std::min(kRunInBits + kFramingBits - 1 + kChannelCentre, grid_bits - 1);
  if (last_sample < first_sample) {
    return true;
  }

  const auto residual_for = [&](const FramingCode& candidate) {
    double sum_sq = 0.0;
    for (int k = first_sample; k <= last_sample; ++k) {
      for (int p = 0; p < phases; ++p) {
        double predicted = fit.offset;
        for (int j = 0; j < kChannelTaps; ++j) {
          const int source = k - kChannelCentre + j;
          const int value = (source >= 0 && source < kPreambleBits)
                                ? preamble_bit(source, candidate)
                                : 0;
          predicted +=
              fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] *
              static_cast<double>(value);
        }
        const double error = grid[grid_index(k, p, phases)] - predicted;
        sum_sq += error * error;
      }
    }
    return sum_sq;
  };

  const double configured = residual_for(framing);
  const double margin = kFramingDiscriminationBits * channel_bit_energy(fit);
  bool fits = true;
  for (const FramingCode& candidate : {kFramingCodeWst, kFramingCodeNabts}) {
    if (candidate == framing) {
      continue;
    }
    const double other = residual_for(candidate);
    // Said with no margin, for the caller who can judge a line's run of
    // packets rather than one (TeletextLineResult::other_framing_fits_better).
    if (other < configured) {
      other_fits_better = true;
    }
    if (other + margin < configured) {
      fits = false;
    }
  }
  return fits;
}

// Survivor histories the tail can be read for: every pattern of the
// kChannelTailTaps bits before a state (see mlse_detect()).
constexpr uint32_t kTailHistories = 1U << kChannelTailTaps;

// Two doubles side by side: the two branches out of one trellis state, which
// the detector works out together — a two-lane vector where the compiler
// offers them (GCC and Clang, on any target), two scalars otherwise. Either
// way each lane is computed with exactly the operations a scalar would use, so
// the two give the same bits. pair_add_square() is one expression in either
// form, as the sum of squares it stands for was, so that a compiler fusing a
// multiply into an add within an expression fuses all of them alike.
#if defined(__GNUC__)
using PairLanes = double __attribute__((vector_size(2 * sizeof(double))));
struct BranchPair {
  PairLanes lanes;
};
inline BranchPair pair_load(const double* from) {
  BranchPair pair;
  std::memcpy(&pair.lanes, from, sizeof(pair.lanes));
  return pair;
}
inline BranchPair pair_splat(double value) { return {PairLanes{value, value}}; }
inline void pair_store(double* to, BranchPair pair) {
  std::memcpy(to, &pair.lanes, sizeof(pair.lanes));
}
inline BranchPair operator+(BranchPair a, BranchPair b) {
  return {a.lanes + b.lanes};
}
inline BranchPair operator-(BranchPair a, BranchPair b) {
  return {a.lanes - b.lanes};
}
inline BranchPair operator*(BranchPair a, BranchPair b) {
  return {a.lanes * b.lanes};
}
// |sum| plus the square of |error|, lane by lane.
inline BranchPair pair_add_square(BranchPair sum, BranchPair error) {
  return {sum.lanes + error.lanes * error.lanes};
}
// Bit i set where lane i of |a| is less than that of |b|.
inline unsigned pair_less(BranchPair a, BranchPair b) {
  const auto less = a.lanes < b.lanes;  // all ones where less, else zero
  return (less[0] != 0 ? 1U : 0U) | (less[1] != 0 ? 2U : 0U);
}
// Lane by lane, |a| where it is less than |b|, else |b|: the bits of each
// lane taken whole from one or the other.
inline BranchPair pair_min(BranchPair a, BranchPair b) {
  auto less = a.lanes < b.lanes;
  decltype(less) from_a;
  decltype(less) from_b;
  std::memcpy(&from_a, &a.lanes, sizeof(from_a));
  std::memcpy(&from_b, &b.lanes, sizeof(from_b));
  const decltype(less) chosen = (from_a & less) | (from_b & ~less);
  BranchPair pair;
  std::memcpy(&pair.lanes, &chosen, sizeof(pair.lanes));
  return pair;
}
#else
struct BranchPair {
  double low;
  double high;
};
inline BranchPair pair_load(const double* from) { return {from[0], from[1]}; }
inline BranchPair pair_splat(double value) { return {value, value}; }
inline void pair_store(double* to, BranchPair pair) {
  to[0] = pair.low;
  to[1] = pair.high;
}
inline BranchPair operator+(BranchPair a, BranchPair b) {
  return {a.low + b.low, a.high + b.high};
}
inline BranchPair operator-(BranchPair a, BranchPair b) {
  return {a.low - b.low, a.high - b.high};
}
inline BranchPair operator*(BranchPair a, BranchPair b) {
  return {a.low * b.low, a.high * b.high};
}
inline BranchPair pair_add_square(BranchPair sum, BranchPair error) {
  return {sum.low + error.low * error.low, sum.high + error.high * error.high};
}
inline unsigned pair_less(BranchPair a, BranchPair b) {
  return (a.low < b.low ? 1U : 0U) | (a.high < b.high ? 2U : 0U);
}
inline BranchPair pair_min(BranchPair a, BranchPair b) {
  return {a.low < b.low ? a.low : b.low, a.high < b.high ? a.high : b.high};
}
#endif

// The survivor histories after a step of a trellis of |kStates| states (see
// mlse_detect()): state |to| continues the history of its lower origin,
// to >> 1, or of its upper one, (to >> 1) + kStates / 2, where bit |to| of
// |upper| is set, and shifts in the bit that left the origin's register, which
// is that bit. Only the |kHistories| - 1 mask's most recent bits are kept.
template <int kStates, uint32_t kHistories>
std::array<uint8_t, kStates> next_histories(
    const std::array<uint8_t, kStates>& history, uint32_t upper) {
  static_assert(kHistories <= 256, "a history is held in a byte");
  std::array<uint8_t, kStates> next;
  for (int to = 0; to < kStates; ++to) {
    const uint32_t from_top = (upper >> to) & 1U;
    const uint32_t origin =
        static_cast<uint32_t>(to >> 1) + from_top * (kStates / 2);
    next[static_cast<size_t>(to)] = static_cast<uint8_t>(
        ((static_cast<uint32_t>(history[origin]) << 1) | from_top) &
        (kHistories - 1));
  }
  return next;
}

// One add-compare-select step of a trellis of |kStates| states (see
// mlse_detect()): the cost of reaching each state through either origin, from
// |cost| and the branch metrics |metric| (indexed origin * 2 + bit), the
// cheaper written to |next|. Returns which states were reached from their
// upper origin, bit |to| set where state |to| was — and only where that origin
// was strictly cheaper. The two states a state's pair of branches leads to,
// its new bit 0 and 1, share their origins, so they are worked out side by
// side (BranchPair).
template <int kStates>
uint32_t add_compare_select(const std::array<double, kStates>& cost,
                            const double* metric,
                            std::array<double, kStates>& next) {
  static_assert(kStates <= 32, "the winners of a step are held in 32 bits");
  constexpr int kUpperHalf = kStates / 2;
  uint32_t upper = 0;
  for (int lower = 0; lower < kUpperHalf; ++lower) {
    const auto pair = 2 * static_cast<ptrdiff_t>(lower);
    const BranchPair via_lower =
        pair_splat(cost[static_cast<size_t>(lower)]) + pair_load(metric + pair);
    const BranchPair via_upper =
        pair_splat(cost[static_cast<size_t>(lower) + kUpperHalf]) +
        pair_load(metric + pair + kStates);
    upper |= static_cast<uint32_t>(pair_less(via_upper, via_lower))
             << (2 * lower);
    pair_store(next.data() + 2 * lower, pair_min(via_upper, via_lower));
  }
  return upper;
}

// The steps of a trellis of |kStates| states, the forward pass of
// mlse_detect() and of mlse_detect_wide() (trellis_steps()). Step k scores
// grid bit |first_sample_bit| + k — every branch costing nothing where that
// bit is off the grid — and from step |zero_from| on offers only the zero
// branch, the transmitted bits being known black there.
// How a trellis adds the response to the bits older than its window (see
// TrellisSteps): not at all, from the rows laid out per history, or added to
// the rows at every step.
enum class TrellisTail { kNone, kTabled, kAdded };

template <int kStates>
struct TrellisSteps {
  // The grid, |phases| samples a bit, and |grid_bits| bits of it.
  const double* grid = nullptr;
  int grid_bits = 0;
  int phases = 1;
  int first_sample_bit = 0;
  int steps = 0;
  int zero_from = 0;
  // The expected samples of state s's two branches at phase p, its new bit 0
  // and 1, are rows[p * 2 * kStates + 2 s] and the next. A trellis with a
  // tail adds the one its state's history implies: either read from |tailed|,
  // the rows with each history's tail added once for the fit, per state and
  // history (mlse_detect(), whose states have 16 histories), or added to the
  // row at every step from |tail_for|, each history's tail a row per phase
  // (mlse_detect_wide(), whose states have 256).
  const double* rows = nullptr;
  const double* tailed = nullptr;
  const double* tail_for = nullptr;
  // The states' costs and survivor histories, carried in and out.
  std::array<double, kStates> cost{};
  std::array<uint8_t, kStates> history{};
  // Where they are not null, each step's branch metrics (indexed origin * 2 +
  // bit) and the costs entering it, for trellis_confidence().
  double* metrics = nullptr;
  double* forward_cost = nullptr;
  // Whether the steps may run on AVX2 (see QuadLanes).
  bool avx2 = false;
};

// The expected samples of state |s|'s pair of branches at phase |p| (see
// TrellisSteps).
template <int kStates, uint32_t kHistories, int kPhases, TrellisTail kTail>
BranchPair branch_pair(const TrellisSteps<kStates>& run, int s, int p) {
  const int history = run.history[static_cast<size_t>(s)];
  if constexpr (kTail == TrellisTail::kTabled) {
    return pair_load(
        run.tailed +
        (s * static_cast<int>(kHistories) + history) * 2 * kPhases + 2 * p);
  }
  const BranchPair row = pair_load(run.rows + p * 2 * kStates + 2 * s);
  if constexpr (kTail == TrellisTail::kAdded) {
    return row +
           pair_splat(run.tail_for[p * static_cast<int>(kHistories) + history]);
  }
  return row;
}

// The steps of |run|, a pair of branches at a time (BranchPair): each step's
// squared errors summed over the |kPhases| samples of the bit — the first
// phase's stored rather than added to zero, which is the same number — then
// add_compare_select() and the survivor histories. The winners of each step
// go to |from_upper|. Returns the steps whose grid bit was on the grid.
template <int kStates, uint32_t kHistories, int kPhases, TrellisTail kTail,
          typename Winners>
int trellis_steps_pairs(TrellisSteps<kStates>& run, Winners* from_upper) {
  constexpr int kBranches = 2 * kStates;
  std::array<double, kBranches> step_metric{};
  int evaluated = 0;
  for (int k = 0; k < run.steps; ++k) {
    double* metric = run.metrics != nullptr
                         ? run.metrics + static_cast<ptrdiff_t>(k) * kBranches
                         : step_metric.data();
    const int bit = run.first_sample_bit + k;
    if (bit >= 0 && bit < run.grid_bits) {
      ++evaluated;
      const double* observed = run.grid + static_cast<ptrdiff_t>(bit) * kPhases;
      std::array<BranchPair, kPhases> sample;
      for (int p = 0; p < kPhases; ++p) {
        sample[static_cast<size_t>(p)] = pair_splat(observed[p]);
      }
      for (int s = 0; s < kStates; ++s) {
        BranchPair error =
            sample[0] -
            branch_pair<kStates, kHistories, kPhases, kTail>(run, s, 0);
        BranchPair sum = error * error;
        for (int p = 1; p < kPhases; ++p) {
          error = sample[static_cast<size_t>(p)] -
                  branch_pair<kStates, kHistories, kPhases, kTail>(run, s, p);
          sum = pair_add_square(sum, error);
        }
        pair_store(metric + 2 * static_cast<ptrdiff_t>(s), sum);
      }
    } else {
      std::fill(metric, metric + kBranches, 0.0);
    }
    if (run.forward_cost != nullptr) {
      std::copy(run.cost.begin(), run.cost.end(),
                run.forward_cost + static_cast<ptrdiff_t>(k) * kStates);
    }
    std::array<double, kStates> next;
    const uint32_t upper = add_compare_select<kStates>(run.cost, metric, next);
    if constexpr (kTail != TrellisTail::kNone) {
      // The bit leaving the register is the origin's oldest, its top bit,
      // which is set exactly when the origin is in the upper half.
      run.history = next_histories<kStates, kHistories>(run.history, upper);
    }
    if (k >= run.zero_from) {
      for (int to = 1; to < kStates; to += 2) {
        next[static_cast<size_t>(to)] = std::numeric_limits<double>::infinity();
      }
    }
    from_upper[k] = static_cast<Winners>(upper);
    run.cost = next;
  }
  return evaluated;
}

// The calling thread's own |T|, made on its first use and kept for the
// thread's life. Code holding it reaches the thread's storage once, through
// the pointer the thread keeps: a thread_local object reached directly is a
// call (__tls_get_addr) at every reach in a shared library, and the compiler
// repeats it inside loops.
template <typename T>
T& per_thread() {
  thread_local std::unique_ptr<T> held;
  if (!held) {
    held = std::make_unique<T>();
  }
  return *held;
}

// The storage a trellis pass needs and does not keep: which predecessor won
// each state at each step, the decided bits, and the expected samples of every
// state's branches with each history's tail added (see mlse_detect()). A line
// is read some twenty times over, and every element is written before it is
// read, so each thread keeps one set and reuses it, neither clearing nor
// reallocating it.
struct TrellisStorage {
  std::vector<uint16_t> from_upper;
  std::vector<uint8_t> decided;
  std::vector<double> tailed;
};

TrellisStorage& trellis_storage(int steps) {
  TrellisStorage& storage = per_thread<TrellisStorage>();
  const auto count = static_cast<size_t>(steps);
  if (storage.decided.size() < count) {
    storage.from_upper.resize(count);
    storage.decided.resize(count);
  }
  return storage;
}

// A detection's trellis as its confidence sweep (mlse_confidence()) reads it:
// the branch metrics and the forward cost of every state at every step, the
// bits decided and the winning path's cost. A reading's channel is refitted
// several times and one detection kept, so the sweep runs once, on the trellis
// of the detection kept, rather than after each of them.
struct TrellisRecord {
  std::vector<double> metrics;
  std::vector<double> forward_cost;
  std::vector<uint8_t> decided;
  int steps = 0;
  int bit_count = 0;
  int tail_bits = 0;
  bool terminated = false;
  double best = 0.0;
  double energy = 0.0;
};

// The trellises a reading keeps while it refits its channel: that of the
// detection it holds and that of the one it is trying. Per thread, and reused.
std::array<TrellisRecord, 2>& trellis_records() {
  return per_thread<std::array<TrellisRecord, 2>>();
}

// The trellis on AVX2 (see QuadLanes): two states' pairs of branches at a
// time where BranchPair works out one state's.
#if defined(ORC_TELETEXT_AVX2)
// The survivor histories of the states of a trellis, a byte each.
template <int kStates>
struct HistoryLanesOf;
template <>
struct HistoryLanesOf<kMlseStates> {
  using Lanes = uint8_t __attribute__((vector_size(kMlseStates)));
};
template <>
struct HistoryLanesOf<2 * kMlseStates> {
  using Lanes = uint8_t __attribute__((vector_size(2 * kMlseStates)));
};
template <int kStates>
using HistoryLanes = typename HistoryLanesOf<kStates>::Lanes;

// Lanes 0 and 1 of |quad| (kHigh false) or 2 and 3, each twice: the costs of
// two states, once for each branch out of them.
template <bool kHigh>
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_doubled(
    QuadLanes quad) {
  if constexpr (kHigh) {
    return __builtin_shufflevector(quad, quad, 2, 2, 3, 3);
  } else {
    return __builtin_shufflevector(quad, quad, 0, 0, 1, 1);
  }
}
// The even lanes of |a| then of |b| (kOdd false), or the odd ones.
template <bool kOdd>
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes quad_alternate(
    QuadLanes a, QuadLanes b) {
  if constexpr (kOdd) {
    return __builtin_shufflevector(a, b, 1, 3, 5, 7);
  } else {
    return __builtin_shufflevector(a, b, 0, 2, 4, 6);
  }
}

// add_compare_select() of a trellis of |kStates| states a quad at a time:
// |metric|[g] holds the branch metrics of origins 2g and 2g + 1, and
// |cost|[q] / |next|[q] the costs of states 4q to 4q + 3 — the states the
// origins of |metric|[q] lead to.
template <int kStates>
[[gnu::target("avx2"), gnu::always_inline]] inline uint32_t
add_compare_select_avx2(const QuadLanes* cost, const QuadLanes* metric,
                        QuadLanes* next) {
  static_assert(kStates <= 32, "the winners of a step are held in 32 bits");
  uint32_t upper = 0;
  for (int q = 0; q < kStates / 4; ++q) {
    // Origins 2q and 2q + 1, and their upper twins half the trellis on.
    const QuadLanes& lower_costs = cost[q / 2];
    const QuadLanes& upper_costs = cost[kStates / 8 + q / 2];
    const QuadLanes via_lower = (q % 2 == 0 ? quad_doubled<false>(lower_costs)
                                            : quad_doubled<true>(lower_costs)) +
                                metric[q];
    const QuadLanes via_upper = (q % 2 == 0 ? quad_doubled<false>(upper_costs)
                                            : quad_doubled<true>(upper_costs)) +
                                metric[q + kStates / 4];
    upper |= quad_less(via_upper, via_lower) << (4 * q);
    next[q] = quad_min(via_upper, via_lower);
  }
  return upper;
}

// next_histories() of every state at once.
template <int kStates, uint32_t kHistories>
[[gnu::target("avx2"), gnu::always_inline]] inline HistoryLanes<kStates>
next_histories_avx2(HistoryLanes<kStates> history, uint32_t upper) {
  using Lanes = HistoryLanes<kStates>;
  Lanes winners{};
  std::memcpy(&winners, &upper, sizeof(upper));
  Lanes from_lower;
  Lanes from_upper;
  Lanes word;
  if constexpr (kStates == 16) {
    from_lower = __builtin_shufflevector(history, history, 0, 0, 1, 1, 2, 2, 3,
                                         3, 4, 4, 5, 5, 6, 6, 7, 7);
    from_upper =
        __builtin_shufflevector(history, history, 8, 8, 9, 9, 10, 10, 11, 11,
                                12, 12, 13, 13, 14, 14, 15, 15);
    word = __builtin_shufflevector(winners, winners, 0, 0, 0, 0, 0, 0, 0, 0, 1,
                                   1, 1, 1, 1, 1, 1, 1);
  } else {
    static_assert(kStates == 32, "histories of 16 or 32 states");
    from_lower = __builtin_shufflevector(
        history, history, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
        9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15);
    from_upper = __builtin_shufflevector(history, history, 16, 16, 17, 17, 18,
                                         18, 19, 19, 20, 20, 21, 21, 22, 22, 23,
                                         23, 24, 24, 25, 25, 26, 26, 27, 27, 28,
                                         28, 29, 29, 30, 30, 31, 31);
    word = __builtin_shufflevector(winners, winners, 0, 0, 0, 0, 0, 0, 0, 0, 1,
                                   1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
                                   3, 3, 3, 3, 3, 3, 3, 3);
  }
  Lanes bit;
  for (int i = 0; i < kStates; ++i) {
    bit[i] = static_cast<uint8_t>(1U << (i % 8));
  }
  // All ones where the state was reached from its upper origin.
  const auto top = lanes_as<Lanes>((word & bit) != 0);
  const Lanes origin = (from_upper & top) | (from_lower & ~top);
  return ((origin + origin) | (top & 1)) & static_cast<uint8_t>(kHistories - 1);
}

// The expected samples of the branches of states |s| and |s| + 1 at phase |p|
// (branch_pair() of each).
template <int kStates, uint32_t kHistories, int kPhases, TrellisTail kTail>
[[gnu::target("avx2"), gnu::always_inline]] inline QuadLanes branch_pairs_avx2(
    const TrellisSteps<kStates>& run, int s, int p) {
  const int low = run.history[static_cast<size_t>(s)];
  const int high = run.history[static_cast<size_t>(s) + 1];
  if constexpr (kTail == TrellisTail::kTabled) {
    constexpr int kStride = 2 * kPhases;
    const int histories = static_cast<int>(kHistories);
    return quad_join(
        run.tailed + (s * histories + low) * kStride + 2 * p,
        run.tailed + ((s + 1) * histories + high) * kStride + 2 * p);
  }
  const QuadLanes row = quad_load(run.rows + p * 2 * kStates + 2 * s);
  if constexpr (kTail == TrellisTail::kAdded) {
    const double* tails = run.tail_for + p * static_cast<int>(kHistories);
    return row + QuadLanes{tails[low], tails[low], tails[high], tails[high]};
  }
  return row;
}

// trellis_steps_pairs() on AVX2: two states' pairs of branches at a time.
template <int kStates, uint32_t kHistories, int kPhases, TrellisTail kTail,
          typename Winners>
[[gnu::target("avx2")]] int trellis_steps_avx2(TrellisSteps<kStates>& run,
                                               Winners* from_upper) {
  constexpr int kQuads = kStates / 4;
  constexpr int kGroups = kStates / 2;
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  const QuadMask odd = {0, -1, 0, -1};
  QuadLanes costs[kQuads];
  for (int q = 0; q < kQuads; ++q) {
    costs[q] = quad_load(run.cost.data() + 4 * q);
  }
  HistoryLanes<kStates> histories;
  std::memcpy(&histories, run.history.data(), sizeof(histories));
  // Held apart from |run|, whose fields the stores below could otherwise be
  // taken to change.
  double* const metrics = run.metrics;
  double* const forward_cost = run.forward_cost;
  const double* const grid = run.grid;
  int evaluated = 0;
  for (int k = 0; k < run.steps; ++k) {
    QuadLanes metric[kGroups];
    const int bit = run.first_sample_bit + k;
    if (bit >= 0 && bit < run.grid_bits) {
      ++evaluated;
      const double* observed = grid + static_cast<ptrdiff_t>(bit) * kPhases;
      QuadLanes sample[kPhases];
      for (int p = 0; p < kPhases; ++p) {
        sample[p] = quad_splat(observed[p]);
      }
      for (int g = 0; g < kGroups; ++g) {
        QuadLanes error =
            sample[0] - branch_pairs_avx2<kStates, kHistories, kPhases, kTail>(
                            run, 2 * g, 0);
        QuadLanes sum = error * error;
        for (int p = 1; p < kPhases; ++p) {
          error = sample[p] -
                  branch_pairs_avx2<kStates, kHistories, kPhases, kTail>(
                      run, 2 * g, p);
          sum = sum + error * error;
        }
        metric[g] = sum;
      }
    } else {
      for (int g = 0; g < kGroups; ++g) {
        metric[g] = quad_splat(0.0);
      }
    }
    if (metrics != nullptr) {
      for (int g = 0; g < kGroups; ++g) {
        quad_store(metrics + static_cast<ptrdiff_t>(k) * 2 * kStates +
                       4 * static_cast<ptrdiff_t>(g),
                   metric[g]);
      }
    }
    if (forward_cost != nullptr) {
      for (int q = 0; q < kQuads; ++q) {
        quad_store(forward_cost + static_cast<ptrdiff_t>(k) * kStates +
                       4 * static_cast<ptrdiff_t>(q),
                   costs[q]);
      }
    }
    QuadLanes next[kQuads];
    const uint32_t upper =
        add_compare_select_avx2<kStates>(costs, metric, next);
    if constexpr (kTail != TrellisTail::kNone) {
      histories = next_histories_avx2<kStates, kHistories>(histories, upper);
      std::memcpy(run.history.data(), &histories, sizeof(histories));
    }
    if (k >= run.zero_from) {
      for (int q = 0; q < kQuads; ++q) {
        next[q] = quad_select(odd, quad_splat(kInfinity), next[q]);
      }
    }
    from_upper[k] = static_cast<Winners>(upper);
    for (int q = 0; q < kQuads; ++q) {
      costs[q] = next[q];
    }
  }
  for (int q = 0; q < kQuads; ++q) {
    quad_store(run.cost.data() + 4 * q, costs[q]);
  }
  return evaluated;
}

// trellis_confidence() on AVX2: the same sweep, a quad of states at a time.
template <int kStates>
[[gnu::target("avx2")]] void trellis_confidence_avx2(
    const TrellisRecord& record, float* bit_confidence_out) {
  constexpr int kQuads = kStates / 4;
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  const int bit_count = record.bit_count;
  const double best = record.best;
  const double energy = record.energy;
  const double* forward_cost = record.forward_cost.data();
  // A terminated trellis may only finish in the all-zero state.
  QuadLanes remaining[kQuads];
  for (int q = 0; q < kQuads; ++q) {
    remaining[q] = quad_splat(record.terminated ? kInfinity : 0.0);
  }
  remaining[0][0] = 0.0;
  for (int k = record.steps - 1; k >= 0; --k) {
    const double* metric =
        record.metrics.data() + static_cast<ptrdiff_t>(k) * 2 * kStates;
    // States 4q to 4q + 3: the metrics of their zero and one branches, and
    // the remaining cost of the states those reach — 2s and 2s + 1 modulo the
    // states, the even and the odd states of quads 2q and 2q + 1 modulo the
    // quads.
    QuadLanes zero_metric[kQuads];
    QuadLanes one_metric[kQuads];
    QuadLanes zero_after[kQuads];
    QuadLanes one_after[kQuads];
    for (int q = 0; q < kQuads; ++q) {
      const auto at = 8 * static_cast<ptrdiff_t>(q);
      const QuadLanes low = quad_load(metric + at);
      const QuadLanes high = quad_load(metric + at + 4);
      zero_metric[q] = quad_alternate<false>(low, high);
      one_metric[q] = quad_alternate<true>(low, high);
      const QuadLanes& first = remaining[(2 * q) % kQuads];
      const QuadLanes& second = remaining[(2 * q + 1) % kQuads];
      zero_after[q] = quad_alternate<false>(first, second);
      one_after[q] = quad_alternate<true>(first, second);
    }
    if (k < bit_count) {
      const bool flipped_to_one = record.decided[static_cast<size_t>(k)] == 0;
      const double* forward =
          forward_cost + static_cast<ptrdiff_t>(k) * kStates;
      QuadLanes cheapest = quad_splat(kInfinity);
      for (int q = 0; q < kQuads; ++q) {
        const QuadLanes through =
            (quad_load(forward + 4 * static_cast<ptrdiff_t>(q)) +
             (flipped_to_one ? one_metric[q] : zero_metric[q])) +
            (flipped_to_one ? one_after[q] : zero_after[q]);
        cheapest = quad_min(through, cheapest);
      }
      // No sum here is a NaN or a negative zero, so the least of them is the
      // same number in whatever order they are compared.
      double best_flipped = cheapest[0];
      for (int lane = 1; lane < 4; ++lane) {
        best_flipped =
            cheapest[lane] < best_flipped ? cheapest[lane] : best_flipped;
      }
      double confidence = 1.0;
      if (std::isfinite(best_flipped) && energy > 0.0) {
        confidence = std::clamp((best_flipped - best) / energy, 0.0, 1.0);
      }
      bit_confidence_out[k] = static_cast<float>(confidence);
    }
    const bool both = !(record.tail_bits > 0 && k >= bit_count);
    for (int q = 0; q < kQuads; ++q) {
      const QuadLanes through_zero = zero_metric[q] + zero_after[q];
      remaining[q] = both ? quad_min(one_metric[q] + one_after[q], through_zero)
                          : through_zero;
    }
  }
}
#endif
// The steps of |run| (see TrellisSteps), its winners to |from_upper|: on AVX2
// where the processor has it, a pair of branches at a time otherwise, the
// same either way. Returns the steps whose grid bit was on the grid.
template <int kStates, uint32_t kHistories, int kPhases, TrellisTail kTail,
          typename Winners>
int trellis_steps_at(TrellisSteps<kStates>& run, Winners* from_upper) {
#if defined(ORC_TELETEXT_AVX2)
  if (run.avx2) {
    return trellis_steps_avx2<kStates, kHistories, kPhases, kTail>(run,
                                                                   from_upper);
  }
#endif
  return trellis_steps_pairs<kStates, kHistories, kPhases, kTail>(run,
                                                                  from_upper);
}

template <int kStates, uint32_t kHistories, TrellisTail kTail, typename Winners>
int trellis_steps(TrellisSteps<kStates>& run, Winners* from_upper) {
  switch (run.phases) {
    case 1:
      return trellis_steps_at<kStates, kHistories, 1, kTail>(run, from_upper);
    case 2:
      return trellis_steps_at<kStates, kHistories, 2, kTail>(run, from_upper);
    default:
      return trellis_steps_at<kStates, kHistories, kMaxMlseSamplesPerBit,
                              kTail>(run, from_upper);
  }
}

// Viterbi detection of |bit_count| payload bits from |grid| (a
// fit.phases-per-bit grid, see resample_bit_grid), the first payload bit being
// grid bit |first_payload_bit|. The trellis starts from the known tail of the
// framing code rather than an unknown state, so the first payload bits are
// detected with the same context as the rest. Returns the RMS reconstruction
// error over the evaluated grid samples.
//
// When |bit_errors_out| is non-null it receives |bit_count| RMS reconstruction
// errors, one per payload bit, in level-domain counts — the same quantity the
// return value summarises, resolved along the packet so its shape can be read.
//
// When |fit| has a tail (fit_channel_with_tail), the response to the bits older
// than the tap window is taken from each state's survivor path: the trellis
// keeps its kChannelTaps - 1 bits of state, and every branch out of a state
// adds the tail its own history implies (delayed decision-feedback sequence
// estimation, see kChannelTailTaps).
//
// When |record| is non-null the trellis is kept in it — the branch metrics and
// the forward costs of every step — for mlse_confidence() to read the
// confidence of each decision off later.
double mlse_detect(const std::vector<double>& grid, int first_payload_bit,
                   const ChannelFit& fit, const FramingCode& framing,
                   int bit_count, uint8_t* bits_out, float* bit_errors_out,
                   TrellisRecord* record, int tail_bits, bool avx2) {
  // The two states that can precede a state differ only in the bit that has
  // just left the register, which is the top bit of the state.
  constexpr int kUpperHalf = kMlseStates / 2;
  const int phases = fit.phases;
  const auto grid_bits =
      static_cast<int>(grid.size() / static_cast<size_t>(phases));

  // The channel spreads each bit kChannelCentre bit periods forward, so the
  // last payload samples are only fully determined kChannelCentre steps after
  // the last payload bit. Run the trellis on past the payload and discard
  // those trailing bits, otherwise the final bits are decided by no sample of
  // their own and come back arbitrary.
  //
  // |tail_bits| > 0 goes further and terminates the trellis: the line returns
  // to black after the 360th bit (ETSI EN 300 706 §7.1), so those trailing bits
  // are known zeros, and forcing them constrains the last payload bits with
  // evidence on both sides — the same footing every other bit is decided on.
  const int steps = bit_count + std::max(kChannelCentre, tail_bits);

  // Expected sample for each phase and each branch: the tap window spans the
  // new bit and the kChannelTaps - 1 bits held in the state. Each phase reads
  // the channel at a different point of its pulse response, so each has its
  // own tap vector and therefore its own row.
  std::array<double, static_cast<size_t>(kTapPatterns) * kMaxMlseSamplesPerBit>
      expected{};
  for (int p = 0; p < phases; ++p) {
    const auto& taps = fit.taps[static_cast<size_t>(p)];
    double* row = &expected[static_cast<size_t>(p) * kTapPatterns];
    for (int s = 0; s < kMlseStates; ++s) {
      double base = fit.offset;
      for (int j = 0; j < kChannelTaps - 1; ++j) {
        const int shift = kChannelTaps - 2 - j;
        base += taps[static_cast<size_t>(j)] *
                static_cast<double>((s >> shift) & 1);
      }
      const auto at = 2 * static_cast<ptrdiff_t>(s);
      row[at] = base;
      row[at + 1] = base + taps[kChannelTaps - 1];
    }
  }

  // Branch metrics of step k: they depend on the state and the new bit but
  // not on the path that reached the state, save through the tail, which is
  // that path's. Every phase sample of the bit is evidence for the same branch,
  // which is what multiplies the evidence per bit by |phases|. At the first
  // steps the sample fully determined by a branch sits inside the preamble: the
  // channel has smeared the leading payload bits back into it, and it is the
  // only evidence those bits get on that side, so skipping it costs accuracy in
  // the first byte.
  TrellisStorage& storage = trellis_storage(steps);
  TrellisSteps<kMlseStates> run;
  run.grid = grid.data();
  run.grid_bits = grid_bits;
  run.phases = phases;
  run.first_sample_bit = first_payload_bit - kChannelCentre;
  run.steps = steps;
  // Past the payload the transmitted bits are known black, so a terminated
  // trellis has only the zero branch to offer.
  run.zero_from = tail_bits > 0 ? bit_count : steps;
  run.rows = expected.data();
  run.avx2 = avx2;

  // With a tail, the bits of each state's survivor path older than the state
  // itself, the most recent in bit 0, and the tail each such history implies,
  // per phase, worked out once for the fit. Only the kChannelTailTaps most
  // recent of those bits are ever read, so only they are kept. A branch's tail
  // is its origin state's, read off that state's history, so the pairs of
  // expected samples are laid out per state and history, each the row's and
  // that history's tail added once for the fit rather than once a step.
  std::vector<double>& tailed = storage.tailed;
  if (fit.has_tail) {
    std::array<std::array<double, kTailHistories>, kMaxMlseSamplesPerBit>
        tail_for{};
    for (int p = 0; p < phases; ++p) {
      for (uint32_t h = 0; h < kTailHistories; ++h) {
        double sum = 0.0;
        for (int d = 0; d < kChannelTailTaps; ++d) {
          if (((h >> d) & 1U) != 0) {
            sum += fit.tail[static_cast<size_t>(p)][static_cast<size_t>(d)];
          }
        }
        tail_for[static_cast<size_t>(p)][h] = sum;
      }
    }
    const int pair_stride = 2 * phases;
    tailed.resize(static_cast<size_t>(kMlseStates) * kTailHistories *
                  static_cast<size_t>(pair_stride));
    for (int s = 0; s < kMlseStates; ++s) {
      for (uint32_t h = 0; h < kTailHistories; ++h) {
        double* out = &tailed[(static_cast<size_t>(s) * kTailHistories + h) *
                              static_cast<size_t>(pair_stride)];
        for (int p = 0; p < phases; ++p) {
          const double* row = &expected[static_cast<size_t>(p) * kTapPatterns];
          const double tail = tail_for[static_cast<size_t>(p)][h];
          const auto pair = 2 * static_cast<ptrdiff_t>(s);
          const auto at = 2 * static_cast<ptrdiff_t>(p);
          out[at] = row[pair] + tail;
          out[at + 1] = row[pair + 1] + tail;
        }
      }
    }
    run.tailed = tailed.data();
  }

  // The traceback needs one bit per state and step: which of the two
  // predecessors won. The bit that entered the state is its low bit.
  //
  // Confidence needs the trellis read backwards as well as forwards, which
  // needs the cost of reaching each state at each step and the branch metrics
  // of each step again. Both are kept, in |record|, only when a caller asked
  // for them.
  uint16_t* from_upper = storage.from_upper.data();
  const bool want_confidence = record != nullptr;
  uint8_t* decided = storage.decided.data();
  if (want_confidence) {
    const auto count = static_cast<size_t>(steps);
    if (record->decided.size() < count) {
      record->metrics.resize(count * kTapPatterns);
      record->forward_cost.resize(count * kMlseStates);
      record->decided.resize(count);
    }
    run.forward_cost = record->forward_cost.data();
    run.metrics = record->metrics.data();
    decided = record->decided.data();
  }

  run.cost.fill(std::numeric_limits<double>::infinity());
  int init_state = 0;
  for (int i = 0; i < kChannelTaps - 1; ++i) {
    init_state |= preamble_bit(kPreambleBits - 1 - i, framing) << i;
  }
  run.cost[static_cast<size_t>(init_state)] = 0.0;
  if (fit.has_tail) {
    uint32_t preamble_history = 0;
    for (int d = 0; d < kChannelTailTaps; ++d) {
      const int index = kPreambleBits - kChannelTaps - d;
      if (index >= 0 && preamble_bit(index, framing) != 0) {
        preamble_history |= 1U << d;
      }
    }
    run.history[static_cast<size_t>(init_state)] =
        static_cast<uint8_t>(preamble_history & (kTailHistories - 1));
  }

  const int evaluated =
      fit.has_tail
          ? trellis_steps<kMlseStates, kTailHistories, TrellisTail::kTabled>(
                run, from_upper)
          : trellis_steps<kMlseStates, kTailHistories, TrellisTail::kNone>(
                run, from_upper);
  const std::array<double, kMlseStates>& cost = run.cost;

  int state = 0;
  double best = std::numeric_limits<double>::infinity();
  if (tail_bits >= kChannelTaps - 1) {
    // Enough known-zero bits were pushed through to fill the state register, so
    // the terminal state is not a choice: it is all zeros.
    best = cost[0];
    state = 0;
  }
  const bool terminated = std::isfinite(best);
  if (!terminated) {
    for (int s = 0; s < kMlseStates; ++s) {
      if (cost[static_cast<size_t>(s)] < best) {
        best = cost[static_cast<size_t>(s)];
        state = s;
      }
    }
  }
  for (int k = steps - 1; k >= 0; --k) {
    decided[k] = static_cast<uint8_t>(state & 1);
    const bool upper = ((from_upper[k] >> state) & 1) != 0;
    state = (state >> 1) | (upper ? kUpperHalf : 0);
  }
  for (int k = 0; k < bit_count; ++k) {
    bits_out[k] = decided[k];
  }

  if (bit_errors_out != nullptr) {
    // Per-bit reconstruction error: the decided sequence pushed back through
    // the fitted channel, compared with the samples actually read. The trellis
    // ran on past the payload, so the bits the channel spreads into the final
    // payload samples are decided (or known, on a terminated trellis) as well,
    // and the last bits are measured on the same evidence as the rest. Bits
    // before the payload are the known preamble.
    //
    // The channel's output for a window of bits is the trellis's expected
    // sample for the branch that window spells, so it is read from there.
    const auto bit_at = [&](int index) {
      return index < 0 ? preamble_bit(first_payload_bit + index, framing)
                       : static_cast<int>(decided[index]);
    };
    int pattern = 0;
    for (int j = 0; j < kChannelTaps - 1; ++j) {
      pattern = (pattern << 1) | bit_at(j - kChannelCentre);
    }
    for (int n = 0; n < bit_count; ++n) {
      const int sample_bit = first_payload_bit + n;
      if (sample_bit >= grid_bits) {
        break;
      }
      pattern =
          ((pattern << 1) | bit_at(n - kChannelCentre + kChannelTaps - 1)) &
          (kTapPatterns - 1);
      double sum_sq = 0.0;
      for (int p = 0; p < phases; ++p) {
        const double error = grid[grid_index(sample_bit, p, phases)] -
                             expected[static_cast<size_t>(p) * kTapPatterns +
                                      static_cast<size_t>(pattern)];
        sum_sq += error * error;
      }
      bit_errors_out[n] =
          static_cast<float>(std::sqrt(sum_sq / static_cast<double>(phases)));
    }
  }

  if (want_confidence) {
    record->steps = steps;
    record->bit_count = bit_count;
    record->tail_bits = tail_bits;
    record->terminated = terminated;
    record->best = best;
    record->energy = channel_bit_energy(fit);
  }

  // The metric accumulated one squared error per evaluated grid sample: every
  // phase of the payload's own bits, of the kChannelCentre preamble bits above,
  // and of whatever trailing bits a terminated trellis reached. Normalising by
  // that count keeps the returned residual on the same per-sample scale
  // whatever the grid rate and however the trellis was run.
  if (evaluated == 0) {
    return std::numeric_limits<double>::infinity();
  }
  return std::sqrt(best / static_cast<double>(evaluated * phases));
}

// The confidence of every bit |record|'s detection decided in a trellis of
// |kStates| states (see mlse_detect()): the amount by which the best path
// through its trellis carrying the opposite decision for that bit costs more
// than the winning path, as a fraction of the channel's bit energy (see
// channel_bit_energy()), written to |bit_confidence_out|. The winning path's
// cost is a forward quantity and the cost of finishing from any state a
// backward one, so the exact margin for every bit at once needs one extra sweep
// of the trellis, with the forward costs kept.
template <int kStates>
void trellis_confidence(const TrellisRecord& record, float* bit_confidence_out,
                        bool avx2) {
#if defined(ORC_TELETEXT_AVX2)
  if (avx2) {
    trellis_confidence_avx2<kStates>(record, bit_confidence_out);
    return;
  }
#endif
  constexpr int kStateMask = kStates - 1;
  const int steps = record.steps;
  const int bit_count = record.bit_count;
  const int tail_bits = record.tail_bits;
  const bool terminated = record.terminated;
  const double best = record.best;
  const double energy = record.energy;
  const uint8_t* decided = record.decided.data();
  const double* forward_cost = record.forward_cost.data();
  // Backward sweep. remaining[s] is the cheapest way of finishing the trellis
  // from state s at the step under consideration, so the cheapest path that
  // takes a named branch is the forward cost of its origin, plus the branch,
  // plus the remaining cost of where it lands. Comparing that against the
  // winning path's cost gives the margin of the decision at that step —
  // exactly, and for every bit in one sweep.
  std::array<double, kStates> remaining{};
  for (int s = 0; s < kStates; ++s) {
    // A terminated trellis may only finish in the all-zero state; otherwise
    // any state will do, as mlse_detect()'s traceback assumed.
    remaining[static_cast<size_t>(s)] =
        (terminated && s != 0) ? std::numeric_limits<double>::infinity() : 0.0;
  }

  for (int k = steps - 1; k >= 0; --k) {
    const double* metric =
        record.metrics.data() + static_cast<ptrdiff_t>(k) * 2 * kStates;
    const int branches = (tail_bits > 0 && k >= bit_count) ? 1 : 2;
    const double* forward = forward_cost + static_cast<ptrdiff_t>(k) * kStates;

    if (k < bit_count) {
      const int flipped = 1 - static_cast<int>(decided[k]);
      double best_flipped = std::numeric_limits<double>::infinity();
      for (int s = 0; s < kStates; ++s) {
        const int to = ((s << 1) | flipped) & kStateMask;
        best_flipped =
            std::min(best_flipped, forward[s] + metric[2 * s + flipped] +
                                       remaining[static_cast<size_t>(to)]);
      }
      // Normalised by the penalty an undamaged line would impose on the same
      // flip, and clipped there: past that the decision is not usefully more
      // certain, and noise pushing in the helpful direction can carry the
      // margin above it.
      double confidence = 1.0;
      if (std::isfinite(best_flipped) && energy > 0.0) {
        confidence = std::clamp((best_flipped - best) / energy, 0.0, 1.0);
      }
      bit_confidence_out[k] = static_cast<float>(confidence);
    }

    std::array<double, kStates> previous;
    for (int s = 0; s < kStates; ++s) {
      const int to = (s << 1) & kStateMask;
      const auto pair = 2 * static_cast<ptrdiff_t>(s);
      double cheapest = metric[pair] + remaining[static_cast<size_t>(to)];
      if (branches == 2) {
        cheapest =
            std::min(cheapest,
                     metric[pair + 1] + remaining[static_cast<size_t>(to | 1)]);
      }
      previous[static_cast<size_t>(s)] = cheapest;
    }
    remaining = previous;
  }
}

void mlse_confidence(const TrellisRecord& record, float* bit_confidence_out,
                     bool avx2) {
  trellis_confidence<kMlseStates>(record, bit_confidence_out, avx2);
}

// The channel of the wide reading (see kWideTaps), fitted with its tail.
using WideChannelFit = WindowFit<kWideTaps, kWideTailTaps>;

// Survivor histories the wide reading's tail is read for: every pattern of the
// kWideTailTaps bits before a state.
constexpr uint32_t kWideTailHistories = 1U << kWideTailTaps;

// The wide channel's response to one bit, as channel_bit_energy() has it.
double wide_bit_energy(const WideChannelFit& fit) {
  double energy = 0.0;
  for (int p = 0; p < fit.phases; ++p) {
    for (const double tap : fit.taps[static_cast<size_t>(p)]) {
      energy += tap * tap;
    }
    for (const double tap : fit.tail[static_cast<size_t>(p)]) {
      energy += tap * tap;
    }
  }
  return energy;
}

// The storage a wide detection needs and does not keep (see TrellisStorage).
struct WideTrellisStorage {
  std::vector<uint32_t> from_upper;
  std::vector<uint8_t> decided;
  // The trellis of the wide reading's last detection (see read_wide()).
  TrellisRecord record;
};

WideTrellisStorage& wide_trellis_storage(int steps) {
  WideTrellisStorage& storage = per_thread<WideTrellisStorage>();
  const auto count = static_cast<size_t>(steps);
  if (storage.decided.size() < count) {
    storage.from_upper.resize(count);
    storage.decided.resize(count);
  }
  return storage;
}

// mlse_detect() over the wide window (kWideTaps): step k decides payload bit k
// and scores grid bit |first_payload_bit| + k - kWideAhead, whose window that
// bit closes, on through every grid bit after the packet, where the
// transmitted bits are known black, which terminates the trellis; the bits
// older than the window are each state's survivor's (kWideTailTaps). When
// |record| is not null the trellis is kept in it for trellis_confidence().
void mlse_detect_wide(const std::vector<double>& grid, int first_payload_bit,
                      const WideChannelFit& fit, const FramingCode& framing,
                      int bit_count, uint8_t* bits_out, TrellisRecord* record,
                      bool avx2) {
  constexpr int kUpperHalf = kWideStates / 2;
  const int phases = fit.phases;
  const auto grid_bits =
      static_cast<int>(grid.size() / static_cast<size_t>(phases));
  const int steps =
      std::max(bit_count, grid_bits - first_payload_bit + kWideAhead);

  // Each state's pair of expected samples at each phase, its new bit 0 and 1,
  // a row per phase as TrellisSteps reads them; and the tail of each history,
  // per phase, added to them at every step.
  std::array<double, static_cast<size_t>(kWidePatterns) * kMaxMlseSamplesPerBit>
      rows{};
  std::array<double,
             static_cast<size_t>(kWideTailHistories) * kMaxMlseSamplesPerBit>
      tail_for{};
  for (int p = 0; p < phases; ++p) {
    const auto& taps = fit.taps[static_cast<size_t>(p)];
    double* row = &rows[static_cast<size_t>(p) * kWidePatterns];
    for (int s = 0; s < kWideStates; ++s) {
      double base = fit.offset;
      for (int j = 0; j < kWideTaps - 1; ++j) {
        base += taps[static_cast<size_t>(j)] *
                static_cast<double>((s >> (kWideTaps - 2 - j)) & 1);
      }
      const auto at = 2 * static_cast<ptrdiff_t>(s);
      row[at] = base;
      row[at + 1] = base + taps[kWideTaps - 1];
    }
    for (uint32_t h = 0; h < kWideTailHistories; ++h) {
      double sum = 0.0;
      for (int d = 0; d < kWideTailTaps; ++d) {
        if (((h >> d) & 1U) != 0) {
          sum += fit.tail[static_cast<size_t>(p)][static_cast<size_t>(d)];
        }
      }
      tail_for[static_cast<size_t>(p) * kWideTailHistories + h] = sum;
    }
  }

  WideTrellisStorage& storage = wide_trellis_storage(steps);
  uint32_t* from_upper = storage.from_upper.data();
  uint8_t* decided = storage.decided.data();
  TrellisSteps<kWideStates> run;
  run.grid = grid.data();
  run.grid_bits = grid_bits;
  run.phases = phases;
  run.first_sample_bit = first_payload_bit - kWideAhead;
  run.steps = steps;
  // Past the payload the transmitted bits are known black.
  run.zero_from = bit_count;
  run.rows = rows.data();
  run.tail_for = tail_for.data();
  run.avx2 = avx2;
  if (record != nullptr) {
    const auto count = static_cast<size_t>(steps);
    if (record->decided.size() < count) {
      record->metrics.resize(count * kWidePatterns);
      record->forward_cost.resize(count * kWideStates);
      record->decided.resize(count);
    }
    run.metrics = record->metrics.data();
    run.forward_cost = record->forward_cost.data();
    decided = record->decided.data();
  }

  run.cost.fill(std::numeric_limits<double>::infinity());
  int init_state = 0;
  for (int i = 0; i < kWideTaps - 1; ++i) {
    init_state |= preamble_bit(first_payload_bit - 1 - i, framing) << i;
  }
  run.cost[static_cast<size_t>(init_state)] = 0.0;
  uint32_t preamble_history = 0;
  for (int d = 0; d < kWideTailTaps; ++d) {
    const int index = first_payload_bit - kWideTaps - d;
    if (index >= 0 && preamble_bit(index, framing) != 0) {
      preamble_history |= 1U << d;
    }
  }
  run.history[static_cast<size_t>(init_state)] =
      static_cast<uint8_t>(preamble_history);

  trellis_steps<kWideStates, kWideTailHistories, TrellisTail::kAdded>(
      run, from_upper);
  const std::array<double, kWideStates>& cost = run.cost;

  // Enough known-black bits are pushed through to fill the state register
  // wherever the grid holds the line's return to black; otherwise the cheapest
  // state is taken, as mlse_detect() takes it.
  int state = 0;
  double best = cost[0];
  const bool terminated =
      steps - bit_count >= kWideTaps - 1 && std::isfinite(best);
  if (!terminated) {
    best = std::numeric_limits<double>::infinity();
    for (int s = 0; s < kWideStates; ++s) {
      if (cost[static_cast<size_t>(s)] < best) {
        best = cost[static_cast<size_t>(s)];
        state = s;
      }
    }
  }
  for (int k = steps - 1; k >= 0; --k) {
    decided[k] = static_cast<uint8_t>(state & 1);
    const bool upper = ((from_upper[k] >> state) & 1U) != 0;
    state = (state >> 1) | (upper ? kUpperHalf : 0);
  }
  std::copy(decided, decided + bit_count, bits_out);

  if (record != nullptr) {
    record->steps = steps;
    record->bit_count = bit_count;
    record->tail_bits = steps - bit_count;
    record->terminated = terminated;
    record->best = best;
    record->energy = wide_bit_energy(fit);
  }
}

// Whether a System C packet structure byte describes a packet that can exist.
// CEA-516 §4.2.1 places the data group header in the data block of the
// synchronizing packet that opens a group (PS b2 = 1), and §3.4 gives a packet
// whose suffix bits b8,b6 are 1,1 no data block at all — everything after its
// prefix is error protection. A packet claiming to be both has nowhere to carry
// the header it opens a group with. A line repeating one byte that happens to
// be the Hamming 8/4 codeword for 1101 — a test signal of period three bits
// does — decodes as exactly that, with every byte of its prefix a codeword.
bool nabts_packet_structure_possible(uint8_t packet_structure) {
  const int ps = teletext_hamming84_decode(packet_structure);
  if (ps < 0) {
    return false;
  }
  const bool synchronizing = (ps & 0x1) != 0;
  const bool suffix_only = ((ps >> 2) & 0x3) == 0x3;
  return !(synchronizing && suffix_only);
}

// The synchronization sequence as the channel |fit| reads it off |grid| with
// none of its 24 bits assumed. The line is black before the clock run-in and
// carries |payload| after the framing code, so a trellis started from the
// all-black state and held to the payload decides the most likely sequence.
//
// The detector's own channel fits cannot be used for this: both were fitted
// with the sequence assumed, and a fit that has taken it as given reads it back
// from almost any line. |fit| must take its equations from the payload alone.
struct SyncReadBack {
  // Framing-code bits that differ from the service's code.
  int framing_errors = kFramingBits;
  // Run-in bits after the first kRunInUnjudgedBits that break the alternation,
  // in whichever phase fits better: a reading at a shifted alignment sees the
  // run-in a bit off, and the run-in carries the bit clock, not the byte
  // alignment.
  int run_in_errors = kRunInBits;
};

SyncReadBack read_back_sync(const std::vector<double>& grid,
                            const ChannelFit& fit, const FramingCode& framing,
                            const uint8_t* payload, int payload_bits) {
  constexpr int kUpperHalf = kMlseStates / 2;
  constexpr int kSteps = kPreambleBits + kChannelTaps - 1;
  const int phases = fit.phases;
  const auto grid_bits =
      static_cast<int>(grid.size() / static_cast<size_t>(phases));

  std::array<std::array<double, kTapPatterns>, kMaxMlseSamplesPerBit>
      expected{};
  for (int p = 0; p < phases; ++p) {
    const auto& taps = fit.taps[static_cast<size_t>(p)];
    auto& row = expected[static_cast<size_t>(p)];
    for (int s = 0; s < kMlseStates; ++s) {
      double base = fit.offset;
      for (int j = 0; j < kChannelTaps - 1; ++j) {
        const int shift = kChannelTaps - 2 - j;
        base += taps[static_cast<size_t>(j)] *
                static_cast<double>((s >> shift) & 1);
      }
      row[2 * static_cast<size_t>(s)] = base;
      row[2 * static_cast<size_t>(s) + 1] = base + taps[kChannelTaps - 1];
    }
  }

  std::array<double, kMlseStates> cost{};
  cost.fill(std::numeric_limits<double>::infinity());
  cost[0] = 0.0;
  std::array<uint16_t, kSteps> from_upper{};
  for (int k = 0; k < kSteps; ++k) {
    std::array<double, kTapPatterns> metric{};
    const int sample_bit = k - kChannelCentre;
    if (sample_bit >= 0 && sample_bit < grid_bits) {
      const double* observed = &grid[grid_index(sample_bit, 0, phases)];
      for (int p = 0; p < phases; ++p) {
        const auto& row = expected[static_cast<size_t>(p)];
        for (int b = 0; b < kTapPatterns; ++b) {
          const double error = observed[p] - row[static_cast<size_t>(b)];
          metric[static_cast<size_t>(b)] += error * error;
        }
      }
    }
    std::array<double, kMlseStates> next{};
    unsigned upper = 0;
    for (int to = 0; to < kMlseStates; ++to) {
      const int lower = to >> 1;
      const double via_lower =
          cost[static_cast<size_t>(lower)] + metric[static_cast<size_t>(to)];
      const double via_upper = cost[static_cast<size_t>(lower) + kUpperHalf] +
                               metric[static_cast<size_t>(to) + kMlseStates];
      const bool take_upper = via_upper < via_lower;
      next[static_cast<size_t>(to)] = take_upper ? via_upper : via_lower;
      upper |= static_cast<unsigned>(take_upper) << to;
    }
    if (k >= kPreambleBits) {
      const int n = k - kPreambleBits;
      const int known = n < payload_bits ? payload[n] : 0;
      for (int to = 0; to < kMlseStates; ++to) {
        if ((to & 1) != known) {
          next[static_cast<size_t>(to)] =
              std::numeric_limits<double>::infinity();
        }
      }
    }
    from_upper[static_cast<size_t>(k)] = static_cast<uint16_t>(upper);
    cost = next;
  }

  int state = 0;
  double best = std::numeric_limits<double>::infinity();
  for (int s = 0; s < kMlseStates; ++s) {
    if (cost[static_cast<size_t>(s)] < best) {
      best = cost[static_cast<size_t>(s)];
      state = s;
    }
  }
  SyncReadBack read_back;
  read_back.framing_errors = 0;
  int run_in_mismatches = 0;
  for (int k = kSteps - 1; k >= 0; --k) {
    const int bit = state & 1;
    if (k >= kRunInBits && k < kPreambleBits) {
      read_back.framing_errors +=
          bit != framing[static_cast<size_t>(k - kRunInBits)] ? 1 : 0;
    } else if (k >= kRunInUnjudgedBits && k < kRunInBits) {
      run_in_mismatches += bit != preamble_bit(k, framing) ? 1 : 0;
    }
    const bool upper = ((from_upper[static_cast<size_t>(k)] >> state) & 1) != 0;
    state = (state >> 1) | (upper ? kUpperHalf : 0);
  }
  constexpr int kJudgedRunInBits = kRunInBits - kRunInUnjudgedBits;
  read_back.run_in_errors =
      std::min(run_in_mismatches, kJudgedRunInBits - run_in_mismatches);
  return read_back;
}

// The channel's prediction of grid bit |k| at phase |p| for the bits |bit_at|
// supplies: the tap window and, where the fit has one, its tail.
template <typename BitAt>
double predicted_sample(const ChannelFit& fit, int k, int p, BitAt bit_at) {
  double predicted = fit.offset;
  for (int j = 0; j < kChannelTaps; ++j) {
    if (bit_at(k - kChannelCentre + j) != 0) {
      predicted += fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)];
    }
  }
  if (fit.has_tail) {
    for (int d = 0; d < kChannelTailTaps; ++d) {
      if (bit_at(k - kChannelCentre - 1 - d) != 0) {
        predicted += fit.tail[static_cast<size_t>(p)][static_cast<size_t>(d)];
      }
    }
  }
  return predicted;
}

// predicted_sample() of every phase of grid bits |first| to |last|, a bit's
// phases together in |out|. Each bit's window and tail are read once for all
// its phases, and the window's part of a prediction, which depends on the
// window's pattern alone, is worked out once for each of the 32 patterns,
// adding what predicted_sample() adds in the order it adds it.
template <typename BitAt>
void predict_samples(const ChannelFit& fit, BitAt bit_at, int first, int last,
                     double* out) {
  const int phases = fit.phases;
  std::array<double, kTapPatterns * kMaxMlseSamplesPerBit> window_part;
  for (int p = 0; p < phases; ++p) {
    for (int pattern = 0; pattern < kTapPatterns; ++pattern) {
      double value = fit.offset;
      for (int j = 0; j < kChannelTaps; ++j) {
        if (tap_bit(pattern, j) != 0) {
          value += fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)];
        }
      }
      const int at = p * kTapPatterns + pattern;
      window_part[static_cast<size_t>(at)] = value;
    }
  }
  for (int k = first; k <= last; ++k) {
    int pattern = 0;
    for (int j = 0; j < kChannelTaps; ++j) {
      pattern = (pattern << 1) | (bit_at(k - kChannelCentre + j) != 0 ? 1 : 0);
    }
    unsigned older = 0;
    if (fit.has_tail) {
      for (int d = 0; d < kChannelTailTaps; ++d) {
        if (bit_at(k - kChannelCentre - 1 - d) != 0) {
          older |= 1U << d;
        }
      }
    }
    for (int p = 0; p < phases; ++p) {
      const int at = p * kTapPatterns + pattern;
      double value = window_part[static_cast<size_t>(at)];
      for (int d = 0; d < kChannelTailTaps; ++d) {
        if (((older >> d) & 1U) != 0) {
          value += fit.tail[static_cast<size_t>(p)][static_cast<size_t>(d)];
        }
      }
      out[(k - first) * phases + p] = value;
    }
  }
}

// The Catmull-Rom interpolant of |line| and its slope at |t|, from the line's
// cubic_coefficients where the four-point window is inside it.
void interpolate_with_slope(const int16_t* line, size_t sample_count,
                            const std::vector<double>& cubic, double t,
                            double& value, double& slope) {
  if (t >= 1.0 && t < static_cast<double>(sample_count) - 2.0) {
    const auto base = static_cast<size_t>(t);
    const double x = t - static_cast<double>(base);
    const double* c = &cubic[4 * base];
    value = catmull_rom_at(c, x);
    slope = (3.0 * c[0] * x + 2.0 * c[1]) * x + c[2];
    return;
  }
  value = teletext_interpolate_sample(line, sample_count, t);
  slope = 2.0 * (teletext_interpolate_sample(line, sample_count, t + 0.25) -
                 teletext_interpolate_sample(line, sample_count, t - 0.25));
}

// Where the line's bits fall against the grid read at |t0|: for each
// kTimingSegmentBits stretch of grid bits |first| to |last|, the shift of the
// samples (Gauss-Newton on the channel's prediction from |bit_at|) that best
// matches them, and a straight line through those shifts, each weighed by how
// much its samples' slope says about it. Returns the line as |offset| +
// |drift| × bit position, in samples; false when nothing could be measured.
template <typename BitAt>
bool measure_timing(const int16_t* line, size_t sample_count,
                    const std::vector<double>& cubic, double t0, double spb,
                    int phases, const ChannelFit& fit, BitAt bit_at, int first,
                    int last, double& offset, double& drift) {
  // The channel's prediction of every sample the segments read; the shifts
  // move the samples, not the prediction, so it is worked out once.
  if (last - first + 1 > kMaxFitPositions) {
    return false;
  }
  std::array<double, kMaxFitPositions * kMaxMlseSamplesPerBit> predicted;
  predict_samples(fit, bit_at, first, last, predicted.data());
  double sw = 0.0;
  double sx = 0.0;
  double sy = 0.0;
  double sxx = 0.0;
  double sxy = 0.0;
  for (int a = first; a <= last; a += kTimingSegmentBits) {
    const int b = std::min(a + kTimingSegmentBits, last + 1);
    double shift = 0.0;
    double weight = 0.0;
    for (int step = 0; step < kTimingSteps; ++step) {
      double along = 0.0;
      double slope_energy = 0.0;
      for (int k = a; k < b; ++k) {
        for (int p = 0; p < phases; ++p) {
          const double position =
              static_cast<double>(k) + phase_offset(p, phases);
          double value = 0.0;
          double slope = 0.0;
          interpolate_with_slope(line, sample_count, cubic,
                                 t0 + position * spb + shift, value, slope);
          const int at = (k - first) * phases + p;
          along += (predicted[static_cast<size_t>(at)] - value) * slope;
          slope_energy += slope * slope;
        }
      }
      if (!(slope_energy > 0.0)) {
        break;
      }
      shift +=
          std::clamp(along / slope_energy, -kTimingMaxStep, kTimingMaxStep);
      weight = slope_energy;
    }
    const double centre = 0.5 * static_cast<double>(a + b - 1);
    sw += weight;
    sx += weight * centre;
    sy += weight * shift;
    sxx += weight * centre * centre;
    sxy += weight * centre * shift;
  }
  const double determinant = sw * sxx - sx * sx;
  if (!(sw > 0.0) || !(determinant > 0.0)) {
    return false;
  }
  drift = (sw * sxy - sx * sy) / determinant;
  offset = (sy - drift * sx) / sw;
  return true;
}

// Byte |index| of a reading's payload bits, LSB first (§7.1).
uint8_t packet_byte(const std::vector<uint8_t>& bits, size_t index) {
  uint8_t byte = 0;
  for (size_t n = 0; n < 8; ++n) {
    if (bits[index * 8 + n] != 0) {
      byte = static_cast<uint8_t>(byte | (1u << n));
    }
  }
  return byte;
}

// The suffix code of a System C reading's packet structure byte, PS b8,b6
// (CEA-516 §3.4): 0 none, 1 the longitudinal check, 2 and 3 the others; -1
// when PS did not decode.
int packet_suffix_code(const std::vector<uint8_t>& bits) {
  if (bits.size() != kNabtsPacketBytes * 8) {
    return -1;
  }
  const int ps =
      teletext_hamming84_decode(packet_byte(bits, kNabtsPacketStructureByte));
  return ps < 0 ? -1 : ((((ps >> 3) & 1) << 1) | ((ps >> 2) & 1));
}

// The checks of its data a System C reading breaks (its prefix is judged
// apart, as the refits judge it — see PrefixStanding): on a packet whose
// structure byte announces them, each data byte failing odd parity (CEA-516
// §3.3) and a failing longitudinal check (§3.4); on a packet with no suffix,
// each data byte failing the parity a broadcast teletext group gives its bytes.
int data_check_failures(const std::vector<uint8_t>& bits) {
  if (bits.size() != kNabtsPacketBytes * 8) {
    return 0;
  }
  std::array<uint8_t, kNabtsPacketBytes> bytes{};
  for (size_t n = 0; n < bits.size(); ++n) {
    if (bits[n] != 0) {
      bytes[n >> 3] |= static_cast<uint8_t>(1u << (n & 7));
    }
  }
  const int suffix = packet_suffix_code(bits);
  int failures = 0;
  if (suffix == 0) {
    for (size_t b = kNabtsPacketStructureByte + 1; b < kNabtsPacketBytes; ++b) {
      failures += odd_parity(bytes[b]) ? 0 : 1;
    }
  } else if (suffix == 1) {
    uint8_t check = 0;
    for (size_t b = kNabtsPacketStructureByte + 1; b < kNabtsPacketBytes; ++b) {
      check = static_cast<uint8_t>(check ^ bytes[b]);
      if (static_cast<int>(b) <= kNabtsLastCheckedDataByte) {
        failures += odd_parity(bytes[b]) ? 0 : 1;
      }
    }
    failures += check == kNabtsLongitudinalCheck ? 0 : 1;
  }
  return failures;
}

// The exact cost of a whole decided sequence: the squared error, over every
// grid sample mlse_detect() evaluates for it, between the samples and the
// channel's output for the sequence |bit_at| supplies (grid bit indices, so the
// preamble and the black after the packet included), the tail of a fit that has
// one read off the sequence itself. Two readings costed here are compared on
// exactly the terms the trellis compares paths on.
template <typename BitAt>
double sequence_cost(const std::vector<double>& grid, int first_payload_bit,
                     const ChannelFit& fit, int bit_count, int tail_bits,
                     BitAt bit_at) {
  const int phases = fit.phases;
  const auto grid_bits =
      static_cast<int>(grid.size() / static_cast<size_t>(phases));
  const int steps = bit_count + std::max(kChannelCentre, tail_bits);
  // The grid bits the steps score, in order.
  const int first = std::max(first_payload_bit - kChannelCentre, 0);
  const int last =
      std::min(first_payload_bit + steps - 1 - kChannelCentre, grid_bits - 1);
  std::array<double, kMaxFitPositions * kMaxMlseSamplesPerBit> predicted;
  const bool tabled = last - first + 1 <= kMaxFitPositions;
  if (tabled) {
    predict_samples(fit, bit_at, first, last, predicted.data());
  }
  double cost = 0.0;
  for (int bit = first; bit <= last; ++bit) {
    for (int p = 0; p < phases; ++p) {
      const int at = (bit - first) * phases + p;
      const double error = grid[grid_index(bit, p, phases)] -
                           (tabled ? predicted[static_cast<size_t>(at)]
                                   : predicted_sample(fit, bit, p, bit_at));
      cost += error * error;
    }
  }
  return cost;
}

// Byte parity × running exclusive-or: the code registers of the checked
// search's states (see mlse_detect_checked).
constexpr int kCodeStates = 2 * 256;

// The bit position of each value the de Bruijn sequence 0x077CB531 (every
// 5-bit window of it distinct) leaves in the top five bits once multiplied by
// a single set bit.
constexpr std::array<int, 32> kDeBruijnBitPosition = {
    0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
    31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};

// Index of the lowest set bit of a non-zero |bits|, so that the states a mask
// holds are visited in ascending order as a scan would visit them.
inline int lowest_set_bit(unsigned bits) {
  const uint32_t lowest = static_cast<uint32_t>(bits) & (0U - bits);
  return kDeBruijnBitPosition[static_cast<size_t>((lowest * 0x077CB531U) >>
                                                  27)];
}

// The codes one step of the checked search reached, in the order it reached
// them: of each its code registers, the costs of its channel states (infinite
// where it reached none), the mask of the states it reached and of those the
// ones reached from their upper origin. A step reaches each code at most
// once, so the arrays are sized for every code and only |count| are used.
struct CheckedCodes {
  std::vector<uint16_t> code;
  std::vector<double> cost;
  std::vector<uint16_t> finite;
  std::vector<uint16_t> upper;
  int count = 0;

  CheckedCodes()
      : code(kCodeStates),
        cost(static_cast<size_t>(kCodeStates) * kMlseStates),
        finite(kCodeStates),
        upper(kCodeStates) {}
};

// The storage the checked search keeps per thread (see TrellisStorage): the
// branch metrics and the unconstrained finishes of every step; the codes the
// step under way started from and those it reaches; where a code is among the
// latter (none between steps); and for the traceback, the codes each step
// reached and their winners.
struct CheckedStorage {
  std::vector<double> metrics;
  std::vector<double> to_go;
  CheckedCodes live;
  CheckedCodes next;
  std::vector<int16_t> slot_of = std::vector<int16_t>(kCodeStates, -1);
  std::vector<uint16_t> traced_codes;
  std::vector<uint16_t> traced_upper;
  std::vector<size_t> traced_from;
};

// The storage for a search over |steps| trellis steps, of which the first
// kNabtsPrefixBits are held to the reading and need none.
CheckedStorage& checked_storage(int steps) {
  CheckedStorage& storage = per_thread<CheckedStorage>();
  const auto count = static_cast<size_t>(steps - kNabtsPrefixBits);
  if (storage.metrics.size() < count * kTapPatterns) {
    storage.metrics.resize(count * kTapPatterns);
    storage.to_go.resize((count + 1) * kMlseStates);
  }
  return storage;
}

// The most likely payload among those that pass a System C packet's own
// checks (see kMlseMaxCheckedCost): every byte of |parity_bytes| (a mask of
// payload byte indices) closes with odd parity, CEA-516 §3.3, and the
// exclusive-or of everything after the prefix comes to
// kNabtsLongitudinalCheck, §3.4. The prefix bits are held to |decided|, the
// unconstrained detection of the same grid, so the packet keeps the structure
// its prefix declared; on success the payload is written to |bits_out|.
//
// The trellis is mlse_detect()'s with two registers added to each channel
// state: the exclusive-or of the check range so far and the parity of the
// byte under way — 16 × 256 × 2 states. Nearly all of them cost far more than
// the unconstrained reading, and a reading is only wanted within
// |budget_energies| of it, so a state is visited only while its cost so far
// plus the cheapest unconstrained finish from its channel state stays within
// that budget: an admissible bound, since a constrained path can finish no
// cheaper than an unconstrained one. What survives is the handful of states
// that differ from the decided path by an error event cheap enough to matter.
//
// The tail beyond the tap window (kChannelTailTaps) is read off |decided|
// rather than off each path: the paths that survive differ from it in a few
// bits, and the caller costs the one returned exactly before accepting it.
bool mlse_detect_checked(const std::vector<double>& grid, int first_payload_bit,
                         const ChannelFit& fit, const FramingCode& framing,
                         int bit_count, int tail_bits, const uint8_t* decided,
                         uint64_t parity_bytes, double budget_energies,
                         uint8_t* bits_out) {
  constexpr int kUpperHalf = kMlseStates / 2;
  constexpr int kStateMask = kMlseStates - 1;
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  const int fixed_bits = kNabtsPrefixBits;
  const int phases = fit.phases;
  const auto grid_bits =
      static_cast<int>(grid.size() / static_cast<size_t>(phases));
  const int steps = bit_count + std::max(kChannelCentre, tail_bits);
  if (bit_count <= fixed_bits || steps <= fixed_bits) {
    return false;
  }
  const auto decided_at = [&](int index) {
    if (index < first_payload_bit) {
      return index >= 0 ? preamble_bit(index, framing) : 0;
    }
    const int n = index - first_payload_bit;
    return n < bit_count ? static_cast<int>(decided[n]) : 0;
  };
  CheckedStorage& storage = checked_storage(steps);

  // Branch metrics of every step from the first bit after the prefix on, as
  // mlse_detect() forms them.
  std::array<std::array<double, kTapPatterns>, kMaxMlseSamplesPerBit>
      expected{};
  for (int p = 0; p < phases; ++p) {
    const auto& taps = fit.taps[static_cast<size_t>(p)];
    for (int s = 0; s < kMlseStates; ++s) {
      double base = fit.offset;
      for (int j = 0; j < kChannelTaps - 1; ++j) {
        base += taps[static_cast<size_t>(j)] *
                static_cast<double>((s >> (kChannelTaps - 2 - j)) & 1);
      }
      expected[static_cast<size_t>(p)][2 * static_cast<size_t>(s)] = base;
      expected[static_cast<size_t>(p)][2 * static_cast<size_t>(s) + 1] =
          base + taps[kChannelTaps - 1];
    }
  }
  // The storage counts steps from the first bit after the prefix.
  const auto from_prefix = [fixed_bits](int k, int width) {
    return static_cast<size_t>(k - fixed_bits) * static_cast<size_t>(width);
  };
  double* const metrics = storage.metrics.data();
  for (int k = fixed_bits; k < steps; ++k) {
    double* metric = &metrics[from_prefix(k, kTapPatterns)];
    const int bit = first_payload_bit + k - kChannelCentre;
    if (bit < 0 || bit >= grid_bits) {
      std::fill(metric, metric + kTapPatterns, 0.0);
      continue;
    }
    std::array<double, kMaxMlseSamplesPerBit> tail{};
    if (fit.has_tail) {
      std::array<bool, kChannelTailTaps> older{};
      for (int d = 0; d < kChannelTailTaps; ++d) {
        older[static_cast<size_t>(d)] =
            decided_at(bit - kChannelCentre - 1 - d) != 0;
      }
      for (int p = 0; p < phases; ++p) {
        for (int d = 0; d < kChannelTailTaps; ++d) {
          if (older[static_cast<size_t>(d)]) {
            tail[static_cast<size_t>(p)] +=
                fit.tail[static_cast<size_t>(p)][static_cast<size_t>(d)];
          }
        }
      }
    }
    for (int b = 0; b < kTapPatterns; ++b) {
      double sum = 0.0;
      for (int p = 0; p < phases; ++p) {
        const double error =
            grid[grid_index(bit, p, phases)] -
            expected[static_cast<size_t>(p)][static_cast<size_t>(b)] -
            tail[static_cast<size_t>(p)];
        sum += error * error;
      }
      metric[b] = sum;
    }
  }
  // Past the payload a terminated trellis knows the bits are black.
  const auto zero_only = [&](int k) { return tail_bits > 0 && k >= bit_count; };
  const bool terminated = tail_bits >= kChannelTaps - 1;

  // The cheapest unconstrained finish from each channel state at each step:
  // the bound the constrained search is pruned against.
  double* const to_go = storage.to_go.data();
  for (int s = 0; s < kMlseStates; ++s) {
    to_go[from_prefix(steps, kMlseStates) + static_cast<size_t>(s)] =
        (terminated && s != 0) ? kInfinity : 0.0;
  }
  for (int k = steps - 1; k >= fixed_bits; --k) {
    const double* metric = &metrics[from_prefix(k, kTapPatterns)];
    const double* after = &to_go[from_prefix(k + 1, kMlseStates)];
    double* here = &to_go[from_prefix(k, kMlseStates)];
    for (int s = 0; s < kMlseStates; ++s) {
      const int to = (s << 1) & kStateMask;
      const auto pair = 2 * static_cast<ptrdiff_t>(s);
      double cheapest = metric[pair] + after[to];
      if (!zero_only(k)) {
        cheapest = std::min(cheapest, metric[pair + 1] + after[to | 1]);
      }
      here[s] = cheapest;
    }
  }

  int start = 0;
  for (int i = 0; i < kChannelTaps - 1; ++i) {
    start |= decided_at(first_payload_bit + fixed_bits - 1 - i) << i;
  }
  const double unconstrained =
      to_go[from_prefix(fixed_bits, kMlseStates) + static_cast<size_t>(start)];
  if (!std::isfinite(unconstrained)) {
    return false;
  }
  const double limit =
      unconstrained + budget_energies * channel_bit_energy(fit);

  // The code registers of the state a bit leads to at step |k|: the
  // exclusive-or takes the bit at its place in the byte, and a parity byte
  // must close odd. What the step decides is worked out once a step.
  struct StepCoding {
    bool in_payload = false;
    int place = 0;
    bool parity_byte = false;
    bool closing = false;
  };
  const auto coding_of = [&](int k) {
    StepCoding coding;
    coding.in_payload = k < bit_count;
    coding.place = k % 8;
    coding.parity_byte =
        coding.in_payload && ((parity_bytes >> (k / 8)) & 1U) != 0;
    coding.closing = coding.parity_byte && coding.place == 7;
    return coding;
  };
  const auto code_after = [](const StepCoding& coding, int code, int bit,
                             bool& allowed) {
    allowed = true;
    if (!coding.in_payload) {
      return code;
    }
    int parity = code >> 8;
    const int check = (code & 0xFF) ^ (bit << coding.place);
    if (coding.parity_byte) {
      parity ^= bit;
      if (coding.closing) {
        allowed = parity == 1;
        parity = 0;
      }
    }
    return (parity << 8) | check;
  };

  // A forward pass over the states within the limit, one cost per (code,
  // channel) state and, for the traceback, the winning predecessor's top bit.
  // Only the codes a step reached are held, in the order it reached them, and
  // of each only the channel states it reached at a finite cost are carried
  // on.
  CheckedCodes* live = &storage.live;
  CheckedCodes* next = &storage.next;
  int16_t* const slot_of = storage.slot_of.data();
  std::vector<uint16_t>& traced_codes = storage.traced_codes;
  std::vector<uint16_t>& traced_upper = storage.traced_upper;
  std::vector<size_t>& traced_from = storage.traced_from;
  live->count = 1;
  live->code[0] = 0;
  std::fill_n(live->cost.begin(), kMlseStates, kInfinity);
  live->cost[static_cast<size_t>(start)] = 0.0;
  live->finite[0] = static_cast<uint16_t>(1U << start);
  traced_codes.clear();
  traced_upper.clear();
  traced_from.clear();
  for (int k = fixed_bits; k < steps; ++k) {
    const double* metric = &metrics[from_prefix(k, kTapPatterns)];
    const double* after = &to_go[from_prefix(k + 1, kMlseStates)];
    const StepCoding coding = coding_of(k);
    const uint16_t* const codes = live->code.data();
    const double* const costs = live->cost.data();
    const uint16_t* const finite = live->finite.data();
    uint16_t* const next_codes = next->code.data();
    double* const next_costs = next->cost.data();
    uint16_t* const next_finite = next->finite.data();
    uint16_t* const next_upper = next->upper.data();
    const int live_count = live->count;
    int reached_count = 0;
    for (int i = 0; i < live_count; ++i) {
      const int code = codes[i];
      const double* here = costs + static_cast<ptrdiff_t>(i) * kMlseStates;
      const unsigned reachable = finite[i];
      for (int bit = 0; bit < (zero_only(k) ? 1 : 2); ++bit) {
        bool allowed = true;
        const int code_next = code_after(coding, code, bit, allowed);
        if (!allowed) {
          continue;
        }
        int slot = slot_of[code_next];
        for (unsigned left = reachable; left != 0; left &= left - 1) {
          const int s = lowest_set_bit(left);
          const int to = ((s << 1) | bit) & kStateMask;
          const double reached = here[s] + metric[2 * s + bit];
          const double held =
              slot < 0
                  ? kInfinity
                  : next_costs[static_cast<ptrdiff_t>(slot) * kMlseStates + to];
          if (reached + after[to] > limit || !(reached < held)) {
            continue;
          }
          if (slot < 0) {
            slot = reached_count++;
            slot_of[code_next] = static_cast<int16_t>(slot);
            next_codes[slot] = static_cast<uint16_t>(code_next);
            std::fill_n(next_costs + static_cast<ptrdiff_t>(slot) * kMlseStates,
                        kMlseStates, kInfinity);
            next_finite[slot] = 0;
            next_upper[slot] = 0;
          }
          next_costs[static_cast<ptrdiff_t>(slot) * kMlseStates + to] = reached;
          const auto mask = static_cast<uint16_t>(1U << to);
          next_finite[slot] = static_cast<uint16_t>(next_finite[slot] | mask);
          next_upper[slot] =
              s >= kUpperHalf ? static_cast<uint16_t>(next_upper[slot] | mask)
                              : static_cast<uint16_t>(next_upper[slot] & ~mask);
        }
      }
    }
    next->count = reached_count;
    traced_from.push_back(traced_codes.size());
    traced_codes.insert(traced_codes.end(), next_codes,
                        next_codes + reached_count);
    traced_upper.insert(traced_upper.end(), next_upper,
                        next_upper + reached_count);
    for (int i = 0; i < reached_count; ++i) {
      slot_of[next_codes[i]] = -1;
    }
    std::swap(live, next);
    if (live->count == 0) {
      return false;
    }
  }
  traced_from.push_back(traced_codes.size());

  // The check closes with even parity carried into no byte.
  const int final_code = kNabtsLongitudinalCheck;
  int state = 0;
  double best = kInfinity;
  for (int i = 0; i < live->count; ++i) {
    if (live->code[static_cast<size_t>(i)] != final_code) {
      continue;
    }
    for (int s = 0; s < (terminated ? 1 : kMlseStates); ++s) {
      const double reached = live->cost[static_cast<size_t>(i) * kMlseStates +
                                        static_cast<size_t>(s)];
      if (reached < best) {
        best = reached;
        state = s;
      }
    }
  }
  if (!std::isfinite(best)) {
    return false;
  }
  // The winners of the step the traceback is at, of the code its path held.
  const auto upper_at = [&](int k, int code) {
    const size_t step = static_cast<size_t>(k - fixed_bits);
    for (size_t i = traced_from[step]; i < traced_from[step + 1]; ++i) {
      if (traced_codes[i] == code) {
        return static_cast<unsigned>(traced_upper[i]);
      }
    }
    return 0U;
  };
  int code = final_code;
  for (int k = steps - 1; k >= fixed_bits; --k) {
    const int bit = state & 1;
    const bool upper = ((upper_at(k, code) >> state) & 1U) != 0;
    if (k < bit_count) {
      bits_out[k] = static_cast<uint8_t>(bit);
      // The code registers before this bit: the exclusive-or without it, and
      // the parity of the byte so far without it (a parity byte's last bit
      // left the parity at one, the byte closing odd).
      const int byte = k / 8;
      const int place = k % 8;
      int parity = 0;
      if (((parity_bytes >> byte) & 1U) != 0) {
        parity = place == 7 ? (1 ^ bit) : ((code >> 8) ^ bit);
      }
      code = (parity << 8) | ((code & 0xFF) ^ (bit << place));
    }
    state = (state >> 1) | (upper ? kUpperHalf : 0);
  }
  std::copy(decided, decided + fixed_bits, bits_out);
  return true;
}

// The second look a System C reading gets under the packet's own checks (see
// the class comment): nothing unless its structure byte arrived as sent
// announcing the longitudinal check (CEA-516 §3.4), nor when the reading
// passes that and the odd
// parity of every data byte (§3.3); otherwise the most likely reading that
// passes both, taken into |bits| when it costs at most kMlseMaxCheckedCost bit
// energies more than the decided one. Returns the number of bits that
// changed.
int read_under_packet_checks(const std::vector<double>& grid,
                             const ChannelFit& fit, const FramingCode& framing,
                             int payload_bits, int trailing,
                             std::vector<uint8_t>& bits) {
  if (payload_bits != static_cast<int>(kNabtsPacketBytes) * 8 ||
      bits.size() != static_cast<size_t>(payload_bits)) {
    return 0;
  }
  std::array<uint8_t, kNabtsPacketBytes> bytes{};
  for (int n = 0; n < payload_bits; ++n) {
    if (bits[static_cast<size_t>(n)] != 0) {
      bytes[static_cast<size_t>(n) >> 3] |= static_cast<uint8_t>(1u << (n & 7));
    }
  }
  // CEA-516 §3.2.5 names PS's bits b2 b4 b6 b8, and the decoded nibble holds
  // them most significant first as b8 b6 b4 b2: the suffix code b8,b6 = 0,1
  // is the nibble's bit 2 alone of those two. The byte is believed only as
  // sent: three bits wrong in a Hamming 8/4 byte decode as another value with
  // one corrected, and on a service that sends no suffix, PS 0x15 read as
  // 0x2B or 0x2D announced a check that was never sent — a reading made to
  // pass it put parity-passing errors into the data.
  const int ps = teletext_hamming84_decode(bytes[kNabtsPacketStructureByte]);
  if (ps < 0 || !teletext_hamming84_clean(bytes[kNabtsPacketStructureByte]) ||
      ((ps >> 3) & 1) != 0 || ((ps >> 2) & 1) != 1) {
    return 0;
  }
  uint64_t parity_bytes = 0;
  uint8_t check = 0;
  bool parity_passes = true;
  for (size_t b = kNabtsPacketStructureByte + 1; b < kNabtsPacketBytes; ++b) {
    check = static_cast<uint8_t>(check ^ bytes[b]);
    if (static_cast<int>(b) <= kNabtsLastCheckedDataByte) {
      parity_bytes |= uint64_t{1} << b;
      parity_passes = parity_passes && odd_parity(bytes[b]);
    }
  }
  if (parity_passes && check == kNabtsLongitudinalCheck) {
    return 0;
  }
  std::vector<uint8_t> checked(bits.size(), 0);
  if (!mlse_detect_checked(grid, kPreambleBits, fit, framing, payload_bits,
                           trailing, bits.data(), parity_bytes,
                           kMlseMaxCheckedCost + kMlseCheckedSearchMargin,
                           checked.data())) {
    return 0;
  }
  const auto sequence = [&framing,
                         payload_bits](const std::vector<uint8_t>& v) {
    return [&framing, payload_bits, &v](int k) {
      if (k < kPreambleBits) {
        return k >= 0 ? preamble_bit(k, framing) : 0;
      }
      const int index = k - kPreambleBits;
      return index < payload_bits
                 ? static_cast<int>(v[static_cast<size_t>(index)])
                 : 0;
    };
  };
  int changed = 0;
  bool data_changed = false;
  for (size_t n = 0; n < bits.size(); ++n) {
    if (checked[n] != bits[n]) {
      ++changed;
      data_changed =
          data_changed || static_cast<int>(n / 8) <= kNabtsLastCheckedDataByte;
    }
  }
  // Where no data byte failed its parity the check alone placed the error,
  // and a data byte is changed only on the closer call (see
  // kMlseMaxUnlocatedCheckedCost).
  const double limit = parity_passes && data_changed
                           ? kMlseMaxUnlocatedCheckedCost
                           : kMlseMaxCheckedCost;
  const double energy = channel_bit_energy(fit);
  const double extra = sequence_cost(grid, kPreambleBits, fit, payload_bits,
                                     trailing, sequence(checked)) -
                       sequence_cost(grid, kPreambleBits, fit, payload_bits,
                                     trailing, sequence(bits));
  if (!(energy > 0.0) || extra > limit * energy) {
    return 0;
  }
  bits = std::move(checked);
  return changed;
}

// How well a System C reading's Hamming 8/4 protected prefix arrived (CEA-516
// §3.2.2, ETSI EN 300 706 §8.2).
PrefixStanding nabts_prefix_standing(const std::vector<uint8_t>& bits) {
  PrefixStanding standing;
  for (size_t b = 0; b <= kNabtsPacketStructureByte; ++b) {
    const uint8_t byte = packet_byte(bits, b);
    if (teletext_hamming84_decode(byte) < 0) {
      ++standing.uncorrectable;
    } else if (!teletext_hamming84_clean(byte)) {
      ++standing.corrected;
    }
  }
  return standing;
}

// Whether a System C reading's packet structure byte, as read, opens a data
// group (PS b2, CEA-516 §3.2.5): the synchronizing packet, whose data block
// carries the group header (§4.2.1) and the record header after it (§5.2).
bool nabts_synchronizing(const std::vector<uint8_t>& bits) {
  const int ps =
      teletext_hamming84_decode(packet_byte(bits, kNabtsPacketStructureByte));
  return ps >= 0 && (ps & 1) != 0;
}

// The response of |fit| to the bit kWideAhead bit periods past the one a
// sample belongs to, the tap no window of kChannelTaps holds, as a fraction of
// its gain: the mean over the sample phases.
double wide_ahead_fraction(const WideChannelFit& fit) {
  double ahead = 0.0;
  for (int p = 0; p < fit.phases; ++p) {
    ahead += fit.taps[static_cast<size_t>(p)][kWideTaps - 1];
  }
  return ahead / (static_cast<double>(fit.phases) * fit.gain);
}

// The wide reading (see the class comment) of a System C reading |bits|, from
// |grid|, read at the lock, where a bit's response is centred on the tap
// window's centre: the wide channel fitted to the bits decided and the packet
// detected again, refitted while that changes them (kMaxWideRefits). The
// reading is kept — returned true, with it in |bits| and its decision margins
// in |bit_confidence| — only where fewer of its data checks fail (see
// data_check_failures()); its prefix is no worse than the one it started from
// and, where it changed, arrived as sent; and its structure byte is one a
// packet can carry, with the suffix it announced before and still not opening
// a group.
//
// Fewer, not merely no more: the wider channel has more taps to fit to the same
// bits, and on a packet whose data repeat — a record's NUL padding, sent as
// 0x80 — some of them are barely determined. Kept wherever its checks came out
// no worse, it read a constant record's 0x8F as 0x97 on two of three
// transfers of one tape, a transposition parity cannot see; held to mending a
// check, it leaves every packet that arrived passing its checks as it was.
//
// A synchronizing packet is not read wide: its data block is mostly the
// group's and the record's headers (CEA-516 §4.2.1, §5.2), Hamming 8/4 bytes
// that name the record and say how long it is, and a reading made to pass
// more parity checks turns a byte the code would have corrected into another
// codeword. Read wide, tapes filed records under other names: pages were lost,
// records were listed that are no page, and a copy of one page opened a
// second content.
//
// Nor is a packet read wide where the channel fitted to it does not respond to
// the bit kWideAhead periods on (kWideMinAheadFraction): there the wider
// window models nothing the tail refits did not, and what its extra taps fit
// is noise.
bool read_wide(const std::vector<double>& grid, int phases,
               const FramingCode& framing, int payload_bits,
               std::vector<uint8_t>& bits,
               std::array<float, kTeletextPayloadBits>& bit_confidence,
               bool avx2) {
  if (payload_bits != static_cast<int>(kNabtsPacketBytes) * 8 ||
      bits.size() != static_cast<size_t>(payload_bits) ||
      nabts_synchronizing(bits)) {
    return false;
  }
  // The trellis of each detection is kept, for the decision margins of the
  // reading kept: the last detection decided it, against the last channel
  // fitted.
  TrellisRecord& record = per_thread<WideTrellisStorage>().record;
  std::vector<uint8_t> wide = bits;
  std::vector<uint8_t> trial(bits.size(), 0);
  bool detected = false;
  const int last = static_cast<int>(grid.size()) / phases - 1;
  for (int pass = 0; pass < kMaxWideRefits; ++pass) {
    // ETSI EN 300 706 §7.1: nothing follows the packet, so the line is black
    // past it, out to the window's reach beyond the grid.
    const auto bit_at = [&wide, payload_bits, &framing](int k) {
      if (k < kPreambleBits) {
        return k >= 0 ? preamble_bit(k, framing) : 0;
      }
      const int index = k - kPreambleBits;
      return index < payload_bits
                 ? static_cast<int>(wide[static_cast<size_t>(index)])
                 : 0;
    };
    const WideChannelFit fit = fit_window_with_tail<kWideTaps, kWideTailTaps>(
        grid, phases, bit_at, kChannelCentre, last);
    if (!(fit.ok && fit.gain > 0.0)) {
      break;
    }
    if (pass == 0 &&
        std::abs(wide_ahead_fraction(fit)) < kWideMinAheadFraction) {
      return false;
    }
    mlse_detect_wide(grid, kPreambleBits, fit, framing, payload_bits,
                     trial.data(), &record, avx2);
    detected = true;
    const bool changed = trial != wide;
    wide.swap(trial);
    if (!changed) {
      break;
    }
  }
  if (!detected || wide == bits) {
    return false;
  }

  const PrefixStanding before = nabts_prefix_standing(bits);
  const PrefixStanding after = nabts_prefix_standing(wide);
  const bool prefix_kept =
      std::equal(bits.begin(), bits.begin() + kNabtsPrefixBits, wide.begin());
  const bool keep =
      data_check_failures(wide) < data_check_failures(bits) &&
      !after.worse_than(before) &&
      (prefix_kept || (after.uncorrectable == 0 && after.corrected == 0)) &&
      nabts_packet_structure_possible(
          packet_byte(wide, kNabtsPacketStructureByte)) &&
      packet_suffix_code(wide) == packet_suffix_code(bits) &&
      !nabts_synchronizing(wide);
  if (!keep) {
    return false;
  }
  trellis_confidence<kWideStates>(record, bit_confidence.data(), avx2);
  bits.swap(wide);
  return true;
}

}  // namespace

double teletext_interpolate_sample(const int16_t* line, size_t sample_count,
                                   double t) {
  if (line == nullptr || sample_count == 0) {
    return 0.0;
  }
  const double floor_t = std::floor(t);
  const auto base = static_cast<int64_t>(floor_t);
  const double x = t - floor_t;
  const auto last = static_cast<int64_t>(sample_count) - 1;

  double p0 = 0.0;
  double p1 = 0.0;
  double p2 = 0.0;
  double p3 = 0.0;
  if (base >= 1 && base + 2 <= last) {
    // Whole four-point window inside the line, which needs no clamp. The
    // MLSE resampler reads these positions from the line's
    // cubic_coefficients and comes here only near the ends of the line.
    const auto* window = line + (base - 1);
    p0 = static_cast<double>(window[0]);
    p1 = static_cast<double>(window[1]);
    p2 = static_cast<double>(window[2]);
    p3 = static_cast<double>(window[3]);
  } else {
    const auto at = [line, last](int64_t index) {
      const int64_t clamped = std::min(std::max(index, int64_t{0}), last);
      return static_cast<double>(line[static_cast<size_t>(clamped)]);
    };
    p0 = at(base - 1);
    p1 = at(base);
    p2 = at(base + 1);
    p3 = at(base + 2);
  }

  const auto coefficients = catmull_rom_coefficients(p0, p1, p2, p3);
  return catmull_rom_at(coefficients.data(), x);
}

std::string_view teletext_reject_reason_name(TeletextRejectReason reason,
                                             TeletextSystem system) {
  switch (reason) {
    case TeletextRejectReason::kNone:
      return "none";
    case TeletextRejectReason::kInsufficientSamples:
      return "insufficient samples";
    case TeletextRejectReason::kAmplitudeGate:
      return "amplitude gate";
    case TeletextRejectReason::kNoRunInLock:
      return "no run-in lock";
    case TeletextRejectReason::kRunInAmplitude:
      return "run-in amplitude";
    case TeletextRejectReason::kRunInPattern:
      return "run-in pattern";
    case TeletextRejectReason::kFramingCodeMiss:
      return "framing code miss";
    case TeletextRejectReason::kInvalidMrag:
      // The same gate under each service's own name for the bytes it tests.
      return system == TeletextSystem::kNabts525 ? "invalid packet prefix"
                                                 : "invalid MRAG";
    case TeletextRejectReason::kNoPreambleLock:
      return "no preamble lock";
    case TeletextRejectReason::kPreambleResidual:
      return "preamble residual";
    case TeletextRejectReason::kPayloadResidual:
      return "payload residual";
    case TeletextRejectReason::kParityFraction:
      return "parity fraction";
  }
  return "unknown";
}

uint8_t teletext_hamming84_encode(uint8_t value) {
  // ETSI EN 300 706 §8.2 encoding equations. Bit numbering: spec bit 1 (first
  // transmitted) is the byte LSB, so P1..P4 occupy bits 0/2/4/6 and D1..D4
  // bits 1/3/5/7.
  const int d1 = (value >> 0) & 1;
  const int d2 = (value >> 1) & 1;
  const int d3 = (value >> 2) & 1;
  const int d4 = (value >> 3) & 1;
  const int p1 = 1 ^ d1 ^ d3 ^ d4;
  const int p2 = 1 ^ d1 ^ d2 ^ d4;
  const int p3 = 1 ^ d1 ^ d2 ^ d3;
  const int p4 = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
  return static_cast<uint8_t>((p1 << 0) | (d1 << 1) | (p2 << 2) | (d2 << 3) |
                              (p3 << 4) | (d3 << 5) | (p4 << 6) | (d4 << 7));
}

int teletext_hamming84_decode(uint8_t byte) {
  // ETSI EN 300 706 §8.2: Hamming 8/4 has minimum distance 4, so every byte
  // within Hamming distance 1 of a codeword decodes to that codeword (single
  // errors corrected, including protection-bit errors) and every byte at
  // distance 2 is uncorrectable (double error detected). A 256-entry table
  // realises exactly that decision rule.
  static const auto kTable = [] {
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (int value = 0; value < 16; ++value) {
      const uint8_t code =
          teletext_hamming84_encode(static_cast<uint8_t>(value));
      table[code] = static_cast<int8_t>(value);
      for (int bit = 0; bit < 8; ++bit) {
        table[code ^ (1u << bit)] = static_cast<int8_t>(value);
      }
    }
    return table;
  }();
  return kTable[byte];
}

bool teletext_hamming84_clean(uint8_t byte) {
  // A codeword decodes to itself, so re-encoding what came out is the whole
  // test. An uncorrectable byte re-encodes to something else and reports false,
  // which is the right answer: nothing was received cleanly there either.
  const int value = teletext_hamming84_decode(byte);
  return value >= 0 &&
         teletext_hamming84_encode(static_cast<uint8_t>(value)) == byte;
}

namespace {

// Transmission-order bit positions (1-based, as ETSI EN 300 706 §8.3 numbers
// them) of D1 to D18 within a Hamming 24/18 triplet. The six that are missing
// — 1, 2, 4, 8, 16 and 24 — are the protection bits P1 to P6.
constexpr std::array<int, 18> kHamming2418DataBits = {
    3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23};

// The five odd-parity tests A to E of §8.3, as masks over the triplet's 24
// transmission-order bits held with bit 1 in the least significant position.
//
// Test i covers every position whose own number has bit i set, which is what
// makes the five failures spell out the position of a single error directly
// (§8.3 note 2). Position 24 is the exception the standard's table draws: it
// is P6, the parity of the whole triplet, and takes part in no test but F.
constexpr std::array<uint32_t, 5> hamming2418_test_masks() {
  std::array<uint32_t, 5> masks{};
  for (size_t test = 0; test < masks.size(); ++test) {
    for (uint32_t position = 1; position <= 23; ++position) {
      if ((position & (1u << test)) != 0u) {
        masks[test] |= 1u << (position - 1u);
      }
    }
  }
  return masks;
}

constexpr std::array<uint32_t, 5> kHamming2418TestMasks =
    hamming2418_test_masks();

bool odd_parity_of(uint32_t bits) {
  // True when the number of set bits is odd, which is what each of the
  // standard's tests requires of the bits it covers.
  int ones = 0;
  for (int bit = 0; bit < 24; ++bit) {
    ones += static_cast<int>((bits >> bit) & 1u);
  }
  return (ones % 2) == 1;
}

}  // namespace

void teletext_hamming2418_encode(uint32_t value, uint8_t out_bytes[3]) {
  uint32_t bits = 0;
  for (size_t index = 0; index < kHamming2418DataBits.size(); ++index) {
    if (((value >> index) & 1u) != 0u) {
      bits |= 1u << (kHamming2418DataBits[index] - 1);
    }
  }

  // P1 to P5 make their own test odd; P6 makes the whole triplet odd. Each
  // protection bit sits inside its own test and outside the others', so they
  // can be solved in this order without iterating.
  for (size_t test = 0; test < kHamming2418TestMasks.size(); ++test) {
    if (odd_parity_of(bits & kHamming2418TestMasks[test])) {
      continue;  // Already odd over this test's bits.
    }
    // The protection bit of test i is the one at position 2^i.
    bits |= 1u << ((1u << test) - 1u);
  }
  if (!odd_parity_of(bits)) {
    bits |= 1u << 23;  // P6, transmission-order bit 24
  }

  out_bytes[0] = static_cast<uint8_t>(bits & 0xFFu);
  out_bytes[1] = static_cast<uint8_t>((bits >> 8) & 0xFFu);
  out_bytes[2] = static_cast<uint8_t>((bits >> 16) & 0xFFu);
}

int32_t teletext_hamming2418_decode(uint8_t byte_n, uint8_t byte_n1,
                                    uint8_t byte_n2) {
  uint32_t bits = static_cast<uint32_t>(byte_n) |
                  (static_cast<uint32_t>(byte_n1) << 8) |
                  (static_cast<uint32_t>(byte_n2) << 16);

  // ETSI EN 300 706 §8.3: tests A to E locate a single error, test F over the
  // whole triplet separates one error from two. Every test is an *odd* parity
  // test, so a failure is an even count.
  uint32_t error_position = 0;
  for (size_t test = 0; test < kHamming2418TestMasks.size(); ++test) {
    if (!odd_parity_of(bits & kHamming2418TestMasks[test])) {
      error_position |= 1u << test;
    }
  }
  const bool test_f_ok = odd_parity_of(bits);

  if (error_position != 0) {
    if (test_f_ok) {
      return -1;  // Two errors: detected, not correctable.
    }
    // One error, at the transmission-order position the tests spell out. A
    // position beyond the triplet cannot arise from a single error, so it is
    // rejected rather than used to flip a bit that is not there.
    if (error_position > 24) {
      return -1;
    }
    bits ^= 1u << (error_position - 1u);
  }
  // error_position == 0 with test F failing is an error in P6 alone, which
  // leaves the data bits good — so there is nothing to do in either case.

  int32_t value = 0;
  for (size_t index = 0; index < kHamming2418DataBits.size(); ++index) {
    if (((bits >> (kHamming2418DataBits[index] - 1)) & 1u) != 0u) {
      value |= 1 << index;
    }
  }
  return value;
}

std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes, size_t byte_count) {
  const size_t count = std::min(byte_count, kTeletextPacketBytes);
  std::string hex;
  hex.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) {
    hex.push_back(kHexDigits[bytes[i] >> 4]);
    hex.push_back(kHexDigits[bytes[i] & 0x0F]);
  }
  return hex;
}

std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes,
    const TeletextPacketConfidence& confidence, size_t byte_count) {
  const size_t count = std::min(byte_count, kTeletextPacketBytes);
  std::string hex = teletext_packet_to_hex(bytes, count);
  hex.reserve(count * 3);
  for (size_t i = 0; i < count; ++i) {
    const double scaled = static_cast<double>(confidence[i]) *
                          static_cast<double>(kTeletextConfidenceLevels - 1);
    const int level = static_cast<int>(std::lround(std::clamp(
        scaled, 0.0, static_cast<double>(kTeletextConfidenceLevels - 1))));
    hex.push_back(kHexDigits[level]);
  }
  return hex;
}

std::optional<std::array<uint8_t, kTeletextPacketBytes>> teletext_hex_to_packet(
    std::string_view hex) {
  const auto observed = teletext_hex_to_observed_packet(hex);
  // 625-line packets only: the array carries no length, so a shorter packet
  // would reach the caller as eight bytes it never received (see the header).
  if (!observed.has_value() || observed->byte_count != kTeletextPacketBytes) {
    return std::nullopt;
  }
  return observed->bytes;
}

std::optional<TeletextObservedPacket> teletext_hex_to_observed_packet(
    std::string_view hex) {
  // The string's length names the packet length and says whether the
  // confidence suffix is there. Both are read from the same table of the
  // lengths a WST service actually transmits, so a string of any other length
  // is rejected rather than truncated to one that fits.
  size_t byte_count = 0;
  bool with_confidence = false;
  for (const size_t candidate : kPacketByteLengths) {
    if (hex.size() == candidate * 2) {
      byte_count = candidate;
      break;
    }
    if (hex.size() == candidate * 3) {
      byte_count = candidate;
      with_confidence = true;
      break;
    }
  }
  if (byte_count == 0) {
    return std::nullopt;
  }

  TeletextObservedPacket packet;
  packet.byte_count = byte_count;
  packet.confidence.fill(1.0F);
  for (size_t i = 0; i < byte_count; ++i) {
    const int high = hex_nibble(hex[i * 2]);
    const int low = hex_nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    packet.bytes[i] = static_cast<uint8_t>((high << 4) | low);
  }
  if (with_confidence) {
    for (size_t i = 0; i < byte_count; ++i) {
      const int level = hex_nibble(hex[byte_count * 2 + i]);
      if (level < 0) {
        return std::nullopt;
      }
      packet.confidence[i] = static_cast<float>(
          static_cast<double>(level) /
          static_cast<double>(kTeletextConfidenceLevels - 1));
    }
    packet.has_confidence = true;
  }
  return packet;
}

TeletextSlicer::TeletextSlicer(double sample_rate, double bit_rate,
                               TeletextSlicerOptions options)
    : sample_rate_(sample_rate),
      samples_per_bit_(sample_rate / (bit_rate * options.bit_clock)),
      options_(options),
      packet_bytes_(system_geometry(options.system).packet_bytes),
      payload_bits_(static_cast<int>(packet_bytes_ * 8)),
      data_one_fraction_(system_geometry(options.system).data_one_fraction),
      search_start_samples_(system_geometry(options.system).search_start_us *
                            sample_rate / 1e6),
      search_end_samples_(system_geometry(options.system).search_end_us *
                          sample_rate / 1e6) {}

TeletextSlicer::TeletextSlicer(double sample_rate, TeletextSystem system,
                               TeletextSlicerOptions options)
    : TeletextSlicer(sample_rate, teletext_bit_rate(system), [system, options] {
        TeletextSlicerOptions resolved = options;
        resolved.system = system;
        return resolved;
      }()) {}

TeletextLineResult TeletextSlicer::new_result() const {
  TeletextLineResult result;
  result.packet_bytes = packet_bytes_;
  result.system = options_.system;
  return result;
}

TeletextSlicer::AcquisitionWindow TeletextSlicer::full_window() const {
  return AcquisitionWindow{search_start_samples_, search_end_samples_};
}

std::optional<TeletextSlicer::AcquisitionWindow> TeletextSlicer::hinted_window(
    const TeletextPhaseHint& hint) const {
  if (!hint.valid || hint.radius <= 0.0) {
    return std::nullopt;
  }
  const AcquisitionWindow full = full_window();
  const AcquisitionWindow narrowed{
      std::max(full.first, hint.centre - hint.radius),
      std::min(full.last, hint.centre + hint.radius)};
  if (narrowed.first > narrowed.last) {
    // The hint sits outside the window the system's timing allows, so there is
    // nothing to try: sweep the full one and let the tracker learn better.
    return std::nullopt;
  }
  // A hint that spans the whole window would cost a wasted first attempt and
  // save nothing; anything narrower earns its retry.
  if (narrowed.last - narrowed.first >= full.last - full.first) {
    return std::nullopt;
  }
  return narrowed;
}

TeletextLineResult TeletextSlicer::slice(
    const int16_t* line, size_t sample_count, int16_t black_level,
    int16_t white_level, const TeletextPhaseHint& phase_hint) const {
  TeletextLineResult result = new_result();

  const double spb = samples_per_bit_;
  // Whole packet (§7.1 / ITU-R BT.653 Table 1a-1b row 1.7) must fit after the
  // earliest search start.
  const double search_start = search_start_samples_;
  const double min_samples =
      search_start + (kRunInBits + kFramingBits + payload_bits_) * spb + 2.0;
  if (line == nullptr || static_cast<double>(sample_count) < min_samples) {
    return reject(result, TeletextRejectReason::kInsufficientSamples);
  }

  // Nominal '1' amplitude above black level (ETSI EN 300 706 §5.2 on 625
  // lines, ITU-R BT.653 Table 1b on 525).
  const double nominal_amplitude =
      data_one_fraction_ *
      (static_cast<double>(white_level) - static_cast<double>(black_level));
  if (nominal_amplitude <= 0.0) {
    return reject(result, TeletextRejectReason::kInsufficientSamples);
  }
  const double amplitude_gate = kAmplitudeGateFraction * nominal_amplitude;

  // Step 1 — coarse gate: most VBI lines are empty; reject immediately when
  // the line never rises meaningfully above black level.
  int16_t peak = black_level;
  const auto gate_begin = static_cast<size_t>(search_start);
  for (size_t i = gate_begin; i < sample_count; ++i) {
    peak = std::max(peak, line[i]);
  }
  if (static_cast<double>(peak) - static_cast<double>(black_level) <
      amplitude_gate) {
    return reject(result, TeletextRejectReason::kAmplitudeGate);
  }

  // Step 2 — detection over one acquisition window. The threshold detector
  // runs unless MLSE was asked for outright; under kAuto the MLSE fallback
  // sees only lines that carry a data burst (they passed the coarse gate) but
  // would not lock, or locked with an eye too closed to trust, so a source the
  // threshold detector handles pays nothing for the fallback.
  const auto detect = [&](AcquisitionWindow window) {
    if (options_.detector != TeletextDetector::kMlse) {
      TeletextLineResult attempt =
          slice_threshold(line, sample_count, amplitude_gate, window);
      if (attempt.valid && options_.detector == TeletextDetector::kAuto) {
        const float weakest =
            *std::min_element(attempt.byte_confidence.begin(),
                              attempt.byte_confidence.begin() +
                                  static_cast<ptrdiff_t>(packet_bytes_));
        if (weakest < kAutoFallbackEyeMargin) {
          TeletextLineResult retry =
              slice_mlse(line, sample_count, nominal_amplitude, window);
          if (retry.valid) {
            return retry;
          }
        }
      }
      if (attempt.valid || options_.detector == TeletextDetector::kThreshold) {
        return attempt;
      }
    }
    return slice_mlse(line, sample_count, nominal_amplitude, window);
  };

  // Step 3 — which window, or windows, that runs over. A valid hint is tried
  // first and the full window only if the hinted attempt recovered nothing, so
  // pinning changes what a line costs but never whether it can be read.
  if (const std::optional<AcquisitionWindow> hinted = hinted_window(phase_hint);
      hinted.has_value()) {
    result = detect(*hinted);
    if (result.valid) {
      return result;
    }
  }
  return detect(full_window());
}

TeletextLineResult TeletextSlicer::slice_threshold(
    const int16_t* line, size_t sample_count, double amplitude_gate,
    AcquisitionWindow window) const {
  TeletextLineResult result = new_result();
  result.detector = TeletextDetector::kThreshold;
  const double spb = samples_per_bit_;
  const double search_start = window.first;

  // Clock run-in acquisition (§6.1): correlate against a ±
  // alternating kernel at the known bit period across the §6.3 timing window.
  // The correlation peak yields the bit phase; the recovered 0/1 levels set
  // an adaptive slicing threshold local to the data burst.
  const double search_end = window.last;
  double best_corr = 0.0;
  double best_t0 = -1.0;
  for (double t0 = search_start; t0 <= search_end; t0 += kPhaseSearchStep) {
    double corr = 0.0;
    for (int k = 0; k < kRunInBits; ++k) {
      const double s = sample_at(line, t0 + k * spb);
      corr += (k % 2 == 0) ? s : -s;  // §6.1: even bit indices are ones
    }
    if (corr > best_corr) {
      best_corr = corr;
      best_t0 = t0;
    }
  }
  if (best_t0 < 0.0) {
    return reject(result, TeletextRejectReason::kNoRunInLock);
  }
  result.lock_sample = best_t0;

  double ones_level = 0.0;
  double zeros_level = 0.0;
  for (int k = 0; k < kRunInBits; ++k) {
    const double s = sample_at(line, best_t0 + k * spb);
    ((k % 2 == 0) ? ones_level : zeros_level) += s;
  }
  ones_level /= kRunInBits / 2.0;
  zeros_level /= kRunInBits / 2.0;
  const double amplitude = ones_level - zeros_level;
  if (amplitude < amplitude_gate) {
    return reject(result, TeletextRejectReason::kRunInAmplitude);
  }
  const double threshold = 0.5 * (ones_level + zeros_level);

  // Validate the run-in bit pattern at the locked phase (§6.1, allowing for
  // the note's reduced leading ones).
  int run_in_matches = 0;
  for (int k = 0; k < kRunInBits; ++k) {
    const int bit = sample_at(line, best_t0 + k * spb) > threshold ? 1 : 0;
    run_in_matches += (bit == (k % 2 == 0 ? 1 : 0)) ? 1 : 0;
  }
  if (run_in_matches < kMinRunInMatches) {
    return reject(result, TeletextRejectReason::kRunInPattern);
  }

  // Framing-code lock (ETSI EN 300 706 §6.2, CEA-516 §2.2.3): resolve the
  // run-in's even-bit-shift ambiguity by searching for the service's framing
  // code around the correlation lock. An alignment is a candidate only where
  // the code matches with no bit wrong and the Hamming-coded addressing prefix
  // that follows decodes (the MRAG of §7.1.2 on System B, the packet prefix of
  // CEA-516 §3.2 on System C), and the candidate nearest the lock is kept. The
  // prefix gate matters because widening the search also widens the chance of
  // the payload happening to spell the framing code; an alignment whose address
  // bytes decode is the real one.
  const SystemGeometry geometry = system_geometry(options_.system);
  const auto bit_at = [&](double t) {
    return sample_at(line, t) > threshold ? 1 : 0;
  };
  const auto payload_start = [&](int shift) {
    return best_t0 + (kRunInBits + shift + kFramingBits) * spb;
  };

  int best_shift = 0;
  bool found = false;
  // Whether any alignment matched the framing code, which separates "no
  // framing code here" from "framing code found but its prefix was rejected".
  bool framing_matched = false;
  for (int shift = -kFramingSearchBits; shift <= kFramingSearchBits; ++shift) {
    bool framing_ok = true;
    for (int k = 0; k < kFramingBits && framing_ok; ++k) {
      framing_ok = bit_at(best_t0 + (kRunInBits + shift + k) * spb) ==
                   geometry.framing[static_cast<size_t>(k)];
    }
    if (!framing_ok) {
      continue;
    }
    framing_matched = true;
    // The whole packet must fit at this alignment (§7.1, CEA-516 §2.1).
    const double start = payload_start(shift);
    if (start < 0.0 || start + (payload_bits_ - 1) * spb + 1.0 >=
                           static_cast<double>(sample_count)) {
      continue;
    }

    const int prefix_bits = static_cast<int>(geometry.hamming_prefix_bytes) * 8;
    std::array<uint8_t, kTeletextPacketBytes> prefix{};
    for (int n = 0; n < prefix_bits; ++n) {
      if (bit_at(start + n * spb) != 0) {
        prefix[static_cast<size_t>(n) >> 3] |=
            static_cast<uint8_t>(1u << (n & 7));
      }
    }
    bool prefix_ok = true;
    for (size_t i = 0; i < geometry.hamming_prefix_bytes; ++i) {
      if (teletext_hamming84_decode(prefix[i]) < 0) {
        prefix_ok = false;
        break;
      }
    }
    if (prefix_ok && !geometry.parity_coded_rows &&
        !nabts_packet_structure_possible(prefix[kNabtsPacketStructureByte])) {
      prefix_ok = false;
    }
    if (!prefix_ok) {
      continue;
    }

    if (!found || std::abs(shift) < std::abs(best_shift)) {
      found = true;
      best_shift = shift;
    }
  }
  if (!found) {
    return reject(result, framing_matched
                              ? TeletextRejectReason::kInvalidMrag
                              : TeletextRejectReason::kFramingCodeMiss);
  }

  // Payload extraction (§7.1): the system's payload bits at bit-centre
  // positions, LSB first per byte. No Hamming/parity correction is applied:
  // the T42 contract preserves transmission coding.
  const double data_start = payload_start(best_shift);
  const double margin_scale = 2.0 / amplitude;
  result.byte_confidence.fill(1.0F);
  for (int n = 0; n < payload_bits_; ++n) {
    const double s = sample_at(line, data_start + n * spb);
    const size_t byte = static_cast<size_t>(n) >> 3;
    if (s > threshold) {
      result.bytes[byte] |= static_cast<uint8_t>(1u << (n & 7));
    }
    const auto margin = static_cast<float>(
        std::min(1.0, std::fabs(s - threshold) * margin_scale));
    result.byte_confidence[byte] =
        std::min(result.byte_confidence[byte], margin);
  }
  result.has_byte_confidence = true;

  // The addressing-prefix plausibility filter (ETSI EN 300 706 §7.1.2 and
  // §8.2, CEA-516 §3.2.2) was applied per candidate alignment during the
  // framing lock; the stored bytes stay as transmitted.

  result.data_start_sample = data_start;
  result.detector = TeletextDetector::kThreshold;
  result.valid = true;
  return result;
}

struct TeletextSlicer::MlseReading {
  // Whether the rest describes a reading: detect_mlse_at() sets it for a
  // System C reading that passed every gate.
  bool valid = false;
  // The grid the reading was detected from, the channel it was detected
  // against, and the payload bits it decided (see detect_mlse_at()).
  std::vector<double> grid;
  ChannelFit fit;
  std::vector<uint8_t> bits;
  // Known-black bits the grid holds past the packet (see kTrailingBits).
  int trailing = 0;
};

TeletextLineResult TeletextSlicer::slice_mlse(const int16_t* line,
                                              size_t sample_count,
                                              double nominal_amplitude,
                                              AcquisitionWindow window) const {
  TeletextLineResult result = new_result();
  result.detector = TeletextDetector::kMlse;
  const SystemGeometry geometry = system_geometry(options_.system);
  const double spb = samples_per_bit_;
  const double search_start = window.first;
  const double search_end = window.last;
  const double min_gain = kMlseMinGainFraction * nominal_amplitude;
  const int phases =
      std::clamp(options_.mlse_samples_per_bit, 1, kMaxMlseSamplesPerBit);

  // Bit-phase acquisition: fit the channel at every candidate phase and keep
  // the phase whose fit explains the known preamble best. This replaces the
  // threshold detector's run-in correlation, which needs the 3,47 MHz run-in
  // fundamental the band-limited channel has already removed — the framing
  // code and the shape of the smeared run-in still carry the timing.
  //
  // Acquisition reads one sample per bit even when detection will read three.
  // The fractionally-spaced fit gives each phase its own free tap vector, so a
  // sub-bit timing error is absorbed into the taps instead of showing in the
  // residual: its residual barely varies with t0, which is precisely what an
  // acquisition criterion must do. The bit-spaced fit does sharpen with phase,
  // and costs a fraction as much over the ~350 candidates of the §6.3 window.
  //
  // Both the preamble window and the payload that follows must fit the line,
  // which bounds the latest phase the sweep may try.
  const double latest_t0 = static_cast<double>(sample_count) - 1.0 -
                           (kPreambleBits + payload_bits_ - 1) * spb;
  const bool avx2 = options_.use_avx2 && avx2_available();
  std::vector<double> cubic;
  cubic_coefficients(line, sample_count, cubic, avx2);
  std::vector<double> grid;
  std::vector<double> phases_tried;
  for (double t0 = search_start; t0 <= search_end && t0 < latest_t0;
       t0 += kMlsePhaseStep) {
    phases_tried.push_back(t0);
  }
  std::vector<PreambleScore> scores;
  score_preambles(line, sample_count, cubic, phases_tried, spb,
                  geometry.framing, avx2, grid, scores);
  double best_t0 = -1.0;
  PreambleScore acquire_fit;
  for (size_t i = 0; i < phases_tried.size(); ++i) {
    const PreambleScore& fit = scores[i];
    if (!fit.ok || fit.gain < min_gain) {
      continue;
    }
    if (best_t0 < 0.0 || fit.residual < acquire_fit.residual) {
      best_t0 = phases_tried[i];
      acquire_fit = fit;
    }
  }
  if (best_t0 < 0.0) {
    return reject(result, TeletextRejectReason::kNoPreambleLock);
  }

  // The clock run-in repeats every two bits (ETSI EN 300 706 §6.1, CEA-516
  // §2.2.2) and the five free taps absorb a shift of up to two bits, so the fit
  // above cannot settle which whole or half bit the packet starts on — and
  // need not: read from any of those shifts the bits come out in order, and
  // the prefix settles byte synchronization (§6.2, §2.2.3). The shift that
  // centres a bit's response on the centre tap (the lock reported, where the
  // §6.3 timing reference places the run-in) and the best fit are read first,
  // then the next best fits. The surest reading whose Hamming-protected prefix
  // arrived as sent is kept; failing one, the reading of those first two
  // shifts with the fewest corrections, then the surest decisions.
  struct Candidate {
    double t0;
    double residual;
    double centre;
  };
  std::array<Candidate, kAlignmentCandidates> candidates{};
  int candidate_count = 0;
  candidates[static_cast<size_t>(candidate_count++)] = {
      best_t0, acquire_fit.residual / acquire_fit.gain, acquire_fit.centre};
  // The shifted grid lands between sweep phases, so each shift is refined
  // over the sweep step either side of it: the phases of every shift are
  // fitted together, then each shift keeps its best.
  phases_tried.clear();
  std::vector<int> shift_of;
  for (int shift = -2 * kAlignmentBits; shift <= 2 * kAlignmentBits; ++shift) {
    if (shift == 0) {
      continue;
    }
    const double centre = best_t0 + shift * 0.5 * spb;
    for (int step = -2; step <= 2; ++step) {
      const double t0 = centre + step * kMlsePhaseStep;
      if (t0 < search_start || t0 > search_end || t0 >= latest_t0) {
        continue;
      }
      phases_tried.push_back(t0);
      shift_of.push_back(shift);
    }
  }
  score_preambles(line, sample_count, cubic, phases_tried, spb,
                  geometry.framing, avx2, grid, scores);
  for (size_t i = 0; i < phases_tried.size();) {
    const int shift = shift_of[i];
    Candidate found{-1.0, std::numeric_limits<double>::infinity(), 0.0};
    for (; i < phases_tried.size() && shift_of[i] == shift; ++i) {
      const PreambleScore& fit = scores[i];
      if (!fit.ok || fit.gain < min_gain) {
        continue;
      }
      const double residual = fit.residual / fit.gain;
      if (residual < found.residual) {
        found = {phases_tried[i], residual, fit.centre};
      }
    }
    if (found.t0 >= 0.0) {
      candidates[static_cast<size_t>(candidate_count++)] = found;
    }
  }
  std::sort(candidates.begin(),
            candidates.begin() + static_cast<ptrdiff_t>(candidate_count),
            [](const Candidate& a, const Candidate& b) {
              return a.residual < b.residual;
            });
  // The two shifts with a claim to the front: the centred one, then the best
  // fit (already first by residual unless it is the centred one).
  {
    int centred = 0;
    for (int i = 1; i < candidate_count; ++i) {
      if (candidates[static_cast<size_t>(i)].centre >
          candidates[static_cast<size_t>(centred)].centre) {
        centred = i;
      }
    }
    std::rotate(candidates.begin(),
                candidates.begin() + static_cast<ptrdiff_t>(centred),
                candidates.begin() + static_cast<ptrdiff_t>(centred) + 1);
  }
  const double lock = candidates[0].t0;

  const size_t prefix_bytes = teletext_hamming_prefix_bytes(options_.system);
  TeletextLineResult chosen;
  MlseReading chosen_reading;
  MlseReading reading;
  bool chosen_as_sent = false;
  double chosen_confidence = -1.0;
  size_t chosen_corrected = prefix_bytes + 1;
  const int readings = std::min(candidate_count, kAlignmentReadings);
  for (int i = 0; i < readings; ++i) {
    reading.valid = false;
    TeletextLineResult attempt = detect_mlse_at(
        line, sample_count, cubic, candidates[static_cast<size_t>(i)].t0,
        phases, grid, reading);
    if (!attempt.valid) {
      if (i == 0) {
        chosen = std::move(attempt);
      }
      continue;
    }
    size_t corrected = 0;
    for (size_t b = 0; b < prefix_bytes; ++b) {
      corrected += teletext_hamming84_clean(attempt.bytes[b]) ? 0 : 1;
    }
    double confidence = 0.0;
    if (attempt.has_byte_confidence) {
      for (size_t b = 0; b < packet_bytes_; ++b) {
        confidence += static_cast<double>(attempt.byte_confidence[b]);
      }
    }
    if (corrected == 0) {
      if (!chosen_as_sent || confidence > chosen_confidence) {
        chosen = std::move(attempt);
        std::swap(chosen_reading, reading);
        chosen_as_sent = true;
        chosen_confidence = confidence;
      }
    } else if (!chosen_as_sent && i < kClaimedShifts &&
               (corrected < chosen_corrected ||
                (corrected == chosen_corrected &&
                 confidence > chosen_confidence))) {
      chosen = std::move(attempt);
      std::swap(chosen_reading, reading);
      chosen_corrected = corrected;
      chosen_confidence = confidence;
    }
  }

  // The second look under the packet's own checks (see the class comment and
  // read_under_packet_checks()), taken once, at the reading chosen: the prefix
  // it is held to is what chose it, so the choice does not depend on it. A
  // byte it changed is reported with no confidence: the samples alone did not
  // decide it, and a copy of the packet that arrived whole outweighs it.
  if (chosen.valid && chosen_reading.valid) {
    const std::vector<uint8_t> decided = chosen_reading.bits;
    const int changed = read_under_packet_checks(
        chosen_reading.grid, chosen_reading.fit, geometry.framing,
        payload_bits_, chosen_reading.trailing, chosen_reading.bits);
    if (changed > 0) {
      chosen.checked_bits = changed;
      for (size_t n = 0; n < decided.size(); ++n) {
        if (chosen_reading.bits[n] != decided[n]) {
          chosen.bytes[n >> 3] =
              static_cast<uint8_t>(chosen.bytes[n >> 3] ^ (1u << (n & 7)));
          chosen.byte_confidence[n >> 3] = 0.0F;
        }
      }
    }

    // The wide reading (see the class comment and read_wide()), once, of the
    // reading chosen and checked, where its data checks still fail. It reads
    // the grid at the lock, where a bit's response is centred in the window;
    // the bits are the packet's at any of the shifts read. The bytes it
    // changed are reported (TeletextLineResult::wide_bytes), with the margins
    // of its own detection as every byte's confidence.
    if (data_check_failures(chosen_reading.bits) > 0) {
      const int trailing =
          (lock + (kPreambleBits + payload_bits_ + kTrailingBits - 1) * spb +
               2.0 <
           static_cast<double>(sample_count))
              ? kTrailingBits
              : 0;
      resample_bit_grid(line, sample_count, cubic, lock, spb,
                        kPreambleBits + payload_bits_ + trailing, phases, grid);
      std::array<float, kTeletextPayloadBits> bit_confidence{};
      if (read_wide(grid, phases, geometry.framing, payload_bits_,
                    chosen_reading.bits, bit_confidence, avx2)) {
        const std::array<uint8_t, kTeletextPacketBytes> before = chosen.bytes;
        chosen.bytes.fill(0);
        for (int n = 0; n < payload_bits_; ++n) {
          if (chosen_reading.bits[static_cast<size_t>(n)] != 0) {
            chosen.bytes[static_cast<size_t>(n) >> 3] |=
                static_cast<uint8_t>(1u << (n & 7));
          }
        }
        for (size_t i = 0; i < packet_bytes_; ++i) {
          float lowest = 1.0F;
          for (size_t bit = 0; bit < 8; ++bit) {
            lowest = std::min(lowest, bit_confidence[i * 8 + bit]);
          }
          chosen.byte_confidence[i] = lowest;
          if (chosen.bytes[i] != before[i]) {
            chosen.wide_bytes |= uint64_t{1} << i;
          }
        }
        chosen.has_byte_confidence = true;
      }
    }
  }
  if (chosen.valid) {
    chosen.lock_sample = lock;
    chosen.data_start_sample = lock + kPreambleBits * spb;
  }
  return chosen;
}

TeletextLineResult TeletextSlicer::detect_mlse_at(
    const int16_t* line, size_t sample_count, const std::vector<double>& cubic,
    double t0, int phases, std::vector<double>& grid,
    MlseReading& reading) const {
  TeletextLineResult result = new_result();
  result.detector = TeletextDetector::kMlse;
  result.lock_sample = t0;
  const SystemGeometry geometry = system_geometry(options_.system);
  const double spb = samples_per_bit_;
  const bool avx2 = options_.use_avx2 && avx2_available();

  // Resample the whole packet span once at the locked phase, and fit the
  // fractionally-spaced channel the detector runs against. Everything from
  // here on reads the grid rather than the line.
  //
  // The span runs kTrailingBits past the packet when the line is long enough to
  // hold them: those samples carry the known black level the line returns to
  // after the last packet bit, which is what lets the trellis be terminated
  // rather than left to end wherever it likes.
  const int trailing =
      (t0 + (kPreambleBits + payload_bits_ + kTrailingBits - 1) * spb + 2.0 <
       static_cast<double>(sample_count))
          ? kTrailingBits
          : 0;
  resample_bit_grid(line, sample_count, cubic, t0, spb,
                    kPreambleBits + payload_bits_ + trailing, phases, grid);
  const ChannelFit best_fit =
      fit_preamble_channel(grid, phases, geometry.framing);
  // Only a positive gain is required here — acquisition has already applied
  // the min-gain test, and a fractionally-spaced fit that then claims a weak
  // channel says so through its relative residual below, which is the same
  // judgement measured against the amplitude the fit itself asserts.
  if (!best_fit.ok || best_fit.gain <= 0.0) {
    return reject(result, TeletextRejectReason::kNoPreambleLock);
  }

  // A preamble the model cannot explain is not a preamble. Without this the
  // fit would happily describe whatever picture content sat under the search
  // window, and the MRAG filter alone lets through too much of it.
  const double relative_residual = best_fit.residual / best_fit.gain;
  // Recorded before the gate so a rejected line still reports how close it
  // came — the residual distribution is what the gate is tuned against.
  result.preamble_residual = relative_residual;
  if (relative_residual > kMlseMaxResidualFraction) {
    return reject(result, TeletextRejectReason::kPreambleResidual);
  }

  // The framing code was assumed in order to fit the channel; now check that
  // no other service's code explains the samples better. This is what stops a
  // line of the other 525-line service being decoded as this one (see
  // framing_code_fits_best).
  bool other_framing_fits_better = false;
  if (!framing_code_fits_best(grid, phases, best_fit, geometry.framing,
                              other_framing_fits_better)) {
    return reject(result, TeletextRejectReason::kFramingCodeMiss);
  }
  result.other_framing_fits_better = other_framing_fits_better;

  const double data_start = t0 + kPreambleBits * spb;
  std::vector<uint8_t> bits(static_cast<size_t>(payload_bits_), 0);

  // Pass 1 — detect against the channel fitted to the preamble. That channel
  // knows nothing of the payload, which is what makes this pass, and only this
  // pass, a usable judge of whether the line carries a teletext packet at all:
  // its residual is the gate below, and its per-bit errors are the timing
  // diagnostic.
  const double payload_residual =
      mlse_detect(grid, kPreambleBits, best_fit, geometry.framing,
                  payload_bits_, bits.data(), result.payload_bit_errors.data(),
                  /*record=*/nullptr, trailing, avx2) /
      best_fit.gain;
  result.payload_residual = payload_residual;
  // Per-bit errors carry the same normalisation as the residual above: a
  // fraction of the fitted gain, so profiles from lines of different amplitude
  // are on one scale.
  for (int n = 0; n < payload_bits_; ++n) {
    result.payload_bit_errors[static_cast<size_t>(n)] = static_cast<float>(
        static_cast<double>(result.payload_bit_errors[static_cast<size_t>(n)]) /
        best_fit.gain);
  }

  // The preamble gate above rules out lines whose first 24 bits are not a
  // preamble; this one rules out lines where they happened to be but the rest
  // is not a teletext packet, using every payload bit rather than 20.
  //
  // It is applied between the two passes deliberately. The refit below fits the
  // channel to the bits it is then judged against, so its residual is
  // optimistic by construction and would blunt this gate — measured on the
  // reference VHS captures, gating on the refit residual let through all but
  // one of the 7393 lines this discards. Rejecting here also spares a discarded
  // line the second detection.
  if (payload_residual > kMlseMaxPayloadResidualFraction) {
    return reject(result, TeletextRejectReason::kPayloadResidual);
  }

  // Pass 2 — refit the channel against the whole line and detect again.
  //
  // The preamble offers 20 usable equations per phase; a 625-line packet offers
  // 336 of them. Five bit-spaced taps fitted to the shorter set describe the
  // head of the channel's pulse response well and its tail poorly, and what the
  // tail costs is bytes. Refitting against the bits pass 1 decided and
  // re-running the trellis recovers them: on synthesized band-limited lines it
  // takes exact packet recovery from 29 of 48 to 48 of 48, and on the reference
  // VHS captures it raises the packets whose data bytes are all parity-clean by
  // 71 % (LP) and 34 % (SP).
  //
  // Decision-directed refitting is stable here because pass 1 is already right
  // about the overwhelming majority of the payload bits, and a handful of wrong
  // ones move a fit of that many equations by very little.
  //
  // The confidences come from this pass rather than the first: they describe
  // the bits that are emitted, and those are these. A refit that fails is the
  // one case where none are reported — re-running the first pass purely to
  // measure it would cost every line a third trellis for a case that needs a
  // singular normal matrix to arise at all.
  std::array<float, kTeletextPayloadBits> bit_confidence{};
  const int last_fittable = kPreambleBits + payload_bits_ + trailing - 1 -
                            (kChannelTaps - 1 - kChannelCentre);
  const int payload_bits = payload_bits_;
  const auto decided_bit = [&bits, payload_bits, &geometry](int k) {
    if (k < kPreambleBits) {
      return preamble_bit(k, geometry.framing);
    }
    const int index = k - kPreambleBits;
    // ETSI EN 300 706 §7.1: nothing follows the packet, so the line is black.
    return index < payload_bits
               ? static_cast<int>(bits[static_cast<size_t>(index)])
               : 0;
  };
  // How well a reading's Hamming 8/4 protected prefix arrived (§8.2).
  const auto prefix_standing = [&geometry](const std::vector<uint8_t>& read) {
    PrefixStanding standing;
    for (size_t b = 0; b < geometry.hamming_prefix_bytes; ++b) {
      uint8_t byte = 0;
      for (size_t bit = 0; bit < 8; ++bit) {
        byte = static_cast<uint8_t>(byte | (read[b * 8 + bit] << bit));
      }
      if (teletext_hamming84_decode(byte) < 0) {
        ++standing.uncorrectable;
      } else if (!teletext_hamming84_clean(byte)) {
        ++standing.corrected;
      }
    }
    return standing;
  };
  // The channel the emitted bits were last detected against, and the trellis
  // they were decided in, whose confidence sweep runs once they are final.
  ChannelFit detection_fit = best_fit;
  auto& records = trellis_records();
  TrellisRecord* decided_in = &records[0];
  TrellisRecord* trying = &records[1];
  const ChannelFit refit =
      fit_channel_range(grid, phases, decided_bit, kChannelCentre,
                        last_fittable, /*with_residual=*/false);
  if (refit.ok && refit.gain > 0.0) {
    mlse_detect(grid, kPreambleBits, refit, geometry.framing, payload_bits_,
                bits.data(), /*bit_errors_out=*/nullptr, decided_in, trailing,
                avx2);
    result.has_byte_confidence = true;
    detection_fit = refit;

    // Passes 3 on — the refit again, now with the channel's tail
    // (kChannelTailTaps), which the preamble's alternating run-in cannot fit,
    // and repeated while the decisions it leads to keep changing
    // (kMaxChannelRefits): each refit is fitted to fewer wrong bits than the
    // one before. Measured on a capture card dump of a tape against the bytes
    // of two other transfers of it, bit errors per packet fall from 17,7 to
    // 5,2 and the packets read exactly rise from 115 to 884; on its
    // vhs-decode transfer from 5,8 to 2,7, and on a cleaner card dump from
    // 0,60 to 0,27.
    //
    // A refit fitted to its own decisions can also settle on a wrong reading of
    // a context the payload rarely shows, and the one it sees least well is the
    // head of the packet, straight after the preamble: on one EP tape refits
    // read the first prefix byte 0x15 as 0x19 on half the lines, a byte no
    // correction mends, and the lines were lost. The prefix is the one part of
    // every packet that checks itself (ETSI EN 300 706 §8.2, CEA-516 §3.2.2),
    // so a refit is kept only while its prefix arrives no worse than the
    // reading before it, the five-tap one included — no more bytes past
    // correction, then no more needing it — and otherwise that reading stands.
    PrefixStanding standing = prefix_standing(bits);
    std::vector<uint8_t> trial(bits.size(), 0);
    for (int refit_pass = 0; refit_pass < kMaxChannelRefits; ++refit_pass) {
      const ChannelFit tail_fit = fit_channel_with_tail(
          grid, phases, decided_bit, kChannelCentre, last_fittable);
      if (!(tail_fit.ok && tail_fit.gain > 0.0)) {
        break;
      }
      mlse_detect(grid, kPreambleBits, tail_fit, geometry.framing,
                  payload_bits_, trial.data(), /*bit_errors_out=*/nullptr,
                  trying, trailing, avx2);
      const PrefixStanding trial_standing = prefix_standing(trial);
      if (trial_standing.worse_than(standing)) {
        break;
      }
      standing = trial_standing;
      const bool changed = trial != bits;
      bits.swap(trial);
      std::swap(decided_in, trying);
      detection_fit = tail_fit;
      if (!changed) {
        break;
      }
    }
  }

  // The grid the timing step below read the bits from again, when it kept its
  // reading; empty while they come from |grid|.
  std::vector<double> retimed;

  // The synchronization sequence was assumed by both fits above, so a reading
  // whose prefix needed correction has it read back before it is accepted,
  // with a channel fitted to the payload alone: the framing code must be within
  // kMlseMaxFramingErrors of the service's, and the run-in within
  // kMlseMaxRunInErrors of alternating. A prefix that arrived as sent is
  // evidence enough on its own (see kMlseMaxFramingErrors). Run last, on a
  // reading every other gate has passed, and by the timing step on the reading
  // it makes, against the grid that reading was detected on.
  const auto sync_read_back = [&]() -> std::optional<TeletextRejectReason> {
    const PrefixStanding standing = prefix_standing(bits);
    if (standing.uncorrectable == 0 && standing.corrected == 0) {
      result.framing_bit_errors = 0;
      return std::nullopt;
    }
    const std::vector<double>& detected = retimed.empty() ? grid : retimed;
    const ChannelFit payload_fit =
        fit_channel_range(detected, phases, decided_bit,
                          kPreambleBits + kChannelCentre, last_fittable,
                          /*with_residual=*/false);
    SyncReadBack read_back;
    if (payload_fit.ok && payload_fit.gain > 0.0) {
      read_back = read_back_sync(detected, payload_fit, geometry.framing,
                                 bits.data(), payload_bits_);
    }
    result.framing_bit_errors = read_back.framing_errors;
    if (read_back.framing_errors > kMlseMaxFramingErrors) {
      return TeletextRejectReason::kFramingCodeMiss;
    }
    if (read_back.run_in_errors > kMlseMaxRunInErrors) {
      return TeletextRejectReason::kRunInPattern;
    }
    return std::nullopt;
  };

  // System C: a packet carrying the longitudinal check (CEA-516 §3.4) whose
  // reading breaks one of its checks is read again along its bits' own timing
  // (see kTimingSegmentBits). The grid was read at one bit phase and the
  // nominal clock, and the channel fitted to it absorbs a constant offset but
  // not a drift across the line, which leaves the bits at one end read
  // off-centre. Fitted against the bits just decided, the timing is measured
  // where those are right and they are refitted where it moved them.
  //
  // As with the refits, the new reading is kept only where the packet's checks
  // come out no worse: its prefix no worse, as the refits judge it, and no more
  // of its data checks broken. It must also pass the gates below that the
  // reading before it was put to — a structure byte a packet can carry, and the
  // read-back of the run-in and framing code — so that no packet the first
  // reading delivers is lost to the second.
  //
  // Only a packet with the check is read again, and only where its structure
  // byte, which says the check is there, arrived as sent (see
  // read_under_packet_checks()): a new reading moves bytes both ways, and on
  // recordings with parity alone to judge by, it put about as many
  // parity-passing errors into bytes that had been right as it took out (379
  // against 350 on one card dump) — errors no vote can tell from a right
  // byte — where on recordings sent with the check it leaves 2,975 taken out
  // against 119 put in.
  if (!geometry.parity_coded_rows && detection_fit.ok &&
      detection_fit.gain > 0.0) {
    const PrefixStanding untimed_prefix = prefix_standing(bits);
    const int untimed_failures = data_check_failures(bits);
    double offset = 0.0;
    double drift = 0.0;
    if (packet_suffix_code(bits) == 1 &&
        teletext_hamming84_clean(
            packet_byte(bits, kNabtsPacketStructureByte)) &&
        (untimed_prefix.uncorrectable + untimed_prefix.corrected > 0 ||
         untimed_failures > 0) &&
        measure_timing(line, sample_count, cubic, t0, spb, phases,
                       detection_fit, decided_bit,
                       kChannelCentre + kChannelTailTaps, last_fittable, offset,
                       drift)) {
      retimed.resize(grid.size());
      const int grid_bits = static_cast<int>(grid.size()) / phases;
      for (int k = 0; k < grid_bits; ++k) {
        for (int p = 0; p < phases; ++p) {
          const double position =
              static_cast<double>(k) + phase_offset(p, phases);
          double slope = 0.0;
          interpolate_with_slope(
              line, sample_count, cubic,
              t0 + position * spb + offset + drift * position,
              retimed[grid_index(k, p, phases)], slope);
        }
      }
      const std::vector<uint8_t> untimed = bits;
      ChannelFit retimed_fit = detection_fit;
      bool refitted = false;
      for (int refit_pass = 0; refit_pass < kMaxChannelRefits; ++refit_pass) {
        const ChannelFit tail_fit = fit_channel_with_tail(
            retimed, phases, decided_bit, kChannelCentre, last_fittable);
        if (!(tail_fit.ok && tail_fit.gain > 0.0)) {
          break;
        }
        const std::vector<uint8_t> before = bits;
        mlse_detect(retimed, kPreambleBits, tail_fit, geometry.framing,
                    payload_bits_, bits.data(), /*bit_errors_out=*/nullptr,
                    trying, trailing, avx2);
        retimed_fit = tail_fit;
        refitted = true;
        if (bits == before) {
          break;
        }
      }
      const bool keep = refitted &&
                        !prefix_standing(bits).worse_than(untimed_prefix) &&
                        data_check_failures(bits) <= untimed_failures &&
                        nabts_packet_structure_possible(
                            packet_byte(bits, kNabtsPacketStructureByte)) &&
                        !sync_read_back();
      if (keep) {
        detection_fit = retimed_fit;
        std::swap(decided_in, trying);
      } else {
        bits = untimed;
        retimed.clear();
      }
    }
  }

  if (result.has_byte_confidence) {
    mlse_confidence(*decided_in, bit_confidence.data(), avx2);
  }

  // §7.1: LSB first per byte, transmission coding preserved.
  for (int n = 0; n < payload_bits_; ++n) {
    if (bits[static_cast<size_t>(n)] != 0) {
      result.bytes[static_cast<size_t>(n) >> 3] |=
          static_cast<uint8_t>(1u << (n & 7));
    }
  }

  // A byte is only as sure as the least sure of its eight bits: one wrong bit
  // is a wrong byte.
  if (result.has_byte_confidence) {
    for (size_t i = 0; i < packet_bytes_; ++i) {
      float lowest = 1.0F;
      for (size_t bit = 0; bit < 8; ++bit) {
        lowest = std::min(lowest, bit_confidence[i * 8 + bit]);
      }
      result.byte_confidence[i] = lowest;
    }
  }

  // Addressing plausibility over the service's Hamming-coded prefix: the two
  // MRAG bytes on System B (ETSI EN 300 706 §7.1.2), the five packet prefix
  // bytes on System C (CEA-516 §3.2.1).
  bool prefix_ok = true;
  for (size_t i = 0; i < geometry.hamming_prefix_bytes; ++i) {
    if (teletext_hamming84_decode(result.bytes[i]) < 0) {
      prefix_ok = false;
      break;
    }
  }
  if (prefix_ok && !geometry.parity_coded_rows &&
      !nabts_packet_structure_possible(
          result.bytes[kNabtsPacketStructureByte])) {
    prefix_ok = false;
  }
  if (!prefix_ok) {
    return reject(result, TeletextRejectReason::kInvalidMrag);
  }

  // Transmission-coding plausibility, which the MLSE detector needs and the
  // threshold detector does not: an exact framing-code match already rules out
  // noise there, whereas here the framing code is fitted rather than matched.
  //
  // System C has no equivalent of the row-parity gate below — CEA-516 §3.3
  // makes byte parity conditional on the data group type, which one packet
  // cannot establish — so the prefix gate above, over its five Hamming-coded
  // prefix bytes, is the whole of its gate. ETSI EN 300 706 §8.2 corrects a
  // single-bit error, so a random byte decodes with probability 144/256 and
  // all five with about one in eighteen; that gate is a sieve behind the
  // residual gates above, not a substitute for them.
  if (!geometry.parity_coded_rows) {
    // Parity-guided repair is skipped for the same reason the parity gate is:
    // a data byte that fails odd parity is only known to be damaged when the
    // group it belongs to is type 0, and the packet does not say.
    if (const auto reason = sync_read_back()) {
      return reject(result, *reason);
    }
    result.data_start_sample = data_start;
    result.valid = true;
    reading.valid = true;
    if (retimed.empty()) {
      // The caller reads |grid| afresh for the next reading.
      reading.grid.swap(grid);
    } else {
      reading.grid = std::move(retimed);
    }
    reading.fit = detection_fit;
    reading.bits = bits;
    reading.trailing = trailing;
    return result;
  }

  const int mrag_low = teletext_hamming84_decode(result.bytes[0]);
  const int mrag_high = teletext_hamming84_decode(result.bytes[1]);
  if (mrag_low >= 0 && mrag_high >= 0) {
    const int row = ((mrag_low >> 3) & 0x01) | ((mrag_high << 1) & 0x1E);
    if (row <= kMlseLastParityCodedRow) {
      int odd_bytes = 0;
      for (size_t i = kMragBytes; i < packet_bytes_; ++i) {
        odd_bytes += odd_parity(result.bytes[i]) ? 1 : 0;
      }
      const double parity_fraction =
          static_cast<double>(odd_bytes) /
          static_cast<double>(packet_bytes_ - kMragBytes);
      if (parity_fraction < kMlseMinDataParityFraction) {
        return reject(result, TeletextRejectReason::kParityFraction);
      }

      // Parity-guided repair (TeletextSlicerOptions::parity_repair). ETSI EN
      // 300 706 §8.1 odd parity says a byte carries an odd number of bit
      // errors; the detector's own margins say which bit it was closest to
      // reading the other way. Flipping that one is the maximum-likelihood
      // repair of the single-bit case, and it restores parity, so the emitted
      // byte is still valid transmission coding.
      //
      // Deliberately after the plausibility gate above: repairing first would
      // manufacture the parity the gate tests for and let a false lock through.
      if (options_.parity_repair && result.has_byte_confidence) {
        // The header's page address and control bits are Hamming 8/4 (§9.3.1),
        // which is odd parity as well but carries its own correction — leave
        // those to the decoder that understands them.
        const size_t first_repairable =
            (row == 0) ? kHeaderControlBytes : kMragBytes;
        for (size_t i = first_repairable; i < packet_bytes_; ++i) {
          if (odd_parity(result.bytes[i])) {
            continue;
          }
          size_t weakest = 0;
          float lowest = std::numeric_limits<float>::infinity();
          int at_lowest = 0;
          for (size_t bit = 0; bit < 8; ++bit) {
            const float confidence = bit_confidence[i * 8 + bit];
            if (confidence < lowest) {
              lowest = confidence;
              weakest = bit;
              at_lowest = 1;
            } else if (confidence == lowest) {
              ++at_lowest;
            }
          }
          // Nothing to act on when the detector was equally unsure of two bits:
          // a coin toss between them is as likely to invent a wrong byte that
          // now passes parity as it is to mend one, and a byte that still fails
          // parity at least says so.
          if (at_lowest != 1) {
            continue;
          }
          result.bytes[i] =
              static_cast<uint8_t>(result.bytes[i] ^ (1u << weakest));
          ++result.repaired_bytes;
        }
      }
    }
  }

  if (const auto reason = sync_read_back()) {
    return reject(result, *reason);
  }
  result.data_start_sample = data_start;
  result.valid = true;
  return result;
}

}  // namespace orc
