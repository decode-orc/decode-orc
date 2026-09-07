/*
 * File:        sector_map_coordinates_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for R-8: the bad-sector map (.bsm) must index the
 *              data image it accompanies, not the disc's sector address space
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sector.h"
#include "writer_sector_metadata.h"

namespace {

Sector makeSector(int32_t address, bool valid) {
  Sector sector;
  sector.setAddress(SectorAddress(address));
  sector.setMode(1);
  sector.dataValid(valid);
  sector.pushData(std::vector<uint8_t>(2048, 0));
  sector.pushErrorData(std::vector<uint8_t>(2048, valid ? 0 : 1));
  return sector;
}

std::vector<int64_t> writeAndRead(const std::vector<Sector>& sectors) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("orc-bsm-test-" + std::to_string(::getpid()) + ".bsm");

  WriterSectorMetadata writer;
  EXPECT_TRUE(writer.open(path.string()));
  // The processor calls the image writer and the map writer once each per
  // emitted sector, in the same order; the fixture mirrors that contract.
  for (const Sector& sector : sectors) writer.write(sector);
  writer.close();

  std::vector<int64_t> entries;
  std::ifstream in(path);
  int64_t value = 0;
  while (in >> value) entries.push_back(value);
  std::filesystem::remove(path);
  return entries;
}

// While every gap is filled the image offset and the sector address coincide,
// so this case cannot tell the two coordinate spaces apart - it just pins the
// ordinary behaviour.
TEST(SectorMapCoordinates, ContiguousImageRecordsTheBadSectorPositions) {
  std::vector<Sector> sectors;
  for (int32_t address = 0; address < 10; ++address) {
    sectors.push_back(makeSector(address, address != 3 && address != 7));
  }

  EXPECT_EQ(writeAndRead(sectors), (std::vector<int64_t>{3, 7}));
}

// The case that matters: an address-space discontinuity makes the image offset
// stop tracking the sector address. Recording the address would point every
// later entry at the wrong sector - and, on a real capture, past the end of
// the image entirely.
TEST(SectorMapCoordinates, EntriesFollowTheImageAcrossAnAddressDiscontinuity) {
  std::vector<Sector> sectors;
  for (int32_t address = 0; address < 4; ++address) {
    sectors.push_back(makeSector(address, address != 1));
  }
  // The address space steps by 50000; no sectors are emitted for the skipped
  // addresses, so the image is unaffected and offsets simply carry on.
  for (int32_t i = 0; i < 4; ++i) {
    sectors.push_back(makeSector(50000 + i, i != 2));
  }

  const std::vector<int64_t> entries = writeAndRead(sectors);

  // Positions 1 and 6 in the image - not addresses 1 and 50002.
  EXPECT_EQ(entries, (std::vector<int64_t>{1, 6}));
  for (int64_t entry : entries) {
    EXPECT_LT(entry, static_cast<int64_t>(sectors.size()))
        << "an entry must index a sector the image actually contains";
  }
}

}  // namespace
