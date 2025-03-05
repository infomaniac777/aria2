#include "IOUringDiskWriter.h"
#include "RecoverableException.h"

#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <sys/stat.h>
#include <sys/types.h>

#include "File.h"
#include "util.h"
#include "a2io.h"
#include "a2functional.h"
#include "fmt.h"
#include "LogFactory.h"
#include "DlAbortEx.h"
#include "message.h"

namespace aria2 {

IOUringDiskWriter::IOUringDiskWriter(const std::string& filename)
    : fd_(-1), 
      filename_(filename), 
      readOnly_(false),
      queueSize_(32),
      uringInitialized_(false),
      directIO_(false)
{
}

IOUringDiskWriter::~IOUringDiskWriter()
{
  closeFile();
}

void IOUringDiskWriter::init()
{
  if (fd_ != -1) {
    return;
  }
  
  if (uringInitialized_) {
    return;  // Already initialized
  }
  
  // Initialize IO_URING with queueSize_ entries
  int ret = io_uring_queue_init(queueSize_, &ring_, 0);
  if (ret < 0) {
    int errNum = errno;
    A2_LOG_ERROR(fmt("io_uring_queue_init failed: %s", strerror(errNum)));
    throw DL_ABORT_EX(fmt(EX_FILE_OPEN, filename_.c_str(), 
                          "io_uring initialization failed"));
  }
  uringInitialized_ = true;
  A2_LOG_DEBUG(fmt("IO_URING initialized"));
}

void IOUringDiskWriter::initAndOpenFile(int64_t totalLength)
{
  init();
  openFile(totalLength);
}

void IOUringDiskWriter::openFile(int64_t totalLength)
{
  init(); // Initialize io_uring first
  
  try {
    // First try to open as an existing file
    openExistingFile(totalLength);
  } 
  catch (RecoverableException& e) {
    if (e.getErrNum() == ENOENT) {
      // File doesn't exist, create it
      createFile(totalLength);
    } 
    else {
      // Some other error occurred, re-throw
      throw;
    }
  }
}

void IOUringDiskWriter::createFile(int64_t totalLength)
{
  if (fd_ != -1) {
    closeFile();
  }
  
  // No need to call init() here since openFile already does
  
  A2_LOG_DEBUG(fmt("Creating file %s", filename_.c_str()));
  // Create parent directories if they don't exist
  assert(!filename_.empty());
  util::mkdirs(File(filename_).getDirname());
  
  int flags = O_CREAT | O_RDWR | O_TRUNC | O_BINARY;
  
  // Add O_DIRECT flag when available
#ifdef HAVE_O_DIRECT
  if (directIO_) {
    flags |= O_DIRECT;
    A2_LOG_DEBUG(fmt("Direct I/O enabled for file %s", filename_.c_str()));
  }
#endif
  
  fd_ = open(filename_.c_str(), flags, OPEN_MODE);
  
  if (fd_ == -1) {
    int errNum = errno;
    A2_LOG_ERROR(fmt("Failed to create file %s, cause: %s", 
                    filename_.c_str(), util::safeStrerror(errNum).c_str()));
    throw DL_ABORT_EX(fmt(EX_FILE_OPEN, filename_.c_str(), 
                          util::safeStrerror(errNum).c_str()));
  }
  
  if (totalLength > 0) {
    // Set file length
    if (ftruncate(fd_, totalLength) == -1) {
      int errNum = errno;
      A2_LOG_ERROR(fmt("ftruncate failed for:%s, cause:%s", 
                     filename_.c_str(), util::safeStrerror(errNum).c_str()));
      throw DL_ABORT_EX(fmt("Failed to truncate file %s to size %ld", 
                            filename_.c_str(), totalLength));
    }
  }
  A2_LOG_DEBUG(fmt("File %s created with fd %d", filename_.c_str(), fd_));
}

void IOUringDiskWriter::openExistingFile(int64_t totalLength) 
{
  if (fd_ != -1) {
    closeFile();
  }
  
  if (!uringInitialized_) {
    init(); // Initialize io_uring first
  }
  // No need to call init() here since openFile already does
  
  A2_LOG_DEBUG(fmt("Opening existing file %s", filename_.c_str()));
  
  int flags = O_BINARY | O_RDWR;
  if (readOnly_) {
    flags = O_BINARY | O_RDONLY;
  }
  
  // Add O_DIRECT flag when available (only for read-write mode)
#ifdef HAVE_O_DIRECT
  if (directIO_ && !readOnly_) {
    flags |= O_DIRECT;
    A2_LOG_DEBUG(fmt("Direct I/O enabled for existing file %s", filename_.c_str()));
  }
#endif
  
  fd_ = open(filename_.c_str(), flags, OPEN_MODE);
  if (fd_ == -1) {
    int errNum = errno;
    A2_LOG_DEBUG(fmt("Failed to open file %s, cause: %s", 
                    filename_.c_str(), util::safeStrerror(errNum).c_str()));
    // The correct way to create a RecoverableException with file/line information
    RecoverableException e(__FILE__, __LINE__, errNum,
                         fmt(EX_FILE_OPEN, filename_.c_str(),
                             util::safeStrerror(errNum).c_str()));
    throw e;
  }
  
  A2_LOG_DEBUG(fmt("Existing file %s opened with fd %d", filename_.c_str(), fd_));
}

void IOUringDiskWriter::closeFile()
{
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }
  
