/*
 * File:        buffered_file_io.h
 * Module:      orc-core
 * Purpose:     Buffered file I/O for high-performance reading and writing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/stage/file_io_interface.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief High-performance buffered file writer
 *
 * Accumulates data in a large internal buffer and writes to disk in
 * large chunks to minimize filesystem thrashing and system calls.
 *
 * Template parameter T is the data type being written (e.g., uint16_t, int16_t,
 * uint8_t)
 */
template <typename T>
class BufferedFileWriter : public IFileWriter<T> {
 public:
  /**
   * @brief Construct a buffered writer
   * @param buffer_size Size of internal buffer in bytes (default: 4MB)
   */
  explicit BufferedFileWriter(size_t buffer_size = 4UL * 1024 * 1024)
      : buffer_size_(buffer_size),
        buffer_(),
        bytes_written_(0),
        is_open_(false) {
    // Reserve buffer capacity
    buffer_.reserve(buffer_size_ / sizeof(T));
  }

  /**
   * @brief Destructor - automatically flushes and closes
   */
  ~BufferedFileWriter() override {
    if (is_open_) {
      try {
        close();
      } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Suppress exceptions in destructor
      }
    }
  }

  // Non-copyable
  BufferedFileWriter(const BufferedFileWriter&) = delete;
  BufferedFileWriter& operator=(const BufferedFileWriter&) = delete;

  // Movable
  BufferedFileWriter(BufferedFileWriter&&) = default;
  BufferedFileWriter& operator=(BufferedFileWriter&&) = default;

  /**
   * @brief Open a file for buffered writing
   * @param filepath Path to output file
   * @param mode Open mode flags (default: binary | trunc)
   * @return true if opened successfully
   */
  using IFileWriter<T>::open;
  bool open(const std::string& filepath, std::ios::openmode mode) override {
    if (is_open_) {
      close();
    }

    file_.open(filepath, mode);
    if (!file_.is_open()) {
      return false;
    }

    filepath_ = filepath;
    is_open_ = true;
    bytes_written_ = 0;
    buffer_.clear();

    return true;
  }

  /**
   * @brief Write data to buffer (will auto-flush when buffer is full)
   * @param data Pointer to data
   * @param count Number of elements to write
   */
  void write(const T* data, size_t count) override {
    if (!is_open_) {
      throw std::runtime_error("BufferedFileWriter: File not open");
    }

    // If this single write is larger than our buffer, write it directly
    if (count * sizeof(T) > buffer_size_) {
      flush();  // Flush existing buffer first
      file_.write(reinterpret_cast<const char*>(data), count * sizeof(T));
      if (!file_.good()) {
        throw std::runtime_error(
            "BufferedFileWriter: Failed to write to file: " + filepath_);
      }
      bytes_written_ += count * sizeof(T);
      return;
    }

    // Add to buffer
    buffer_.insert(buffer_.end(), data, data + count);

    // Flush if buffer is getting full
    if (buffer_.size() * sizeof(T) >= buffer_size_) {
      flush();
    }
  }

  /**
   * @brief Write a vector of data
   * @param data Vector of data to write
   */
  void write(const std::vector<T>& data) override {
    if (!data.empty()) {
      write(data.data(), data.size());
    }
  }

  /**
   * @brief Flush buffered data to disk
   */
  void flush() override {
    if (!is_open_) {
      throw std::runtime_error("BufferedFileWriter: File not open");
    }

    if (buffer_.empty()) {
      return;
    }

    size_t bytes_to_write = buffer_.size() * sizeof(T);
    file_.write(reinterpret_cast<const char*>(buffer_.data()),
                static_cast<std::streamsize>(bytes_to_write));

    if (!file_.good()) {
      throw std::runtime_error(
          "BufferedFileWriter: Failed to flush buffer to file: " + filepath_);
    }

    bytes_written_ += bytes_to_write;
    buffer_.clear();
  }

  /**
   * @brief Close the file (automatically flushes)
   */
  void close() override {
    if (!is_open_) {
      return;
    }

    flush();
    file_.close();
    is_open_ = false;
  }

  /**
   * @brief Get total bytes written to file
   */
  uint64_t bytes_written() const override { return bytes_written_; }

  /**
   * @brief Check if file is open
   */
  bool is_open() const override { return is_open_; }

  /**
   * @brief Get the filepath
   */
  const std::string& filepath() const override { return filepath_; }

 private:
  size_t buffer_size_;      ///< Buffer size in bytes
  std::vector<T> buffer_;   ///< Internal buffer
  uint64_t bytes_written_;  ///< Total bytes written to file
  std::ofstream file_;      ///< Underlying file stream
  std::string filepath_;    ///< Path to file
  bool is_open_;            ///< Whether file is open
};

/**
 * @brief High-performance buffered file reader
 *
 * Reads data in large chunks to minimize filesystem access and system calls.
 * Provides random access with efficient buffering.
 *
 * Template parameter T is the data type being read (e.g., uint16_t, int16_t,
 * uint8_t)
 */
