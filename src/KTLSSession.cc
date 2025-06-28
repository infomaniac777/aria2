#include "KTLSSession.h"

#ifdef HAVE_KTLS

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <linux/errqueue.h>
#include <sys/epoll.h>

#include "LogFactory.h"
#include "fmt.h"
#include "KTLSContext.h"
#include "util.h"

namespace aria2 {

KTLSSession::KTLSSession(KTLSContext* ctx)
  : ssl_(nullptr), ctx_(ctx), sockfd_(-1), good_(false), ktlsEnabled_(false), zeroCopySupported_(false)
{
}

KTLSSession::~KTLSSession()
{
  if (ssl_) {
    SSL_free(ssl_);
  }
}

int KTLSSession::init(sock_t sockfd)
{
  sockfd_ = sockfd;
  
  if (!ctx_ || !ctx_->good()) {
    lastError_ = "Invalid TLS context";
    return TLS_ERR_ERROR;
  }

  ssl_ = SSL_new(ctx_->getSSLCtx());
  if (!ssl_) {
    lastError_ = "Failed to create SSL object";
    return TLS_ERR_ERROR;
  }

  if (SSL_set_fd(ssl_, sockfd) != 1) {
    lastError_ = "Failed to set socket file descriptor";
    return TLS_ERR_ERROR;
  }

  // Check if zero-copy is supported on this socket
  int val = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val)) == 0) {
    zeroCopySupported_ = true;
    A2_LOG_INFO("Zero-copy network I/O supported on socket");
  } else {
    zeroCopySupported_ = false;
    A2_LOG_DEBUG("Zero-copy network I/O not supported on socket");
  }

  good_ = true;
  A2_LOG_INFO(fmt("kTLS session initialized - Zero-copy: %s", 
                  zeroCopySupported_ ? "SUPPORTED" : "NOT SUPPORTED"));
  return TLS_ERR_OK;
}

int KTLSSession::setSNIHostname(const std::string& hostname)
{
  if (!ssl_) {
    return TLS_ERR_ERROR;
  }

  if (SSL_set_tlsext_host_name(ssl_, hostname.c_str()) != 1) {
    lastError_ = "Failed to set SNI hostname";
    return TLS_ERR_ERROR;
  }

  return TLS_ERR_OK;
}

int KTLSSession::closeConnection()
{
  if (ssl_) {
    SSL_shutdown(ssl_);
  }
  good_ = false;
  return TLS_ERR_OK;
}

int KTLSSession::checkDirection()
{
  if (!ssl_) {
    return TLS_ERR_ERROR;
  }

  int ret = SSL_get_error(ssl_, 0);
  switch (ret) {
    case SSL_ERROR_WANT_READ:
      return TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
      return TLS_WANT_WRITE;
    default:
      return TLS_ERR_OK;
  }
}

ssize_t KTLSSession::writeData(const void* data, size_t len)
{
  if (!ssl_ || !good_) {
    return TLS_ERR_ERROR;
  }

  // Always use SSL_write for kTLS - OpenSSL 3.0+ handles kTLS internally
  // We should NOT bypass OpenSSL with direct socket operations
  int ret = SSL_write(ssl_, data, len);
  if (ret <= 0) {
    int err = SSL_get_error(ssl_, ret);
    switch (err) {
      case SSL_ERROR_WANT_READ:
        return TLS_ERR_WOULDBLOCK;
      case SSL_ERROR_WANT_WRITE:
        return TLS_ERR_WOULDBLOCK;
      case SSL_ERROR_ZERO_RETURN:
        return 0;
      default:
        lastError_ = "SSL write error";
        return TLS_ERR_ERROR;
    }
  }

  return ret;
}

ssize_t KTLSSession::readData(void* data, size_t len)
{
  if (!ssl_ || !good_) {
    return TLS_ERR_ERROR;
  }

  // Always use SSL_read for kTLS - OpenSSL 3.0+ handles kTLS internally
  // We should NOT bypass OpenSSL with direct socket operations
  int ret = SSL_read(ssl_, data, len);
  if (ret <= 0) {
    int err = SSL_get_error(ssl_, ret);
    switch (err) {
      case SSL_ERROR_WANT_READ:
        return TLS_ERR_WOULDBLOCK;
      case SSL_ERROR_WANT_WRITE:
        return TLS_ERR_WOULDBLOCK;
      case SSL_ERROR_ZERO_RETURN:
        return 0;
      default:
        lastError_ = "SSL read error";
        return TLS_ERR_ERROR;
    }
  }

  return ret;
}

