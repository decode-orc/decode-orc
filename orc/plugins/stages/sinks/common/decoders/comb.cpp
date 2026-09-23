/*
 * File:        comb.cpp
 * Module:      orc-core
 * Purpose:     NTSC comb filter decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018 Chad Page
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2020-2021 Adam Sampson
 * SPDX-FileCopyrightText: 2021 Phillip Blucas
 */

#include "comb.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <cmath>

#include "deemp.h"
#include "firfilter.h"
#include "framecanvas.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <orc/support/logging.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "../video_parameter_safety.h"

// Indexes for the candidates considered in 3D adaptive mode
enum CandidateIndex : int32_t {
  CAND_LEFT,
  CAND_RIGHT,
  CAND_UP,
  CAND_DOWN,
  CAND_PREV_FIELD,
  CAND_NEXT_FIELD,
  CAND_PREV_FRAME,
  CAND_NEXT_FRAME,
  NUM_CANDIDATES
};

// Map colours for the candidates
static constexpr uint32_t CANDIDATE_SHADES[] = {
    0xFF8080,  // CAND_LEFT - red
    0xFF8080,  // CAND_RIGHT - red
    0xFFFF80,  // CAND_UP - yellow
    0xFFFF80,  // CAND_DOWN - yellow
    0x80FF80,  // CAND_PREV_FIELD - green
    0x80FF80,  // CAND_NEXT_FIELD - green
    0x8080FF,  // CAND_PREV_FRAME - blue
    0xFF80FF,  // CAND_NEXT_FRAME - purple
};

// Since we are at exactly 4fsc, calculating the value of a in-phase sine wave
// at a specific position is very simple.
static constexpr double sin4fsc_data[] = {1.0, 0.0, -1.0, 0.0};

// 4fsc sine wave
constexpr double sin4fsc(const int32_t i) { return sin4fsc_data[i % 4]; }

// 4fsc cos wave
constexpr double cos4fsc(const int32_t i) {
  // cos(rad) is just sin(rad + pi/2) and we are at 4 fsc.
  return sin4fsc(i + 1);
}

// Public methods
// -----------------------------------------------------------------------------------------------------

Comb::Comb() : configurationSet(false) {}

int32_t Comb::Configuration::getLookBehind() const {
  if (dimensions == 3) {
    // In 3D mode, we need to see the previous frame
    return 1;
  }

  return 0;
}

int32_t Comb::Configuration::getLookAhead() const {
  if (dimensions == 3) {
    // ... and also the next frame
    return 1;
  }

  return 0;
}

// Return the current configuration
const Comb::Configuration& Comb::getConfiguration() const {
  return configuration;
}

// Set the comb filter configuration parameters
void Comb::updateConfiguration(const ::orc::SourceParameters& _videoParameters,
                               const Comb::Configuration& _configuration) {
  configuration = _configuration;

  const auto safety = ::orc::chroma_sink::sanitize_video_parameters(
      _videoParameters, ::orc::chroma_sink::DecoderVideoProfile::NtscColour);

  if (!safety.warnings.empty()) {
    ORC_LOG_WARN(
        "Comb::updateConfiguration(): Adjusted unsafe video parameters: {}",
        ::orc::chroma_sink::join_issues(safety.warnings));
  }

  if (!safety.ok) {
    ORC_LOG_ERROR("Comb::updateConfiguration(): Invalid video parameters: {}",
                  ::orc::chroma_sink::join_issues(safety.errors));
    configurationSet = false;
    return;
  }

  videoParameters = safety.params;

  // Check the sample rate is close to 4 * fSC.
  // Older versions of ld-decode used integer approximations, so this needs
  // to be an approximate comparison.
  // Verify the sample rate is close to 4 × fsc for this video system.
  // Older ld-decode versions used integer approximations; allow a small margin.
  if (fabs((sample_rate_from_system(videoParameters.system) /
            fsc_from_system(videoParameters.system)) -
           4.0) > 1.0e-6) {
    ORC_LOG_WARN(
        "Data is not in 4fsc sample rate, color decoding will not work "
        "properly!");
  }

  configurationSet = true;
}

void Comb::decodeFrames(const std::vector<SourceField>& inputFields,
                        int32_t startIndex, int32_t endIndex,
                        std::vector<ComponentFrame>& componentFrames) {
  if (!configurationSet) {
    ORC_LOG_ERROR("Comb::decodeFrames(): Decoder configuration is invalid");
    return;
  }
  assert((componentFrames.size() * 2) == (endIndex - startIndex));

  // Check if we have YC sources (separate Y and C channels)
  // All fields must be the same type (YC or composite)
  bool is_yc_source = !inputFields.empty() && inputFields[0].is_yc;

  if (is_yc_source) {
    // YC DECODE PATH - Y is already clean, only demodulate C
    ORC_LOG_TRACE("Comb: Using YC decode path (separate Y/C channels)");
    decodeFramesYC(inputFields, startIndex, endIndex, componentFrames);
  } else {
    // COMPOSITE DECODE PATH - full comb filter for Y/C separation
    ORC_LOG_TRACE("Comb: Using composite decode path (Y+C modulated)");
    decodeFramesComposite(inputFields, startIndex, endIndex, componentFrames);
  }
}

// YC decode path - for sources with separate Y and C channels
void Comb::decodeFramesYC(const std::vector<SourceField>& inputFields,
                          int32_t startIndex, int32_t endIndex,
                          std::vector<ComponentFrame>& componentFrames) {
  // For YC sources:
  // - Y is already clean (no comb filtering needed)
  // - C only needs demodulation (no Y/C separation needed)
  // - This is MUCH simpler and faster than composite decode
  // - No 1D/2D/3D comb filtering on luma

  // We still need one frame buffer for current frame
  auto currentFrameBuffer =
      std::make_unique<FrameBuffer>(videoParameters, configuration);

  // Decode each pair of fields into a frame
  for (int32_t fieldIndex = startIndex; fieldIndex < endIndex;
       fieldIndex += 2) {
    const int32_t frameIndex = (fieldIndex - startIndex) / 2;

    // Load YC fields (separate Y and C) into the buffer
    currentFrameBuffer->loadFieldsYC(inputFields[fieldIndex],
                                     inputFields[fieldIndex + 1]);

    // Initialize and clear the component frame
    componentFrames[frameIndex].init(videoParameters);
    currentFrameBuffer->setComponentFrame(componentFrames[frameIndex]);

    // Y channel: Direct copy from luma_buffer (already clean!)
    // C channel: Demodulate to get I/Q (no comb filtering needed)
    if (configuration.phaseCompensation) {
      currentFrameBuffer->splitIQlocked_YC();
    } else {
      currentFrameBuffer->splitIQ_YC();
    }

    // Filter I/Q (same as composite)
    currentFrameBuffer->filterIQ();

    // Apply noise reduction
    currentFrameBuffer->doCNR();
    currentFrameBuffer->doYNR();

    // Transform I/Q to U/V (same as composite)
    currentFrameBuffer->transformIQ(configuration.chromaGain,
                                    configuration.chromaPhase);
  }
}

