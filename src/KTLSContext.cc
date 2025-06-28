#include "KTLSContext.h"

#ifdef HAVE_KTLS

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "LogFactory.h"
#include "fmt.h"
#include "KTLSSession.h"

namespace aria2 {

KTLSContext::KTLSContext(TLSSessionSide side, TLSVersion minVer)
  : sslCtx_(nullptr), side_(side), minVer_(minVer), good_(false), verifyPeer_(true)
{
  SSL_library_init();
  SSL_load_error_strings();

  const SSL_METHOD* meth = nullptr;
  if (side == TLS_CLIENT) {
    meth = TLS_client_method();
  } else {
    meth = TLS_server_method();
  }

  sslCtx_ = SSL_CTX_new(meth);
  if (!sslCtx_) {
    A2_LOG_ERROR("Failed to create SSL_CTX");
    return;
  }

  // Enable kTLS in OpenSSL 3.0+
#ifdef HAVE_OPENSSL_KTLS
  SSL_CTX_set_options(sslCtx_, SSL_OP_ENABLE_KTLS);
  A2_LOG_INFO("kTLS enabled in OpenSSL context");
#endif

  // Set minimum TLS version
  int minVersion = 0;
  switch (minVer) {
  case TLS_PROTO_TLS11:
    minVersion = TLS1_1_VERSION;
    break;
  case TLS_PROTO_TLS12:
    minVersion = TLS1_2_VERSION;
    break;
  case TLS_PROTO_TLS13:
    minVersion = TLS1_3_VERSION;
    break;
  default:
    minVersion = TLS1_2_VERSION;
    break;
  }

  if (SSL_CTX_set_min_proto_version(sslCtx_, minVersion) != 1) {
    A2_LOG_ERROR("Failed to set minimum TLS version");
    SSL_CTX_free(sslCtx_);
    sslCtx_ = nullptr;
    return;
  }

  // Set verification mode
  if (side == TLS_CLIENT) {
    SSL_CTX_set_verify(sslCtx_, SSL_VERIFY_PEER, nullptr);
  } else {
    SSL_CTX_set_verify(sslCtx_, SSL_VERIFY_NONE, nullptr);
  }

  good_ = true;
  A2_LOG_INFO("kTLS context created successfully");
}

KTLSContext::~KTLSContext()
{
  if (sslCtx_) {
    SSL_CTX_free(sslCtx_);
  }
}

bool KTLSContext::addCredentialFile(const std::string& certfile,
                                    const std::string& keyfile)
{
  if (!sslCtx_) {
    return false;
  }

  if (SSL_CTX_use_certificate_chain_file(sslCtx_, certfile.c_str()) != 1) {
    A2_LOG_ERROR(fmt("Failed to load certificate file: %s", certfile.c_str()));
    return false;
  }

  if (SSL_CTX_use_PrivateKey_file(sslCtx_, keyfile.c_str(), SSL_FILETYPE_PEM) != 1) {
    A2_LOG_ERROR(fmt("Failed to load private key file: %s", keyfile.c_str()));
    return false;
  }

  if (SSL_CTX_check_private_key(sslCtx_) != 1) {
    A2_LOG_ERROR("Private key does not match certificate");
    return false;
  }

  return true;
}

bool KTLSContext::addSystemTrustedCACerts()
{
  if (!sslCtx_) {
    return false;
  }

  if (SSL_CTX_set_default_verify_paths(sslCtx_) != 1) {
    A2_LOG_ERROR("Failed to load system trusted CA certificates");
    return false;
  }

  return true;
}

bool KTLSContext::addTrustedCACertFile(const std::string& certfile)
{
  if (!sslCtx_) {
    return false;
  }

  if (SSL_CTX_load_verify_locations(sslCtx_, certfile.c_str(), nullptr) != 1) {
    A2_LOG_ERROR(fmt("Failed to load CA certificate file: %s", certfile.c_str()));
    return false;
  }

  return true;
}

bool KTLSContext::good() const
{
  return good_;
}

TLSSessionSide KTLSContext::getSide() const
{
  return side_;
}

bool KTLSContext::getVerifyPeer() const
{
  return verifyPeer_;
}

void KTLSContext::setVerifyPeer(bool verify)
{
  verifyPeer_ = verify;
  if (sslCtx_) {
    if (verify) {
      SSL_CTX_set_verify(sslCtx_, SSL_VERIFY_PEER, nullptr);
    } else {
      SSL_CTX_set_verify(sslCtx_, SSL_VERIFY_NONE, nullptr);
    }
  }
}

SSL_CTX* KTLSContext::getSSLCtx() const
{
  return sslCtx_;
}

} // namespace aria2

#endif // HAVE_KTLS 