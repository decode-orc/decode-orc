/*
 * File:        comb.h
 * Module:      orc-core
 * Purpose:     NTSC comb filter decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2021 Chad Page
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019 Adam Sampson
 */

#ifndef COMB_H
#define COMB_H

#include <orc/stage/orc_source_parameters.h>

#include <cmath>
#include <fstream>
#include <iostream>

#include "componentframe.h"
#include "decoder.h"
#include "sourcefield.h"

class Comb {
 public:
  Comb();

  // Information about burst phase on a line (used by both composite and YC
  // paths)
  struct BurstInfo {
    double bsin, bcos;
  };

  // Comb filter configuration parameters
  struct Configuration {
    double chromaGain = 1.0;
    double chromaPhase = 0.0;
    int32_t dimensions = 2;
    bool adaptive = true;
    bool showMap = false;
    bool phaseCompensation = false;

    double cNRLevel = 0.0;
    double yNRLevel = 0.0;

    // Adaptation sensitivity for 3D filter (higher = prefer 3D, lower = prefer
    // 1D/2D)
    double adaptThreshold = 1.0;

    // Chroma weight for 3D adaptive filter (higher = prefer more 2D, lower =
    // prefer more 3D)
    double chromaWeight = 1.0;

    int32_t getLookBehind() const;
    int32_t getLookAhead() const;
  };

  const Configuration& getConfiguration() const;
  void updateConfiguration(const ::orc::SourceParameters& videoParameters,
                           const Configuration& configuration);

  // Decode a sequence of fields into a sequence of interlaced frames
  void decodeFrames(const std::vector<SourceField>& inputFields,
                    int32_t startIndex, int32_t endIndex,
                    std::vector<ComponentFrame>& componentFrames);

  // Maximum frame size
  static constexpr int32_t MAX_WIDTH = 910;
  static constexpr int32_t MAX_HEIGHT = 625;  // PAL frame height

 protected:
  // YC decode path - for sources with separate Y and C channels
  void decodeFramesYC(const std::vector<SourceField>& inputFields,
                      int32_t startIndex, int32_t endIndex,
                      std::vector<ComponentFrame>& componentFrames);

  // Composite decode path - full comb filter for Y/C separation
  void decodeFramesComposite(const std::vector<SourceField>& inputFields,
                             int32_t startIndex, int32_t endIndex,
                             std::vector<ComponentFrame>& componentFrames);

 private:
  // Comb-filter configuration parameters
  bool configurationSet;
  Configuration configuration;
  ::orc::SourceParameters videoParameters;

  // An input frame in the process of being decoded
  class FrameBuffer {
   public:
    FrameBuffer(const ::orc::SourceParameters& videoParameters_,
                const Configuration& configuration_);

    void loadFields(const SourceField& firstField,
                    const SourceField& secondField);

    // YC-specific loading - for sources with separate Y and C channels
    void loadFieldsYC(const SourceField& firstField,
                      const SourceField& secondField);

    void split1D();
    void split2D();
    void split3D(const FrameBuffer& previousFrame,
                 const FrameBuffer& nextFrame);

    void setComponentFrame(ComponentFrame& _componentFrame) {
      componentFrame = &_componentFrame;
    }

    void splitIQ();
    void splitIQlocked();

    // YC-specific chroma demodulation - skip Y/C separation, only demodulate C
    void splitIQ_YC();
    void splitIQlocked_YC();

    void filterIQ();
    void filterIQFull();
    void adjustY();
    void doCNR();
    void doYNR();
    void transformIQ(double chromaGain, double chromaPhase);

    void overlayMap(const FrameBuffer& previousFrame,
                    const FrameBuffer& nextFrame);

   private:
    const ::orc::SourceParameters& videoParameters;
    const Configuration& configuration;

    // Calculated frame height
    int32_t frameHeight;

    // IRE scaling
    double irescale;

    // Baseband samples (interlaced to form a complete frame).
    // int16_t in CVBS_U10_4FSC 10-bit domain.
    std::vector<int16_t> rawbuffer;

    // YC-specific: separate Y and C buffers for YC sources.
    // int16_t in CVBS_U10_4FSC 10-bit domain.
    std::vector<int16_t> luma_buffer;
    std::vector<int16_t> chroma_buffer;
    bool is_yc = false;  // True if loaded from YC source

    // Per-field phase id (1-4) of the frame's two fields, measured from the
    // burst by the colour_frame_phase observer; -1 unknown.  getLinePhase()
    // needs the per-field value: the two fields of a frame have opposite
    // burst phase on the same field line.
    int32_t firstFieldPhaseID;
    int32_t secondFieldPhaseID;

    // 1D, 2D and 3D-filtered chroma samples
    struct Sample {
      double pixel[MAX_HEIGHT][MAX_WIDTH];
    } clpbuffer[3];

    // Result of evaluating a 3D candidate
    struct Candidate {
      double penalty;
      double sample;
    };

    // The component frame for output (if there is one)
    ComponentFrame* componentFrame;

    inline int32_t getFieldID(int32_t lineNumber) const;
    inline bool getLinePhase(int32_t lineNumber) const;
    void getBestCandidate(int32_t lineNumber, int32_t h,
                          const FrameBuffer& previousFrame,
                          const FrameBuffer& nextFrame, int32_t& bestIndex,
                          double& bestSample) const;
    Candidate getCandidate(int32_t refLineNumber, int32_t refH,
                           const FrameBuffer& frameBuffer, int32_t lineNumber,
                           int32_t h, double adjustPenalty) const;

    // Helper to demodulate chroma with phase compensation (shared between
    // composite and YC)
    void demodulateChromaLocked(const double* chromaLine, int32_t lineNumber,
                                const Comb::BurstInfo& burstInfo, double* I,
                                double* Q, int32_t xOffset);

    // Helper to demodulate chroma without phase compensation (shared between
    // composite and YC)
    void demodulateChroma(const double* chromaLine, int32_t lineNumber,
                          bool linePhase, double* I, double* Q,
                          int32_t xOffset);
  };
};

#endif  // COMB_H