// Composite decode path - full comb filter for Y/C separation
void Comb::decodeFramesComposite(const std::vector<SourceField>& inputFields,
                                 int32_t startIndex, int32_t endIndex,
                                 std::vector<ComponentFrame>& componentFrames) {
  assert(configurationSet);
  assert((componentFrames.size() * 2) == (endIndex - startIndex));

  // Buffers for the next, current and previous frame.
  // Because we only need three of these, we allocate them upfront then
  // rotate the pointers below.
  auto nextFrameBuffer =
      std::make_unique<FrameBuffer>(videoParameters, configuration);
  auto currentFrameBuffer =
      std::make_unique<FrameBuffer>(videoParameters, configuration);
  auto previousFrameBuffer =
      std::make_unique<FrameBuffer>(videoParameters, configuration);

  // Decode each pair of fields into a frame.
  // To support 3D operation, where we need to see three input frames at a time,
  // each iteration of the loop loads and 1D/2D-filters frame N + 1, then
  // 3D-filters and outputs frame N.
  const int32_t preStartIndex =
      (configuration.dimensions == 3) ? startIndex - 4 : startIndex - 2;
  for (int32_t fieldIndex = preStartIndex; fieldIndex < endIndex;
       fieldIndex += 2) {
    const int32_t frameIndex = (fieldIndex - startIndex) / 2;

    // Rotate the buffers
    {
      auto recycle = std::move(previousFrameBuffer);
      previousFrameBuffer = std::move(currentFrameBuffer);
      currentFrameBuffer = std::move(nextFrameBuffer);
      nextFrameBuffer = std::move(recycle);
    }

    // If there's another input field, bring it into nextFrameBuffer
    if (fieldIndex + 3 >= 0 &&
        fieldIndex + 3 < static_cast<int32_t>(inputFields.size())) {
      // Load fields into the buffer
      nextFrameBuffer->loadFields(inputFields[fieldIndex + 2],
                                  inputFields[fieldIndex + 3]);

      // Extract chroma using 1D filter
      nextFrameBuffer->split1D();

      // Extract chroma using 2D filter
      nextFrameBuffer->split2D();
    }

    if (fieldIndex < startIndex) {
      // This is a look-behind frame; no further decoding needed.
      continue;
    }

    if (configuration.dimensions == 3) {
      // Extract chroma using 3D filter
      currentFrameBuffer->split3D(*previousFrameBuffer, *nextFrameBuffer);
    }

    // Initialise and clear the component frame
    componentFrames[frameIndex].init(videoParameters);
    currentFrameBuffer->setComponentFrame(componentFrames[frameIndex]);

    // Demodulate chroma giving I/Q
    if (configuration.phaseCompensation) {
      currentFrameBuffer->splitIQlocked();
    } else {
      currentFrameBuffer->splitIQ();
      // Extract Y from baseband and I/Q
      currentFrameBuffer->adjustY();
    }
    currentFrameBuffer->filterIQ();

    // Apply noise reduction
    currentFrameBuffer->doCNR();
    currentFrameBuffer->doYNR();

    // Transform I/Q to U/V
    currentFrameBuffer->transformIQ(configuration.chromaGain,
                                    configuration.chromaPhase);

    // Overlay the map if required
    if (configuration.dimensions == 3 && configuration.showMap) {
      currentFrameBuffer->overlayMap(*previousFrameBuffer, *nextFrameBuffer);
    }
  }
}

// Private methods
// ----------------------------------------------------------------------------------------------------

Comb::FrameBuffer::FrameBuffer(const ::orc::SourceParameters& videoParameters_,
                               const Configuration& configuration_)
    : videoParameters(videoParameters_), configuration(configuration_) {
  // Set the frame height
  frameHeight = static_cast<int32_t>(
                    calculate_padded_field_height(videoParameters.system)) *
                    2 -
                1;

  // SMPTE 244M-2003: NTSC/PAL-M CVBS_U10_4FSC 10-bit domain.
  // Scale: samples per 1 IRE = (white - blanking) / 100.
  irescale = static_cast<double>(orc::kNtscWhite - orc::kNtscBlanking) / 100.0;
}

/*
 * The color burst frequency is 227.5 cycles per line, so it flips 180 degrees
 * for each line.
 *
 * The color burst *signal* is at 180 degrees, which is a greenish yellow.
 *
 * When SCH phase is 0 (properly aligned) the color burst is in phase with the
 * leading edge of the HSYNC pulse.
 *
 * Per RS-170 note 6, Fields 1 and 4 have positive/rising burst phase at that
 * point on even (1-based!) lines. The color burst signal should begin exactly
 * 19 cycles later.
 *
 * getLinePhase returns true if the color burst is rising at the leading edge.
 */

inline int32_t Comb::FrameBuffer::getFieldID(int32_t lineNumber) const {
  bool isFirstField = ((lineNumber % 2) == 0);

  return isFirstField ? firstFieldPhaseID : secondFieldPhaseID;
}

// NOTE:  lineNumber is presumed to be starting at 1.  (This lines up with how
// splitIQ calls it)
inline bool Comb::FrameBuffer::getLinePhase(int32_t lineNumber) const {
  int32_t fieldID = getFieldID(lineNumber);
  bool isPositivePhaseOnEvenLines = (fieldID == 1) || (fieldID == 4);

  int fieldLine = (lineNumber / 2);
  bool isEvenLine = (fieldLine % 2) == 0;

  return isEvenLine ? isPositivePhaseOnEvenLines : !isPositivePhaseOnEvenLines;
}

