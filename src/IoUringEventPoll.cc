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
#include "IoUringEventPoll.h"

#ifdef HAVE_LIBURING

#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>

#include "LogFactory.h"
#include "Logger.h"
#include "util.h"
#include "fmt.h"

namespace aria2 {

namespace {
int accumulateEvent(int events, const IoUringEventPoll::KEvent& event)
{
  return events | event.getEvents();
}
} // namespace

IoUringEventPoll::KSocketEntry::KSocketEntry(sock_t socket)
    : SocketEntry<KCommandEvent, KADNSEvent>(socket)
{
}

struct epoll_event IoUringEventPoll::KSocketEntry::getEvents()
{
  struct epoll_event epEvent;
  memset(&epEvent, 0, sizeof(struct epoll_event));
  epEvent.data.ptr = this;

#ifdef ENABLE_ASYNC_DNS
  epEvent.events =
      std::accumulate(adnsEvents_.begin(), adnsEvents_.end(),
                      std::accumulate(commandEvents_.begin(),
                                      commandEvents_.end(), 0, accumulateEvent),
                      accumulateEvent);
#else // !ENABLE_ASYNC_DNS
  epEvent.events = std::accumulate(commandEvents_.begin(), commandEvents_.end(),
                                   0, accumulateEvent);
#endif // !ENABLE_ASYNC_DNS
  return epEvent;
}

IoUringEventPoll::IoUringEventPoll()
    : ring_fd_(-1),
      ring_initialized_(false),
      epoll_fd_(-1),
      epoll_events_size_(EPOLL_EVENTS_MAX),
      epoll_events_(std::make_unique<struct epoll_event[]>(epoll_events_size_)),
      next_op_id_(0)
{
  // Initialize epoll for network I/O
  epoll_fd_ = epoll_create(EPOLL_EVENTS_MAX);
  if (epoll_fd_ == -1) {
    A2_LOG_ERROR("Failed to create epoll fd for IoUringEventPoll");
    return;
  }

  // Initialize io_uring for disk I/O
  if (!initializeIoUring()) {
    A2_LOG_ERROR("Failed to initialize io_uring");
    return;
  }

  A2_LOG_INFO("IoUringEventPoll initialized successfully");
}

IoUringEventPoll::~IoUringEventPoll()
{
  cleanupIoUring();
  if (epoll_fd_ != -1) {
    close(epoll_fd_);
  }
}

bool IoUringEventPoll::initializeIoUring()
{
  int ret = io_uring_queue_init(URING_ENTRIES, &ring_, 0);
  if (ret < 0) {
    A2_LOG_ERROR(fmt("io_uring_queue_init failed: %s", strerror(-ret)));
    return false;
  }

  ring_fd_ = ring_.ring_fd;
  ring_initialized_ = true;

  A2_LOG_DEBUG(fmt("io_uring initialized with %u entries, fd=%d", 
                   URING_ENTRIES, ring_fd_));
  return true;
}

void IoUringEventPoll::cleanupIoUring()
{
  if (ring_initialized_) {
    // Cancel any pending operations
    for (auto& [id, op] : pending_ops_) {
      A2_LOG_DEBUG(fmt("Cancelling pending io_uring operation %lu", id));
    }
    pending_ops_.clear();

    io_uring_queue_exit(&ring_);
    ring_initialized_ = false;
    ring_fd_ = -1;
  }
}

void IoUringEventPoll::poll(const struct timeval& tv)
{
  if (!good()) {
    return;
  }

  // Convert timeout to milliseconds
  int timeout_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;

  // Process io_uring completions first
  processIoUringCompletions();

  // Poll network events with epoll
  int res;
  while ((res = epoll_wait(epoll_fd_, epoll_events_.get(), EPOLL_EVENTS_MAX,
                           timeout_ms)) == -1 &&
         errno == EINTR)
    ;

  if (res > 0) {
    for (int i = 0; i < res; ++i) {
      KSocketEntry* p = reinterpret_cast<KSocketEntry*>(epoll_events_[i].data.ptr);
      p->processEvents(epoll_events_[i].events);
    }
  }
  else if (res == -1) {
    int errNum = errno;
    A2_LOG_INFO(fmt("epoll_wait error: %s", util::safeStrerror(errNum).c_str()));
  }

#ifdef ENABLE_ASYNC_DNS
  // Handle async DNS
  for (auto& i : nameResolverEntries_) {
    auto& ent = i.second;
    ent.processTimeout();
    ent.removeSocketEvents(this);
    ent.addSocketEvents(this);
  }
#endif // ENABLE_ASYNC_DNS
}

void IoUringEventPoll::processIoUringCompletions()
{
  struct io_uring_cqe* cqe;
  
  // Process all available completions
  while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
    uint64_t op_id = static_cast<uint64_t>(cqe->user_data);
    ssize_t result = cqe->res;
    
    A2_LOG_DEBUG(fmt("io_uring completion: op_id=%lu, result=%ld", op_id, result));
    
    auto it = pending_ops_.find(op_id);
    if (it != pending_ops_.end()) {
      auto& op = it->second;
      
      // Call completion callback
      if (op->callback) {
        int error = (result < 0) ? -result : 0;
        op->callback(result, error);
      }
      
      // Remove completed operation
      pending_ops_.erase(it);
    } else {
      A2_LOG_WARN(fmt("Unknown io_uring completion: op_id=%lu", op_id));
    }
    
    io_uring_cqe_seen(&ring_, cqe);
  }
}

