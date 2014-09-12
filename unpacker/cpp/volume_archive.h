// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VOLUME_ARCHIVE_H_
#define VOLUME_ARCHIVE_H_

#include <string>

#include "volume_reader.h"

// Defines a wrapper for operations executed on an archive. API is not meant
// to be thread safe and its methods shouldn't be called in parallel.
class VolumeArchive {
 public:
  // VolumeReader should be allocated with new and the memory handling should be
  // done by the implementation of VolumeArchive.
  VolumeArchive(const std::string& request_id, VolumeReader* reader)
      : request_id_(request_id), reader_(reader) {}

  virtual ~VolumeArchive() {}

  // Initializes VolumeArchive. Should be called only once.
  // In case of any errors call VolumeArchive::Cleanup and the error message can
  // be obtained with VolumeArchive::error_message().
  virtual bool Init() = 0;

  // Gets the next header. If path_name is set to NULL, then there are no more
  // available headers. Returns true if reading next header was successful.
  // In case of failure the error message can be obtained with
  // VolumeArchive::error_message().
  virtual bool GetNextHeader(const char** path_name,
                             int64_t* size,
                             bool* is_directory,
                             time_t* modification_time) = 0;

  // Gets data from offset to offset + length for the file reached
  // with VolumeArchiveInterace::GetNextHeader. The data should be
  // stored starting from *buffer. In case offset is less then last
  // VolumeArchiveInterace::ReadData offset, then the read will be done
  // from the start of the archive. The API assumes offset is valid.
  // JavaScript shouldn't make requests with offset greater than data size.
  // Returns true if reading was successful for all the required number of
  // bytes. Length must be greater than 0.
  // In case of failure the error message can be obtained with
  // VolumeArchive::error_message().
  virtual bool ReadData(int64_t offset, int32_t length, char* buffer) = 0;

  // Cleans all resources. Should be called only once. Returns true if
  // successful. In case of failure the error message can be obtained with
  // VolumeArchive::error_message().
  virtual bool Cleanup() = 0;

  std::string request_id() const { return request_id_; }
  VolumeReader* reader() const { return reader_; }
  std::string error_message() const { return error_message_; }

 protected:
  // Cleans up the reader. Can be called multiple times, but once called reader
  // cannot be reinitialized.
  void CleanupReader() {
    delete reader_;
    reader_ = NULL;
  }

  void set_error_message(const std::string& error_message) {
    error_message_ = error_message;
  }

 private:
  std::string request_id_;  // The request id for which the VolumeArchive was
                            // created.
  VolumeReader* reader_;    // The reader that actually reads the archive data.
  std::string error_message_;  // An error message set in case of any errors.
};

#endif  // VOLUME_ARCHIVE_H_