// Interlace two source fields into the framebuffer.
void Comb::FrameBuffer::loadFields(const SourceField& firstField,
                                   const SourceField& secondField) {
  // Interlace the input fields and place in the frame buffer.
  // SourceField is a non-owning view into the VFrameR buffer; we copy
  // field_width samples per line so rawbuffer retains uniform stride.
  rawbuffer.clear();
  rawbuffer.reserve(static_cast<size_t>(frameHeight) *
                    static_cast<size_t>(videoParameters.frame_width_nominal));

  auto appendLineOrBlack = [&](const SourceField& field, size_t sourceLine) {
    if (sourceLine < field.line_count) {
      const int16_t* line = field.getLine(sourceLine);
      rawbuffer.insert(rawbuffer.end(), line,
                       line + videoParameters.frame_width_nominal);
    } else {
      rawbuffer.insert(rawbuffer.end(), videoParameters.frame_width_nominal,
                       int16_t{0});
    }
  };

  for (int32_t fieldLine = 0;
       fieldLine < static_cast<int32_t>(
                       calculate_padded_field_height(videoParameters.system));
       fieldLine++) {
    appendLineOrBlack(firstField, static_cast<size_t>(fieldLine));

    if ((fieldLine * 2) + 1 < frameHeight) {
      appendLineOrBlack(secondField, static_cast<size_t>(fieldLine));
    }
  }

  // Set the phase IDs for the frame
  firstFieldPhaseID = firstField.field_phase_id.value_or(-1);
  secondFieldPhaseID = secondField.field_phase_id.value_or(-1);

  // Clear clpbuffer
  for (int32_t buf = 0; buf < 3; buf++) {
    for (int32_t y = 0; y < MAX_HEIGHT; y++) {
      for (int32_t x = 0; x < MAX_WIDTH; x++) {
        clpbuffer[buf].pixel[y][x] = 0.0;
      }
    }
  }

  // No component frame yet
  componentFrame = nullptr;
  is_yc = false;
}

// Interlace two YC source fields into separate Y and C framebuffers.
// For YC sources, Y and C are already separated, so no comb filtering needed
// on Y.
void Comb::FrameBuffer::loadFieldsYC(const SourceField& firstField,
                                     const SourceField& secondField) {
  // Interlace the Y fields into luma_buffer.
  luma_buffer.clear();
  luma_buffer.reserve(static_cast<size_t>(frameHeight) *
                      static_cast<size_t>(videoParameters.frame_width_nominal));

  auto appendLumaLineOrBlack = [&](const SourceField& field,
                                   size_t sourceLine) {
    if (sourceLine < field.line_count) {
      const int16_t* line = field.getLumaLine(sourceLine);
      luma_buffer.insert(luma_buffer.end(), line,
                         line + videoParameters.frame_width_nominal);
    } else {
      luma_buffer.insert(luma_buffer.end(), videoParameters.frame_width_nominal,
                         int16_t{0});
    }
  };

  for (int32_t fieldLine = 0;
       fieldLine < static_cast<int32_t>(
                       calculate_padded_field_height(videoParameters.system));
       fieldLine++) {
    appendLumaLineOrBlack(firstField, static_cast<size_t>(fieldLine));

    if ((fieldLine * 2) + 1 < frameHeight) {
      appendLumaLineOrBlack(secondField, static_cast<size_t>(fieldLine));
    }
  }

  // Interlace the C fields into chroma_buffer.
  chroma_buffer.clear();
  chroma_buffer.reserve(
      static_cast<size_t>(frameHeight) *
      static_cast<size_t>(videoParameters.frame_width_nominal));

  auto appendChromaLineOrBlack = [&](const SourceField& field,
                                     size_t sourceLine) {
    if (sourceLine < field.line_count) {
      const int16_t* line = field.getChromaLine(sourceLine);
      chroma_buffer.insert(chroma_buffer.end(), line,
                           line + videoParameters.frame_width_nominal);
    } else {
      chroma_buffer.insert(chroma_buffer.end(),
                           videoParameters.frame_width_nominal, int16_t{0});
    }
  };

  for (int32_t fieldLine = 0;
       fieldLine < static_cast<int32_t>(
                       calculate_padded_field_height(videoParameters.system));
       fieldLine++) {
    appendChromaLineOrBlack(firstField, static_cast<size_t>(fieldLine));

    if ((fieldLine * 2) + 1 < frameHeight) {
      appendChromaLineOrBlack(secondField, static_cast<size_t>(fieldLine));
    }
  }

  // Set the phase IDs for the frame
  firstFieldPhaseID = firstField.field_phase_id.value_or(-1);
  secondFieldPhaseID = secondField.field_phase_id.value_or(-1);

  // Clear clpbuffer (not used for YC, but clear anyway for consistency)
  for (int32_t buf = 0; buf < 3; buf++) {
    for (int32_t y = 0; y < MAX_HEIGHT; y++) {
      for (int32_t x = 0; x < MAX_WIDTH; x++) {
        clpbuffer[buf].pixel[y][x] = 0.0;
      }
    }
  }

  // No component frame yet
  componentFrame = nullptr;
  is_yc = true;
}

// Extract chroma into clpbuffer[0] using a 1D bandpass filter.
//
// The filter is [-0.25, 0, 0.5, 0, -0.25], a gentle bandpass centred on fSC.
// So the output will contain all of the chroma signal, but also whatever luma
// components ended up in the same frequency range.
//
// This also acts as an alias removal pre-filter for the quadrature detector in
// splitIQ, so we use its result for split2D rather than the raw signal.
void Comb::FrameBuffer::split1D() {
  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    // Get a pointer to the line's data
    const int16_t* line =
        rawbuffer.data() +
        (static_cast<ptrdiff_t>(lineNumber *
                                videoParameters.frame_width_nominal));

    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      double tc1 = (line[h] - ((line[h - 2] + line[h + 2]) / 2.0)) / 2.0;

      // Record the 1D C value
      clpbuffer[0].pixel[lineNumber][h] = tc1;
    }
  }
}

