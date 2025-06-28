#ifndef D_KTLS_SESSION_H
#define D_KTLS_SESSION_H

#include "common.h"

#ifdef HAVE_KTLS

#include "TLSSession.h"
#include "KTLSContext.h"
#include <memory>
#include <openssl/ssl.h>

namespace aria2 {

class KTLSSession : public TLSSession {
public:
  KTLSSession(KTLSContext* ctx);
  virtual ~KTLSSession();

  virtual int init(sock_t sockfd) CXX11_OVERRIDE;
  virtual int setSNIHostname(const std::string& hostname) CXX11_OVERRIDE;
  virtual int closeConnection() CXX11_OVERRIDE;
  virtual int checkDirection() CXX11_OVERRIDE;
  virtual ssize_t writeData(const void* data, size_t len) CXX11_OVERRIDE;
  virtual ssize_t readData(void* data, size_t len) CXX11_OVERRIDE;
  virtual int tlsConnect(const std::string& hostname, TLSVersion& version,
                         std::string& handshakeErr) CXX11_OVERRIDE;
  virtual int tlsAccept(TLSVersion& version) CXX11_OVERRIDE;
  virtual std::string getLastErrorString() CXX11_OVERRIDE;
  virtual size_t getRecvBufferedLength() CXX11_OVERRIDE;

  // kTLS-specific methods
  bool isKTLSEnabled() const;
  
  // Zero-copy network socket I/O methods
  ssize_t writeDataZeroCopy(const void* data, size_t len);
  ssize_t readDataZeroCopy(void* data, size_t len);
  bool isZeroCopySupported() const;
  
private:
  SSL* ssl_;
  KTLSContext* ctx_;
  sock_t sockfd_;
  bool good_;
  bool ktlsEnabled_;
  bool zeroCopySupported_;
  std::string lastError_;
};

} // namespace aria2

#endif // HAVE_KTLS

#endif // D_KTLS_SESSION_H 