namespace {
int translateEvents(EventPoll::EventType events)
{
  int newEvents = 0;
  if (EventPoll::EVENT_READ & events) {
    newEvents |= EPOLLIN;
  }
  if (EventPoll::EVENT_WRITE & events) {
    newEvents |= EPOLLOUT;
  }
  if (EventPoll::EVENT_ERROR & events) {
    newEvents |= EPOLLERR;
  }
  if (EventPoll::EVENT_HUP & events) {
    newEvents |= EPOLLHUP;
  }
  return newEvents;
}
} // namespace

bool IoUringEventPoll::addEpollEvents(sock_t socket, const KEvent& event)
{
  auto i = socketEntries_.lower_bound(socket);
  int r = 0;
  int errNum = 0;
  
  if (i != std::end(socketEntries_) && (*i).first == socket) {
    auto& socketEntry = (*i).second;
    event.addSelf(&socketEntry);
    
    struct epoll_event epEvent = socketEntry.getEvents();
    r = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, socketEntry.getSocket(), &epEvent);
    if (r == -1) {
      r = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socketEntry.getSocket(), &epEvent);
      errNum = errno;
    }
  }
  else {
    i = socketEntries_.insert(i, std::make_pair(socket, KSocketEntry(socket)));
    auto& socketEntry = (*i).second;
    
    if (socketEntries_.size() > epoll_events_size_) {
      epoll_events_size_ *= 2;
      epoll_events_ = std::make_unique<struct epoll_event[]>(epoll_events_size_);
    }
    
    event.addSelf(&socketEntry);
    struct epoll_event epEvent = socketEntry.getEvents();
    r = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socketEntry.getSocket(), &epEvent);
    errNum = errno;
  }
  
  if (r == -1) {
    A2_LOG_DEBUG(fmt("Failed to add socket event %d:%s", socket,
                     util::safeStrerror(errNum).c_str()));
    return false;
  }
  return true;
}

bool IoUringEventPoll::deleteEpollEvents(sock_t socket, const KEvent& event)
{
  auto i = socketEntries_.find(socket);
  if (i == std::end(socketEntries_)) {
    A2_LOG_DEBUG(fmt("Socket %d is not found in SocketEntries.", socket));
    return false;
  }
  
  auto& socketEntry = (*i).second;
  event.removeSelf(&socketEntry);
  int r = 0;
  int errNum = 0;
  
  if (socketEntry.eventEmpty()) {
    struct epoll_event ev = {0, {0}};
    r = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, socketEntry.getSocket(), &ev);
    errNum = errno;
    socketEntries_.erase(i);
  }
  else {
    struct epoll_event epEvent = socketEntry.getEvents();
    r = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, socketEntry.getSocket(), &epEvent);
    errNum = errno;
    if (r == -1) {
      A2_LOG_DEBUG(fmt("Failed to delete socket event, but may be ignored:%s",
                       util::safeStrerror(errNum).c_str()));
    }
  }
  
  if (r == -1) {
    A2_LOG_DEBUG(fmt("Failed to delete socket event:%s",
                     util::safeStrerror(errNum).c_str()));
    return false;
  }
  return true;
}

bool IoUringEventPoll::addEvents(sock_t socket, Command* command,
                                 EventPoll::EventType events)
{
  int epEvents = translateEvents(events);
  return addEpollEvents(socket, KCommandEvent(command, epEvents));
}

bool IoUringEventPoll::deleteEvents(sock_t socket, Command* command,
                                    EventPoll::EventType events)
{
  int epEvents = translateEvents(events);
  return deleteEpollEvents(socket, KCommandEvent(command, epEvents));
}