// Extract chroma into clpbuffer[1] using a 2D 3-line adaptive filter.
//
// Because the phase of the chroma signal changes by 180 degrees from line to
// line, subtracting two adjacent lines that contain the same information will
// give you just the chroma signal. But real images don't necessarily contain
// the same information on every line.
//
// The "3-line adaptive" part means that we look at both surrounding lines to
// estimate how similar they are to this one. We can then compute the 2D chroma
// value as a blend of the two differences, weighted by similarity.
void Comb::FrameBuffer::split2D() {
  // Dummy black line
  static constexpr double blackLine[MAX_WIDTH] = {0};

  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    // Get pointers to the surrounding lines of 1D chroma.
    // If a line we need is outside the active area, use blackLine instead.
    const double* previousLine = blackLine;
    if (lineNumber - 2 >= videoParameters.first_active_frame_line) {
      previousLine = clpbuffer[0].pixel[lineNumber - 2];
    }
    const double* currentLine = clpbuffer[0].pixel[lineNumber];
    const double* nextLine = blackLine;
    if (lineNumber + 2 < videoParameters.last_active_frame_line) {
      nextLine = clpbuffer[0].pixel[lineNumber + 2];
    }

    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      double kp, kn;

      // Summing the differences of the *absolute* values of the 1D chroma
      // samples will give us a low value if the two lines are nearly in phase
      // (strong Y) or nearly 180 degrees out of phase (strong C) -- i.e. the
      // two cases where the 2D filter is probably usable. Also give a small
      // bonus if there's a large signal (we think).
      kp = fabs(fabs(currentLine[h]) - fabs(previousLine[h]));
      kp += fabs(fabs(currentLine[h - 1]) - fabs(previousLine[h - 1]));
      kp -= (fabs(currentLine[h]) + fabs(previousLine[h - 1])) * .10;
      kn = fabs(fabs(currentLine[h]) - fabs(nextLine[h]));
      kn += fabs(fabs(currentLine[h - 1]) - fabs(nextLine[h - 1]));
      kn -= (fabs(currentLine[h]) + fabs(nextLine[h - 1])) * .10;

      // Map the difference into a weighting 0-1.
      // 1 means in phase or unknown; 0 means out of phase (more than kRange
      // difference).
      // 45 IRE is an empirically tuned threshold with no basis in any NTSC
      // normative specification (SMPTE 170M-2004 / SMPTE 244M-2003).
      const double kRange = 45 * irescale;
      kp = std::clamp(1 - (kp / kRange), 0.0, 1.0);
      kn = std::clamp(1 - (kn / kRange), 0.0, 1.0);

      double sc = 1.0;

      if ((kn > 0) || (kp > 0)) {
        // At least one of the next/previous lines has a good phase
        // relationship.

        // If one of them is much better than the other, only use that one
        if (kn > (3 * kp)) {
          kp = 0;
        } else if (kp > (3 * kn)) {
          kn = 0;
        }

        sc = (2.0 / (kn + kp));
        if (sc < 1.0) sc = 1.0;
      } else {
        // Neither line has a good phase relationship.

        // But are they similar to each other? If so, we can use both of them!
        if ((fabs(fabs(previousLine[h]) - fabs(nextLine[h])) -
             fabs((nextLine[h] + previousLine[h]) * .2)) <= 0) {
          kn = kp = 1;
        }

        // Else kn = kp = 0, so we won't extract any chroma for this sample.
        // (Some NTSC decoders fall back to the 1D chroma in this situation.)
      }

      // Compute the weighted sum of differences, giving the 2D chroma value
      double tc1;
      tc1 = ((currentLine[h] - previousLine[h]) * kp * sc);
      tc1 += ((currentLine[h] - nextLine[h]) * kn * sc);
      tc1 /= 4;

      clpbuffer[1].pixel[lineNumber][h] = tc1;
    }
  }
}

// Extract chroma into clpbuffer[2] using an adaptive 3D filter.
//
// For each sample, this builds a list of candidates from other positions that
// should have a 180 degree phase relationship to the current sample, and look
// like they have similar luma/chroma content. It then picks the most similar
// candidate.
void Comb::FrameBuffer::split3D(const FrameBuffer& previousFrame,
                                const FrameBuffer& nextFrame) {
  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      // Select the best candidate
      int32_t bestIndex;
      double bestSample;
      getBestCandidate(lineNumber, h, previousFrame, nextFrame, bestIndex,
                       bestSample);

      if (bestIndex < CAND_PREV_FIELD) {
        // A 1D or 2D candidate was best.
        // Use split2D's output, to save duplicating the line-blending
        // heuristics here.
        clpbuffer[2].pixel[lineNumber][h] = clpbuffer[1].pixel[lineNumber][h];
      } else {
        // Compute a 3D result.
        // This sample is Y + C; the candidate is (ideally) Y - C. So compute C
        // as ((Y + C) - (Y - C)) / 2.
        clpbuffer[2].pixel[lineNumber][h] =
            (clpbuffer[0].pixel[lineNumber][h] - bestSample) / 2;
      }
    }
  }
}

