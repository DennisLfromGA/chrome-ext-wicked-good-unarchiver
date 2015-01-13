// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "volume.h"

#include <cstring>
#include <sstream>

#include "request.h"
#include "volume_archive_libarchive.h"
#include "volume_reader_javascript_stream.h"

namespace {

typedef std::map<std::string, VolumeArchive*>::const_iterator
    volume_archive_iterator;

const char kPathDelimiter[] = "/";

// size is int64_t and modification_time is time_t because this is how
// libarchive is going to pass them to us.
pp::VarDictionary CreateEntry(int64_t index,
                              const std::string& name,
                              bool is_directory,
                              int64_t size,
                              time_t modification_time) {
  pp::VarDictionary entry_metadata;
  // index is int64_t, unsupported by pp::Var
  std::stringstream ss_index;
  ss_index << index;
  entry_metadata.Set("index", ss_index.str());
  entry_metadata.Set("isDirectory", is_directory);
  entry_metadata.Set("name", name);
  // size is int64_t, unsupported by pp::Var
  std::stringstream ss_size;
  ss_size << size;
  entry_metadata.Set("size", ss_size.str());
  // mtime is time_t, unsupported by pp::Var
  std::stringstream ss_modification_time;
  ss_modification_time << modification_time;
  entry_metadata.Set("modificationTime", ss_modification_time.str());

  if (is_directory)
    entry_metadata.Set("entries", pp::VarDictionary());

  return entry_metadata;
}

void ConstructMetadata(int64_t index,
                       const std::string& entry_path,
                       int64_t size,
                       bool is_directory,
                       time_t modification_time,
                       pp::VarDictionary* parent_metadata) {
  if (entry_path == "")
    return;

  pp::VarDictionary parent_entries =
      pp::VarDictionary(parent_metadata->Get("entries"));

  unsigned int position = entry_path.find(kPathDelimiter);
  pp::VarDictionary entry_metadata;
  std::string entry_name;

  if (position == std::string::npos) {  // The entry itself.
    entry_name = entry_path;
    entry_metadata =
        CreateEntry(index, entry_name, is_directory, size, modification_time);

    // Update directory information. Required as sometimes the directory itself
    // is returned after the files inside it.
    pp::Var old_entry_metadata_var = parent_entries.Get(entry_name);
    if (!old_entry_metadata_var.is_undefined()) {
      pp::VarDictionary old_entry_metadata =
          pp::VarDictionary(old_entry_metadata_var);
      PP_DCHECK(old_entry_metadata.Get("isDirectory").AsBool());
      entry_metadata.Set("entries", old_entry_metadata.Get("entries"));
    }
  } else {  // Get next parent on the way to the entry.
    entry_name = entry_path.substr(0, position);

    // Get next parent metadata. If none, create a new directory entry for it.
    // Some archives don't have directory information inside and for some the
    // information is returned later than the files inside it.
    pp::Var entry_metadata_var = parent_entries.Get(entry_name);
    if (entry_metadata_var.is_undefined())
      entry_metadata = CreateEntry(-1, entry_name, true, 0, modification_time);
    else
      entry_metadata = pp::VarDictionary(parent_entries.Get(entry_name));

    // Continue to construct metadata for all directories on the path to the
    // to the entry and for the entry itself.
    std::string entry_path_without_next_parent = entry_path.substr(
        position + sizeof(kPathDelimiter) - 1 /* Last char is '\0'. */);

    ConstructMetadata(index,
                      entry_path_without_next_parent,
                      size,
                      is_directory,
                      modification_time,
                      &entry_metadata);
  }

  // Recreate parent_metadata. This is necessary because pp::VarDictionary::Get
  // returns a Var, not a Var& or Var* to directly modify the result.
  parent_entries.Set(entry_name, entry_metadata);
  parent_metadata->Set("entries", parent_entries);
}

// An internal implementation of JavaScriptRequestorInterface.
class JavaScriptRequestor : public JavaScriptRequestorInterface {
 public:
  // JavaScriptRequestor does not own the volume pointer.
  explicit JavaScriptRequestor(Volume* volume) : volume_(volume) {}

