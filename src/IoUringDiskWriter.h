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
#ifndef D_IO_URING_DISK_WRITER_H
#define D_IO_URING_DISK_WRITER_H

#include "DiskWriter.h"

#ifdef HAVE_LIBURING

#include <string>
#include <memory>
#include <queue>
#include <functional>

#include "a2io.h"
#include "Buffer.h"

namespace aria2 {

class IoUringEventPoll;

class IoUringDiskWriter : public DiskWriter {
public:
  // Completion callback for async operations
  using CompletionCallback = std::function<void(ssize_t result, int error)>;

private:
  std::string filename_;

#ifdef __MINGW32__
  HANDLE fd_;
#else  // !__MINGW32__
  int fd_;
#endif // !__MINGW32__

  bool readOnly_;
  bool enableMmap_;
  unsigned char* mapaddr_;
  int64_t maplen_;
#ifdef __MINGW32__
  HANDLE mapView_;
#endif // __MINGW32__

  // Reference to io_uring event poll for async operations
  IoUringEventPoll* ioUringPoll_;

  // Pending operations queue for synchronous fallback
  struct PendingOp {
    enum Type { READ, WRITE, FSYNC };
    Type type;
    Buffer buffer;
    size_t bufferOffset;
    size_t length;
    int64_t fileOffset;
    CompletionCallback callback;
    
    PendingOp(Type t, Buffer buf, size_t bufOff, size_t len, int64_t fileOff, CompletionCallback cb)
      : type(t), buffer(std::move(buf)), bufferOffset(bufOff), length(len), 
        fileOffset(fileOff), callback(std::move(cb)) {}
  };

  std::queue<PendingOp> pendingOps_;
  bool asyncMode_;

  // Internal methods
  void seek(int64_t offset);
  void ensureMmapWrite(size_t len, int64_t offset);
  ssize_t syncWrite(Buffer buffer, size_t bufferOffset, size_t length, int64_t fileOffset);
  ssize_t syncRead(Buffer buffer, size_t bufferOffset, size_t length, int64_t fileOffset);
  void processPendingOps();
  
protected:
  void createFile(int addFlags = 0);

public:
  IoUringDiskWriter(const std::string& filename, IoUringEventPoll* ioUringPoll = nullptr);
  virtual ~IoUringDiskWriter();

  virtual void initAndOpenFile(int64_t totalLength = 0) CXX11_OVERRIDE;
  virtual void openFile(int64_t totalLength = 0) CXX11_OVERRIDE;
  virtual void closeFile() CXX11_OVERRIDE;
  virtual void openExistingFile(int64_t totalLength = 0) CXX11_OVERRIDE;

  // Primary async interface
  virtual void writeData(Buffer buffer, size_t bufferOffset, size_t length,
                        int64_t fileOffset) CXX11_OVERRIDE;
  virtual ssize_t readData(Buffer buffer, size_t bufferOffset, size_t length,
                          int64_t fileOffset) CXX11_OVERRIDE;

  virtual int64_t size() CXX11_OVERRIDE;
  virtual void truncate(int64_t length) CXX11_OVERRIDE;
  virtual void allocate(int64_t offset, int64_t length, bool sparse) CXX11_OVERRIDE;

  virtual void enableReadOnly() CXX11_OVERRIDE;
  virtual void disableReadOnly() CXX11_OVERRIDE;
  virtual void enableMmap() CXX11_OVERRIDE;
  virtual void dropCache(int64_t len, int64_t offset) CXX11_OVERRIDE;
  virtual void flushOSBuffers() CXX11_OVERRIDE;

  // Async-specific methods
  void writeDataAsync(Buffer buffer, size_t bufferOffset, size_t length,
                     int64_t fileOffset, CompletionCallback callback);
  void readDataAsync(Buffer buffer, size_t bufferOffset, size_t length,
                    int64_t fileOffset, CompletionCallback callback);
  
  // Set io_uring event poll (can be nullptr for synchronous fallback)
  void setIoUringPoll(IoUringEventPoll* poll);
  
  // Check if async operations are available
  bool isAsyncAvailable() const { return ioUringPoll_ != nullptr && asyncMode_; }

private:
  // File error handling
  int fileError() const;
  std::string fileStrerror(int errNum) const;
};

} // namespace aria2

#endif // HAVE_LIBURING

#endif // D_IO_URING_DISK_WRITER_H 