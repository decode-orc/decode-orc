/*
 * File:        videosynth_main.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     gtest main for the videosynth suite: the host's services for
 *              the stages, and every test's result to the run report
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <core_observation_service.h>
#include <gtest/gtest.h>
#include <orc/abi/orc_plugin_services.h>
#include <orc/plugin/orc_stage_services.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "videosynth_harness.h"

namespace {

// A file writer as the host's stage services give the sinks, unbuffered.
template <typename Interface, typename T>
class FileWriter final : public Interface {
 public:
  ~FileWriter() override { close(); }
  bool open(const std::string& filepath) override {
    stream_.open(filepath, std::ios::binary | std::ios::trunc);
    return stream_.is_open();
  }
  void write(const T* data, size_t count) override {
    stream_.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(count * sizeof(T)));
  }
  void write(const std::vector<T>& data) override {
    write(data.data(), data.size());
  }
  void flush() override { stream_.flush(); }
  void close() override {
    if (stream_.is_open()) stream_.close();
  }

 private:
  std::ofstream stream_;
};

class StageServices final : public orc::IStageServices {
 public:
  std::shared_ptr<orc::IFileWriterUint8> create_buffered_file_writer_uint8(
      size_t /*buffer_size*/) override {
    return std::make_shared<FileWriter<orc::IFileWriterUint8, uint8_t>>();
  }
  std::shared_ptr<orc::IFileWriterUint16> create_buffered_file_writer_uint16(
      size_t /*buffer_size*/) override {
    return std::make_shared<FileWriter<orc::IFileWriterUint16, uint16_t>>();
  }
  std::shared_ptr<orc::IFileWriterInt16> create_buffered_file_writer_int16(
      size_t /*buffer_size*/) override {
    return std::make_shared<FileWriter<orc::IFileWriterInt16, int16_t>>();
  }
};

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  orc_videosynth::install_result_writer();

  // The stages run in process, linked rather than loaded, so nothing gives
  // them the host's services. They are installed here as the plugin loader
  // installs them, so a stage behaves as it does in the application: the
  // observation service measures colour-frame phase and the NTSC comb's
  // per-field phase, and the sinks write through the stage services.
  static orc::CoreObservationService observation_service;
  static StageServices stage_services;
  static orc::OrcPluginServices services{};
  services.services_size =
      static_cast<uint32_t>(sizeof(orc::OrcPluginServices));
  services.stage_services = &stage_services;
  services.observation_service = &observation_service;
  orc::plugin::set_services(&services);

  return RUN_ALL_TESTS();
}
