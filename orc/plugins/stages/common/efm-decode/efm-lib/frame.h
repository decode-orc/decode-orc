/*
 * File:        frame.h
 * Purpose:     EFM-library - EFM Frame type classes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef FRAME_H
#define FRAME_H

#include <cstdint>
#include <fstream>
#include <vector>

// Frame class - base class for F1, F2, and F3 frames
class Frame {
 public:
  virtual ~Frame() {}                 // Virtual destructor
  virtual int frameSize() const = 0;  // Pure virtual function to get frame size

  virtual void setData(const std::vector<uint8_t>& data);
  virtual const std::vector<uint8_t>& data() const;

  virtual void setErrorData(const std::vector<uint8_t>& errorData);
  virtual const std::vector<uint8_t>& errorData() const;
  virtual uint32_t countErrors() const;

  // Per-symbol doubt carried up from the producer's EFM t-values (0 trusted,
  // 15 distrusted - CVBS File Format Specification, EFM extension format).
  // Unlike the error and padded vectors this one is left EMPTY when every
  // symbol is trusted, which is the whole-stream case for a producer that
  // emits no confidence information; consumers must read an empty vector as
  // "all zero" rather than assuming frameSize() entries.
  virtual void setDoubtData(const std::vector<uint8_t>& doubtData);
  virtual const std::vector<uint8_t>& doubtData() const;

  virtual void setPaddedData(const std::vector<uint8_t>& paddedData);
  virtual const std::vector<uint8_t>& paddedData() const;
  virtual uint32_t countPadded() const;

  bool isFull() const;
  bool isEmpty() const;

 protected:
  std::vector<uint8_t> m_frameData;
  std::vector<uint8_t> m_frameErrorData;
  std::vector<uint8_t> m_framePaddedData;
  std::vector<uint8_t> m_frameDoubtData;
};

class Data24 : public Frame {
 public:
  Data24();
  int frameSize() const override;
  void showData();
  void setData(const std::vector<uint8_t>& data) override;
  void setErrorData(const std::vector<uint8_t>& errorData) override;
};

class F1Frame : public Frame {
 public:
  F1Frame();
  int frameSize() const override;
  void showData();
};

class F2Frame : public Frame {
 public:
  F2Frame();
  int frameSize() const override;
  void showData();
};

class F3Frame : public Frame {
 public:
  enum F3FrameType { Subcode, Sync0, Sync1 };

  F3Frame();
  int frameSize() const override;

  void setFrameTypeAsSubcode(uint8_t subcode);
  void setFrameTypeAsSync0();
  void setFrameTypeAsSync1();

  F3FrameType f3FrameType() const;
  std::string f3FrameTypeAsString() const;
  uint8_t subcodeByte() const;

  void showData();

 private:
  F3FrameType m_f3FrameType;
  uint8_t m_subcodeByte;
};

#endif  // FRAME_H