  virtual void RequestFileChunk(const std::string& request_id,
                                int64_t offset,
                                int64_t bytes_to_read) {
    PP_DCHECK(offset >= 0);
    PP_DCHECK(bytes_to_read > 0);
    volume_->message_sender()->SendFileChunkRequest(
        volume_->file_system_id(), request_id, offset, bytes_to_read);
  }

  virtual void RequestPassphrase(const std::string& request_id) {
    volume_->message_sender()->SendPassphraseRequest(
        volume_->file_system_id(), request_id);
  }

 private:
  Volume* volume_;
};

// An internal implementation of VolumeArchiveFactoryInterface for default
// Volume constructor.
class VolumeArchiveFactory : public VolumeArchiveFactoryInterface {
 public:
  virtual VolumeArchive* Create(VolumeReader* reader) {
    return new VolumeArchiveLibarchive(reader);
  }
};

// An internal implementation of VolumeReaderFactoryInterface for default Volume
// constructor.
class VolumeReaderFactory : public VolumeReaderFactoryInterface {
 public:
  // VolumeReaderFactory does not own the volume pointer.
  explicit VolumeReaderFactory(Volume* volume) : volume_(volume) {}

  virtual VolumeReader* Create(int64_t archive_size) {
    return new VolumeReaderJavaScriptStream(archive_size, volume_->requestor());
  }

 private:
  Volume* volume_;
};

}  // namespace

struct Volume::OpenFileArgs {
  OpenFileArgs(const std::string& request_id,
               int64_t index,
               const std::string& encoding,
               int64_t archive_size) : request_id(request_id),
                                       index(index),
                                       encoding(encoding),
                                       archive_size(archive_size) {}
  const std::string request_id;
  const int64_t index;
  const std::string encoding;
  const int64_t archive_size;
};

Volume::Volume(const pp::InstanceHandle& instance_handle,
               const std::string& file_system_id,
               JavaScriptMessageSenderInterface* message_sender)
    : volume_archive_(NULL),
      file_system_id_(file_system_id),
      message_sender_(message_sender),
      worker_(instance_handle),
      callback_factory_(this) {
  requestor_ = new JavaScriptRequestor(this);
  volume_archive_factory_ = new VolumeArchiveFactory();
  volume_reader_factory_ = new VolumeReaderFactory(this);
  // Delegating constructors only from c++11.
}

Volume::Volume(const pp::InstanceHandle& instance_handle,
               const std::string& file_system_id,
               JavaScriptMessageSenderInterface* message_sender,
               VolumeArchiveFactoryInterface* volume_archive_factory,
               VolumeReaderFactoryInterface* volume_reader_factory)
    : volume_archive_(NULL),
      file_system_id_(file_system_id),
      message_sender_(message_sender),
      worker_(instance_handle),
      callback_factory_(this),
      volume_archive_factory_(volume_archive_factory),
      volume_reader_factory_(volume_reader_factory) {
  requestor_ = new JavaScriptRequestor(this);
}

Volume::~Volume() {
  worker_.Join();

  if (volume_archive_) {
    volume_archive_->Cleanup();
    delete volume_archive_;
  }

  delete requestor_;
  delete volume_archive_factory_;
  delete volume_reader_factory_;
}

bool Volume::Init() {
  return worker_.Start();
}

void Volume::ReadMetadata(const std::string& request_id,
                          const std::string& encoding,
                          int64_t archive_size) {
  worker_.message_loop().PostWork(callback_factory_.NewCallback(
      &Volume::ReadMetadataCallback, request_id, encoding, archive_size));
}

void Volume::OpenFile(const std::string& request_id,
                      int64_t index,
                      const std::string& encoding,
                      int64_t archive_size) {
  worker_.message_loop().PostWork(callback_factory_.NewCallback(
      &Volume::OpenFileCallback, OpenFileArgs(request_id, index, encoding,
      archive_size)));
}