template <typename T>
class BufferedFileReader {
 public:
  /**
   * @brief Construct a buffered reader
   * @param buffer_size Size of internal buffer in bytes (default: 4MB)
   */
  explicit BufferedFileReader(size_t buffer_size = 4UL * 1024 * 1024)
      : buffer_size_(buffer_size),
        buffer_(),
        buffer_file_offset_(0),
        file_size_(0),
        is_open_(false) {
    buffer_.reserve(buffer_size_ / sizeof(T));
  }

  /**
   * @brief Destructor
   */
  ~BufferedFileReader() {
    if (is_open_) {
      close();
    }
  }

  // Non-copyable
  BufferedFileReader(const BufferedFileReader&) = delete;
  BufferedFileReader& operator=(const BufferedFileReader&) = delete;

  // Movable
  BufferedFileReader(BufferedFileReader&&) = default;
  BufferedFileReader& operator=(BufferedFileReader&&) = default;

  /**
   * @brief Open a file for buffered reading
   * @param filepath Path to input file
   * @return true if opened successfully
   */
  bool open(const std::string& filepath) {
    if (is_open_) {
      close();
    }

    file_.open(filepath, std::ios::binary);
    if (!file_.is_open()) {
      return false;
    }

    // Get file size
    file_.seekg(0, std::ios::end);
    file_size_ = file_.tellg();
    file_.seekg(0, std::ios::beg);

    filepath_ = filepath;
    is_open_ = true;
    buffer_.clear();
    buffer_file_offset_ = 0;

    return true;
  }

  /**
   * @brief Read data from file at specified byte offset
   * @param byte_offset Offset in file (in bytes)
   * @param count Number of elements to read
   * @return Vector of data read
   */
  std::vector<T> read(uint64_t byte_offset, size_t count) {
    if (!is_open_) {
      throw std::runtime_error("BufferedFileReader: File not open");
    }

    if (byte_offset + count * sizeof(T) > file_size_) {
      throw std::runtime_error("BufferedFileReader: Read beyond end of file");
    }

    std::vector<T> result(count);

    // Check if requested data is in buffer
    uint64_t buffer_start = buffer_file_offset_;
    uint64_t buffer_end = buffer_file_offset_ + buffer_.size() * sizeof(T);

    if (byte_offset >= buffer_start &&
        byte_offset + count * sizeof(T) <= buffer_end) {
      // Data is entirely in buffer
      size_t buffer_element_offset =
          (byte_offset - buffer_file_offset_) / sizeof(T);
      std::copy(buffer_.begin() + buffer_element_offset,
                buffer_.begin() + buffer_element_offset + count,
                result.begin());
      return result;
    }

    // Data not in buffer - need to read from file
    // For large reads, read directly; for small reads, refill buffer
    if (count * sizeof(T) > buffer_size_ / 2) {
      // Large read - read directly
      file_.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
      file_.read(reinterpret_cast<char*>(result.data()),
                 static_cast<std::streamsize>(count * sizeof(T)));

      if (file_.gcount() != static_cast<std::streamsize>(count * sizeof(T))) {
        throw std::runtime_error(
            "BufferedFileReader: Failed to read from file");
      }
    } else {
      // Small read - refill buffer starting at requested offset
      refill_buffer(byte_offset);

      // Now data should be in buffer
      size_t buffer_element_offset =
          (byte_offset - buffer_file_offset_) / sizeof(T);
      if (buffer_element_offset + count > buffer_.size()) {
        throw std::runtime_error(
            "BufferedFileReader: Insufficient data in buffer");
      }

      std::copy(buffer_.begin() + buffer_element_offset,
                buffer_.begin() + buffer_element_offset + count,
                result.begin());
    }

    return result;
  }

  /**
   * @brief Close the file
   */
  void close() {
    if (!is_open_) {
      return;
    }

    file_.close();
    is_open_ = false;
    buffer_.clear();
  }

  /**
   * @brief Get file size in bytes
   */
  uint64_t file_size() const { return file_size_; }

  /**
   * @brief Check if file is open
   */
  bool is_open() const { return is_open_; }

  /**
   * @brief Get the filepath
   */
  const std::string& filepath() const { return filepath_; }

 private:
  /**
   * @brief Refill internal buffer starting at specified offset
   */
  void refill_buffer(uint64_t byte_offset) {
    buffer_file_offset_ = byte_offset;

    // Calculate how much to read
    size_t elements_to_read = buffer_size_ / sizeof(T);
    uint64_t bytes_remaining = file_size_ - byte_offset;
    size_t bytes_to_read = std::min(
        static_cast<uint64_t>(elements_to_read * sizeof(T)), bytes_remaining);

    buffer_.resize(bytes_to_read / sizeof(T));

    file_.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(buffer_.data()),
               static_cast<std::streamsize>(bytes_to_read));

    if (file_.gcount() != static_cast<std::streamsize>(bytes_to_read)) {
      throw std::runtime_error("BufferedFileReader: Failed to refill buffer");
    }
  }

  size_t buffer_size_;           ///< Buffer size in bytes
  std::vector<T> buffer_;        ///< Internal buffer
  uint64_t buffer_file_offset_;  ///< File offset of first byte in buffer
  uint64_t file_size_;           ///< Total file size
  std::ifstream file_;           ///< Underlying file stream
  std::string filepath_;         ///< Path to file
  bool is_open_;                 ///< Whether file is open
};

}  // namespace orc