// Evaluate all candidates for 3D decoding for a given position, and return the
// best one
void Comb::FrameBuffer::getBestCandidate(int32_t lineNumber, int32_t h,
                                         const FrameBuffer& previousFrame,
                                         const FrameBuffer& nextFrame,
                                         int32_t& bestIndex,
                                         double& bestSample) const {
  Candidate candidates[8];

  // Candidate selection bias weights. Higher adaptThreshold strengthens
  // preference for 2D/3D temporal candidates over 1D spatial candidates.
  // The multipliers (2×, 4×, 6× adaptThreshold) are empirically tuned
  // implementation values with no basis in any NTSC normative specification
  // (SMPTE 170M-2004 / SMPTE 244M-2003).
  const double LINE_BONUS = -2.0 * configuration.adaptThreshold;
  const double FIELD_BONUS = LINE_BONUS - (2.0 * configuration.adaptThreshold);
  const double FRAME_BONUS = FIELD_BONUS - (2.0 * configuration.adaptThreshold);

  // 1D: Same line, 2 samples left and right
  candidates[CAND_LEFT] =
      getCandidate(lineNumber, h, *this, lineNumber, h - 2, 0);
  candidates[CAND_RIGHT] =
      getCandidate(lineNumber, h, *this, lineNumber, h + 2, 0);

  // 2D: Same field, 1 line up and down
  candidates[CAND_UP] =
      getCandidate(lineNumber, h, *this, lineNumber - 2, h, LINE_BONUS);
  candidates[CAND_DOWN] =
      getCandidate(lineNumber, h, *this, lineNumber + 2, h, LINE_BONUS);

  // Immediately adjacent lines in previous/next field
  if (getLinePhase(lineNumber) == getLinePhase(lineNumber - 1)) {
    candidates[CAND_PREV_FIELD] = getCandidate(lineNumber, h, previousFrame,
                                               lineNumber - 1, h, FIELD_BONUS);
    candidates[CAND_NEXT_FIELD] =
        getCandidate(lineNumber, h, *this, lineNumber + 1, h, FIELD_BONUS);
  } else {
    candidates[CAND_PREV_FIELD] =
        getCandidate(lineNumber, h, *this, lineNumber - 1, h, FIELD_BONUS);
    candidates[CAND_NEXT_FIELD] =
        getCandidate(lineNumber, h, nextFrame, lineNumber + 1, h, FIELD_BONUS);
  }

  // Previous/next frame, same position
  candidates[CAND_PREV_FRAME] =
      getCandidate(lineNumber, h, previousFrame, lineNumber, h, FRAME_BONUS);
  candidates[CAND_NEXT_FRAME] =
      getCandidate(lineNumber, h, nextFrame, lineNumber, h, FRAME_BONUS);

  if (configuration.adaptive) {
    // Find the candidate with the lowest penalty
    bestIndex = 0;
    for (int32_t i = 1; i < NUM_CANDIDATES; i++) {
      if (candidates[i].penalty < candidates[bestIndex].penalty) bestIndex = i;
    }
  } else {
    // Adaptive mode is disabled - do 3D against the previous frame
    bestIndex = CAND_PREV_FRAME;
  }

  bestSample = candidates[bestIndex].sample;
}

// Evaluate a candidate for 3D decoding
Comb::FrameBuffer::Candidate Comb::FrameBuffer::getCandidate(
    int32_t refLineNumber, int32_t refH, const FrameBuffer& frameBuffer,
    int32_t lineNumber, int32_t h, double adjustPenalty) const {
  Candidate result;
  result.sample = frameBuffer.clpbuffer[0].pixel[lineNumber][h];

  // If the candidate is outside the active region (vertically), it's not viable
  if (lineNumber < videoParameters.first_active_frame_line ||
      lineNumber >= videoParameters.last_active_frame_line) {
    result.penalty = 1000.0;
    return result;
  }

  // The target sample should have 180 degrees phase difference from the
  // reference. If it doesn't (e.g. because it's a blank frame or the player
  // skipped), it's not viable.
  const int32_t wantPhase =
      (2 + (getLinePhase(refLineNumber) ? 2 : 0) + refH) % 4;
  const int32_t havePhase =
      ((frameBuffer.getLinePhase(lineNumber) ? 2 : 0) + h) % 4;
  if (wantPhase != havePhase) {
    result.penalty = 1000.0;
    return result;
  }

  // Pointers to the baseband data
  const int16_t* refLine =
      rawbuffer.data() +
      (static_cast<ptrdiff_t>(refLineNumber *
                              videoParameters.frame_width_nominal));
  const int16_t* candidateLine =
      frameBuffer.rawbuffer.data() +
      (static_cast<ptrdiff_t>(lineNumber *
                              videoParameters.frame_width_nominal));

  // Penalty based on mean luma difference in IRE over surrounding three samples
  double yPenalty = 0.0;
  for (int32_t offset = -1; offset < 2; offset++) {
    const double refC = clpbuffer[1].pixel[refLineNumber][refH + offset];
    const double refY = refLine[refH + offset] - refC;

    const double candidateC =
        frameBuffer.clpbuffer[1].pixel[lineNumber][h + offset];
    const double candidateY = candidateLine[h + offset] - candidateC;

    yPenalty += fabs(refY - candidateY);
  }
  yPenalty = yPenalty / 3 / irescale;

  // Penalty based on mean I/Q difference in IRE over surrounding three samples
  double iqPenalty = 0.0;
  for (int32_t offset = -1; offset < 2; offset++) {
    // The reference and candidate are 180 degrees out of phase here, so negate
    // one
    const double refC = clpbuffer[1].pixel[refLineNumber][refH + offset];
    const double candidateC =
        -frameBuffer.clpbuffer[1].pixel[lineNumber][h + offset];

    // I and Q samples alternate, so weight the two channels equally
    static constexpr double weights[] = {0.5, 1.0, 0.5};
    iqPenalty += fabs(refC - candidateC) * weights[offset + 1];
  }
  // Weaken chroma penalty relative to luma to prevent spurious colour in the
  // 2D result from biasing 3D candidate selection. The 0.28 factor is an
  // empirically tuned implementation value with no basis in any NTSC normative
  // specification (SMPTE 170M-2004 / SMPTE 244M-2003).
  iqPenalty = (iqPenalty / 2 / irescale) * 0.28;

  result.penalty =
      yPenalty + iqPenalty + adjustPenalty * configuration.chromaWeight;
  return result;
}