int KTLSSession::tlsConnect(const std::string& hostname, TLSVersion& version,
                             std::string& handshakeErr)
{
  if (!ssl_) {
    handshakeErr = "SSL object not initialized";
    return TLS_ERR_ERROR;
  }

  // Set SNI hostname if provided
  if (!hostname.empty()) {
    setSNIHostname(hostname);
  }

  int ret = SSL_connect(ssl_);
  if (ret != 1) {
    int err = SSL_get_error(ssl_, ret);
    switch (err) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE:
        return TLS_ERR_WOULDBLOCK; // Continue handshake later
      default:
        handshakeErr = fmt("SSL handshake failed: %s", 
                          ERR_error_string(ERR_get_error(), nullptr));
        good_ = false;
        return TLS_ERR_ERROR;
    }
  }

  // Get TLS version
  int ssl_version = SSL_version(ssl_);
  switch (ssl_version) {
    case TLS1_1_VERSION:
      version = TLS_PROTO_TLS11;
      break;
    case TLS1_2_VERSION:
      version = TLS_PROTO_TLS12;
      break;
    case TLS1_3_VERSION:
      version = TLS_PROTO_TLS13;
      break;
    default:
      version = TLS_PROTO_TLS12;
      break;
  }

  // Check if kTLS was successfully enabled
#ifdef SSL_OP_ENABLE_KTLS
  if (SSL_get_options(ssl_) & SSL_OP_ENABLE_KTLS) {
    ktlsEnabled_ = true;
    A2_LOG_INFO(fmt("kTLS ACTIVE: kernel TLS offload enabled%s", 
                    zeroCopySupported_ ? " with zero-copy support" : ""));
  } else {
    ktlsEnabled_ = false;
    A2_LOG_INFO("kTLS INACTIVE: using regular TLS (userspace encryption)");
  }
#endif

  A2_LOG_INFO(fmt("TLS connection established - Protocol: %s, kTLS: %s, Zero-copy: %s",
                  version == TLS_PROTO_TLS13 ? "TLS 1.3" :
                  version == TLS_PROTO_TLS12 ? "TLS 1.2" : "TLS 1.1",
                  ktlsEnabled_ ? "YES" : "NO",
                  zeroCopySupported_ ? "YES" : "NO"));
  return TLS_ERR_OK;
}

int KTLSSession::tlsAccept(TLSVersion& version)
{
  if (!ssl_) {
    return TLS_ERR_ERROR;
  }

  int ret = SSL_accept(ssl_);
  if (ret != 1) {
    int err = SSL_get_error(ssl_, ret);
    switch (err) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE:
        return TLS_ERR_WOULDBLOCK; // Continue handshake later
      default:
        lastError_ = fmt("SSL accept failed: %s", 
                        ERR_error_string(ERR_get_error(), nullptr));
        good_ = false;
        return TLS_ERR_ERROR;
    }
  }

  // Get TLS version
  int ssl_version = SSL_version(ssl_);
  switch (ssl_version) {
    case TLS1_1_VERSION:
      version = TLS_PROTO_TLS11;
      break;
    case TLS1_2_VERSION:
      version = TLS_PROTO_TLS12;
      break;
    case TLS1_3_VERSION:
      version = TLS_PROTO_TLS13;
      break;
    default:
      version = TLS_PROTO_TLS12;
      break;
  }

  // Check if kTLS was successfully enabled
#ifdef SSL_OP_ENABLE_KTLS
  if (SSL_get_options(ssl_) & SSL_OP_ENABLE_KTLS) {
    ktlsEnabled_ = true;
    A2_LOG_INFO(fmt("kTLS ACTIVE: kernel TLS offload enabled%s", 
                    zeroCopySupported_ ? " with zero-copy support" : ""));
  } else {
    ktlsEnabled_ = false;
    A2_LOG_INFO("kTLS INACTIVE: using regular TLS (userspace encryption)");
  }
#endif

  A2_LOG_INFO(fmt("TLS connection established - Protocol: %s, kTLS: %s, Zero-copy: %s",
                  version == TLS_PROTO_TLS13 ? "TLS 1.3" :
                  version == TLS_PROTO_TLS12 ? "TLS 1.2" : "TLS 1.1",
                  ktlsEnabled_ ? "YES" : "NO",
                  zeroCopySupported_ ? "YES" : "NO"));
  return TLS_ERR_OK;
}

std::string KTLSSession::getLastErrorString()
{
  return lastError_;
}

size_t KTLSSession::getRecvBufferedLength()
{
  if (!ssl_) {
    return 0;
  }
  return SSL_pending(ssl_);
}

bool KTLSSession::isKTLSEnabled() const
{
  return ktlsEnabled_;
}

ssize_t KTLSSession::writeDataZeroCopy(const void* data, size_t len)
{
  if (!zeroCopySupported_) {
    // Fallback to regular write
    return writeData(data, len);
  }

  // Use MSG_ZEROCOPY flag for zero-copy transmission
  ssize_t ret = send(sockfd_, data, len, MSG_ZEROCOPY | MSG_DONTWAIT);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return TLS_ERR_WOULDBLOCK;
    }
    lastError_ = fmt("Zero-copy write failed: %s", strerror(errno));
    return TLS_ERR_ERROR;
  }

  return ret;
}

ssize_t KTLSSession::readDataZeroCopy(void* data, size_t len)
{
  if (!zeroCopySupported_) {
    // Fallback to regular read
    return readData(data, len);
  }

  // For kTLS, we can use direct socket read after handshake
  // But we should still use SSL_read to maintain proper state
  return readData(data, len);
}

bool KTLSSession::isZeroCopySupported() const
{
  return zeroCopySupported_;
}

} // namespace aria2

#endif // HAVE_KTLS 