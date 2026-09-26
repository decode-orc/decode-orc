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
#include <iterator>
#include <limits>
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

// Resample |bit_count| bit periods of |line|, starting from the bit whose
// centre is at sample |t0|, onto a fixed |phases|-per-bit grid. Output element
// k * phases + p holds bit k at phase p. Written into |out| (resized to fit)
// so a caller sweeping candidate phases can reuse one buffer.
//
// Doing this once per lock, rather than interpolating inside the trellis,
// keeps the interpolation cost linear in the packet length however many times
// the detector reads a sample.
void resample_bit_grid(const int16_t* line, size_t sample_count, double t0,
                       double spb, int bit_count, int phases,
                       std::vector<double>& out) {
  out.resize(static_cast<size_t>(bit_count) * static_cast<size_t>(phases));
  for (int k = 0; k < bit_count; ++k) {
    for (int p = 0; p < phases; ++p) {
      const double t =
          t0 + (static_cast<double>(k) + phase_offset(p, phases)) * spb;
      out[grid_index(k, p, phases)] =
          teletext_interpolate_sample(line, sample_count, t);
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
};

using FitMatrix =
    std::array<std::array<double, kMaxFitUnknowns>, kMaxFitUnknowns>;
using FitVector = std::array<double, kMaxFitUnknowns>;

// Solve the leading |n| by |n| system in place by Gaussian elimination with
// partial pivoting. False when the matrix is singular to working precision.
bool solve_in_place(FitMatrix& a, FitVector& b, int n) {
  for (int col = 0; col < n; ++col) {
    int pivot = col;
    for (int row = col + 1; row < n; ++row) {
      if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
        pivot = row;
      }
    }
    if (std::abs(a[pivot][col]) < 1e-9) {
      return false;
    }
    std::swap(a[col], a[pivot]);
    std::swap(b[col], b[pivot]);
    for (int row = col + 1; row < n; ++row) {
      const double factor = a[row][col] / a[col][col];
      if (factor == 0.0) {
        continue;
      }
      for (int k = col; k < n; ++k) {
        a[row][k] -= factor * a[col][k];
      }
      b[row] -= factor * b[col];
    }
  }
  for (int row = n - 1; row >= 0; --row) {
    double sum = b[row];
    for (int k = row + 1; k < n; ++k) {
      sum -= a[row][k] * b[k];
    }
    b[row] = sum / a[row][row];
  }
  return true;
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
template <typename BitAt>
ChannelFit fit_channel_range(const std::vector<double>& grid, int phases,
                             BitAt bit_at, int first, int last) {
  ChannelFit fit;
  fit.phases = phases;
  const int unknowns = kChannelTaps * phases + 1;
  const int offset_index = unknowns - 1;
  const int positions = last - first + 1;
  if (positions <= 0 || positions > kMaxFitPositions) {
    return fit;
  }
  // Clear only the leading |unknowns| block rather than the whole
  // kMaxFitUnknowns square. Acquisition fits one phase — 36 entries of the
  // 256 — and runs this once per candidate bit phase of every line, so the
  // difference is not academic.
  FitMatrix normal;
  FitVector rhs;
  for (int r = 0; r < unknowns; ++r) {
    rhs[static_cast<size_t>(r)] = 0.0;
    for (int c = 0; c < unknowns; ++c) {
      normal[static_cast<size_t>(r)][static_cast<size_t>(c)] = 0.0;
    }
  }

  // The regressors are transmitted bits, so every entry of the normal matrix
  // counts the equations in which two taps both saw a one: it depends only on
  // how often each pattern of the tap window occurred, and one set of counts
  // serves every phase. Only the right-hand side reads the samples.
  std::array<int, kTapPatterns> occurrences{};
  std::array<uint8_t, kMaxFitPositions> pattern_at{};
  for (int k = first; k <= last; ++k) {
    std::array<double, kChannelTaps> value{};
    int pattern = 0;
    for (int j = 0; j < kChannelTaps; ++j) {
      const int bit = bit_at(k - kChannelCentre + j);
      value[static_cast<size_t>(j)] = static_cast<double>(bit);
      pattern = (pattern << 1) | bit;
    }
    ++occurrences[static_cast<size_t>(pattern)];
    pattern_at[static_cast<size_t>(k - first)] = static_cast<uint8_t>(pattern);
    for (int p = 0; p < phases; ++p) {
      const double y = grid[grid_index(k, p, phases)];
      for (int j = 0; j < kChannelTaps; ++j) {
        rhs[static_cast<size_t>(p * kChannelTaps + j)] +=
            value[static_cast<size_t>(j)] * y;
      }
      rhs[static_cast<size_t>(offset_index)] += y;
    }
  }
  const auto tap_bit = [](int pattern, int j) {
    return (pattern >> (kChannelTaps - 1 - j)) & 1;
  };
  for (int pattern = 0; pattern < kTapPatterns; ++pattern) {
    const int count = occurrences[static_cast<size_t>(pattern)];
    if (count == 0) {
      continue;
    }
    for (int i = 0; i < kChannelTaps; ++i) {
      if (tap_bit(pattern, i) == 0) {
        continue;
      }
      for (int p = 0; p < phases; ++p) {
        const auto row = static_cast<size_t>(p * kChannelTaps + i);
        for (int j = 0; j < kChannelTaps; ++j) {
          if (tap_bit(pattern, j) != 0) {
            normal[row][static_cast<size_t>(p * kChannelTaps + j)] +=
                static_cast<double>(count);
          }
        }
        normal[row][static_cast<size_t>(offset_index)] +=
            static_cast<double>(count);
        normal[static_cast<size_t>(offset_index)][row] +=
            static_cast<double>(count);
      }
    }
  }
  normal[static_cast<size_t>(offset_index)][static_cast<size_t>(offset_index)] =
      static_cast<double>(positions * phases);

  if (!solve_in_place(normal, rhs, unknowns)) {
    return fit;
  }
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

  // The modelled sample likewise depends only on the pattern and the phase.
  std::array<std::array<double, kMaxMlseSamplesPerBit>, kTapPatterns>
      predicted{};
  for (int pattern = 0; pattern < kTapPatterns; ++pattern) {
    if (occurrences[static_cast<size_t>(pattern)] == 0) {
      continue;
    }
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
    const auto& row = predicted[pattern_at[static_cast<size_t>(k - first)]];
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

// The channel fit against the known preamble: the acquisition and detection
// model of every line.
ChannelFit fit_preamble_channel(const std::vector<double>& grid, int phases,
                                const FramingCode& framing) {
  return fit_channel_range(
      grid, phases, [&framing](int k) { return preamble_bit(k, framing); },
      kChannelCentre, kPreambleBits - 1 - (kChannelTaps - 1 - kChannelCentre));
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
                            const ChannelFit& fit, const FramingCode& framing) {
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
  for (const FramingCode& candidate : {kFramingCodeWst, kFramingCodeNabts}) {
    if (candidate == framing) {
      continue;
    }
    if (residual_for(candidate) + margin < configured) {
      return false;
    }
  }
  return true;
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
// When |bit_confidence_out| is non-null it receives |bit_count| decision
// confidences in 0 … 1: the amount by which the best path through the trellis
// carrying the opposite decision for that bit costs more than the winning path,
// as a fraction of channel_bit_energy(). The winning path's cost is a forward
// quantity and the cost of finishing from any state a backward one, so the
// exact margin for every bit at once needs one extra sweep of the trellis, with
// the forward costs kept.
double mlse_detect(const std::vector<double>& grid, int first_payload_bit,
                   const ChannelFit& fit, const FramingCode& framing,
                   int bit_count, uint8_t* bits_out, float* bit_errors_out,
                   float* bit_confidence_out, int tail_bits) {
  constexpr int kStateMask = kMlseStates - 1;
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
      row[static_cast<size_t>(2 * s)] = base;
      row[static_cast<size_t>(2 * s + 1)] = base + taps[kChannelTaps - 1];
    }
  }

  // Branch metrics of step k: they depend on the state and the new bit but
  // not on the path that reached the state. Every phase sample of the bit is
  // evidence for the same branch, which is what multiplies the evidence per
  // bit by |phases|. At the first steps the sample fully determined by a branch
  // sits inside the preamble: the channel has smeared the leading payload bits
  // back into it, and it is the only evidence those bits get on that side, so
  // skipping it costs accuracy in the first byte.
  std::array<double, kTapPatterns> metric{};
  const auto branch_metrics = [&](int k) {
    metric.fill(0.0);
    const int sample_bit = first_payload_bit + k - kChannelCentre;
    if (sample_bit < 0 || sample_bit >= grid_bits) {
      return false;
    }
    const double* observed = &grid[grid_index(sample_bit, 0, phases)];
    for (int p = 0; p < phases; ++p) {
      const double sample = observed[p];
      const auto& row = expected[static_cast<size_t>(p)];
      for (int b = 0; b < kTapPatterns; ++b) {
        const double error = sample - row[static_cast<size_t>(b)];
        metric[static_cast<size_t>(b)] += error * error;
      }
    }
    return true;
  };

  // The traceback needs one bit per state and step: which of the two
  // predecessors won. The bit that entered the state is its low bit.
  std::vector<uint16_t> from_upper(static_cast<size_t>(steps), 0);

  // Confidence needs the trellis read backwards as well as forwards, which
  // needs the cost of reaching each state at each step. It is kept only when a
  // caller asked for confidence; the branch metrics are cheaper to recompute
  // than to keep.
  const bool want_confidence = bit_confidence_out != nullptr;
  std::vector<double> forward_cost;
  if (want_confidence) {
    forward_cost.resize(static_cast<size_t>(steps) * kMlseStates);
  }

  std::array<double, kMlseStates> cost{};
  cost.fill(std::numeric_limits<double>::infinity());
  int init_state = 0;
  for (int i = 0; i < kChannelTaps - 1; ++i) {
    init_state |= preamble_bit(kPreambleBits - 1 - i, framing) << i;
  }
  cost[static_cast<size_t>(init_state)] = 0.0;

  int evaluated = 0;
  for (int k = 0; k < steps; ++k) {
    evaluated += branch_metrics(k) ? 1 : 0;
    if (want_confidence) {
      std::copy(cost.begin(), cost.end(),
                forward_cost.begin() + static_cast<ptrdiff_t>(k) * kMlseStates);
    }

    // Past the payload the transmitted bits are known black, so a terminated
    // trellis has only the zero branch to offer.
    const bool zero_only = tail_bits > 0 && k >= bit_count;
    std::array<double, kMlseStates> next{};
    unsigned upper = 0;
    for (int to = 0; to < kMlseStates; ++to) {
      const int lower = to >> 1;
      const double via_lower =
          cost[static_cast<size_t>(lower)] + metric[static_cast<size_t>(to)];
      const double via_upper = cost[static_cast<size_t>(lower + kUpperHalf)] +
                               metric[static_cast<size_t>(to + kMlseStates)];
      const bool take_upper = via_upper < via_lower;
      next[static_cast<size_t>(to)] = take_upper ? via_upper : via_lower;
      upper |= static_cast<unsigned>(take_upper) << to;
    }
    if (zero_only) {
      for (int to = 1; to < kMlseStates; to += 2) {
        next[static_cast<size_t>(to)] = std::numeric_limits<double>::infinity();
      }
    }
    from_upper[static_cast<size_t>(k)] = static_cast<uint16_t>(upper);
    cost = next;
  }

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
  std::vector<uint8_t> decided(static_cast<size_t>(steps), 0);
  for (int k = steps - 1; k >= 0; --k) {
    decided[static_cast<size_t>(k)] = static_cast<uint8_t>(state & 1);
    const bool upper = ((from_upper[static_cast<size_t>(k)] >> state) & 1) != 0;
    state = (state >> 1) | (upper ? kUpperHalf : 0);
  }
  for (int k = 0; k < bit_count; ++k) {
    bits_out[k] = decided[static_cast<size_t>(k)];
  }

  if (bit_errors_out != nullptr) {
    // Per-bit reconstruction error: the decided sequence pushed back through
    // the fitted channel, compared with the samples actually read. The trellis
    // ran on past the payload, so the bits the channel spreads into the final
    // payload samples are decided (or known, on a terminated trellis) as well,
    // and the last bits are measured on the same evidence as the rest. Bits
    // before the payload are the known preamble.
    const auto bit_value = [&](int index) {
      if (index < 0) {
        return static_cast<double>(
            preamble_bit(first_payload_bit + index, framing));
      }
      return static_cast<double>(decided[static_cast<size_t>(index)]);
    };
    for (int n = 0; n < bit_count; ++n) {
      const int sample_bit = first_payload_bit + n;
      if (sample_bit >= grid_bits) {
        break;
      }
      double sum_sq = 0.0;
      for (int p = 0; p < phases; ++p) {
        double predicted = fit.offset;
        for (int j = 0; j < kChannelTaps; ++j) {
          predicted +=
              fit.taps[static_cast<size_t>(p)][static_cast<size_t>(j)] *
              bit_value(n - kChannelCentre + j);
        }
        const double error =
            grid[grid_index(sample_bit, p, phases)] - predicted;
        sum_sq += error * error;
      }
      bit_errors_out[n] =
          static_cast<float>(std::sqrt(sum_sq / static_cast<double>(phases)));
    }
  }

  if (want_confidence) {
    // Backward sweep. remaining[s] is the cheapest way of finishing the trellis
    // from state s at the step under consideration, so the cheapest path that
    // takes a named branch is the forward cost of its origin, plus the branch,
    // plus the remaining cost of where it lands. Comparing that against the
    // winning path's cost gives the margin of the decision at that step —
    // exactly, and for every bit in one sweep.
    const double energy = channel_bit_energy(fit);
    std::array<double, kMlseStates> remaining{};
    for (int s = 0; s < kMlseStates; ++s) {
      // A terminated trellis may only finish in the all-zero state; otherwise
      // any state will do, as the traceback above assumed.
      remaining[static_cast<size_t>(s)] =
          (terminated && s != 0) ? std::numeric_limits<double>::infinity()
                                 : 0.0;
    }

    for (int k = steps - 1; k >= 0; --k) {
      branch_metrics(k);
      const int branches = (tail_bits > 0 && k >= bit_count) ? 1 : 2;
      const size_t base = static_cast<size_t>(k) * kMlseStates;

      if (k < bit_count) {
        const int flipped =
            1 - static_cast<int>(decided[static_cast<size_t>(k)]);
        double best_flipped = std::numeric_limits<double>::infinity();
        for (int s = 0; s < kMlseStates; ++s) {
          const int to = ((s << 1) | flipped) & kStateMask;
          best_flipped = std::min(
              best_flipped, forward_cost[base + static_cast<size_t>(s)] +
                                metric[static_cast<size_t>(2 * s + flipped)] +
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

      std::array<double, kMlseStates> previous{};
      for (int s = 0; s < kMlseStates; ++s) {
        const int to = (s << 1) & kStateMask;
        double cheapest = metric[static_cast<size_t>(2 * s)] +
                          remaining[static_cast<size_t>(to)];
        if (branches == 2) {
          cheapest =
              std::min(cheapest, metric[static_cast<size_t>(2 * s + 1)] +
                                     remaining[static_cast<size_t>(to | 1)]);
        }
        previous[static_cast<size_t>(s)] = cheapest;
      }
      remaining = previous;
    }
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
    // Whole four-point window inside the line, which is every read of a data
    // burst: this is the hot path, taken once per bit phase of every candidate
    // lock, so it skips the per-tap clamp entirely.
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

  // Catmull-Rom cubic in Horner form.
  const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
  const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
  const double c = -0.5 * p0 + 0.5 * p2;
  return ((a * x + b) * x + c) * x + p1;
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
      samples_per_bit_(sample_rate / bit_rate),
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
  // code around the correlation lock. Candidates are ranked by framing-code bit
  // errors first, then by whether the Hamming-coded addressing prefix that
  // follows decodes (the MRAG of §7.1.2 on System B, the packet prefix of
  // CEA-516 §3.2 on System C), then by proximity to the lock. The prefix
  // tie-break matters because widening the search also widens the chance of the
  // payload happening to spell the framing code; an alignment whose address
  // bytes decode is the real one.
  const SystemGeometry geometry = system_geometry(options_.system);
  const int max_framing_errors = options_.tolerant_framing ? 1 : 0;
  const auto bit_at = [&](double t) {
    return sample_at(line, t) > threshold ? 1 : 0;
  };
  const auto payload_start = [&](int shift) {
    return best_t0 + (kRunInBits + shift + kFramingBits) * spb;
  };

  int best_shift = 0;
  int best_errors = kFramingBits + 1;
  bool best_prefix_ok = false;
  bool found = false;
  // Whether any alignment matched the framing code, which separates "no
  // framing code here" from "framing code found but its prefix was rejected".
  bool framing_matched = false;
  for (int shift = -kFramingSearchBits; shift <= kFramingSearchBits; ++shift) {
    int errors = 0;
    for (int k = 0; k < kFramingBits; ++k) {
      errors += (bit_at(best_t0 + (kRunInBits + shift + k) * spb) !=
                 geometry.framing[static_cast<size_t>(k)])
                    ? 1
                    : 0;
    }
    if (errors > max_framing_errors) {
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
    if (options_.require_valid_mrag && !prefix_ok) {
      continue;
    }

    const bool better =
        !found || errors < best_errors ||
        (errors == best_errors && prefix_ok && !best_prefix_ok) ||
        (errors == best_errors && prefix_ok == best_prefix_ok &&
         std::abs(shift) < std::abs(best_shift));
    if (better) {
      found = true;
      best_errors = errors;
      best_shift = shift;
      best_prefix_ok = prefix_ok;
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

  result.framing_bit_errors = best_errors;
  result.data_start_sample = data_start;
  result.detector = TeletextDetector::kThreshold;
  result.valid = true;
  return result;
}

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
  std::vector<double> grid;
  double best_t0 = -1.0;
  ChannelFit acquire_fit;
  for (double t0 = search_start; t0 <= search_end && t0 < latest_t0;
       t0 += kMlsePhaseStep) {
    resample_bit_grid(line, sample_count, t0, spb, kPreambleBits, 1, grid);
    const ChannelFit fit = fit_preamble_channel(grid, 1, geometry.framing);
    if (!fit.ok || fit.gain < min_gain) {
      continue;
    }
    if (best_t0 < 0.0 || fit.residual < acquire_fit.residual) {
      best_t0 = t0;
      acquire_fit = fit;
    }
  }
  if (best_t0 < 0.0) {
    return reject(result, TeletextRejectReason::kNoPreambleLock);
  }

  // ETSI EN 300 706 §6.1 and CEA-516 §2.2.2 give the clock run-in as an
  // alternating sequence of ones and zeros, so it repeats every two bits, and
  // the five free taps absorb a shift of up to two bits either way: the fit
  // above is as good a whole number of bits from the first run-in bit as at
  // it, nearly as good half a bit from it, and its residual cannot settle
  // which. Nor need it: read from any of those shifts the packet's bits come
  // out in their own order, since the taps carry the shift, and what differs
  // is how much of the channel the tap window covers. What §6.2 and §2.2.3
  // make the framing code for — byte synchronization — is therefore taken
  // from the packet itself. Two shifts have a claim of their own: the one
  // whose fit centres the response of a bit on the centre tap, which is where
  // the §6.3 and §1.3 timing reference places the received run-in ones and is
  // the lock reported, and the best fit itself. Those and the next best fits
  // are each read. Of the readings whose Hamming-protected prefix bytes all
  // arrive as sent, the one whose bit decisions were surest is kept: §8.2
  // finds no fault to choose by among them. §8.2 does mark a byte it has
  // corrected, and a prefix of corrected bytes is also what a line that is
  // not a packet reads as, so a reading with corrections is kept only from
  // the two shifts with a claim, and of those the one with fewer.
  struct Candidate {
    double t0;
    double residual;
    double centre;
  };
  std::array<Candidate, kAlignmentCandidates> candidates{};
  int candidate_count = 0;
  candidates[static_cast<size_t>(candidate_count++)] = {
      best_t0, acquire_fit.residual / acquire_fit.gain,
      acquire_fit.taps[0][kChannelCentre]};
  for (int shift = -2 * kAlignmentBits; shift <= 2 * kAlignmentBits; ++shift) {
    if (shift == 0) {
      continue;
    }
    // The shifted grid lands between sweep phases, so each shift is refined
    // over the sweep step either side of it.
    const double centre = best_t0 + shift * 0.5 * spb;
    Candidate found{-1.0, std::numeric_limits<double>::infinity(), 0.0};
    for (int step = -2; step <= 2; ++step) {
      const double t0 = centre + step * kMlsePhaseStep;
      if (t0 < search_start || t0 > search_end || t0 >= latest_t0) {
        continue;
      }
      resample_bit_grid(line, sample_count, t0, spb, kPreambleBits, 1, grid);
      const ChannelFit fit = fit_preamble_channel(grid, 1, geometry.framing);
      if (!fit.ok || fit.gain < min_gain) {
        continue;
      }
      const double residual = fit.residual / fit.gain;
      if (residual < found.residual) {
        found = {t0, residual, fit.taps[0][kChannelCentre]};
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
  bool chosen_as_sent = false;
  double chosen_confidence = -1.0;
  size_t chosen_corrected = prefix_bytes + 1;
  const int readings = std::min(candidate_count, kAlignmentReadings);
  for (int i = 0; i < readings; ++i) {
    TeletextLineResult attempt =
        detect_mlse_at(line, sample_count,
                       candidates[static_cast<size_t>(i)].t0, phases, grid);
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
    if (corrected == 0) {
      double confidence = 0.0;
      if (attempt.has_byte_confidence) {
        for (size_t b = 0; b < packet_bytes_; ++b) {
          confidence += static_cast<double>(attempt.byte_confidence[b]);
        }
      }
      if (!chosen_as_sent || confidence > chosen_confidence) {
        chosen = std::move(attempt);
        chosen_as_sent = true;
        chosen_confidence = confidence;
      }
    } else if (!chosen_as_sent && i < kClaimedShifts &&
               corrected < chosen_corrected) {
      chosen = std::move(attempt);
      chosen_corrected = corrected;
    }
  }
  if (chosen.valid) {
    chosen.lock_sample = lock;
    chosen.data_start_sample = lock + kPreambleBits * spb;
  }
  return chosen;
}

TeletextLineResult TeletextSlicer::detect_mlse_at(
    const int16_t* line, size_t sample_count, double t0, int phases,
    std::vector<double>& grid) const {
  TeletextLineResult result = new_result();
  result.detector = TeletextDetector::kMlse;
  result.lock_sample = t0;
  const SystemGeometry geometry = system_geometry(options_.system);
  const double spb = samples_per_bit_;

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
  resample_bit_grid(line, sample_count, t0, spb,
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
  if (!framing_code_fits_best(grid, phases, best_fit, geometry.framing)) {
    return reject(result, TeletextRejectReason::kFramingCodeMiss);
  }

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
                  /*bit_confidence_out=*/nullptr, trailing) /
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
  const ChannelFit refit = fit_channel_range(grid, phases, decided_bit,
                                             kChannelCentre, last_fittable);
  if (refit.ok && refit.gain > 0.0) {
    mlse_detect(grid, kPreambleBits, refit, geometry.framing, payload_bits_,
                bits.data(), /*bit_errors_out=*/nullptr, bit_confidence.data(),
                trailing);
    result.has_byte_confidence = true;
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
  if (options_.require_valid_mrag && !prefix_ok) {
    return reject(result, TeletextRejectReason::kInvalidMrag);
  }

  // Transmission-coding plausibility, which the MLSE detector needs and the
  // threshold detector does not: an exact framing-code match already rules out
  // noise there, whereas here the framing code is fitted rather than matched.
  //
  // System C has no equivalent of the row-parity gate below — CEA-516 §3.3
  // makes byte parity conditional on the data group type, which one packet
  // cannot establish — so its five Hamming-coded prefix bytes carry the gate
  // instead. ETSI EN 300 706 §8.2 corrects a single-bit error, so a random
  // byte decodes with probability 144/256 and all five with about one in
  // eighteen; the gate is a sieve behind the residual gates above, not a
  // substitute for them. Applied unconditionally, exactly as the parity gate
  // is, rather than only when require_valid_mrag is set.
  if (!geometry.parity_coded_rows) {
    if (!prefix_ok) {
      return reject(result, TeletextRejectReason::kInvalidMrag);
    }
    // Parity-guided repair is skipped for the same reason the parity gate is:
    // a data byte that fails odd parity is only known to be damaged when the
    // group it belongs to is type 0, and the packet does not say.
    result.framing_bit_errors = 0;
    result.data_start_sample = data_start;
    result.valid = true;
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

  // The framing code was fitted rather than matched bit by bit, so there is
  // no per-bit error count to report.
  result.framing_bit_errors = 0;
  result.data_start_sample = data_start;
  result.valid = true;
  return result;
}

}  // namespace orc