namespace {
// SMPTE 170M-2004 §6.1: I-axis is rotated 33° from the U-axis in NTSC I/Q
// colour space. Pre-computed to avoid per-pixel trigonometry.
constexpr double ROTATE_SIN = 0.5446390350150271;  // sin(33° × π/180)
constexpr double ROTATE_COS = 0.838670567945424;   // cos(33° × π/180)

// SMPTE 170M-2004 Table 1: nominal NTSC burst amplitude = 40 ± 1 IRE p-p,
// giving a minimum of 39 IRE p-p (half-amplitude ≥ 19.5 IRE at minimum).
// At exactly 4FSC, product detection over a burst window of N samples yields
// burstNorm ≈ half_amplitude / 2 after averaging (half the sin4fsc terms are
// non-zero).  Minimum burstNorm in the CVBS_U10_4FSC 10-bit domain:
//   19.5 IRE × (kNtscWhite − kNtscBlanking) / 100 / 2  ≈  54.6 units.
// The floor is set to 1/8 of this minimum (≈ 6.83) to prevent divide-by-zero
// on burst-free lines (blanking, VBI) while remaining well below any real
// burst.
constexpr double kNtscBurstNormFloor =
    (19.5 * static_cast<double>(orc::kNtscWhite - orc::kNtscBlanking) / 100.0) /
    2.0 / 8.0;

Comb::BurstInfo detectBurst(const int16_t* lineData,
                            const ::orc::SourceParameters& videoParameters) {
  double bsin = 0, bcos = 0;

  // Find absolute burst phase relative to the reference carrier by
  // product detection.
  // For now we just use the burst on the current line, but we could possibly do
  // some averaging with neighbouring lines later if needed.
  const auto [comb_burst_start, comb_burst_end] =
      colour_burst_range(videoParameters.system);
  if (comb_burst_end > videoParameters.frame_width_nominal) {
    return Comb::BurstInfo{0.0, 0.0};
  }
  for (int32_t i = comb_burst_start; i < comb_burst_end; i++) {
    bsin += lineData[i] * sin4fsc(i);
    bcos += lineData[i] * cos4fsc(i);
  }

  // Normalise the sums: burstNorm ≈ half_amplitude / 2.
  // The floor prevents divide-by-zero on lines without a burst signal.
  // SMPTE 170M-2004 Table 1: nominal burst gives burstNorm ≈ 56 in the
  // CVBS_U10_4FSC 10-bit domain; kNtscBurstNormFloor is well below that.
  const int32_t colourBurstLength = comb_burst_end - comb_burst_start;
  bsin /= colourBurstLength;
  bcos /= colourBurstLength;

  const double burstNorm =
      std::max(sqrt(bsin * bsin + bcos * bcos), kNtscBurstNormFloor);

  bsin /= burstNorm;
  bcos /= burstNorm;

  const Comb::BurstInfo info{bsin, bcos};
  return info;
}
}  // namespace

// Helper: Demodulate chroma with phase compensation (shared code between
// composite and YC)
void Comb::FrameBuffer::demodulateChromaLocked(const double* chromaLine,
                                               int32_t lineNumber,
                                               const Comb::BurstInfo& burstInfo,
                                               double* I, double* Q,
                                               int32_t xOffset) {
  const int32_t outputWidth = videoParameters.frame_width_nominal;

  // SMPTE 170M-2004 §9: Y'/chroma co-siting tolerance is ±25 ns (≈ ±0.36
  // sample at 4fsc). Demodulated I/Q is written at the same sample position
  // as the input chroma to maintain co-siting within spec. The output index
  // is h - xOffset, so the samples that land inside [0, outputWidth) are the
  // range below; settling it here rather than testing every sample keeps the
  // loop free of control flow, which is what lets it vectorise.
  const int32_t hStart = std::max(videoParameters.active_video_start, xOffset);
  const int32_t hEnd =
      std::min(videoParameters.active_video_end, xOffset + outputWidth);
  for (int32_t h = hStart; h < hEnd; h++) {
    const double cval = chromaLine[h];

    // Demodulate the sine and cosine components. At exactly 4fsc the
    // reference carrier is the four-sample pattern sin = {1, 0, -1, 0} and
    // cos = {0, -1, 0, 1}, written as selects rather than table lookups so the
    // loop stays vectorisable; the products are the same exact values.
    const int32_t phase = h & 3;
    const double s = (phase & 1) ? 0.0 : ((phase & 2) ? -1.0 : 1.0);
    const double c = (phase & 1) ? ((phase & 2) ? 1.0 : -1.0) : 0.0;
    const auto lsin = cval * s * 2;
    const auto lcos = cval * c * 2;
    // Rotate the demodulated vector by the burst phase
    const auto ti = (lsin * burstInfo.bcos - lcos * burstInfo.bsin);
    const auto tq = (lsin * burstInfo.bsin + lcos * burstInfo.bcos);

    // Invert Q and rotate to get the correct I/Q vector.
    const int32_t outIndex = h - xOffset;
    I[outIndex] = ti * ROTATE_COS - tq * -ROTATE_SIN;
    Q[outIndex] = -(ti * -ROTATE_SIN + tq * ROTATE_COS);
  }
}

// Helper: Demodulate chroma without phase compensation (shared code between
// composite and YC)
void Comb::FrameBuffer::demodulateChroma(const double* chromaLine,
                                         int32_t lineNumber, bool linePhase,
                                         double* I, double* Q,
                                         int32_t xOffset) {
  double si = 0, sq = 0;
  for (int32_t h = videoParameters.active_video_start;
       h < videoParameters.active_video_end; h++) {
    int32_t phase = h % 4;

    double cavg = chromaLine[h];

    if (linePhase) cavg = -cavg;

    switch (phase) {
      case 0:
        sq = cavg;
        break;
      case 1:
        si = -cavg;
        break;
      case 2:
        sq = -cavg;
        break;
      case 3:
        si = cavg;
        break;
      default:
        break;
    }

    I[h - xOffset] = si;
    Q[h - xOffset] = sq;
  }
}

// Split I and Q, taking burst phase into account.
void Comb::FrameBuffer::splitIQlocked() {
  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? videoParameters.active_video_start
                              : 0;

  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    // Bounds check to ensure lineNumber is valid for clpbuffer access
    if (lineNumber >= MAX_HEIGHT) {
      continue;  // Skip lines outside the maximum buffer size
    }

    // Get a pointer to the line's data
    const int16_t* line =
        rawbuffer.data() +
        (static_cast<ptrdiff_t>(lineNumber *
                                videoParameters.frame_width_nominal));
    // Calculate burst phase
    const auto info = detectBurst(line, videoParameters);

    double* Y = componentFrame->y(lineNumber - lineOffset);
    double* I = componentFrame->u(lineNumber - lineOffset);
    double* Q = componentFrame->v(lineNumber - lineOffset);

    // Build chroma line buffer from comb-filtered chroma
    std::vector<double> chromaLine(videoParameters.frame_width_nominal, 0.0);
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      // Bounds check for both dimensions
      if (h < videoParameters.frame_width_nominal) {
        const auto val =
            clpbuffer[configuration.dimensions - 1].pixel[lineNumber][h];
        chromaLine[h] = val;  // Store as double to preserve precision
        // Subtract the split chroma part from the luma signal
        Y[h - xOffset] = line[h] - val;
      }
    }

    // Demodulate chroma to I/Q using shared helper
    demodulateChromaLocked(chromaLine.data(), lineNumber, info, I, Q, xOffset);
  }
}

