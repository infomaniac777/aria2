#ifndef D_KTLS_CONTEXT_H
#define D_KTLS_CONTEXT_H

#include "common.h"

#ifdef HAVE_KTLS

#include "TLSContext.h"
#include "TLSSession.h"
#include <memory>
#include <openssl/ssl.h>

namespace aria2 {

class KTLSContext : public TLSContext {
public:
  KTLSContext(TLSSessionSide side, TLSVersion minVer = TLS_PROTO_TLS12);
  virtual ~KTLSContext();

  virtual bool addCredentialFile(const std::string& certfile,
                                 const std::string& keyfile) CXX11_OVERRIDE;
  virtual bool addSystemTrustedCACerts() CXX11_OVERRIDE;
  virtual bool addTrustedCACertFile(const std::string& certfile) CXX11_OVERRIDE;
  virtual bool good() const CXX11_OVERRIDE;
  virtual TLSSessionSide getSide() const CXX11_OVERRIDE;
  virtual bool getVerifyPeer() const CXX11_OVERRIDE;
  virtual void setVerifyPeer(bool verify) CXX11_OVERRIDE;

  // kTLS-specific methods
  SSL_CTX* getSSLCtx() const;

private:
  SSL_CTX* sslCtx_;
  TLSSessionSide side_;
  TLSVersion minVer_;
  bool good_;
  bool verifyPeer_;
};

} // namespace aria2

#endif // HAVE_KTLS

#endif // D_KTLS_CONTEXT_H 