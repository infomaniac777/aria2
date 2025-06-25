/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
/* copyright --> */
#include "IoUringDiskWriter.h"

#ifdef HAVE_LIBURING

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <cstring>
#include <cassert>

#ifdef HAVE_MMAP
#  include <sys/mman.h>
#endif // HAVE_MMAP

#ifdef HAVE_SOME_FALLOCATE
#  include <linux/falloc.h>
#endif // HAVE_SOME_FALLOCATE

#ifdef HAVE_POSIX_FADVISE
#  include <fcntl.h>
#endif // HAVE_POSIX_FADVISE

#include "IoUringEventPoll.h"
#include "DlAbortEx.h"
#include "error_code.h"
#include "fmt.h"
#include "File.h"
#include "util.h"
#include "LogFactory.h"
#include "Logger.h"
#include "DownloadFailureException.h"
#include "message.h"

namespace aria2 {

namespace {
#ifdef __MINGW32__
int openFileWithFlags(const std::string& filename, int flags, error_code::Value errCode)
{
  HANDLE fd;
  DWORD dwDesiredAccess = 0;
  DWORD dwCreationDisposition = 0;
  DWORD dwFlagsAndAttributes = FILE_ATTRIBUTE_NORMAL;

  if (flags & O_RDONLY) {
    dwDesiredAccess |= GENERIC_READ;
  }
  if (flags & O_WRONLY) {
    dwDesiredAccess |= GENERIC_WRITE;
  }
  if (flags & O_RDWR) {
    dwDesiredAccess |= GENERIC_READ | GENERIC_WRITE;
  }
  if (flags & O_CREAT) {
    if (flags & O_TRUNC) {
      dwCreationDisposition = CREATE_ALWAYS;
    } else {
      dwCreationDisposition = CREATE_NEW;
    }
  } else if (flags & O_TRUNC) {
    dwCreationDisposition = TRUNCATE_EXISTING;
  } else {
    dwCreationDisposition = OPEN_EXISTING;
  }

  std::wstring wfilename = utf8ToWChar(filename);
  fd = CreateFileW(wfilename.c_str(), dwDesiredAccess, FILE_SHARE_READ,
                   0, dwCreationDisposition, dwFlagsAndAttributes, 0);
  if (fd == INVALID_HANDLE_VALUE) {
    int errNum = GetLastError();
    throw DL_ABORT_EX3(errNum, fmt(EX_FILE_OPEN, filename.c_str(),
                                   util::errnoToString(errNum).c_str()),
                       errCode);
  }
  return fd;
}
#else  // !__MINGW32__
int openFileWithFlags(const std::string& filename, int flags, error_code::Value errCode)
{
  int fd;
  while ((fd = a2open(filename.c_str(), flags, OPEN_MODE)) == -1 && errno == EINTR)
    ;
  if (fd < 0) {
    int errNum = errno;
    throw DL_ABORT_EX3(errNum, fmt(EX_FILE_OPEN, filename.c_str(),
                                   util::safeStrerror(errNum).c_str()),
                       errCode);
  }
  util::make_fd_cloexec(fd);
#  if defined(__APPLE__) && defined(__MACH__)
  fcntl(fd, F_NOCACHE, 1);
#  endif // __APPLE__ && __MACH__
  return fd;
}
#endif   // !__MINGW32__

bool isDiskFullError(int errNum)
{
  return
#ifdef __MINGW32__
      errNum == ERROR_DISK_FULL || errNum == ERROR_HANDLE_DISK_FULL
#else  // !__MINGW32__
      errNum == ENOSPC
#endif // !__MINGW32__
      ;
}
} // namespace

IoUringDiskWriter::IoUringDiskWriter(const std::string& filename, IoUringEventPoll* ioUringPoll)
    : filename_(filename),
      fd_(A2_BAD_FD),
      readOnly_(false),
      enableMmap_(false),
      mapaddr_(nullptr),
      maplen_(0),
      ioUringPoll_(ioUringPoll),
      asyncMode_(false)
{
  asyncMode_ = (ioUringPoll_ != nullptr);
}

IoUringDiskWriter::~IoUringDiskWriter()
{
  closeFile();
}

void IoUringDiskWriter::openExistingFile(int64_t totalLength)
{
  int flags = O_BINARY;
  if (readOnly_) {
    flags |= O_RDONLY;
  } else {
    flags |= O_RDWR;
  }
  fd_ = openFileWithFlags(filename_, flags, error_code::FILE_OPEN_ERROR);
}

void IoUringDiskWriter::createFile(int addFlags)
{
  assert(!filename_.empty());
  util::mkdirs(File(filename_).getDirname());
  fd_ = openFileWithFlags(filename_,
                          O_CREAT | O_RDWR | O_TRUNC | O_BINARY | addFlags,
                          error_code::FILE_CREATE_ERROR);
}

void IoUringDiskWriter::initAndOpenFile(int64_t totalLength)
{
  if (fd_ != A2_BAD_FD) {
    closeFile();
  }
  createFile();
}

void IoUringDiskWriter::openFile(int64_t totalLength)
{
  if (fd_ != A2_BAD_FD) {
    closeFile();
  }
  if (File(filename_).exists()) {
    openExistingFile(totalLength);
  } else {
    createFile();
  }
}

void IoUringDiskWriter::closeFile()
{
#ifdef HAVE_MMAP
  if (mapaddr_) {
    int errNum = 0;
#  ifdef __MINGW32__
    if (!UnmapViewOfFile(mapaddr_)) {
      errNum = GetLastError();
    }
    CloseHandle(mapView_);
    mapView_ = INVALID_HANDLE_VALUE;
#  else  // !__MINGW32__
    if (munmap(mapaddr_, maplen_) == -1) {
      errNum = errno;
    }
#  endif // !__MINGW32__
    if (errNum != 0) {
      A2_LOG_ERROR(fmt("Unmapping file %s failed: %s", filename_.c_str(),
                       fileStrerror(errNum).c_str()));
    }
    mapaddr_ = nullptr;
    maplen_ = 0;
  }
#endif // HAVE_MMAP

  if (fd_ != A2_BAD_FD) {
#ifdef __MINGW32__
    CloseHandle(fd_);
#else  // !__MINGW32__
    close(fd_);
#endif // !__MINGW32__
    fd_ = A2_BAD_FD;
  }
}

ssize_t IoUringDiskWriter::syncWrite(Buffer buffer, size_t bufferOffset,
                                     size_t length, int64_t fileOffset)
{
  const unsigned char* data = buffer::cdata(buffer, bufferOffset);

  if (mapaddr_) {
    std::copy_n(data, length, mapaddr_ + fileOffset);
    return length;
  } else {
    ssize_t writtenLength = 0;
    seek(fileOffset);
    while ((size_t)writtenLength < length) {
#ifdef __MINGW32__
      DWORD nwrite;
      if (WriteFile(fd_, data + writtenLength, length - writtenLength, &nwrite, 0)) {
        writtenLength += nwrite;
      } else {
        return -1;
      }
#else  // !__MINGW32__
      ssize_t ret = 0;
      while ((ret = write(fd_, data + writtenLength, length - writtenLength)) == -1 &&
             errno == EINTR)
        ;
      if (ret == -1) {
        return -1;
      }
      writtenLength += ret;
#endif // !__MINGW32__
    }
    return writtenLength;
  }
}

ssize_t IoUringDiskWriter::syncRead(Buffer buffer, size_t bufferOffset,
                                    size_t length, int64_t fileOffset)
{
  unsigned char* data = buffer::data(buffer, bufferOffset);

  if (mapaddr_) {
    if (fileOffset >= maplen_) {
      return 0;
    }
    auto readlen = std::min(maplen_ - fileOffset, static_cast<int64_t>(length));
    std::copy_n(mapaddr_ + fileOffset, readlen, data);
    return readlen;
  } else {
    seek(fileOffset);
#ifdef __MINGW32__
    DWORD nread;
    if (ReadFile(fd_, data, length, &nread, 0)) {
      return nread;
    } else {
      return -1;
    }
#else  // !__MINGW32__
    ssize_t ret = 0;
    while ((ret = read(fd_, data, length)) == -1 && errno == EINTR)
      ;
    return ret;
#endif // !__MINGW32__
  }
}

void IoUringDiskWriter::writeData(Buffer buffer, size_t bufferOffset, size_t length,
                                  int64_t fileOffset)
{
  if (isAsyncAvailable()) {
    // Async path - submit to io_uring
    writeDataAsync(buffer, bufferOffset, length, fileOffset,
                   [this](ssize_t result, int error) {
                     if (result < 0) {
                       if (isDiskFullError(error)) {
                         throw DOWNLOAD_FAILURE_EXCEPTION3(
                             error,
                             fmt(EX_FILE_WRITE, filename_.c_str(), 
                                 fileStrerror(error).c_str()),
                             error_code::NOT_ENOUGH_DISK_SPACE);
                       } else {
                         throw DL_ABORT_EX3(
                             error,
                             fmt(EX_FILE_WRITE, filename_.c_str(), 
                                 fileStrerror(error).c_str()),
                             error_code::FILE_IO_ERROR);
                       }
                     }
                   });
  } else {
    // Synchronous fallback
    ensureMmapWrite(length, fileOffset);
    if (syncWrite(buffer, bufferOffset, length, fileOffset) < 0) {
      int errNum = fileError();
      if (isDiskFullError(errNum)) {
        throw DOWNLOAD_FAILURE_EXCEPTION3(
            errNum,
            fmt(EX_FILE_WRITE, filename_.c_str(), fileStrerror(errNum).c_str()),
            error_code::NOT_ENOUGH_DISK_SPACE);
      } else {
        throw DL_ABORT_EX3(
            errNum,
            fmt(EX_FILE_WRITE, filename_.c_str(), fileStrerror(errNum).c_str()),
            error_code::FILE_IO_ERROR);
      }
    }
  }
}

ssize_t IoUringDiskWriter::readData(Buffer buffer, size_t bufferOffset, size_t length,
                                    int64_t fileOffset)
{
  if (isAsyncAvailable()) {
    // For now, read operations remain synchronous as they're often needed immediately
    // In the future, this could be made async with proper callback handling
    ssize_t ret = syncRead(buffer, bufferOffset, length, fileOffset);
    if (ret < 0) {
      int errNum = fileError();
      throw DL_ABORT_EX3(errNum, fmt(EX_FILE_READ, filename_.c_str(),
                                     fileStrerror(errNum).c_str()),
                         error_code::FILE_IO_ERROR);
    }
    return ret;
  } else {
    // Synchronous fallback
    ssize_t ret = syncRead(buffer, bufferOffset, length, fileOffset);
    if (ret < 0) {
      int errNum = fileError();
      throw DL_ABORT_EX3(errNum, fmt(EX_FILE_READ, filename_.c_str(),
                                     fileStrerror(errNum).c_str()),
                         error_code::FILE_IO_ERROR);
    }
    return ret;
  }
}

void IoUringDiskWriter::writeDataAsync(Buffer buffer, size_t bufferOffset, size_t length,
                                       int64_t fileOffset, CompletionCallback callback)
{
  if (!ioUringPoll_) {
    A2_LOG_WARN("Async write requested but no io_uring poll available");
    // Fall back to synchronous operation
    try {
      ssize_t result = syncWrite(buffer, bufferOffset, length, fileOffset);
      if (callback) {
        callback(result, (result < 0) ? fileError() : 0);
      }
    } catch (const std::exception& e) {
      if (callback) {
        callback(-1, fileError());
      }
    }
    return;
  }

  ensureMmapWrite(length, fileOffset);
  
  if (mapaddr_) {
    // Memory-mapped files are synchronous
    const unsigned char* data = buffer::cdata(buffer, bufferOffset);
    std::copy_n(data, length, mapaddr_ + fileOffset);
    if (callback) {
      callback(length, 0);
    }
  } else {
    // Submit async write to io_uring
    bool success = ioUringPoll_->asyncWrite(fd_, buffer, bufferOffset, length, fileOffset,
                                            std::move(callback));
    if (!success) {
      A2_LOG_WARN("Failed to submit async write, falling back to sync");
      try {
        ssize_t result = syncWrite(buffer, bufferOffset, length, fileOffset);
        if (callback) {
          callback(result, (result < 0) ? fileError() : 0);
        }
      } catch (const std::exception& e) {
        if (callback) {
          callback(-1, fileError());
        }
      }
    }
  }
}

void IoUringDiskWriter::readDataAsync(Buffer buffer, size_t bufferOffset, size_t length,
                                      int64_t fileOffset, CompletionCallback callback)
{
  if (!ioUringPoll_) {
    A2_LOG_WARN("Async read requested but no io_uring poll available");
    // Fall back to synchronous operation
    try {
      ssize_t result = syncRead(buffer, bufferOffset, length, fileOffset);
      if (callback) {
        callback(result, (result < 0) ? fileError() : 0);
      }
    } catch (const std::exception& e) {
      if (callback) {
        callback(-1, fileError());
      }
    }
    return;
  }

  if (mapaddr_) {
    // Memory-mapped files are synchronous
    unsigned char* data = buffer::data(buffer, bufferOffset);
    if (fileOffset >= maplen_) {
      if (callback) callback(0, 0);
      return;
    }
    auto readlen = std::min(maplen_ - fileOffset, static_cast<int64_t>(length));
    std::copy_n(mapaddr_ + fileOffset, readlen, data);
    if (callback) {
      callback(readlen, 0);
    }
  } else {
    // Submit async read to io_uring
    bool success = ioUringPoll_->asyncRead(fd_, buffer, bufferOffset, length, fileOffset,
                                           std::move(callback));
    if (!success) {
      A2_LOG_WARN("Failed to submit async read, falling back to sync");
      try {
        ssize_t result = syncRead(buffer, bufferOffset, length, fileOffset);
        if (callback) {
          callback(result, (result < 0) ? fileError() : 0);
        }
      } catch (const std::exception& e) {
        if (callback) {
          callback(-1, fileError());
        }
      }
    }
  }
}

void IoUringDiskWriter::seek(int64_t offset)
{
  assert(offset >= 0);
#ifdef __MINGW32__
  LARGE_INTEGER fileLength;
  fileLength.QuadPart = offset;
  if (SetFilePointerEx(fd_, fileLength, 0, FILE_BEGIN) == 0)
#else  // !__MINGW32__
  if (a2lseek(fd_, offset, SEEK_SET) == (a2_off_t)-1)
#endif // !__MINGW32__
  {
    int errNum = fileError();
    throw DL_ABORT_EX2(
        fmt(EX_FILE_SEEK, filename_.c_str(), fileStrerror(errNum).c_str()),
        error_code::FILE_IO_ERROR);
  }
}

void IoUringDiskWriter::ensureMmapWrite(size_t len, int64_t offset)
{
#if defined(HAVE_MMAP) || defined(__MINGW32__)
  if (enableMmap_) {
    if (mapaddr_) {
      if (static_cast<int64_t>(len + offset) > maplen_) {
        int errNum = 0;
#  ifdef __MINGW32__
        if (!UnmapViewOfFile(mapaddr_)) {
          errNum = GetLastError();
        }
        CloseHandle(mapView_);
        mapView_ = INVALID_HANDLE_VALUE;
#  else  // !__MINGW32__
        if (munmap(mapaddr_, maplen_) == -1) {
          errNum = errno;
        }
#  endif // !__MINGW32__
        if (errNum != 0) {
          A2_LOG_ERROR(fmt("Unmapping file %s failed: %s", filename_.c_str(),
                           fileStrerror(errNum).c_str()));
        }
        mapaddr_ = nullptr;
        maplen_ = 0;
        enableMmap_ = false;
      }
    } else {
      int64_t filesize = size();
      if (filesize == 0) {
        enableMmap_ = false;
        return;
      }
      if (static_cast<uint64_t>(std::numeric_limits<size_t>::max()) <
          static_cast<uint64_t>(filesize)) {
        enableMmap_ = false;
        return;
      }
      int errNum = 0;
      if (static_cast<int64_t>(len + offset) <= filesize) {
#  ifdef __MINGW32__
        mapView_ = CreateFileMapping(fd_, 0, PAGE_READWRITE, filesize >> 32,
                                     filesize & 0xffffffffu, 0);
        if (mapView_) {
          mapaddr_ = reinterpret_cast<unsigned char*>(
              MapViewOfFile(mapView_, FILE_MAP_WRITE, 0, 0, 0));
          if (!mapaddr_) {
            errNum = GetLastError();
            CloseHandle(mapView_);
            mapView_ = INVALID_HANDLE_VALUE;
          }
        } else {
          errNum = GetLastError();
        }
#  else  // !__MINGW32__
        auto pa = mmap(nullptr, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (pa == MAP_FAILED) {
          errNum = errno;
        } else {
          mapaddr_ = reinterpret_cast<unsigned char*>(pa);
        }
#  endif // !__MINGW32__
        if (mapaddr_) {
          A2_LOG_DEBUG(fmt("Mapping file %s succeeded, length=%" PRId64 "",
                           filename_.c_str(), static_cast<uint64_t>(filesize)));
          maplen_ = filesize;
        } else {
          A2_LOG_WARN(fmt("Mapping file %s failed: %s", filename_.c_str(),
                          fileStrerror(errNum).c_str()));
          enableMmap_ = false;
        }
      }
    }
  }
#endif // HAVE_MMAP || __MINGW32__
}

void IoUringDiskWriter::truncate(int64_t length)
{
  if (fd_ == A2_BAD_FD) {
    throw DL_ABORT_EX("File not yet opened.");
  }
#ifdef __MINGW32__
  seek(length);
  if (SetEndOfFile(fd_) == 0)
#else  // !__MINGW32__
  if (a2ftruncate(fd_, length) == -1)
#endif // !__MINGW32__
  {
    int errNum = fileError();
    throw DL_ABORT_EX2(
        fmt("File truncation failed. cause: %s", fileStrerror(errNum).c_str()),
        error_code::FILE_IO_ERROR);
  }
}

void IoUringDiskWriter::allocate(int64_t offset, int64_t length, bool sparse)
{
  if (fd_ == A2_BAD_FD) {
    throw DL_ABORT_EX("File not yet opened.");
  }
  if (sparse) {
#ifdef __MINGW32__
    DWORD bytesReturned;
    if (!DeviceIoControl(fd_, FSCTL_SET_SPARSE, 0, 0, 0, 0, &bytesReturned, 0)) {
      A2_LOG_WARN(fmt("Making file sparse failed or pending: %s",
                      fileStrerror(GetLastError()).c_str()));
    }
#endif // __MINGW32__
    truncate(offset + length);
    return;
  }
#ifdef HAVE_SOME_FALLOCATE
  // Implementation for fallocate would go here
#endif // HAVE_SOME_FALLOCATE
}

int64_t IoUringDiskWriter::size() { return File(filename_).size(); }

void IoUringDiskWriter::enableReadOnly() { readOnly_ = true; }
void IoUringDiskWriter::disableReadOnly() { readOnly_ = false; }
void IoUringDiskWriter::enableMmap() { enableMmap_ = true; }

void IoUringDiskWriter::dropCache(int64_t len, int64_t offset)
{
#ifdef HAVE_POSIX_FADVISE
  posix_fadvise(fd_, offset, len, POSIX_FADV_DONTNEED);
#endif // HAVE_POSIX_FADVISE
}

void IoUringDiskWriter::flushOSBuffers()
{
  if (fd_ == A2_BAD_FD) {
    return;
  }
  
  if (isAsyncAvailable()) {
    // Submit async fsync
    ioUringPoll_->asyncFsync(fd_, [this](ssize_t result, int error) {
      if (result < 0) {
        A2_LOG_WARN(fmt("Async fsync failed: %s", fileStrerror(error).c_str()));
      }
    });
  } else {
    // Synchronous fsync
#ifdef __MINGW32__
    FlushFileBuffers(fd_);
#else  // !__MINGW32__
    fsync(fd_);
#endif // !__MINGW32__
  }
}

void IoUringDiskWriter::setIoUringPoll(IoUringEventPoll* poll)
{
  ioUringPoll_ = poll;
  asyncMode_ = (poll != nullptr);
}

int IoUringDiskWriter::fileError() const
{
#ifdef __MINGW32__
  return GetLastError();
#else  // !__MINGW32__
  return errno;
#endif // !__MINGW32__
}

std::string IoUringDiskWriter::fileStrerror(int errNum) const
{
#ifdef __MINGW32__
  return util::errnoToString(errNum);
#else  // !__MINGW32__
  return util::safeStrerror(errNum);
#endif // !__MINGW32__
}

} // namespace aria2

#endif // HAVE_LIBURING 