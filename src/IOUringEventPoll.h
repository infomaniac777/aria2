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
 * [... license text preserved ...]
 */
/* copyright --> */
#ifndef D_IO_URING_EVENT_POLL_H
#define D_IO_URING_EVENT_POLL_H

#include "EventPoll.h"

#include <map>
#include <memory>
#include <unordered_set>
#include <liburing.h>
#include <poll.h> // For POLLIN, POLLOUT flags

#include "Event.h"
#include "a2functional.h"
#ifdef ENABLE_ASYNC_DNS
#  include "AsyncNameResolver.h"
#endif // ENABLE_ASYNC_DNS

namespace aria2 {

class IoUringEventPoll : public EventPoll {
private:
  class KSocketEntry;

  typedef Event<KSocketEntry> KEvent;

  typedef CommandEvent<KSocketEntry, IoUringEventPoll> KCommandEvent;
  typedef ADNSEvent<KSocketEntry, IoUringEventPoll> KADNSEvent;
  typedef AsyncNameResolverEntry<IoUringEventPoll> KAsyncNameResolverEntry;
  friend class AsyncNameResolverEntry<IoUringEventPoll>;

  class KSocketEntry : public SocketEntry<KCommandEvent, KADNSEvent> {
  public:
    KSocketEntry(sock_t socket);

    KSocketEntry(const KSocketEntry&) = delete;
    KSocketEntry(KSocketEntry&&) = default;
    uint32_t getEvents();
  };

  friend int accumulateEvent(int events, const KEvent& event);

private:
  typedef std::map<sock_t, KSocketEntry> KSocketEntrySet;
  KSocketEntrySet socketEntries_;

  std::unordered_set<sock_t> socketsNeedingRegistration_;
  bool needSubmission_;

#ifdef ENABLE_ASYNC_DNS
  typedef std::map<std::pair<AsyncNameResolver*, Command*>,
                  KAsyncNameResolverEntry>
      KAsyncNameResolverEntrySet;
  KAsyncNameResolverEntrySet nameResolverEntries_;
#endif // ENABLE_ASYNC_DNS

  struct io_uring ring_;
  unsigned int ringEntriesSize_;
  bool good_;

  static const size_t IO_URING_ENTRIES = 1024;

  bool addEvents(sock_t socket, const KEvent& event);

  bool deleteEvents(sock_t socket, const KEvent& event);

#ifdef ENABLE_ASYNC_DNS
  bool addEvents(sock_t socket, Command* command, int events,
                 const std::shared_ptr<AsyncNameResolver>& rs);

  bool deleteEvents(sock_t socket, Command* command,
                    const std::shared_ptr<AsyncNameResolver>& rs);
#endif // ENABLE_ASYNC_DNS

public:
  IoUringEventPoll();

  virtual ~IoUringEventPoll();

  virtual bool good() const;

  virtual void poll(const struct timeval& tv) CXX11_OVERRIDE;

  virtual bool addEvents(sock_t socket, Command* command,
                        EventPoll::EventType events) CXX11_OVERRIDE;

  virtual bool deleteEvents(sock_t socket, Command* command,
                           EventPoll::EventType events) CXX11_OVERRIDE;

#ifdef ENABLE_ASYNC_DNS
  virtual bool addNameResolver(const std::shared_ptr<AsyncNameResolver>& resolver,
                              Command* command) CXX11_OVERRIDE;

  virtual bool deleteNameResolver(
      const std::shared_ptr<AsyncNameResolver>& resolver,
      Command* command) CXX11_OVERRIDE;
#endif // ENABLE_ASYNC_DNS

  // Define constants for io_uring events - map to poll flags for compatibility
  static const int IEV_READ = POLLIN;
  static const int IEV_WRITE = POLLOUT;
  static const int IEV_ERROR = POLLERR;
  static const int IEV_HUP = POLLHUP;
};

} // namespace aria2

#endif // D_IO_URING_EVENT_POLL_H