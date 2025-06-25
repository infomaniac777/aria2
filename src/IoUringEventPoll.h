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
#ifndef D_IO_URING_EVENT_POLL_H
#define D_IO_URING_EVENT_POLL_H

#include "EventPoll.h"

#ifdef HAVE_LIBURING

#include <liburing.h>
#include <sys/epoll.h>
#include <map>
#include <memory>
#include <functional>

#include "Event.h"
#include "a2functional.h"
#include "Buffer.h"

#ifdef ENABLE_ASYNC_DNS
#  include "AsyncNameResolver.h"
#endif // ENABLE_ASYNC_DNS

namespace aria2 {

class IoUringEventPoll : public EventPoll {
public:
  // Event type constants for compatibility with existing code
  static const int IEV_READ = EPOLLIN;
  static const int IEV_WRITE = EPOLLOUT;
  static const int IEV_ERROR = EPOLLERR;
  static const int IEV_HUP = EPOLLHUP;
  static const int IEV_RW = IEV_READ | IEV_WRITE;

  // Async operation completion callback
  using AsyncCallback = std::function<void(ssize_t result, int error)>;
  
  // Async operation types
  enum class OpType {
    DISK_READ,
    DISK_WRITE,
    DISK_FSYNC
  };

private:
  class KSocketEntry;

  friend class AsyncNameResolverEntry<IoUringEventPoll>;

public:
  typedef Event<KSocketEntry> KEvent;
  typedef CommandEvent<KSocketEntry, IoUringEventPoll> KCommandEvent;
  typedef ADNSEvent<KSocketEntry, IoUringEventPoll> KADNSEvent;
  typedef AsyncNameResolverEntry<IoUringEventPoll> KAsyncNameResolverEntry;

private:

  class KSocketEntry : public SocketEntry<KCommandEvent, KADNSEvent> {
  public:
    KSocketEntry(sock_t socket);
    KSocketEntry(const KSocketEntry&) = delete;
    KSocketEntry(KSocketEntry&&) = default;
    
    struct epoll_event getEvents();
  };

  // Async operation tracking
  struct AsyncOp {
    OpType type;
    Buffer buffer;          // Keep buffer alive during async operation
    AsyncCallback callback; // Completion callback
    uint64_t id;           // Unique operation ID
    
    AsyncOp(OpType t, Buffer buf, AsyncCallback cb, uint64_t opId)
      : type(t), buffer(std::move(buf)), callback(std::move(cb)), id(opId) {}
  };

  typedef std::map<sock_t, KSocketEntry> KSocketEntrySet;
  KSocketEntrySet socketEntries_;

#ifdef ENABLE_ASYNC_DNS
  typedef std::map<std::pair<AsyncNameResolver*, Command*>,
                   KAsyncNameResolverEntry>
      KAsyncNameResolverEntrySet;
  KAsyncNameResolverEntrySet nameResolverEntries_;
#endif // ENABLE_ASYNC_DNS

  // io_uring state
  struct io_uring ring_;
  int ring_fd_;
  bool ring_initialized_;
  
  // Hybrid polling: epoll for network, io_uring for disk
  int epoll_fd_;
  size_t epoll_events_size_;
  std::unique_ptr<struct epoll_event[]> epoll_events_;
  
  // Async operation management
  uint64_t next_op_id_;
  std::map<uint64_t, std::unique_ptr<AsyncOp>> pending_ops_;
  
  // Internal methods
  bool initializeIoUring();
  void cleanupIoUring();
  bool addEpollEvents(sock_t socket, const KEvent& event);
  bool deleteEpollEvents(sock_t socket, const KEvent& event);
  void processIoUringCompletions();
  uint64_t getNextOpId() { return ++next_op_id_; }
  
  static const size_t EPOLL_EVENTS_MAX = 1024;
  static const unsigned URING_ENTRIES = 256;

public:
  IoUringEventPoll();
  virtual ~IoUringEventPoll();

  bool good() const { return ring_initialized_ && epoll_fd_ != -1; }

  virtual void poll(const struct timeval& tv) CXX11_OVERRIDE;

  virtual bool addEvents(sock_t socket, Command* command,
                         EventPoll::EventType events) CXX11_OVERRIDE;
  virtual bool deleteEvents(sock_t socket, Command* command,
                            EventPoll::EventType events) CXX11_OVERRIDE;

#ifdef ENABLE_ASYNC_DNS
  virtual bool
  addNameResolver(const std::shared_ptr<AsyncNameResolver>& resolver,
                  Command* command) CXX11_OVERRIDE;
  virtual bool
  deleteNameResolver(const std::shared_ptr<AsyncNameResolver>& resolver,
                     Command* command) CXX11_OVERRIDE;
#endif // ENABLE_ASYNC_DNS

  // io_uring async disk operations
  bool asyncRead(int fd, Buffer buffer, size_t bufferOffset, size_t length,
                 int64_t fileOffset, AsyncCallback callback);
  bool asyncWrite(int fd, Buffer buffer, size_t bufferOffset, size_t length,
                  int64_t fileOffset, AsyncCallback callback);
  bool asyncFsync(int fd, AsyncCallback callback);
  
  // Get io_uring completion eventfd for integration
  int getCompletionFd() const { return ring_fd_; }
};

} // namespace aria2

#endif // HAVE_LIBURING

#endif // D_IO_URING_EVENT_POLL_H 