void Volume::CloseFile(const std::string& request_id,
                       const std::string& open_request_id) {
  // Though close file could be executed on main thread, we send it to worker_
  // in order to ensure thread safety.
  worker_.message_loop().PostWork(callback_factory_.NewCallback(
      &Volume::CloseFileCallback, request_id, open_request_id));
}

void Volume::ReadFile(const std::string& request_id,
                      const pp::VarDictionary& dictionary) {
  worker_.message_loop().PostWork(callback_factory_.NewCallback(
      &Volume::ReadFileCallback, request_id, dictionary));
}

void Volume::ReadChunkDone(const std::string& request_id,
                           const pp::VarArrayBuffer& array_buffer,
                           int64_t read_offset) {
  PP_DCHECK(volume_archive_);

  job_lock_.Acquire();
  if (request_id == reader_request_id_) {
    static_cast<VolumeReaderJavaScriptStream*>(volume_archive_->reader())->
        SetBufferAndSignal(array_buffer, read_offset);
  }
  job_lock_.Release();
}

void Volume::ReadChunkError(const std::string& request_id) {
  PP_DCHECK(volume_archive_);
  job_lock_.Acquire();
  if (request_id == reader_request_id_) {
    static_cast<VolumeReaderJavaScriptStream*>(volume_archive_->reader())->
        ReadErrorSignal();
  }
  job_lock_.Release();
}

void Volume::ReadPassphraseDone(const std::string& request_id,
                                const std::string& passphrase) {
  PP_DCHECK(volume_archive_);

  job_lock_.Acquire();
  if (request_id == reader_request_id_) {
    static_cast<VolumeReaderJavaScriptStream*>(volume_archive_->reader())->
        SetPassphraseAndSignal(passphrase);
  }
  job_lock_.Release();
}

void Volume::ReadPassphraseError(const std::string& request_id) {
  PP_DCHECK(volume_archive_);

  job_lock_.Acquire();
  if (request_id == reader_request_id_) {
    static_cast<VolumeReaderJavaScriptStream*>(volume_archive_->reader())->
        PassphraseErrorSignal();
  }
  job_lock_.Release();
}

void Volume::ReadMetadataCallback(int32_t /*result*/,
                                  const std::string& request_id,
                                  const std::string& encoding,
                                  int64_t archive_size) {
  if (volume_archive_) {
     message_sender_->SendFileSystemError(
         file_system_id_, request_id, "ALREADY_OPENED");
  }

  job_lock_.Acquire();
  volume_archive_ = volume_archive_factory_->Create(
      volume_reader_factory_->Create(archive_size));
  static_cast<VolumeReaderJavaScriptStream*>(volume_archive_->reader())->
      SetRequestId(request_id);
  reader_request_id_ = request_id;
  job_lock_.Release();

  if (!volume_archive_->Init(encoding)) {
    message_sender_->SendFileSystemError(
        file_system_id_, request_id, volume_archive_->error_message());
    ClearJob();
    delete volume_archive_;
    volume_archive_ = NULL;
    return;
  }

  // Read and construct metadata.
  pp::VarDictionary root_metadata = CreateEntry(-1, "" /* name */, true, 0, 0);

  const char* path_name = NULL;
  int64_t size = 0;
  bool is_directory = false;
  time_t modification_time = 0;
  int64_t index = 0;

  for (;;) {
    if (!volume_archive_->GetNextHeader(
            &path_name, &size, &is_directory, &modification_time)) {
      message_sender_->SendFileSystemError(
          file_system_id_, request_id, volume_archive_->error_message());
      ClearJob();
      delete volume_archive_;
      volume_archive_ = NULL;
      return;
    }

    if (!path_name)  // End of archive.
      break;

    ConstructMetadata(index, path_name, size, is_directory, modification_time,
        &root_metadata);

    ++index;
  }

  ClearJob();

  // Send metadata back to JavaScript.
  message_sender_->SendReadMetadataDone(
      file_system_id_, request_id, root_metadata);
}