  // Cleanup the IO_URING resources
  if (uringInitialized_) {
    io_uring_queue_exit(&ring_);
    uringInitialized_ = false;
  }
}

void IOUringDiskWriter::writeData(const unsigned char* data, size_t len, int64_t offset)
{
  A2_LOG_DEBUG(fmt("Writing %zu bytes to file %s at offset %ld", 
                  len, filename_.c_str(), offset));
  if (fd_ == -1) {
    throw DL_ABORT_EX(fmt("Cannot write to file %s: not opened yet", 
                          filename_.c_str()));
  }
  
  if (readOnly_) {
    throw DL_ABORT_EX(fmt("Cannot write to file %s: read-only mode", 
                          filename_.c_str()));
  }
  
  if (!uringInitialized_) {
    throw DL_ABORT_EX(fmt("Cannot write to file %s: io_uring not initialized", 
                          filename_.c_str()));
  }
  
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    A2_LOG_ERROR("io_uring_get_sqe failed: submission queue is full");
    throw DL_ABORT_EX("IO queue full, cannot write data");
  }
  
  io_uring_prep_write(sqe, fd_, data, len, offset);
  io_uring_submit(&ring_);
  
  struct io_uring_cqe* cqe;
  int ret = io_uring_wait_cqe(&ring_, &cqe);
  if (ret < 0) {
    A2_LOG_ERROR(fmt("io_uring_wait_cqe failed: %s", strerror(-ret)));
    throw DL_ABORT_EX(fmt("Failed to wait for completion: %s", strerror(-ret)));
  }
  
  if (cqe->res < 0) {
    A2_LOG_ERROR(fmt("write operation failed: %s", strerror(-cqe->res)));
    io_uring_cqe_seen(&ring_, cqe);
    throw DL_ABORT_EX(fmt("Failed to write to file %s: %s", 
                          filename_.c_str(), strerror(-cqe->res)));
  }
  
  if (static_cast<size_t>(cqe->res) != len) {
    io_uring_cqe_seen(&ring_, cqe);
    throw DL_ABORT_EX(fmt("Short write to file %s", filename_.c_str()));
  }
  
  io_uring_cqe_seen(&ring_, cqe);
}

ssize_t IOUringDiskWriter::readData(unsigned char* data, size_t len, int64_t offset)
{
  A2_LOG_DEBUG(fmt("Reading %zu bytes from file %s at offset %ld", 
                  len, filename_.c_str(), offset));
  if (fd_ == -1) {
    throw DL_ABORT_EX(fmt("Cannot read from file %s: not opened yet", 
                          filename_.c_str()));
  }
  
  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    A2_LOG_ERROR("io_uring_get_sqe failed: submission queue is full");
    throw DL_ABORT_EX("IO queue full, cannot read data");
  }
  
  io_uring_prep_read(sqe, fd_, data, len, offset);
  io_uring_submit(&ring_);
  
  struct io_uring_cqe* cqe;
  int ret = io_uring_wait_cqe(&ring_, &cqe);
  if (ret < 0) {
    A2_LOG_ERROR(fmt("io_uring_wait_cqe failed: %s", strerror(-ret)));
    throw DL_ABORT_EX(fmt("Failed to wait for completion: %s", strerror(-ret)));
  }
  
  if (cqe->res < 0) {
    A2_LOG_ERROR(fmt("read operation failed: %s", strerror(-cqe->res)));
    io_uring_cqe_seen(&ring_, cqe);
    throw DL_ABORT_EX(fmt("Failed to read from file %s: %s", 
                          filename_.c_str(), strerror(-cqe->res)));
  }
  
  ssize_t readLength = cqe->res;
  io_uring_cqe_seen(&ring_, cqe);
  return readLength;
}

void IOUringDiskWriter::truncate(int64_t length)
{
  if (fd_ == -1) {
    throw DL_ABORT_EX(fmt("Cannot truncate file %s: not opened yet", 
                          filename_.c_str()));
  }
  
  if (readOnly_) {
    throw DL_ABORT_EX(fmt("Cannot truncate file %s: read-only mode", 
                          filename_.c_str()));
  }
  
  if (ftruncate(fd_, length) == -1) {
    int errNum = errno;
    throw DL_ABORT_EX(fmt("Failed to truncate file %s: %s", 
                      filename_.c_str(), util::safeStrerror(errNum).c_str()));
  }
}

void IOUringDiskWriter::enableReadOnly()
{
  readOnly_ = true;
}

void IOUringDiskWriter::disableReadOnly()
{
  readOnly_ = false;
}

int64_t IOUringDiskWriter::size()
{
  if (fd_ == -1) {
    throw DL_ABORT_EX(fmt("Cannot get file size of %s: not opened yet", 
                          filename_.c_str()));
  }
  
  struct stat st;
  if (fstat(fd_, &st) == -1) {
    int errNum = errno;
    throw DL_ABORT_EX(fmt("fstat failed for %s, cause: %s", 
                      filename_.c_str(), util::safeStrerror(errNum).c_str()));
  }
  
  return st.st_size;
}

} // namespace aria2