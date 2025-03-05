#ifndef D_IO_URING_DISK_WRITER_H
#define D_IO_URING_DISK_WRITER_H

#include "DiskWriter.h"

#include <liburing.h>
#include <memory>
#include <string>

namespace aria2 {

class IOUringDiskWriter : public DiskWriter {
private:
  int fd_;
  std::string filename_;
  bool readOnly_;
  int queueSize_;
  bool uringInitialized_;
  struct io_uring ring_;

public:
  IOUringDiskWriter(const std::string& filename);
  virtual ~IOUringDiskWriter();

  virtual void init();
  virtual void initAndOpenFile(int64_t totalLength = 0) CXX11_OVERRIDE;
  virtual void openFile(int64_t totalLength = 0) CXX11_OVERRIDE;
  virtual void closeFile() CXX11_OVERRIDE;
  virtual void openExistingFile(int64_t totalLength = 0);
  void createFile(int64_t totalLength = 0);

  virtual void writeData(const unsigned char* data, size_t len,
                         int64_t offset) CXX11_OVERRIDE;
                         
  virtual ssize_t readData(unsigned char* data, size_t len,
                           int64_t offset) CXX11_OVERRIDE;
  
  virtual int64_t size() CXX11_OVERRIDE;
  
  // Simple state methods
  virtual void enableReadOnly() CXX11_OVERRIDE;
  virtual void disableReadOnly() CXX11_OVERRIDE;
  
  // Helper method - not part of DiskWriter interface
  int getFd() const { return fd_; }
  
  // Implementation detail used internally
  void truncate(int64_t length);
};

} // namespace aria2

#endif // D_IO_URING_DISK_WRITER_H