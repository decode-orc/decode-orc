/*
 * File:        closed_caption_assembler_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the closed caption trailing-window assembler
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "closed_caption_assembler.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "support/closed_caption_fixtures.h"

namespace gui_unit_test {

constexpr uint64_t kWindow = ClosedCaptionAssembler::kTrailingWindowFrames;

namespace {

// The assembler decodes frames in order and waits for any it has not been
// given, because a caption decoder is a state machine that cannot be fed a
// field it has already passed. The dialog's request loop delivers every frame
// of the window for that reason; tests that care about only a few of them fill
// in the rest here.
void fillGapsBefore(ClosedCaptionAssembler& assembler, uint64_t frame) {
  for (uint64_t earlier = assembler.windowStartFrame(); earlier < frame;
       ++earlier) {
    if (!assembler.hasFrame(earlier)) {
      assembler.markFrameUnavailable(earlier);
    }
  }
}

// Store one caption byte pair in the first field of |frame| (NTSC carries
// captions on one field of each frame).
void storePair(ClosedCaptionAssembler& assembler, uint64_t frame, int32_t data0,
               int32_t data1) {
  fillGapsBefore(assembler, frame);
  assembler.storeFrame(frame, makeCaptionField(data0, data1), makeEmptyField());
}

// Transmit a complete pop-on caption: enter pop-on mode, place the cursor,
// send the text, then swap it on screen with End of Caption. The caption
// appears at the frame carrying the EOC.
void storePopOnCaption(ClosedCaptionAssembler& assembler, uint64_t first_frame,
                       const std::string& text) {
  storePair(assembler, first_frame, kCcControlByte, kCcResumeCaptionLoading);
  storePair(assembler, first_frame + 1, kCcControlByte, kCcPacRow15Col0);
  uint64_t frame = first_frame + 2;
  for (size_t i = 0; i < text.size(); i += 2) {
    const char second = i + 1 < text.size() ? text[i + 1] : ' ';
    storePair(assembler, frame, static_cast<int32_t>(text[i]),
              static_cast<int32_t>(second));
    ++frame;
  }
  storePair(assembler, frame, kCcControlByte, kCcEndOfCaption);
}

// Store one caption byte pair in the second stored field of |frame|: where a
// recording whose first stored field is television field 2 carries CC1.
void storeSecondFieldPair(ClosedCaptionAssembler& assembler, uint64_t frame,
                          int32_t data0, int32_t data1) {
  fillGapsBefore(assembler, frame);
  assembler.storeFrame(frame, makeEmptyField(), makeCaptionField(data0, data1));
}

// storePopOnCaption() on the second stored field.
void storeSecondFieldPopOnCaption(ClosedCaptionAssembler& assembler,
                                  uint64_t first_frame,
                                  const std::string& text) {
  storeSecondFieldPair(assembler, first_frame, kCcControlByte,
                       kCcResumeCaptionLoading);
  storeSecondFieldPair(assembler, first_frame + 1, kCcControlByte,
                       kCcPacRow15Col0);
  uint64_t frame = first_frame + 2;
  for (size_t i = 0; i < text.size(); i += 2) {
    const char second = i + 1 < text.size() ? text[i + 1] : ' ';
    storeSecondFieldPair(assembler, frame, static_cast<int32_t>(text[i]),
                         static_cast<int32_t>(second));
    ++frame;
  }
  storeSecondFieldPair(assembler, frame, kCcControlByte, kCcEndOfCaption);
}

// Roll-up mode with the cursor on the bottom row, at frames 1 and 2, so the
// pairs stored from frame 3 on are written straight to the screen.
void storeRollUpStart(ClosedCaptionAssembler& assembler) {
  storePair(assembler, 1, kCcControlByte, kCcRollUp2);
  storePair(assembler, 2, kCcControlByte, kCcPacRow15Col0);
}

// Frame the caption sent by storePopOnCaption() reaches the screen at.
uint64_t popOnDisplayFrame(uint64_t first_frame, const std::string& text) {
  return first_frame + 2 + (text.size() + 1) / 2;
}

// A window carrying two of line 21's services at once: a CC1 pop-on caption
// reading "HI" with a TEXT2 page reading "42" spliced through the middle of
// it. Read without demultiplexing, the two land in one buffer.
//
// One pair per frame, starting at frame 1; the caption is on screen from
// frame 7.
void storeInterleavedServices(ClosedCaptionAssembler& assembler) {
  storePair(assembler, 1, kCcControlByte, kCcResumeCaptionLoading);  // CC1
  storePair(assembler, 2, kCcControlByte, kCcPacRow15Col0);          // CC1
  storePair(assembler, 3, kCcControlByteChannel2, kCcTextRestart);   // TEXT2
  storePair(assembler, 4, '4', '2');                                 // TEXT2
  storePair(assembler, 5, kCcControlByte, kCcPacRow15Col0);          // CC1
  storePair(assembler, 6, 'H', 'I');                                 // CC1
  storePair(assembler, 7, kCcControlByte, kCcEndOfCaption);          // CC1
}

}  // namespace

TEST(ClosedCaptionAssemblerTest, WindowStartsAtZeroForEarlyFrames) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(3);

  EXPECT_EQ(assembler.windowStartFrame(), 0u);
  const auto needed = assembler.framesNeedingData();
  ASSERT_EQ(needed.size(), 4u);
  EXPECT_EQ(needed.front(), 0u);
  EXPECT_EQ(needed.back(), 3u);
}

// Frames are named a batch at a time: a window is half a minute of video, and
// one observation read per frame of it, queued at once, would sit in front of
// everything else the previewer wants to do.
TEST(ClosedCaptionAssemblerTest, FramesNeedingDataIsCappedPerCall) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(kWindow);

  EXPECT_EQ(assembler.framesNeedingData().size(),
            ClosedCaptionAssembler::kMaxFramesPerRequest);
  // The progress readout counts the whole backlog, not the batch.
  EXPECT_GT(assembler.framesNeedingDataCount(),
            ClosedCaptionAssembler::kMaxFramesPerRequest);
}

// A pop-on caption is assembled off screen and appears whole: nothing is
// displayed until the End of Caption code swaps the buffers.
TEST(ClosedCaptionAssemblerTest, PopOnCaptionAppearsAtEndOfCaption) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storePopOnCaption(assembler, 1, "HELLO");
  const uint64_t display_frame = popOnDisplayFrame(1, "HELLO");

  const auto* before = assembler.screenAt(display_frame - 1);
  EXPECT_TRUE(before == nullptr || before->screen.blank());

  const auto* shown = assembler.screenAt(display_frame);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "HELLO");
  EXPECT_EQ(shown->frame, display_frame);
  EXPECT_EQ(shown->mode, orc::CaptionMode::POP_ON);
}

// The caption stays on screen until something takes it off, so every frame
// after it reports the same caption — which is what the previewer asks for as
// it plays through the seconds a caption is up for.
TEST(ClosedCaptionAssemblerTest, CaptionPersistsUntilErased) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(30);
  storePopOnCaption(assembler, 1, "HELLO");
  const uint64_t display_frame = popOnDisplayFrame(1, "HELLO");
  storePair(assembler, display_frame + 10, kCcControlByte,
            kCcEraseDisplayedMemory);

  const auto* held = assembler.screenAt(display_frame + 5);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->screen.text(), "HELLO");

  const auto* erased = assembler.screenAt(display_frame + 10);
  ASSERT_NE(erased, nullptr);
  EXPECT_TRUE(erased->screen.blank());
}

// Stepping back inside the decoded span answers from the history rather than
// re-decoding, so the caption a frame was showing is exactly what it showed.
TEST(ClosedCaptionAssemblerTest, SteppingBackwardsShowsTheEarlierCaption) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(40);
  storePopOnCaption(assembler, 1, "FIRST");
  const uint64_t first_frame = popOnDisplayFrame(1, "FIRST");
  storePopOnCaption(assembler, first_frame + 2, "SECOND");
  const uint64_t second_frame = popOnDisplayFrame(first_frame + 2, "SECOND");

  ASSERT_NE(assembler.screenAt(second_frame), nullptr);
  EXPECT_EQ(assembler.screenAt(second_frame)->screen.text(), "SECOND");

  // The previewer moves back; the window is unchanged, so nothing is re-read.
  assembler.setCurrentFrame(first_frame);
  EXPECT_TRUE(assembler.framesNeedingData().empty());
  ASSERT_NE(assembler.screenAt(first_frame), nullptr);
  EXPECT_EQ(assembler.screenAt(first_frame)->screen.text(), "FIRST");
}

// The transcript is the captions, not every change: the change that clears the
// screen ends a caption and is not one of its own.
TEST(ClosedCaptionAssemblerTest, CaptionsListSkipsScreenClears) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(40);
  storePopOnCaption(assembler, 1, "FIRST");
  const uint64_t first_frame = popOnDisplayFrame(1, "FIRST");
  storePair(assembler, first_frame + 1, kCcControlByte,
            kCcEraseDisplayedMemory);
  storePopOnCaption(assembler, first_frame + 2, "SECOND");

  const auto captions = assembler.captions();
  ASSERT_EQ(captions.size(), 2u);
  EXPECT_EQ(captions[0].screen.text(), "FIRST");
  EXPECT_EQ(captions[1].screen.text(), "SECOND");
}

// Roll-up captions are written straight to the display, so they appear as they
// arrive rather than waiting for an End of Caption code.
TEST(ClosedCaptionAssemblerTest, RollUpModeIsReportedWithTheCaption) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storePair(assembler, 1, kCcControlByte, kCcRollUp2);
  storePair(assembler, 2, kCcControlByte, kCcPacRow15Col0);
  storePair(assembler, 3, 'H', 'I');

  const auto* shown = assembler.screenAt(3);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "HI");
  EXPECT_EQ(shown->mode, orc::CaptionMode::ROLL_UP);
  EXPECT_EQ(shown->rollup_rows, 2);
}

// A pair neither byte of which passed its parity check is not trustworthy, and
// feeding it to the decoder would put a wrong character on screen or, worse,
// act on a control code that was never sent.
TEST(ClosedCaptionAssemblerTest, PairFailingBothParityChecksIsNotDecoded) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storeRollUpStart(assembler);
  fillGapsBefore(assembler, 3);
  assembler.storeFrame(3, makeDamagedField('H', 'I'), makeEmptyField());
  storePair(assembler, 4, 'O', 'K');

  const auto* shown = assembler.screenAt(4);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "OK");
}

// The caption sink's rule: a pair whose first byte failed its parity may have
// been a control code, so it is dropped whole even though the second byte
// passed — the encoder's second copy of a control code stands in for it.
TEST(ClosedCaptionAssemblerTest, PairWhoseFirstByteFailedIsNotDecoded) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storeRollUpStart(assembler);
  fillGapsBefore(assembler, 3);
  auto damaged = makeCaptionField('H', 'I');
  damaged.parity0_valid = false;
  assembler.storeFrame(3, damaged, makeEmptyField());
  storePair(assembler, 4, 'O', 'K');

  const auto* shown = assembler.screenAt(4);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "OK");
}

// A character whose partner failed its parity is shown alone, as a receiver
// shows the character it did receive.
TEST(ClosedCaptionAssemblerTest, CharacterWhosePartnerFailedIsShownAlone) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storeRollUpStart(assembler);
  fillGapsBefore(assembler, 3);
  auto damaged = makeCaptionField('H', 'I');
  damaged.parity1_valid = false;
  assembler.storeFrame(3, damaged, makeEmptyField());
  storePair(assembler, 4, 'O', 'K');

  const auto* shown = assembler.screenAt(4);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "HOK");
}

// A byte with no function on its field is ignored alone, its partner treated
// normally [47 CFR 15.119(i)(1)]: an Extended Data Service code on the first
// field, and a byte after a character that is neither a character nor a null
// — a text service can end a row with one.
TEST(ClosedCaptionAssemblerTest, ByteWithNoFunctionIsIgnoredAlone) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storeRollUpStart(assembler);
  storePair(assembler, 3, 'A', 0x01);
  storePair(assembler, 4, 0x05, 'B');
  storePair(assembler, 5, 'O', 'K');

  const auto* shown = assembler.screenAt(5);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "ABOK");
}

// A pair that is no code EIA-608 defines for its field is noise however its
// parity reads: row 11 has no preamble address code 60h [CTA-608-E Table 53],
// and taken as row 12's it would move the cursor up a row.
TEST(ClosedCaptionAssemblerTest, PairNoTableDefinesIsNotDecoded) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storeRollUpStart(assembler);
  storePair(assembler, 3, 0x10, 0x60);
  storePair(assembler, 4, 'O', 'K');

  const auto* shown = assembler.screenAt(4);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.rows[14], "OK");
}

// The special characters are shown as the characters they are: the music
// note round a song's words [CTA-608-E Table 49].
TEST(ClosedCaptionAssemblerTest, MusicNoteIsShown) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storeRollUpStart(assembler);
  storePair(assembler, 3, 0x11, 0x37);
  storePair(assembler, 4, 'L', 'A');

  const auto* shown = assembler.screenAt(4);
  ASSERT_NE(shown, nullptr);
  const std::string note = "\xE2\x99\xAA";  // U+266A
  EXPECT_EQ(shown->screen.text(), note + "LA");
}

// A recording whose first stored field is television field 2 carries its CC1
// on the second stored field, spelled as field 1 spells its control codes.
// Once the control codes say so, the run is decoded again the other way round
// — the captions that arrived before the decision included.
TEST(ClosedCaptionAssemblerTest, ReversedFieldsAreReadTheOtherWayRound) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(80);
  // Five captions of one length: a pop-on caption is written over what the
  // memory it is loaded into last held.
  const std::vector<std::string> texts = {"ONE", "TWO", "SIX", "TEN", "FEW"};
  uint64_t frame = 1;
  for (const std::string& text : texts) {
    storeSecondFieldPopOnCaption(assembler, frame, text);
    frame = popOnDisplayFrame(frame, text) + 2;
    // Decoded as it arrives: the first captions are decoded as the second
    // field's service until the control codes have shown the order.
    if (text == "ONE") {
      EXPECT_FALSE(assembler.fieldsReversed());
      EXPECT_TRUE(assembler.captions().empty());
    }
  }
  fillGapsBefore(assembler, 81);

  EXPECT_TRUE(assembler.fieldsReversed());
  const auto captions = assembler.captions();
  ASSERT_EQ(captions.size(), texts.size());
  for (size_t index = 0; index < texts.size(); ++index) {
    EXPECT_EQ(captions[index].screen.text(), texts[index]);
  }
}

// The same captions on the first stored field are the normal order, and no
// decision is made from a recording's first few control codes.
TEST(ClosedCaptionAssemblerTest, FieldsAreReadInStoredOrderByDefault) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(40);
  storeSecondFieldPopOnCaption(assembler, 1, "HELLO");
  fillGapsBefore(assembler, 41);
  EXPECT_FALSE(assembler.fieldsReversed());
  EXPECT_TRUE(assembler.captions().empty());

  assembler.clear();
  assembler.setCurrentFrame(40);
  storePopOnCaption(assembler, 1, "HELLO");
  EXPECT_FALSE(assembler.fieldsReversed());
  ASSERT_EQ(assembler.captions().size(), 1u);
  EXPECT_EQ(assembler.captions()[0].screen.text(), "HELLO");
}

// The recovered bytes are shown for the frame the previewer is on, so a viewer
// can tell a frame that carried nothing from one whose data did not decode.
TEST(ClosedCaptionAssemblerTest, FrameDataReportsTheRecoveredBytes) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(10);
  storePair(assembler, 4, kCcControlByte, kCcEndOfCaption);
  fillGapsBefore(assembler, 10);

  const auto* carried = assembler.frameData(4);
  ASSERT_NE(carried, nullptr);
  EXPECT_TRUE(carried->field1.present);
  EXPECT_EQ(carried->field1.data0, kCcControlByte);
  EXPECT_EQ(carried->field1.data1, kCcEndOfCaption);
  EXPECT_FALSE(carried->field2.present);

  const auto* empty = assembler.frameData(3);
  ASSERT_NE(empty, nullptr);
  EXPECT_FALSE(empty->field1.present);
}

// An unobservable frame is cached as one carrying nothing: it stays
// unobservable for this view node, and re-requesting it on every frame change
// would issue a whole window of reads per step. It also unblocks the decoder,
// which advances only over frames it has.
TEST(ClosedCaptionAssemblerTest, UnavailableFrameIsNotRequestedAgain) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(2);
  assembler.markFrameUnavailable(0);
  assembler.markFrameUnavailable(1);
  assembler.markFrameUnavailable(2);

  EXPECT_TRUE(assembler.framesNeedingData().empty());
  EXPECT_EQ(assembler.framesNeedingDataCount(), 0u);
  EXPECT_TRUE(assembler.hasFrame(1));
}

// A delivery the decoder has already run past cannot contribute — it is fed
// strictly forwards — and must not be mistaken for new data.
TEST(ClosedCaptionAssemblerTest, DeliveryForAPassedFrameIsIgnored) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(5);
  for (uint64_t frame = 0; frame <= 5; ++frame) {
    assembler.markFrameUnavailable(frame);
  }
  const uint64_t revision = assembler.historyRevision();

  assembler.storeFrame(2, makeCaptionField(kCcControlByte, kCcRollUp2),
                       makeEmptyField());

  EXPECT_EQ(assembler.historyRevision(), revision);
  EXPECT_TRUE(assembler.captions().empty());
}

// Jumping somewhere no part of the decode run describes starts again: a
// caption from elsewhere in the recording says nothing about where the
// previewer has landed.
TEST(ClosedCaptionAssemblerTest, SeekDiscardsTheHistory) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(40);
  storePopOnCaption(assembler, 1, "HELLO");
  ASSERT_EQ(assembler.captions().size(), 1u);

  assembler.setCurrentFrame(40 + kWindow * 3);

  EXPECT_TRUE(assembler.captions().empty());
  EXPECT_EQ(assembler.windowStartFrame(), 40 + kWindow * 3 - (kWindow - 1));
}

// Moving back past the start of the run lays it out again from further back;
// the decoder only goes forwards, so the frames in between have to be read
// again.
TEST(ClosedCaptionAssemblerTest, RewindRelaysTheDecodeRun) {
  ClosedCaptionAssembler assembler;
  // Land well into the recording, so the run has somewhere to be laid out
  // again from: playing forward never moves the anchor.
  assembler.setCurrentFrame(kWindow * 3);
  const uint64_t anchor_before = assembler.windowStartFrame();
  ASSERT_GT(anchor_before, 0u);
  for (uint64_t frame = anchor_before; frame <= kWindow * 3; ++frame) {
    assembler.markFrameUnavailable(frame);
  }
  ASSERT_TRUE(assembler.framesNeedingData().empty());

  assembler.setCurrentFrame(anchor_before - 1);

  EXPECT_EQ(assembler.windowStartFrame(), anchor_before - 1 - (kWindow - 1));
  EXPECT_FALSE(assembler.framesNeedingData().empty());
}

// Playback calls the readers on every frame; a frame that changes nothing must
// not make the dialog rebuild its transcript.
TEST(ClosedCaptionAssemblerTest, RevisionOnlyMovesWhenTheScreenChanges) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storePopOnCaption(assembler, 1, "HELLO");
  const uint64_t revision = assembler.historyRevision();

  storePair(assembler, 15, 0, 0);
  fillGapsBefore(assembler, 20);
  assembler.setCurrentFrame(21);

  EXPECT_EQ(assembler.historyRevision(), revision);
}

// A node or DAG change means different observations entirely.
// Issue #273: line 21 multiplexes four services onto one byte-pair stream, and
// decoding them together interleaves their text.
TEST(ClosedCaptionAssemblerTest, OnlyTheSelectedServiceReachesTheScreen) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(40);
  storeInterleavedServices(assembler);

  const auto* shown = assembler.screenAt(7);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "HI");
}

TEST(ClosedCaptionAssemblerTest, ChoosingATextServiceDecodesThatInstead) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(40);
  storeInterleavedServices(assembler);
  ASSERT_EQ(assembler.service(), orc::EIA608Service::CC1);

  // Nothing decoded under CC1 describes TEXT2, so the run is laid out again
  // and the window has to be delivered a second time.
  assembler.setService(orc::EIA608Service::T2);
  EXPECT_EQ(assembler.service(), orc::EIA608Service::T2);
  EXPECT_EQ(assembler.screenAt(7), nullptr);

  storeInterleavedServices(assembler);
  const auto* shown = assembler.screenAt(7);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->screen.text(), "42");
  EXPECT_EQ(shown->mode, orc::CaptionMode::TEXT);
}

TEST(ClosedCaptionAssemblerTest,
     SelectingTheServiceAlreadyInUseChangesNothing) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(40);
  storeInterleavedServices(assembler);
  const uint64_t revision = assembler.historyRevision();

  assembler.setService(orc::EIA608Service::CC1);

  EXPECT_EQ(assembler.historyRevision(), revision);
  ASSERT_NE(assembler.screenAt(7), nullptr);
  EXPECT_EQ(assembler.screenAt(7)->screen.text(), "HI");
}

TEST(ClosedCaptionAssemblerTest, ClearDropsEverything) {
  ClosedCaptionAssembler assembler;
  assembler.setCurrentFrame(20);
  storePopOnCaption(assembler, 1, "HELLO");
  ASSERT_FALSE(assembler.captions().empty());

  assembler.clear();

  EXPECT_TRUE(assembler.captions().empty());
  EXPECT_EQ(assembler.screenAt(20), nullptr);
  EXPECT_EQ(assembler.frameData(4), nullptr);
  EXPECT_FALSE(assembler.framesNeedingData().empty());
}

}  // namespace gui_unit_test