void Volume::OpenFileCallback(int32_t /*result*/,
                              const OpenFileArgs& args) {
  if (!volume_archive_) {
     message_sender_->SendFileSystemError(
         file_system_id_, args.request_id, "NOT_OPENED");
     return;
  }

  job_lock_.Acquire();
  if (!reader_request_id_.empty()) {
    // It is illegal to open a file while another operation is in progress or
    // another file is opened.
    message_sender_->SendFileSystemError(
        file_system_id_, args.request_id, "ILLEGAL");
    job_lock_.Release();
    return;
  }
  static_cast<VolumeReaderJavaScriptStream*>(volume_archive_->reader())->
      SetRequestId(args.request_id);
  reader_request_id_ = args.request_id;
  job_lock_.Release();

  const char* path_name = NULL;
  int64_t size = 0;
  bool is_directory = false;
  time_t modification_time = 0;

  if (!volume_archive_->SeekHeader(args.index)) {
    message_sender_->SendFileSystemError(
        file_system_id_, args.request_id, volume_archive_->error_message());
    ClearJob();
    return;
  }

  if (!volume_archive_->GetNextHeader(
          &path_name, &size, &is_directory, &modification_time)) {
    message_sender_->SendFileSystemError(
        file_system_id_, args.request_id, volume_archive_->error_message());
    ClearJob();
    return;
  }

  // Send successful opened file response to NaCl.
  message_sender_->SendOpenFileDone(file_system_id_, args.request_id);
}

void Volume::CloseFileCallback(int32_t /*result*/,
                               const std::string& request_id,
                               const std::string& open_request_id) {
  job_lock_.Acquire();
  reader_request_id_ = "";
  job_lock_.Release();

  message_sender_->SendCloseFileDone(
      file_system_id_, request_id, open_request_id);
}

void Volume::ReadFileCallback(int32_t /*result*/,
                              const std::string& request_id,
                              const pp::VarDictionary& dictionary) {
  if (!volume_archive_) {
     message_sender_->SendFileSystemError(
         file_system_id_, request_id, "NOT_OPENED");
     return;
  }

  std::string open_request_id(
      dictionary.Get(request::key::kOpenRequestId).AsString());
  int64_t offset =
      request::GetInt64FromString(dictionary, request::key::kOffset);
  int64_t length =
      request::GetInt64FromString(dictionary, request::key::kLength);
  PP_DCHECK(length > 0);  // JavaScript must not make requests with length <= 0.

  job_lock_.Acquire();
  if (open_request_id != reader_request_id_) {
    // The file is not opened.
    message_sender_->SendFileSystemError(
        file_system_id_, request_id, "FILE_NOT_OPENED");
    job_lock_.Release();
    return;
  }
  job_lock_.Release();

  // Decompress data and send it to JavaScript. Sending data is done in chunks
  // depending on how many bytes VolumeArchive::ReadData returns.
  int64_t left_length = length;
  while (left_length > 0) {
    const char* destination_buffer = NULL;
    int64_t read_bytes = volume_archive_->ReadData(
        offset, left_length, &destination_buffer);

    if (read_bytes < 0) {
      // Error messages should be sent to the read request (request_id), not
      // open request (open_request_id), as the last one has finished and this
      // is a read file.
      message_sender_->SendFileSystemError(
          file_system_id_, request_id, volume_archive_->error_message());

      // Should not cleanup VolumeArchive as Volume::CloseFile will be called in
      // case of failure.
      return;
    }

    // Send response back to ReadFile request.
    pp::VarArrayBuffer array_buffer(read_bytes);
    if (read_bytes > 0) {
      char* array_buffer_data = static_cast<char*>(array_buffer.Map());
      memcpy(array_buffer_data, destination_buffer, read_bytes);
      array_buffer.Unmap();
    }

    bool has_more_data = left_length - read_bytes > 0 && read_bytes > 0;
    message_sender_->SendReadFileDone(
        file_system_id_, request_id, array_buffer, has_more_data);

    if (read_bytes == 0)
      break;  // No more available data.

    left_length -= read_bytes;
    offset += read_bytes;
  }
  volume_archive_->MaybeDecompressAhead();
}


void Volume::ClearJob() {
  job_lock_.Acquire();
  reader_request_id_ = "";
  job_lock_.Release();
}
