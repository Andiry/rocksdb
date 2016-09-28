#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <linux/limits.h>
#include "rocksdb/utilities/env_registry.h"
#include "env_nvm.h"
#include <exception>

namespace rocksdb {

static EnvRegistrar nvm_reg(
  "nvm://",
  [](const std::string& uri, std::unique_ptr<Env>* env_guard) {

    env_guard->reset(new EnvNVM(uri));

    return env_guard->get();
  }
);

EnvNVM::EnvNVM(
  const std::string& uri
) : Env(), posix_(Env::Default()), uri_(uri), fs_() {

  std::string uri_prefix = "nvm://";

  if (uri_.find(uri_prefix))
    throw std::runtime_error("invalid uri");

  dev_name_ = uri_.substr(uri_prefix.size());

  NVM_DBG(this, "uri(" << uri << "), dev_name(" << dev_name_ <<")");
}

EnvNVM::~EnvNVM(void) {
  NVM_DBG(this, "");

}

Status EnvNVM::NewSequentialFile(
  const std::string& fpath,
  unique_ptr<SequentialFile>* result,
  const EnvOptions& options
) {
  NVM_DBG(this, "fpath(" << fpath << ")");

  FPathInfo info(fpath);
  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->NewSequentialFile(fpath, result, options);
  }

  MutexLock lock(&fs_mutex_);

  NvmFile *file = FindFileUnguarded(info);
  if (!file) {
    return Status::NotFound();
  }

  result->reset(new NvmSequentialFile(file, options));

  return Status::OK();
}

Status EnvNVM::NewRandomAccessFile(
  const std::string& fpath,
  unique_ptr<RandomAccessFile>* result,
  const EnvOptions& options
) {
  NVM_DBG(this, "fpath(" << fpath << ")");

  FPathInfo info(fpath);
  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->NewRandomAccessFile(fpath, result, options);
  }

  MutexLock lock(&fs_mutex_);

  NvmFile *file = FindFileUnguarded(info);
  if (!file) {
    return Status::NotFound();
  }
  result->reset(new NvmRandomAccessFile(file, options));

  return Status::OK();
}

Status EnvNVM::ReuseWritableFile(
  const std::string& fpath,
  const std::string& fpath_old,
  unique_ptr<WritableFile>* result,
  const EnvOptions& options
) {
  NVM_DBG(this, "fpath(" << fpath << "), fpath_old(" << fpath_old << ")");

  FPathInfo info(fpath);
  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->ReuseWritableFile(fpath, fpath_old, result, options);
  }

  return Status::IOError("ReuseWritableFile --> Not implemented.");
}

Status EnvNVM::NewWritableFile(
  const std::string& fpath,
  unique_ptr<WritableFile>* result,
  const EnvOptions& options
) {
  NVM_DBG(this, "fpath(" << fpath << ")");

  FPathInfo info(fpath);

  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->NewWritableFile(fpath, result, options);
  }

  MutexLock lock(&fs_mutex_);

  NvmFile *file;

  file = FindFileUnguarded(info);       // Delete existing file
  if (file) {
    DeleteFileUnguarded(info);
  }

  try {                                 // Construct the new file
    file = new NvmFile(this, info);
  } catch (std::runtime_error& exc) {
    NVM_DBG(this, "Failed creating NvmFile, e(" << exc.what() << ")");
  }

  if (!file) {
    return Status::IOError("Failed creating NvmFile");
  }

  fs_[info.dpath()].push_back(file);

  result->reset(new NvmWritableFile(file, options));

  return Status::OK();
}

//
// Deletes a file without taking the fs_mutex_
//
Status EnvNVM::DeleteFileUnguarded(const FPathInfo& info) {
  NVM_DBG(this, "info(" << info.txt() << ")");

  auto dit = fs_.find(info.dpath());
  if (dit == fs_.end()) {
    NVM_DBG(this, "Dir NOT found");
    return Status::NotFound();
  }

  for (auto it = dit->second.begin(); it != dit->second.end(); ++it) {
    if ((*it)->IsNamed(info.fname())) {
      NVM_DBG(this, "File found -- erasing");

      (*it)->Unref();
      dit->second.erase(it);

      return Status::OK();
    }
  }

  NVM_DBG(this, "File NOT found");
  return Status::NotFound();
}

Status EnvNVM::DeleteFile(const std::string& fpath) {
  NVM_DBG(this, "fpath(" << fpath << ")");

  FPathInfo info(fpath);

  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->FileExists(fpath);
  }

  MutexLock lock(&fs_mutex_);

  return DeleteFileUnguarded(info);
}