// Spilt the I and Q
void Comb::FrameBuffer::splitIQ() {
  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? videoParameters.active_video_start
                              : 0;

  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    // Bounds check to ensure lineNumber is valid for clpbuffer access
    if (lineNumber >= MAX_HEIGHT) {
      continue;  // Skip lines outside the maximum buffer size
    }

    // Get a pointer to the line's data
    const int16_t* line =
        rawbuffer.data() +
        (static_cast<ptrdiff_t>(lineNumber *
                                videoParameters.frame_width_nominal));

    double* Y = componentFrame->y(lineNumber - lineOffset);
    double* I = componentFrame->u(lineNumber - lineOffset);
    double* Q = componentFrame->v(lineNumber - lineOffset);

    bool linePhase = getLinePhase(lineNumber);

    // Copy Y from composite (will be adjusted later in adjustY)
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      Y[h - xOffset] = line[h];
    }

    // Build chroma line buffer from comb-filtered chroma
    std::vector<double> chromaLine(videoParameters.frame_width_nominal);
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      // Bounds check for both dimensions
      if (h < videoParameters.frame_width_nominal) {
        chromaLine[h] =
            clpbuffer[configuration.dimensions - 1]
                .pixel[lineNumber][h];  // Store as double to preserve precision
      }
    }

    // Demodulate chroma to I/Q using shared helper
    demodulateChroma(chromaLine.data(), lineNumber, linePhase, I, Q, xOffset);
  }
}

// Filter the IQ from the component frame
void Comb::FrameBuffer::filterIQ() {
  auto iqFilter = makeFIRFilter(c_colorlp_b);

  // Temporary output buffer for the filter
  const int width =
      videoParameters.active_video_end - videoParameters.active_video_start;
  std::vector<double> tempBuf(width);

  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? 0
                              : videoParameters.active_video_start;

  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    double* I = componentFrame->u(lineNumber - lineOffset) + xOffset;
    double* Q = componentFrame->v(lineNumber - lineOffset) + xOffset;

    // Apply filter to I
    iqFilter.apply(I, tempBuf.data(), width);
    std::copy(tempBuf.begin(), tempBuf.end(), I);

    // Apply filter to Q
    iqFilter.apply(Q, tempBuf.data(), width);
    std::copy(tempBuf.begin(), tempBuf.end(), Q);
  }
}

// Remove the colour data from the baseband (Y)
void Comb::FrameBuffer::adjustY() {
  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? videoParameters.active_video_start
                              : 0;

  // remove color data from baseband (Y)
  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    double* Y = componentFrame->y(lineNumber - lineOffset);
    double* I = componentFrame->u(lineNumber - lineOffset);
    double* Q = componentFrame->v(lineNumber - lineOffset);

    // The line phase flips the sign of every sample on the line; ±1.0 is
    // exact, so multiplying by it is the same as negating.
    const double lineSign = getLinePhase(lineNumber) ? 1.0 : -1.0;

    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      const int32_t x = h - xOffset;
      // The modulated chroma at sample phase 0..3 of the 4fsc carrier is
      // -Q, +I, +Q, -I. Selects rather than a switch, so the loop vectorises;
      // both channels are read unconditionally because both are needed on
      // alternate samples anyway.
      const int32_t phase = h & 3;
      const double i = I[x];
      const double q = Q[x];
      const double src = (phase & 1) ? i : q;
      const double comp = (phase == 1 || phase == 2) ? src : -src;

      Y[x] -= lineSign * comp;
    }
  }
}

// YC-specific: Demodulate chroma with phase compensation
// Y is already clean in luma_buffer, C only needs demodulation
void Comb::FrameBuffer::splitIQlocked_YC() {
  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? videoParameters.active_video_start
                              : 0;

  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    // Get pointers to Y and C lines
    const int16_t* yLine =
        luma_buffer.data() +
        (static_cast<ptrdiff_t>(lineNumber *
                                videoParameters.frame_width_nominal));
    const int16_t* cLine =
        chroma_buffer.data() +
        (static_cast<ptrdiff_t>(lineNumber *
                                videoParameters.frame_width_nominal));

    // Calculate burst phase from C channel
    const auto info = detectBurst(cLine, videoParameters);

    double* Y = componentFrame->y(lineNumber - lineOffset);
    double* I = componentFrame->u(lineNumber - lineOffset);
    double* Q = componentFrame->v(lineNumber - lineOffset);

    // Y is clean - direct copy from luma_buffer
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      Y[h - xOffset] = yLine[h];
    }

    // Build chroma line buffer from YC chroma data (convert to double)
    std::vector<double> chromaLine(videoParameters.frame_width_nominal, 0.0);
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      chromaLine[h] = static_cast<double>(cLine[h]);
    }

    // Demodulate chroma to I/Q using shared helper
    demodulateChromaLocked(chromaLine.data(), lineNumber, info, I, Q, xOffset);
  }
}

// YC-specific: Demodulate chroma without phase compensation
// Y is already clean in luma_buffer, C only needs demodulation
void Comb::FrameBuffer::splitIQ_YC() {
  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? videoParameters.active_video_start
                              : 0;

  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    // Get pointers to Y and C lines
    const int16_t* yLine =
        luma_buffer.data() +
        (static_cast<ptrdiff_t>(lineNumber *
                                videoParameters.frame_width_nominal));
    const int16_t* cLine =
        chroma_buffer.data() +
        (static_cast<ptrdiff_t>(lineNumber *
                                videoParameters.frame_width_nominal));

    double* Y = componentFrame->y(lineNumber - lineOffset);
    double* I = componentFrame->u(lineNumber - lineOffset);
    double* Q = componentFrame->v(lineNumber - lineOffset);

    bool linePhase = getLinePhase(lineNumber);

    // Y is clean - direct copy from luma_buffer
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      Y[h - xOffset] = yLine[h];
    }

    // Build chroma line buffer from YC chroma data (convert to double)
    std::vector<double> chromaLine(videoParameters.frame_width_nominal);
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      chromaLine[h] = static_cast<double>(cLine[h]);
    }

    // Demodulate chroma to I/Q using shared helper
    demodulateChroma(chromaLine.data(), lineNumber, linePhase, I, Q, xOffset);
  }
}

/*
 * This applies an FIR coring filter to both I and Q color channels.  It's a
 * simple (crude?) NR technique used by LD players, but effective especially on
 * the Y/luma channel.
 *
 * A coring filter removes high frequency components (.4mhz chroma, 2.8mhz luma)
 * of a signal up to a certain point, which removes small high frequency noise.
 */