#ifdef ENABLE_ASYNC_DNS
bool IoUringEventPoll::addNameResolver(
    const std::shared_ptr<AsyncNameResolver>& resolver, Command* command)
{
  auto key = std::make_pair(resolver.get(), command);
  auto itr = nameResolverEntries_.lower_bound(key);
  if (itr != std::end(nameResolverEntries_) && (*itr).first == key) {
    return false;
  }
  nameResolverEntries_.insert(
      itr, std::make_pair(key, KAsyncNameResolverEntry(resolver, command)));
  return true;
}

bool IoUringEventPoll::deleteNameResolver(
    const std::shared_ptr<AsyncNameResolver>& resolver, Command* command)
{
  auto key = std::make_pair(resolver.get(), command);
  return nameResolverEntries_.erase(key) == 1;
}
#endif // ENABLE_ASYNC_DNS

bool IoUringEventPoll::asyncRead(int fd, Buffer buffer, size_t bufferOffset,
                                 size_t length, int64_t fileOffset,
                                 AsyncCallback callback)
{
  if (!ring_initialized_) {
    return false;
  }

  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    A2_LOG_WARN("io_uring submission queue full");
    return false;
  }

  uint64_t op_id = getNextOpId();
  
  // Store operation for completion tracking
  auto op = std::make_unique<AsyncOp>(OpType::DISK_READ, buffer, 
                                      std::move(callback), op_id);
  pending_ops_[op_id] = std::move(op);

  // Setup read operation
  unsigned char* data = buffer::data(buffer, bufferOffset);
  io_uring_prep_read(sqe, fd, data, length, fileOffset);
  sqe->user_data = static_cast<__u64>(op_id);

  // Submit the operation
  int ret = io_uring_submit(&ring_);
  if (ret < 0) {
    A2_LOG_ERROR(fmt("io_uring_submit failed: %s", strerror(-ret)));
    pending_ops_.erase(op_id);
    return false;
  }

  A2_LOG_DEBUG(fmt("Submitted async read: fd=%d, length=%zu, offset=%ld, op_id=%lu",
                   fd, length, fileOffset, op_id));
  return true;
}

bool IoUringEventPoll::asyncWrite(int fd, Buffer buffer, size_t bufferOffset,
                                  size_t length, int64_t fileOffset,
                                  AsyncCallback callback)
{
  if (!ring_initialized_) {
    return false;
  }

  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    A2_LOG_WARN("io_uring submission queue full");
    return false;
  }

  uint64_t op_id = getNextOpId();
  
  // Store operation for completion tracking
  auto op = std::make_unique<AsyncOp>(OpType::DISK_WRITE, buffer, 
                                      std::move(callback), op_id);
  pending_ops_[op_id] = std::move(op);

  // Setup write operation
  const unsigned char* data = buffer::cdata(buffer, bufferOffset);
  io_uring_prep_write(sqe, fd, data, length, fileOffset);
  sqe->user_data = static_cast<__u64>(op_id);

  // Submit the operation
  int ret = io_uring_submit(&ring_);
  if (ret < 0) {
    A2_LOG_ERROR(fmt("io_uring_submit failed: %s", strerror(-ret)));
    pending_ops_.erase(op_id);
    return false;
  }

  A2_LOG_DEBUG(fmt("Submitted async write: fd=%d, length=%zu, offset=%ld, op_id=%lu",
                   fd, length, fileOffset, op_id));
  return true;
}

bool IoUringEventPoll::asyncFsync(int fd, AsyncCallback callback)
{
  if (!ring_initialized_) {
    return false;
  }

  struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    A2_LOG_WARN("io_uring submission queue full");
    return false;
  }

  uint64_t op_id = getNextOpId();
  
  // Store operation for completion tracking
  auto op = std::make_unique<AsyncOp>(OpType::DISK_FSYNC, nullptr, 
                                      std::move(callback), op_id);
  pending_ops_[op_id] = std::move(op);

  // Setup fsync operation
  io_uring_prep_fsync(sqe, fd, 0);
  sqe->user_data = static_cast<__u64>(op_id);

  // Submit the operation
  int ret = io_uring_submit(&ring_);
  if (ret < 0) {
    A2_LOG_ERROR(fmt("io_uring_submit failed: %s", strerror(-ret)));
    pending_ops_.erase(op_id);
    return false;
  }

  A2_LOG_DEBUG(fmt("Submitted async fsync: fd=%d, op_id=%lu", fd, op_id));
  return true;
}

} // namespace aria2

#endif // HAVE_LIBURING