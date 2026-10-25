/*
 * File:        vbi_source_format.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Generic raw VBI container descriptor and its named presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_format.h"

#include <orc/stage/cvbs_signal_constants.h>

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// Preset data (design §3.2 / §3.3).
//
// Presets are pure data: a new format is a new entry in kPresets below and
// nothing else.  The reader, the validator and the stage all work from the
// expanded descriptor and know nothing about preset names.
// ---------------------------------------------------------------------------

// Linux bttv driver, bttv_tvnorms[]: 625-line sampling clock.
constexpr double kBt8x8PALSampleRateHz = 35468950.0;  // 8 x fsc PAL

// bttvp.h VBI_BPL: the 2048 figure is a buffer-size constant retained for
// compatibility with earlier driver versions, not a timing one.
constexpr uint32_t kBt8x8RecordStride = 2048;

// The hardware writes 1024 + vbipack * 4 samples per line; PAL uses
// vbipack = 255, leaving four bytes of padding per record.
constexpr uint32_t kBt8x8PALValidSamples = 2044;

// The final four bytes of every 65 536-byte frame are a u32 frame sequence
// number written by read().  On PAL they occupy exactly the padding of the
// last record.
constexpr uint32_t kBt8x8FrameCounterBytes = 4;

// "Experimentally, the value is measured to be about 244" — Linux
// drivers/media/pci/bt8xx/bttv-vbi.c.  Explicitly unreliable folklore, so it
// is only ever a starting hint for CRI/FRC calibration (design §5.3.3).
constexpr double kBt8x8CaptureOffsetSamples = 244.0;

// How far either side of the folkloric offset the run-in is looked for, in
// 8 x fsc samples.  About 1.4 us, which comfortably covers the spread of the
// vhs-teletext search windows the offsets in design §5.3.3 were inferred from
// while staying well inside the back porch ahead of the data.
constexpr double kBt8x8PALSearchToleranceSamples = 48.0;

// Scatter of the run-in position this card's captures are allowed before the
// fit is rejected, in 8 x fsc samples.  Eight samples is 226 ns, about one and
// a half teletext bit periods: enough for the line-to-line timing of a tape
// played into a capture card, and far too little to hide a wrong sampling
// rate, which walks the position clean out of the search window.
constexpr double kBt8x8PALMaximumSpreadSamples = 8.0;

// Fraction of the sampled records a 625-line card capture must lock before its
// fit is trusted.  A PAL broadcaster has the whole of the WST line list to use
// and the material in circulation uses most of it, so a quarter is a low bar
// that only a wrong configuration falls under.
constexpr double kBt8x8PALMinimumAcceptanceFraction = 0.25;

// The same figure for a SECAM source, where it cannot be anything like as high
// (see make_bt8x8_secam below for why).  It is deliberately not zero: the fit
// still has to lock on something repeatable, and the absolute floor of eight
// accepted records, the spread limit and the drift check are what actually
// catch a wrong container, rate or system.  This one figure only asks that at
// least one record of every other frame carried the data service.
constexpr double kBt8x8SECAMMinimumAcceptanceFraction = 0.03;

// Linux bttv driver, bttv_tvnorms[]: 525-line sampling clock.
constexpr double kBt8x8NTSCSampleRateHz = 28636363.0;  // 8 x fsc NTSC

// 1024 + vbipack * 4 again, this norm's vbipack being 144, which leaves 448
// bytes of padding per record rather than the PAL entry's four.  Measured on
// the reference capture as well as read off the driver: every record is flat
// zero from sample 1600 to the end of its 2048-byte stride.
constexpr uint32_t kBt8x8NTSCValidSamples = 1600;

// Scatter allowed before a 525-line bt8x8 fit is rejected.  The PAL entry
// allows 8 samples at 35,47 MHz, which is 226 ns; the same tolerance in time
// is 6,5 samples at this norm's slower clock, and it is the tolerance that is
// the measured quantity, not the count.  The reference capture sits at 3,3
// samples, so this leaves rather better than the factor of two the PAL entry
// has.
constexpr double kBt8x8NTSCMaximumSpreadSamples = 6.5;

// How far either side of the folkloric offset the run-in is looked for on a
// 525-line dump.  The same count as the 625-line entry but not the same
// reasoning, and not the same span in time: 48 samples is 1,68 us here.  A
// fitted offset is the distance from the service table's nominal anchor to
// where the run-in actually is, and the 525-line anchor is a measurement of
// other captures that broadcasters disagree about by the better part of a
// microsecond (see vbi_teletext_service.cpp).  The window has to cover that
// disagreement as well as the card's own uncertainty; the reference capture
// fits 5,1 samples from the configured offset, so nearly all of the window is
// margin against material this one says nothing about.
constexpr double kBt8x8NTSCSearchToleranceSamples = 48.0;

// Fraction of the sampled records a 525-line capture must lock, and the one
// threshold that cannot be shared with the 625-line entries — for exactly the
// reason the cx23885 preset below needs its own figure.  A 625-line
// broadcaster had the whole line list to fill and used most of it; a US
// broadcaster carried its magazines on a handful of the twelve.  The reference
// capture carries teletext on five records of the first stored field and three
// of the second, which is a third of the twenty-four records sampled per
// frame, and it locks on 33,1% of them.  One tenth is the bar: below what this
// material carries even after a tape has lost some of it, and far above what a
// wrong container, rate or system produces — that is a scatter the spread and
// drift checks reject long before the count does.
constexpr double kBt8x8NTSCMinimumAcceptanceFraction = 0.10;

struct PresetEntry {
  const char* name;
  VBISourceFormat format;
};

VBISourceFormat make_bt8x8_pal() {
  VBISourceFormat format;
  format.sample_rate_hz = kBt8x8PALSampleRateHz;
  format.line_length = kBt8x8RecordStride;
  format.valid_samples = kBt8x8PALValidSamples;
  format.sample_format = VBISampleFormat::kU8;
  format.field_lines = 16;
  format.field_range = VBIFieldRange{0, 15};
  format.frame_trailer_bytes = kBt8x8FrameCounterBytes;
  format.frame_trailer_is_counter = true;
  format.capture_offset_samples = kBt8x8CaptureOffsetSamples;
  format.capture_offset_is_auto = true;
  format.first_field = 1;
  format.tv_system = VBITVSystem::kPAL;
  format.tt_system = VBITeletextSystem::kWST;
  format.family = VBISourceFamily::kCardCapture;

  // A bt8x8 card is how tape and off-air material is captured, so this preset
  // has to accept what such a source really looks like.  The card's horizontal
  // phase-locked loop line-locks the sampling clock, but residual jitter,
  // velocity error and the settling after head switching remain, and the
  // teletext lines sit inside that recovery window (design §5.3.6).  The
  // reference capture measures at about four samples of scatter with a global
  // offset that is nevertheless stable to well under a sample, which is
  // exactly the asymmetry the design describes: this stage needs a global
  // offset and the downstream slicer does the per-line lock.
  //
  // The tight figure is left at the design §5.3.4 value, so a genuinely clean
  // capture is still reported as such rather than being flattered by a
  // threshold set for tape.
  format.calibration.search_tolerance_samples = kBt8x8PALSearchToleranceSamples;
  format.calibration.tight_spread_samples = 0.5;
  format.calibration.maximum_spread_samples = kBt8x8PALMaximumSpreadSamples;
  format.calibration.maximum_drift_samples = kBt8x8PALMaximumSpreadSamples;
  format.calibration.minimum_acceptance_fraction =
      kBt8x8PALMinimumAcceptanceFraction;
  return format;
}

// A bt8x8 dump of a SECAM source.  The container is the PAL one byte for byte:
// the driver's SECAM entry (Linux bttv_tvnorms[]) carries the same 35 468 950
// Hz sampling clock, the same vbipack of 255 and the same vbistart of {7, 320}
// as its PAL entry, so the record stride, the field stride, the frame trailer
// and the lines the sixteen records cover are all unchanged.  Post-decode SECAM
// is a 625-line signal carrying the same World System Teletext, so the output
// geometry and the data service are the PAL ones too.
//
// What differs is how much of the line list can carry teletext.  A SECAM
// transmission with vertical colour identification puts the identification
// signal — a continuous 4,4 MHz burst, the "green bottles" — on frame lines
// 8-15 and 321-328, which is records 1 to 8 of each stored field.  Half of the
// sixteen records are therefore spoken for before any teletext is inserted, and
// broadcasters using those idents typically left the remainder to test signals
// and a very few teletext lines: the Russian tape this preset was measured on
// carries teletext on records 12-14 only (frame lines 19-21 and 332-334), with
// a VITS and a white bar on records 9-11.
//
// That makes the acceptance fraction — the share of sampled records the clock
// run-in is found on — the one calibration threshold that cannot be shared with
// PAL.  Three lines of sixteen is 19% before a single line is lost to tape, and
// the PAL preset's quarter rejects the fit outright even though it is a good
// one: on the reference tape it lands on 57 records with a spread of under two
// samples and no drift, which is a healthy fit by every other measure the
// stage has.
VBISourceFormat make_bt8x8_secam() {
  VBISourceFormat format = make_bt8x8_pal();
  format.calibration.minimum_acceptance_fraction =
      kBt8x8SECAMMinimumAcceptanceFraction;
  return format;
}

// A bt8x8 dump of a 525-line source.  The same card and the same driver, but
// not the same container: bttv_tvnorms[] gives its NTSC entry a different
// sampling clock (28 636 363 Hz, 8 x fsc NTSC), a different vbipack (144, so
// 1600 real samples of the 2048-byte record rather than 2044) and a different
// vbistart ({10, 273} rather than {7, 320}).  Only the record stride, the
// sixteen records a field stores and the frame trailer survive from the 625-
// line entry, so this is written out in full rather than derived from it.
//
// vbistart {10, 273} is broadcast frame lines 10 and 273, which is exactly
// where the 525-line teletext list starts, so records 0 to 11 are the whole of
// it (ITU-R BT.653 §2) and records 12 to 15 are the start of the picture.  The
// four bytes of frame counter at the tail of each 65 536-byte frame land in
// the last record's padding here as they do on PAL — there is simply a great
// deal more padding for them to land in.
//
// The container was measured on the reference capture as well as read off the
// driver: its records are flat zero from sample 1600 to the end of the stride,
// its frame counter increments by one per frame across the file, and the fit
// settles at 249,1 samples against the driver's folkloric 244 with no
// significant drift, which is what says the sampling rate is right.
//
// As with the cx23885 entries below, the container says nothing about the
// service the lines carry: the two 525-line teletext systems sit on the same
// lines and differ in framing code and packet length, so the service is
// configuration and the reader is the same either way.
VBISourceFormat make_bt8x8_ntsc(VBITeletextSystem tt_system) {
  VBISourceFormat format;
  format.sample_rate_hz = kBt8x8NTSCSampleRateHz;
  format.line_length = kBt8x8RecordStride;
  format.valid_samples = kBt8x8NTSCValidSamples;
  format.sample_format = VBISampleFormat::kU8;
  format.field_lines = 16;
  format.field_range = VBIFieldRange{0, 11};
  format.frame_trailer_bytes = kBt8x8FrameCounterBytes;
  format.frame_trailer_is_counter = true;
  format.capture_offset_samples = kBt8x8CaptureOffsetSamples;
  format.capture_offset_is_auto = true;

  // The driver stores count[0] records from start[0] before count[1] from
  // start[1], and start[0] is the field 1 line list.
  format.first_field = 1;

  format.tv_system = VBITVSystem::kNTSC;
  format.tt_system = tt_system;
  format.family = VBISourceFamily::kCardCapture;

  // A bt8x8 card is how tape and off-air material is captured here too, so the
  // spread limit is the 625-line entry's tolerance in time rather than its
  // count of samples, and the tight figure is left where it is so that a
  // genuinely clean capture is still reported as one.
  format.calibration.search_tolerance_samples =
      kBt8x8NTSCSearchToleranceSamples;
  format.calibration.tight_spread_samples = 0.5;
  format.calibration.maximum_spread_samples = kBt8x8NTSCMaximumSpreadSamples;
  format.calibration.maximum_drift_samples = kBt8x8NTSCMaximumSpreadSamples;
  format.calibration.minimum_acceptance_fraction =
      kBt8x8NTSCMinimumAcceptanceFraction;
  return format;
}

// ---------------------------------------------------------------------------
// cx2388x card dumps (Conexant CX23880/1/2/3, the Linux cx88 driver).
//
// The bt8x8's successor from the same lineage, and very nearly its container:
// the same 8 x fsc clock, the same 2048-byte record and the same 1600 real
// samples on 525 lines.  What it does not have is the bt8x8's sixteen-record
// field or its frame counter.
// ---------------------------------------------------------------------------

// CX23880/1/2/3 data sheet (101069A) §2.6.16: "The VBI data is sampled at a
// rate of 8 x Fsc", which on a 525-line norm is the 28,636363 MHz crystal
// itself (Table 2-32, SRC_REG_VAL 16'h20000 — no rate conversion at all).  The
// cx88 driver's cx8800_vbi_fmt() reports the same figure.
constexpr double kCx2388xNTSCSampleRateHz = 28636363.0;

// VBI_LINE_LENGTH: the buffer stride the driver hands each record.
constexpr uint32_t kCx2388xRecordStride = 2048;

// Real samples at the head of each record.  §2.6.16 has the card write
// VBI_PKT dwords per line, and the driver's packet ends here: measured on the
// reference capture, every record runs through the next line's sync pulse,
// which falls at about sample 1578, and is a constant 116 from sample 1600 or
// 1624 to the end of the stride.  The 24 samples between those two figures are
// the sync tip when present, so 1600 is the count that holds on every record.
// It is the bt8x8 NTSC figure too — the same clock spending the same part of
// the line.
constexpr uint32_t kCx2388xNTSCValidSamples = 1600;

// VBI_LINE_NTSC_COUNT records per field from start[] = {10, 273}, so the twelve
// records are field lines 10-21: the whole 525-line teletext list of BT.653
// and nothing more.  Confirmed on the reference capture, whose record 9 carries
// the SMPTE vertical interval reference signal that belongs on line 19.
constexpr uint32_t kCx2388xNTSCFieldLines = 12;

// cx8800_vbi_fmt(): f->fmt.vbi.offset = 244, the bt8x8 driver's experimental
// figure carried across, at the same sampling clock so it does at least mean
// the same point in the line.  Here it is better than folklore: the next line's
// sync pulse lands at sample 1578 of a 1820-sample line, which puts sample 0 at
// 242 from 0H.  Still only a starting hint for the fit — VBI_HDELAY is a
// register, and nothing says every board programs it alike.
constexpr double kCx2388xCaptureOffsetSamples = 244.0;

// The calibration thresholds are the bt8x8 NTSC entry's, for the same reasons:
// the same clock, so the same tolerance in time is the same count of samples;
// the same tape and off-air material behind it; and the same handful of
// teletext lines in use.  The reference capture carries NABTS on records 5-8
// of both fields, a third of the records sampled, so one tenth is the same
// margin it is there.  It locks on 36,2% of its records with a spread of 2,6
// samples and fits at 230,1 — twelve samples short of the sync figure above,
// because this transmission's run-in sits 0,42 us later than the service
// table's NABTS anchor, which is the disagreement between broadcasters the
// search window is sized for.
constexpr double kCx2388xNTSCSearchToleranceSamples = 48.0;
constexpr double kCx2388xNTSCMaximumSpreadSamples = 6.5;
constexpr double kCx2388xNTSCMinimumAcceptanceFraction = 0.10;

// A 525-line cx2388x dump.  Like every card container here it says nothing
// about the service the lines carry, so the service is configuration and the
// reader is the same either way.
//
// There is no frame counter: the record padding is filled with a constant, not
// a sequence number, so a dropped frame cannot be seen.  A frame is 49 152
// bytes — two fields of twelve 2048-byte records.
VBISourceFormat make_cx2388x_ntsc(VBITeletextSystem tt_system) {
  VBISourceFormat format;
  format.sample_rate_hz = kCx2388xNTSCSampleRateHz;
  format.line_length = kCx2388xRecordStride;
  format.valid_samples = kCx2388xNTSCValidSamples;
  format.sample_format = VBISampleFormat::kU8;
  format.field_lines = kCx2388xNTSCFieldLines;
  format.field_range = VBIFieldRange{0, kCx2388xNTSCFieldLines - 1};
  format.frame_trailer_bytes = 0;
  format.frame_trailer_is_counter = false;
  format.capture_offset_samples = kCx2388xCaptureOffsetSamples;
  format.capture_offset_is_auto = true;

  // The driver stores count[0] records from start[0] before count[1] from
  // start[1], and start[0] is the field 1 line list.  The reference capture
  // carries no line 21 caption to check this against.
  format.first_field = 1;

  format.tv_system = VBITVSystem::kNTSC;
  format.tt_system = tt_system;
  format.family = VBISourceFamily::kCardCapture;

  format.calibration.search_tolerance_samples =
      kCx2388xNTSCSearchToleranceSamples;
  format.calibration.tight_spread_samples = 0.5;
  format.calibration.maximum_spread_samples = kCx2388xNTSCMaximumSpreadSamples;
  format.calibration.maximum_drift_samples = kCx2388xNTSCMaximumSpreadSamples;
  format.calibration.minimum_acceptance_fraction =
      kCx2388xNTSCMinimumAcceptanceFraction;
  return format;
}

// ---------------------------------------------------------------------------
// cx23885 card dumps (Hauppauge HVR-1250 and its siblings).
//
// The other card container in circulation, and nothing like the bt8x8 one: the
// figures below are the Linux cx23885 driver's own, from cx23885_vbi_fmt().
// ---------------------------------------------------------------------------

// cx23885_vbi_fmt(): f->fmt.vbi.sampling_rate = 27000000.  Unlike the bt8x8's,
// this rate is not a multiple of a colour subcarrier — it is the 13,5 MHz
// component-digital rate doubled, which is what the card's horizontal timing
// generator runs at.
constexpr double kCx23885SampleRateHz = 27000000.0;

// VBI_LINE_LENGTH.  1440 samples at 27 MHz is 53,33 us, which is exactly the
// ITU-R BT.601 digital active line (720 samples at 13,5 MHz) written at twice
// the rate.  The card hands over that window and nothing else: no sync, no
// burst and no padding, so every stored sample is a real one and there is no
// frame trailer to carry a sequence number.  Confirmed on the reference
// capture, whose blank records are flat to within noise across all 1440
// samples with no blanking structure at either end.
constexpr uint32_t kCx23885LineLength = 1440;

// VBI_NTSC_LINE_COUNT, stored from start[] = V4L2_VBI_ITU_525_F1_START + 9 and
// V4L2_VBI_ITU_525_F2_START + 9 — broadcast field lines 10 and 273.  Twelve
// records from line 10 is exactly the 525-line teletext list of BT.653, so
// every stored record carries the service and the range is the whole field.
constexpr uint32_t kCx23885NTSCFieldLines = 12;

// Where the window opens, in this card's samples from 0H.  The driver's own
// f->fmt.vbi.offset is 0, which is a stub rather than a measurement — the
// hardware plainly does not hand over the sync pulse — so the figure comes
// from the window the timing generator is built to produce: SMPTE 125M puts
// the start of the digital active line 122 samples at 13,5 MHz after 0H, which
// is 244 here, or 9,04 us.
//
// That it is numerically the bt8x8's folklore figure is a coincidence of two
// different rates and must not be read as one constant serving both: 244
// samples at the bt8x8's 35,47 MHz is 6,88 us, an entirely different point in
// the line.
//
// The reference capture agrees to a third of a microsecond.  Its vhs-decode
// sibling — the same broadcast off the same tape, time-base corrected, so its
// records really do start at 0H — puts this transmission's run-in at 9,50 us,
// and the run-in is found 5,8 samples into the stored record, which places
// sample 0 at 9,29 us against the standard's 9,04.
constexpr double kCx23885NTSCCaptureOffsetSamples = 244.0;

// How far either side of that the run-in is looked for.  Wider in time than the
// bt8x8's 1,35 us, because on 525 lines the figure has to cover the anchor as
// well as the window.  A fitted offset is the distance from the service table's
// nominal time to where the run-in actually is, and that table's NABTS entry is
// a measurement of other captures: this one transmits nearly a microsecond
// earlier, which is the same spread between sources the 525-line WST anchor
// records (see vbi_teletext_service.cpp).  The fit on the reference capture
// settles at 272,3 samples, 28 from the figure configured above, so 1,78 us
// leaves some twenty samples of margin beyond the worst case measured — and the
// framing code's three consecutive ones are what stops a search this wide
// latching a bit period out.
constexpr double kCx23885NTSCSearchToleranceSamples = 48.0;

// Scatter allowed before the fit is rejected.  The bt8x8 preset allows 8
// samples at 35,47 MHz, which is 226 ns; the same tolerance in time is 6
// samples here.  The reference capture fits inside it with room to spare, at a
// spread of 1,6 samples, and a capture of the same tape read as the wrong
// 525-line service scatters by 28 — so this limit is what catches that mistake
// as well as a wrong container or rate.
constexpr double kCx23885NTSCMaximumSpreadSamples = 6.0;

// Fraction of the sampled records that must lock.  This is the one threshold
// that cannot be inherited from the 625-line presets, for the same reason the
// SECAM preset needs its own: a 625-line broadcaster had the whole line list to
// fill and used most of it, while a US network carried its magazine on two or
// three lines of the twelve.  Both captures of the reference broadcast agree on
// which — field lines 15 and 16 of both fields carry the service, and the
// vhs-decode sibling of the same tape adds line 18 of field 2.
//
// The reference capture locks on 24,7% of its records, which is three of the
// twelve and a hair under the 625-line quarter — close enough that the PAL
// figure would reject this capture on a slightly worse tape while still
// describing it correctly.  One tenth is the bar: below the two lines this
// material reliably carries, so a capture that loses one of them to head
// switching still passes, and far above what a wrong container, rate or system
// produces, which is a scatter the spread and drift checks reject long before
// the count does.
constexpr double kCx23885NTSCMinimumAcceptanceFraction = 0.10;

// A 525-line cx23885 dump.  The container is the driver's and says nothing
// about the service carried on the lines, exactly as the .tbc crop below does
// not: the two 525-line teletext systems sit on the same lines and differ in
// framing code and packet length, so the service is configuration and the
// reader is the same either way.
VBISourceFormat make_cx23885_ntsc(VBITeletextSystem tt_system) {
  VBISourceFormat format;
  format.sample_rate_hz = kCx23885SampleRateHz;
  format.line_length = kCx23885LineLength;
  format.valid_samples = kCx23885LineLength;
  format.sample_format = VBISampleFormat::kU8;
  format.field_lines = kCx23885NTSCFieldLines;
  format.field_range = VBIFieldRange{0, kCx23885NTSCFieldLines - 1};
  format.frame_trailer_bytes = 0;
  format.frame_trailer_is_counter = false;
  format.capture_offset_samples = kCx23885NTSCCaptureOffsetSamples;
  format.capture_offset_is_auto = true;

  // The driver stores count[0] records from start[0] before count[1] from
  // start[1], and start[0] is the field 1 line list.
  format.first_field = 1;

  format.tv_system = VBITVSystem::kNTSC;
  format.tt_system = tt_system;
  format.family = VBISourceFamily::kCardCapture;

  format.calibration.search_tolerance_samples =
      kCx23885NTSCSearchToleranceSamples;
  format.calibration.tight_spread_samples = 0.5;
  format.calibration.maximum_spread_samples = kCx23885NTSCMaximumSpreadSamples;
  format.calibration.maximum_drift_samples = kCx23885NTSCMaximumSpreadSamples;
  format.calibration.minimum_acceptance_fraction =
      kCx23885NTSCMinimumAcceptanceFraction;
  return format;
}

// Cropped VBI-only .tbc captures: the first sixteen line records of each field
// of a decoded luma .tbc, at the decoder's own 4 x fsc rate.  The rate is taken
// from the CVBS constants rather than written out, so that it is bit-identical
// to the output lattice's and the resampling ratio is exactly one.
constexpr double kTBCVBINTSCSampleRateHz = kNtscSampleRate;

// A .tbc line is the whole line: 910 samples with no padding.
constexpr uint32_t kTBCVBINTSCRecordStride = 910;

// Record 0 of the sixteen is broadcast field line 9 — the last post-equalising
// line, which carries no data service — so the standard's twelve teletext lines
// (field lines 10-21 of each field) are records 1 to 12, and records 13-15 are
// active picture.
//
// Measured on the circulating captures rather than assumed: record 0 carries a
// 2.3 us equalising pulse where every other record carries a 4.7 us line sync,
// and record 12 carries the line 21 closed caption run-in at its standard
// 10.5 us from 0H.
constexpr uint32_t kTBCVBINTSCFirstDataRecord = 1;
constexpr uint32_t kTBCVBINTSCLastDataRecord = 12;

// The first stored field of every circulating capture is television field 1,
// measured two ways on all five of them: record 0 carries two equalising pulses
// on field 1 against one on field 2, and on a captioned recording record 12 is
// line 21, so the field carrying the caption run-in is field 1.  A crop whose
// producer started on the other field would need its own preset.
constexpr uint32_t kFirstStoredField = 1;

VBISourceFormat make_tbc_vbi_ntsc(VBITeletextSystem tt_system) {
  VBISourceFormat format;
  format.sample_rate_hz = kTBCVBINTSCSampleRateHz;
  format.line_length = kTBCVBINTSCRecordStride;
  format.valid_samples = kTBCVBINTSCRecordStride;
  format.sample_format = VBISampleFormat::kU16LE;
  format.field_lines = 16;
  format.field_range =
      VBIFieldRange{kTBCVBINTSCFirstDataRecord, kTBCVBINTSCLastDataRecord};
  format.frame_trailer_bytes = 0;
  format.frame_trailer_is_counter = false;

  // Family B: the upstream time-base correction already put sample 0 of every
  // record at that line's 0H, so the offset is known to be zero and must never
  // be fitted (design §5.3.3).  Validation enforces that.
  format.capture_offset_samples = 0.0;
  format.capture_offset_is_auto = false;

  format.first_field = kFirstStoredField;
  format.tv_system = VBITVSystem::kNTSC;
  format.tt_system = tt_system;
  format.family = VBISourceFamily::kTBCDerived;

  // These thresholds judge the run-in survey, which on a source of this family
  // measures the service anchor rather than fitting a capture offset (design
  // §5.3.3).  They are the survey's own quality bar either way, so the reasons
  // for each figure are the family's.
  //
  // The search is narrower than the 625-line card default.  It only has to
  // cover how much broadcasters disagree about the anchor, and that spread is
  // documented: the 525-line figures in vbi_teletext_service.cpp run from 9,29
  // to 10,5 us, some seventeen samples at this rate.  Twenty-four covers it
  // with margin while keeping the window a good deal shorter than the 60-sample
  // template it is sliding, which is what stops a noisy line latching a bit
  // period out.
  format.calibration.search_tolerance_samples = 24.0;

  // A time-base corrected source really should be tight, so the clean-source
  // classification is left where it is and the rejection limit stays the
  // default three samples — 210 ns, a fifth of what the card presets allow.
  // The reference NABTS crop measures 0,8 samples across hundreds of records.
  format.calibration.tight_spread_samples = 0.5;
  format.calibration.maximum_spread_samples = 3.0;
  format.calibration.maximum_drift_samples = 3.0;

  // How many records must lock, and for the same reason the 525-line card
  // preset needs its own figure: a US network carried its magazine on two or
  // three lines of the twelve, and the reference crop locks on five records of
  // twenty-four.  The 625-line quarter would reject that, and rejecting it here
  // does not stop the run — it silently leaves the tabulated anchor in place,
  // which is the very thing being corrected.
  format.calibration.minimum_acceptance_fraction = 0.10;
  return format;
}

const std::vector<PresetEntry>& presets() {
  // Every preset is a complete configuration: the container, the data service
  // it carries and the policies that follow from both.  There is nothing left
  // for the user to fill in, which is why there is no "custom" entry — a format
  // nobody has measured cannot be described correctly by guessing at fields,
  // and adding one that has been measured is a single entry here.
  //
  // The names are what the user reads and what the project file stores, so they
  // say where the capture came from, what its samples are, and which service it
  // carries; the geometry behind each is in the parameter description.
  static const std::vector<PresetEntry> kPresets = {
      {"bt8x8 card dump, 8-bit (WST)", make_bt8x8_pal()},
      {"bt8x8 card dump, 8-bit (WST, SECAM source)", make_bt8x8_secam()},
      {"bt8x8 card dump, 8-bit (WST, NTSC source)",
       make_bt8x8_ntsc(VBITeletextSystem::kWST)},
      {"bt8x8 card dump, 8-bit (NABTS, NTSC source)",
       make_bt8x8_ntsc(VBITeletextSystem::kNABTS)},
      {"cx2388x card dump, 8-bit (WST, NTSC source)",
       make_cx2388x_ntsc(VBITeletextSystem::kWST)},
      {"cx2388x card dump, 8-bit (NABTS, NTSC source)",
       make_cx2388x_ntsc(VBITeletextSystem::kNABTS)},
      {"cx23885 card dump, 8-bit (WST)",
       make_cx23885_ntsc(VBITeletextSystem::kWST)},
      {"cx23885 card dump, 8-bit (NABTS)",
       make_cx23885_ntsc(VBITeletextSystem::kNABTS)},
      {".tbc VBI crop, 16-bit (WST)",
       make_tbc_vbi_ntsc(VBITeletextSystem::kWST)},
      {".tbc VBI crop, 16-bit (NABTS)",
       make_tbc_vbi_ntsc(VBITeletextSystem::kNABTS)},
  };
  return kPresets;
}

}  // namespace

uint32_t standard_teletext_lines_per_field(VBITVSystem tv_system,
                                           VBITeletextSystem tt_system) {
  // ETSI EN 300 706 §4.1: WST occupies broadcast frame lines 7-22 and
  // 320-335 on 625-line systems — sixteen records per field.
  if (tv_system == VBITVSystem::kPAL && tt_system == VBITeletextSystem::kWST) {
    return 16;
  }

  // ITU-R BT.653 §2: both 525-line teletext systems in scope — System B, the
  // 525-line variant of WST, and System C (NABTS) — occupy broadcast frame
  // lines 10-21 and 273-284, twelve records per field.  The systems differ in
  // framing code and packet length, not in which lines they sit on.
  if (tv_system == VBITVSystem::kNTSC || tv_system == VBITVSystem::kPALM) {
    return 12;
  }

  // Any other pairing has no defined line list; callers report it as an
  // unsupported configuration rather than guessing one.
  return 0;
}

std::vector<std::string> vbi_source_preset_names() {
  std::vector<std::string> names;
  names.reserve(presets().size());
  for (const auto& entry : presets()) {
    names.emplace_back(entry.name);
  }
  return names;
}

std::vector<std::string> vbi_source_preset_names(VBITVSystem tv_system) {
  std::vector<std::string> names;
  names.reserve(presets().size());
  for (const auto& entry : presets()) {
    if (entry.format.tv_system == tv_system) {
      names.emplace_back(entry.name);
    }
  }
  return names;
}

bool expand_vbi_source_preset(const std::string& preset_name,
                              VBISourceFormat& out_format,
                              std::string& error_message) {
  for (const auto& entry : presets()) {
    if (preset_name == entry.name) {
      out_format = entry.format;
      return true;
    }
  }

  std::string known;
  for (const auto& entry : presets()) {
    if (!known.empty()) {
      known += ", ";
    }
    known += entry.name;
  }
  error_message = "Unknown VBI source format preset '" + preset_name +
                  "'. Known presets: " + known + ".";
  return false;
}

std::string to_string(VBISampleFormat sample_format) {
  switch (sample_format) {
    case VBISampleFormat::kU8:
      return "u8";
    case VBISampleFormat::kU16LE:
      return "u16le";
    case VBISampleFormat::kS16LE:
      return "s16le";
  }
  return "u8";
}

}  // namespace orc
