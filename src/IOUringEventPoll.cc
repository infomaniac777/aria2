/* <!-- copyright -->
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2009 Tatsuhiro Tsujikawa
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
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "IOUringEventPoll.h"

#include <cerrno>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <poll.h>

#include "Command.h"
#include "LogFactory.h"
#include "Logger.h"
#include "util.h"
#include "a2functional.h"
#include "fmt.h"

namespace aria2 {

// Define IO_URING_ENTRIES
#define IO_URING_ENTRIES 256

IoUringEventPoll::KSocketEntry::KSocketEntry(sock_t s)
    : SocketEntry<KCommandEvent, KADNSEvent>(s)
{
}

int accumulateEvent(int events, const IoUringEventPoll::KEvent& event)
{
  return events | event.getEvents();
}

// Keep the same translateEvents function
namespace {
    uint32_t translateEvents(EventPoll::EventType events)
    {
      uint32_t newEvents = 0;
      if (EventPoll::EVENT_READ & events) {
        newEvents |= POLLIN;
      }
      if (EventPoll::EVENT_WRITE & events) {
        newEvents |= POLLOUT;
      }
      if (EventPoll::EVENT_ERROR & events) {
        newEvents |= POLLERR;
      }
      if (EventPoll::EVENT_HUP & events) {
        newEvents |= POLLHUP;
      }
      return newEvents;
    }
    } // namespace


uint32_t IoUringEventPoll::KSocketEntry::getEvents()
{
  uint32_t events = 0;

#ifdef ENABLE_ASYNC_DNS
  // Combine events from both command and DNS events
  events = std::accumulate(adnsEvents_.begin(), adnsEvents_.end(),
                          std::accumulate(commandEvents_.begin(),
                                        commandEvents_.end(), 0, accumulateEvent),
                          accumulateEvent);
#else
  // Only use command events
  events = std::accumulate(commandEvents_.begin(), commandEvents_.end(), 0, accumulateEvent);
#endif

  return events;
}


IoUringEventPoll::IoUringEventPoll()
    : ringEntriesSize_(IO_URING_ENTRIES)
{
  // Initialize io_uring instance
  int ret = io_uring_queue_init(ringEntriesSize_, &ring_, 0);
  if (ret < 0) {
    A2_LOG_ERROR(fmt("Failed to initialize io_uring: %s", 
                   util::safeStrerror(-ret).c_str()));
    good_ = false;
  } else {
    good_ = true;
  }
}

IoUringEventPoll::~IoUringEventPoll()
{
  if (good_) {
    io_uring_queue_exit(&ring_);
  }
}

bool IoUringEventPoll::good() const { return good_; }

void IoUringEventPoll::poll(const struct timeval& tv)
{
  // Convert timeval to timespec for io_uring
  struct __kernel_timespec ts;
  ts.tv_sec = tv.tv_sec;
  ts.tv_nsec = tv.tv_usec * 1000;
  
  // Submit any pending operations
  int ret = io_uring_submit(&ring_);
  if (ret < 0) {
    A2_LOG_INFO(fmt("io_uring_submit error: %s", 
                  util::safeStrerror(-ret).c_str()));
  }
  
  // Process completions
  struct io_uring_cqe *cqe = nullptr;
  
  // Wait for events with timeout
  ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
  if (ret < 0 && ret != -ETIME) {
    A2_LOG_INFO(fmt("io_uring_wait_cqe_timeout error: %s", 
                  util::safeStrerror(-ret).c_str()));
  }
  
  // Process all available completions
  unsigned head;
  unsigned count = 0;
  io_uring_for_each_cqe(&ring_, head, cqe) {
    if (cqe->user_data != 0) {
      // Get the socket fd from user_data
      sock_t socket = static_cast<sock_t>(cqe->user_data);
      
      // Look up the socket entry
      auto it = socketEntries_.find(socket);
      if (it != std::end(socketEntries_)) {
        // Convert result to events
        uint32_t events = 0;
        if (cqe->res & POLLIN) events |= IEV_READ;
        if (cqe->res & POLLOUT) events |= IEV_WRITE;
        if (cqe->res & POLLERR) events |= IEV_ERROR;
        if (cqe->res & POLLHUP) events |= IEV_HUP;
        
        // Process the events
        it->second.processEvents(events);
        
        // Mark as unregistered so it will be re-added
        socketsNeedingRegistration_.insert(socket);
      }
    }
    count++;
  }
  
  // Mark that we've consumed these events
  io_uring_cq_advance(&ring_, count);
  // Re-register sockets that need monitoring
  auto it = socketsNeedingRegistration_.begin();
  while (it != socketsNeedingRegistration_.end()) {
    sock_t socket = *it;
    it = socketsNeedingRegistration_.erase(it); // it now points to next element
    
    auto entryIt = socketEntries_.find(socket);
    if (entryIt == socketEntries_.end() || entryIt->second.eventEmpty()) {
      continue;
    }
    
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
      uint32_t events = entryIt->second.getEvents();
      io_uring_prep_poll_add(sqe, entryIt->second.getSocket(), events);
      io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(socket)));
    }
  }  
  

  
  // Submit the new monitoring requests
  ret = io_uring_submit(&ring_);
  if (ret < 0) {
    A2_LOG_DEBUG(fmt("Failed to submit io_uring requests: %s", 
                   util::safeStrerror(-ret).c_str()));
  }
  
#ifdef ENABLE_ASYNC_DNS
  // Process AsyncNameResolver events (similar to epoll implementation)
  for (auto& i : nameResolverEntries_) {
    auto& ent = i.second;
    ent.processTimeout();
    ent.removeSocketEvents(this);
    ent.addSocketEvents(this);
  }
#endif

}

bool IoUringEventPoll::addEvents(sock_t socket,
                              const IoUringEventPoll::KEvent& event)
{
  auto i = socketEntries_.lower_bound(socket);
  if (i != std::end(socketEntries_) && (*i).first == socket) {
    auto& socketEntry = (*i).second;
    event.addSelf(&socketEntry);
    
    if (socketsNeedingRegistration_.count(socket)) {
      // Register with io_uring for POLLIN events
      struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
      if (!sqe) {
        A2_LOG_DEBUG(fmt("Failed to get SQE for socket %d", socket));
        return false;
      }
      
      // Use poll_add for monitoring the socket
      io_uring_prep_poll_add(sqe, socketEntry.getSocket(), socketEntry.getEvents());
      io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(socket)));
      socketsNeedingRegistration_.erase(socket);
      
      // Submit the request
      int ret = io_uring_submit(&ring_);
      if (ret < 0) {
        A2_LOG_DEBUG(fmt("Failed to submit io_uring request for socket %d: %s", 
                       socket, util::safeStrerror(-ret).c_str()));
        socketsNeedingRegistration_.insert(socket);
        return false;
      }
    }
  } else {
    // New socket entry
    i = socketEntries_.insert(i, std::make_pair(socket, KSocketEntry(socket)));
    auto& socketEntry = (*i).second;
    
    event.addSelf(&socketEntry);
    
    // Register with io_uring
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
      A2_LOG_DEBUG(fmt("Failed to get SQE for socket %d", socket));
      return false;
    }

    io_uring_prep_poll_add(sqe, socketEntry.getSocket(), socketEntry.getEvents());
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(socket)));
    socketsNeedingRegistration_.erase(socket);
    
    // Submit the request
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
      A2_LOG_DEBUG(fmt("Failed to submit io_uring request for socket %d: %s", 
                     socket, util::safeStrerror(-ret).c_str()));
      socketsNeedingRegistration_.insert(socket);
      return false;
    }
  }
  
  return true;
}

bool IoUringEventPoll::addEvents(sock_t socket, Command* command,
                              EventPoll::EventType events)
{
  uint32_t ioEvents = translateEvents(events);
  return addEvents(socket, KCommandEvent(command, ioEvents));
}

#ifdef ENABLE_ASYNC_DNS
bool IoUringEventPoll::addEvents(sock_t socket, Command* command, int events,
                              const std::shared_ptr<AsyncNameResolver>& rs)
{
  return addEvents(socket, KADNSEvent(rs, command, socket, events));
}
#endif // ENABLE_ASYNC_DNS

bool IoUringEventPoll::deleteEvents(sock_t socket,
                                 const IoUringEventPoll::KEvent& event)
{
  auto i = socketEntries_.find(socket);
  if (i == std::end(socketEntries_)) {
    A2_LOG_DEBUG(fmt("Socket %d is not found in SocketEntries.", socket));
    return false;
  }
  
  auto& socketEntry = (*i).second;
  event.removeSelf(&socketEntry);
  
  if (socketEntry.eventEmpty()) {
    // If the socket has no more events, remove it from io_uring monitoring
    if (!socketsNeedingRegistration_.count(socket)) {
      // We need to cancel the ongoing poll operation
      struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
      if (!sqe) {
        A2_LOG_DEBUG(fmt("Failed to get SQE for cancellation on socket %d", socket));
        return false;
      }
      
      // Use poll_remove to cancel the monitoring
      // For poll_remove, we still need to provide the user_data from the original registration
      io_uring_prep_poll_remove(sqe, static_cast<__u64>(socket));

      // And use the socket fd as user_data for this operation as well
      io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(socket)));
      io_uring_submit(&ring_);
    }
    else {
      // Socket is already marked for re-registration
      socketsNeedingRegistration_.erase(socket);
    }
    
    // Remove from our tracking
    socketEntries_.erase(i);
    return true;
  } else {
    // Socket still has events, just update our internal state
    // io_uring will continue monitoring it
    return true;
  }
}

#ifdef ENABLE_ASYNC_DNS
bool IoUringEventPoll::deleteEvents(sock_t socket, Command* command,
                                 const std::shared_ptr<AsyncNameResolver>& rs)
{
  return deleteEvents(socket, KADNSEvent(rs, command, socket, 0));
}
#endif // ENABLE_ASYNC_DNS

bool IoUringEventPoll::deleteEvents(sock_t socket, Command* command,
                                 EventPoll::EventType events)
{
  uint32_t ioEvents = translateEvents(events);
  return deleteEvents(socket, KCommandEvent(command, ioEvents));
}



#ifdef ENABLE_ASYNC_DNS
// Keep AsyncNameResolver methods similar, but update for io_uring
bool IoUringEventPoll::addNameResolver(
    const std::shared_ptr<AsyncNameResolver>& resolver, Command* command)
{
  auto key = std::make_pair(resolver.get(), command);
  auto itr = nameResolverEntries_.lower_bound(key);

  if (itr != std::end(nameResolverEntries_) && (*itr).first == key) {
    return false;
  }

  itr = nameResolverEntries_.insert(
      itr, std::make_pair(key, KAsyncNameResolverEntry(resolver, command)));
  (*itr).second.addSocketEvents(this);
  return true;
}

bool IoUringEventPoll::deleteNameResolver(
    const std::shared_ptr<AsyncNameResolver>& resolver, Command* command)
{
  auto key = std::make_pair(resolver.get(), command);
  auto itr = nameResolverEntries_.find(key);
  if (itr == std::end(nameResolverEntries_)) {
    return false;
  }

  (*itr).second.removeSocketEvents(this);
  nameResolverEntries_.erase(itr);
  return true;
}
#endif // ENABLE_ASYNC_DNS

} // namespace aria2