Status EnvNVM::FileExists(const std::string& fpath) {
  NVM_DBG(this, "fpath(" << fpath << ")");

  FPathInfo info(fpath);

  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->FileExists(fpath);
  }

  MutexLock lock(&fs_mutex_);
  if (FindFileUnguarded(info)) {
    return Status::OK();
  }

  return Status::NotFound();
}

Status EnvNVM::GetChildren(
  const std::string& dpath,
  std::vector<std::string>* result
) {
  NVM_DBG(this, "dpath(" << dpath << ")");

  posix_->GetChildren(dpath, result);   // Merging posix and nvm

  MutexLock lock(&fs_mutex_);
  for (auto file : fs_[dpath]) {
      result->push_back(file->GetFname());
  }

  for (auto fname : *result) {
    NVM_DBG(this, "fname(" << fname << ")");
  }

  return Status::OK();
}

Status EnvNVM::GetChildrenFileAttributes(
  const std::string& dpath,
  std::vector<FileAttributes>* result
) {
  NVM_DBG(this, "dpath(" << dpath << ")");

  return Status::IOError("GetChildrenFileAttributes --> Not implemented");
}

NvmFile* EnvNVM::FindFileUnguarded(const FPathInfo& info) {
  NVM_DBG(this, "info(" << info.txt() << ")");

  auto dit = fs_.find(info.dpath());    // Lookup in loaded files
  if (dit == fs_.end()) {
    NVM_DBG(this, "!found");
    return NULL;
  }

  for (auto it = dit->second.begin(); it != dit->second.end(); ++it) {
    if ((*it)->IsNamed(info.fname())) {
      NVM_DBG(this, "found");
      return *it;
    }
  }

  std::vector<std::string> listing;     // Lookup meta-file in default env
  Status s = posix_->GetChildren(info.dpath(), &listing);
  if (!s.ok()) {
    NVM_DBG(this, "Default-env lookup failed.");
    return NULL;
  }

  for (auto entry : listing) {          // Find .meta files
    if (!FPathInfo::ends_with(entry, "meta"))
      continue;
    if (entry.compare(0, info.fname().size(), info.fname()))
      continue;

    try {
      NvmFile *file = new NvmFile(               // Create NvmFile from meta-file
        this, info, info.dpath() + std::string(1, FPathInfo::sep) + entry
      );
      return file;
    } catch (std::runtime_error& exc) {
      NVM_DBG(this, "Failed creation from meta, e(" << exc.what() << ")");
      return NULL;
    }
  }

  NVM_DBG(this, "!found");
  return NULL;
}

Status EnvNVM::GetFileSize(const std::string& fpath, uint64_t* fsize) {
  NVM_DBG(this, "fpath(" << fpath << ")");

  FPathInfo info(fpath);
  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->GetFileSize(fpath, fsize);
  }

  MutexLock lock(&fs_mutex_);

  NvmFile *file = FindFileUnguarded(info);
  if (!file) {
    return Status::IOError("File not not found");
  }

  *fsize = file->GetFileSize();

  return Status::OK();
}

Status EnvNVM::GetFileModificationTime(
  const std::string& fpath,
  uint64_t* file_mtime
) {
  NVM_DBG(this, "fpath(" << fpath << ")");

  FPathInfo info(fpath);
  if (!info.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->GetFileModificationTime(fpath, file_mtime);
  }

  MutexLock lock(&fs_mutex_);

  return Status::IOError("GetFileModificationTime --> Not implemented");
}

Status EnvNVM::RenameFile(
  const std::string& fpath_src,
  const std::string& fpath_tgt
) {
  NVM_DBG(this, "fpath_src(" << fpath_src << "), fpath_tgt(" << fpath_tgt << ")");

  FPathInfo info_src(fpath_src);
  FPathInfo info_tgt(fpath_tgt);

  if (info_src.nvm_managed() ^ info_src.nvm_managed()) {
    return Status::IOError(
      "Renaming a non-NVM file to a NVM file or the other way around."
    );
  }

  if (!info_src.nvm_managed()) {
    NVM_DBG(this, "delegating...");
    return posix_->RenameFile(fpath_src, fpath_tgt);
  }

  if (info_src.dpath().compare(info_tgt.dpath())) {
    return Status::IOError("Directory change not supported when renaming");
  }

  MutexLock lock(&fs_mutex_);

  NvmFile *file = FindFileUnguarded(info_src);
  if (!file) {
    return Status::NotFound();
  }

  NvmFile *file_target = FindFileUnguarded(info_tgt);
  if (file_target) {
    DeleteFileUnguarded(info_tgt);
  }

  file->Rename(info_tgt.fname());

  return Status::OK();
}

}       // namespace rocksdb