void Comb::FrameBuffer::doCNR() {
  if (configuration.cNRLevel == 0) return;

  // nr_c is the coring level
  const double nr_c = configuration.cNRLevel * irescale;

  // High-pass filters for I/Q
  auto iFilter(f_nrc);
  auto qFilter(f_nrc);

  // Filter delay (since it's a symmetric FIR filter)
  const int32_t delay = c_nrc_b.size() / 2;

  // High-pass result
  // TODO(sdi): Cache arrays instead of reallocating every field.
  std::vector<double> hpI(videoParameters.active_video_end + delay);
  std::vector<double> hpQ(videoParameters.active_video_end + delay);

  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    double* I = componentFrame->u(lineNumber);
    double* Q = componentFrame->v(lineNumber);

    // Feed zeros into the filter outside the active area
    for (int32_t h = videoParameters.active_video_start - delay;
         h < videoParameters.active_video_start; h++) {
      iFilter.feed(0.0);
      qFilter.feed(0.0);
    }
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      hpI[h] = iFilter.feed(I[h]);
      hpQ[h] = qFilter.feed(Q[h]);
    }
    for (int32_t h = videoParameters.active_video_end;
         h < videoParameters.active_video_end + delay; h++) {
      hpI[h] = iFilter.feed(0.0);
      hpQ[h] = qFilter.feed(0.0);
    }

    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      // Offset to cover the filter delay
      double ai = hpI[h + delay];
      double aq = hpQ[h + delay];

      // Clip the filter strength
      if (fabs(ai) > nr_c) {
        ai = (ai > 0) ? nr_c : -nr_c;
      }
      if (fabs(aq) > nr_c) {
        aq = (aq > 0) ? nr_c : -nr_c;
      }

      I[h] -= ai;
      Q[h] -= aq;
    }
  }
}

void Comb::FrameBuffer::doYNR() {
  if (configuration.yNRLevel == 0) return;

  // nr_y is the coring level
  double nr_y = configuration.yNRLevel * irescale;

  // High-pass filter for Y
  auto yFilter(f_nr);

  // Filter delay (since it's a symmetric FIR filter)
  const int32_t delay = c_nr_b.size() / 2;

  // High-pass result (size for active width)
  const int32_t activeWidth =
      videoParameters.active_video_end - videoParameters.active_video_start;
  std::vector<double> hpY(videoParameters.active_video_end + delay);

  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? 0
                              : videoParameters.active_video_start;

  for (int32_t absoluteLineNumber = videoParameters.first_active_frame_line;
       absoluteLineNumber < videoParameters.last_active_frame_line;
       absoluteLineNumber++) {
    double* Y = componentFrame->y(absoluteLineNumber - lineOffset);

    // Feed zeros into the filter before the active area
    for (int32_t i = videoParameters.active_video_start - delay;
         i < videoParameters.active_video_start; i++) {
      yFilter.feed(0.0);
    }
    for (int32_t i = videoParameters.active_video_start;
         i < videoParameters.active_video_end; i++) {
      hpY[i] =
          yFilter.feed(Y[i - xOffset - videoParameters.active_video_start]);
    }
    for (int32_t i = videoParameters.active_video_end;
         i < videoParameters.active_video_end + delay; i++) {
      hpY[i] = yFilter.feed(0.0);
    }

    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      // Offset to cover the filter delay
      double a = hpY[h + delay];

      // Clip the filter strength
      if (fabs(a) > nr_y) {
        a = (a > 0) ? nr_y : -nr_y;
      }

      Y[h - xOffset - videoParameters.active_video_start] -= a;
    }
  }
}

// Transform I/Q into U/V, and apply chroma gain
void Comb::FrameBuffer::transformIQ(double chromaGain, double chromaPhase) {
  // Compute components for the rotation vector
  const double theta = ((33 + chromaPhase) * M_PI) / 180;
  const double bp = sin(theta) * chromaGain;
  const double bq = cos(theta) * chromaGain;

  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? videoParameters.active_video_start
                              : 0;

  // Apply the vector to all the samples
  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    double* I = componentFrame->u(lineNumber - lineOffset);
    double* Q = componentFrame->v(lineNumber - lineOffset);

    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      double U = (-bp * I[h - xOffset]) + (bq * Q[h - xOffset]);
      double V = (bq * I[h - xOffset]) + (bp * Q[h - xOffset]);

      I[h - xOffset] = U;
      Q[h - xOffset] = V;
    }
  }
}

// Overlay the 3D filter map onto the output
void Comb::FrameBuffer::overlayMap(const FrameBuffer& previousFrame,
                                   const FrameBuffer& nextFrame) {
  ORC_LOG_DEBUG("Comb::FrameBuffer::overlayMap(): Overlaying map onto output");

  // Create a canvas for colour conversion
  FrameCanvas canvas(*componentFrame, videoParameters);

  // Convert CANDIDATE_SHADES into Y'UV form
  FrameCanvas::Colour shades[NUM_CANDIDATES];
  for (int32_t i = 0; i < NUM_CANDIDATES; i++) {
    const uint32_t shade = CANDIDATE_SHADES[i];
    shades[i] = canvas.rgb(((shade >> 16) & 0xff) << 8,
                           ((shade >> 8) & 0xff) << 8, (shade & 0xff) << 8);
  }

  const int32_t lineOffset = videoParameters.active_area_cropping_applied
                                 ? videoParameters.first_active_frame_line
                                 : 0;
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? videoParameters.active_video_start
                              : 0;

  // For each sample in the frame...
  for (int32_t lineNumber = videoParameters.first_active_frame_line;
       lineNumber < videoParameters.last_active_frame_line; lineNumber++) {
    double* U = componentFrame->u(lineNumber - lineOffset);
    double* V = componentFrame->v(lineNumber - lineOffset);

    // Fill the output frame with the RGB values
    for (int32_t h = videoParameters.active_video_start;
         h < videoParameters.active_video_end; h++) {
      // Select the best candidate
      int32_t bestIndex;
      double bestSample;
      getBestCandidate(lineNumber, h, previousFrame, nextFrame, bestIndex,
                       bestSample);

      // Leave Y' the same, but replace UV with the appropriate shade
      U[h - xOffset] = shades[bestIndex].u;
      V[h - xOffset] = shades[bestIndex].v;
    }
  }
}
