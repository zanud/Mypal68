/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// During certificate authentication, we call CertVerifier::VerifySSLServerCert.
// This function may make zero or more HTTP requests (e.g. to gather revocation
// information). Our fetching logic for these requests processes them on the
// socket transport service thread.
//
// Because the connection for which we are verifying the certificate is
// happening on the socket transport thread, if our cert auth hook were to call
// VerifySSLServerCert directly, there would be a deadlock: VerifySSLServerCert
// would cause an event to be asynchronously posted to the socket transport
// thread, and then it would block the socket transport thread waiting to be
// notified of the HTTP response. However, the HTTP request would never actually
// be processed because the socket transport thread would be blocked and so it
// wouldn't be able process HTTP requests.
//
// Consequently, when we are asked to verify a certificate, we must always call
// VerifySSLServerCert on another thread. To accomplish this, our auth cert hook
// dispatches a SSLServerCertVerificationJob to a pool of background threads,
// and then immediately returns SECWouldBlock to libssl. These jobs are where
// VerifySSLServerCert is actually called.
//
// When our auth cert hook returns SECWouldBlock, libssl will carry on the
// handshake while we validate the certificate. This will free up the socket
// transport thread so that HTTP requests--including the OCSP requests needed
// for cert verification as mentioned above--can be processed.
//
// Once VerifySSLServerCert returns, the cert verification job dispatches a
// SSLServerCertVerificationResult to the socket transport thread; the
// SSLServerCertVerificationResult will notify libssl that the certificate
// authentication is complete. Once libssl is notified that the authentication
// is complete, it will continue the TLS handshake (if it hasn't already
// finished) and it will begin allowing us to send/receive data on the
// connection.
//
// Timeline of events (for connections managed by the socket transport service):
//
//    * libssl calls SSLServerCertVerificationJob::Dispatch on the socket
//      transport thread.
//    * SSLServerCertVerificationJob::Dispatch queues a job
//      (instance of SSLServerCertVerificationJob) to its background thread
//      pool and returns.
//    * One of the background threads calls CertVerifier::VerifySSLServerCert,
//      which may enqueue some HTTP request(s) onto the socket transport thread,
//      and then blocks that background thread waiting for the responses and/or
//      timeouts or errors for those requests.
//    * Once those HTTP responses have all come back or failed, the
//      CertVerifier::VerifySSLServerCert function returns a result indicating
//      that the validation succeeded or failed.
//    * If the validation succeeded, then a SSLServerCertVerificationResult
//      event is posted to the socket transport thread, and the cert
//      verification thread becomes free to verify other certificates.
//    * Otherwise, we do cert override processing to see if the validation
//      error can be convered by override rules. The result of this processing
//      is similarly dispatched in a SSLServerCertVerificationResult.
//    * The SSLServerCertVerificationResult event will either wake up the
//      socket (using SSL_AuthCertificateComplete) if validation succeeded or
//      there was an error override, or it will set an error flag so that the
//      next I/O operation on the socket will fail, causing the socket transport
//      thread to close the connection.
//
// SSLServerCertVerificationResult must be dispatched to the socket transport
// thread because we must only call SSL_* functions on the socket transport
// thread since they may do I/O, because many parts of nsNSSSocketInfo (the
// subclass of TransportSecurityInfo used when validating certificates during
// an SSL handshake) and the PSM NSS I/O layer are not thread-safe, and because
// we need the event to interrupt the PR_Poll that may waiting for I/O on the
// socket for which we are validating the cert.

#include "SSLServerCertVerification.h"

#include <cstring>

#include "BRNameMatchingPolicy.h"
#include "CertVerifier.h"
#include "CryptoTask.h"
#include "ExtendedValidation.h"
#include "NSSCertDBTrustDomain.h"
#include "PSMRunnable.h"
#include "ScopedNSSTypes.h"
#include "SharedCertVerifier.h"
#include "SharedSSLState.h"
#include "TransportSecurityInfo.h"  // For RememberCertErrorsTable
#include "cert.h"
#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Unused.h"
#include "mozilla/net/DNS.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsICertOverrideService.h"
#include "nsISiteSecurityService.h"
#include "nsISocketProvider.h"
#include "nsThreadPool.h"
#include "nsNetUtil.h"
#include "nsNSSCertificate.h"
#include "nsNSSComponent.h"
#include "nsNSSIOLayer.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsURLHelper.h"
#include "nsXPCOMCIDInternal.h"
#include "mozpkix/pkix.h"
#include "mozpkix/pkixnss.h"
#include "secerr.h"
#include "secoidt.h"
#include "secport.h"
#include "ssl.h"
#include "sslerr.h"
#include "sslexp.h"

extern mozilla::LazyLogModule gPIPNSSLog;

using namespace mozilla::pkix;

namespace mozilla {
namespace psm {

namespace {

// do not use a nsCOMPtr to avoid static initializer/destructor
nsIThreadPool* gCertVerificationThreadPool = nullptr;

}  // unnamed namespace

// Called when the socket transport thread starts, to initialize the SSL cert
// verification thread pool. By tying the thread pool startup/shutdown directly
// to the STS thread's lifetime, we ensure that they are *always* available for
// SSL connections and that there are no races during startup and especially
// shutdown. (Previously, we have had multiple problems with races in PSM
// background threads, and the race-prevention/shutdown logic used there is
// brittle. Since this service is critical to things like downloading updates,
// we take no chances.) Also, by doing things this way, we avoid the need for
// locks, since gCertVerificationThreadPool is only ever accessed on the socket
// transport thread.
void InitializeSSLServerCertVerificationThreads() {
  // TODO: tuning, make parameters preferences
  gCertVerificationThreadPool = new nsThreadPool();
  NS_ADDREF(gCertVerificationThreadPool);

  (void)gCertVerificationThreadPool->SetIdleThreadLimit(5);
  (void)gCertVerificationThreadPool->SetIdleThreadTimeout(30 * 1000);
  (void)gCertVerificationThreadPool->SetThreadLimit(5);
  (void)gCertVerificationThreadPool->SetName(NS_LITERAL_CSTRING("SSL Cert"));
}

// Called when the socket transport thread finishes, to destroy the thread
// pool. Since the socket transport service has stopped processing events, it
// will not attempt any more SSL I/O operations, so it is clearly safe to shut
// down the SSL cert verification infrastructure. Also, the STS will not
// dispatch many SSL verification result events at this point, so any pending
// cert verifications will (correctly) fail at the point they are dispatched.
//
// The other shutdown race condition that is possible is a race condition with
// shutdown of the nsNSSComponent service. We use the
// nsNSSShutdownPreventionLock where needed (not here) to prevent that.
void StopSSLServerCertVerificationThreads() {
  if (gCertVerificationThreadPool) {
    gCertVerificationThreadPool->Shutdown();
    NS_RELEASE(gCertVerificationThreadPool);
  }
}

namespace {

// Dispatched to the STS thread to notify the infoObject of the verification
// result.
//
// This will cause the PR_Poll in the STS thread to return, so things work
// correctly even if the STS thread is blocked polling (only) on the file
// descriptor that is waiting for this result.
class SSLServerCertVerificationResult : public Runnable {
 public:
  NS_DECL_NSIRUNNABLE

  SSLServerCertVerificationResult(nsNSSSocketInfo* infoObject,
                                  PRErrorCode errorCode);

  void Dispatch();

 private:
  const RefPtr<nsNSSSocketInfo> mInfoObject;
  const PRErrorCode mErrorCode;
};

SECStatus DetermineCertOverrideErrors(const UniqueCERTCertificate& cert,
                                      const nsACString& hostName, PRTime now,
                                      PRErrorCode defaultErrorCodeToReport,
                                      /*out*/ uint32_t& collectedErrors,
                                      /*out*/ PRErrorCode& errorCodeTrust,
                                      /*out*/ PRErrorCode& errorCodeMismatch,
                                      /*out*/ PRErrorCode& errorCodeTime) {
  MOZ_ASSERT(cert);
  MOZ_ASSERT(collectedErrors == 0);
  MOZ_ASSERT(errorCodeTrust == 0);
  MOZ_ASSERT(errorCodeMismatch == 0);
  MOZ_ASSERT(errorCodeTime == 0);

  // Assumes the error prioritization described in mozilla::pkix's
  // BuildForward function. Also assumes that CheckCertHostname was only
  // called if CertVerifier::VerifyCert succeeded.
  switch (defaultErrorCodeToReport) {
    case SEC_ERROR_CERT_SIGNATURE_ALGORITHM_DISABLED:
    case SEC_ERROR_EXPIRED_ISSUER_CERTIFICATE:
    case SEC_ERROR_UNKNOWN_ISSUER:
    case SEC_ERROR_CA_CERT_INVALID:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_ADDITIONAL_POLICY_CONSTRAINT_FAILED:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_CA_CERT_USED_AS_END_ENTITY:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_EMPTY_ISSUER_NAME:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_INADEQUATE_KEY_SIZE:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_MITM_DETECTED:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_NOT_YET_VALID_ISSUER_CERTIFICATE:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_SELF_SIGNED_CERT:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_V1_CERT_USED_AS_CA: {
      collectedErrors = nsICertOverrideService::ERROR_UNTRUSTED;
      errorCodeTrust = defaultErrorCodeToReport;

      SECCertTimeValidity validity =
          CERT_CheckCertValidTimes(cert.get(), now, false);
      if (validity == secCertTimeUndetermined) {
        // This only happens if cert is null. CERT_CheckCertValidTimes will
        // have set the error code to SEC_ERROR_INVALID_ARGS. We should really
        // be using mozilla::pkix here anyway.
        MOZ_ASSERT(PR_GetError() == SEC_ERROR_INVALID_ARGS);
        return SECFailure;
      }
      if (validity == secCertTimeExpired) {
        collectedErrors |= nsICertOverrideService::ERROR_TIME;
        errorCodeTime = SEC_ERROR_EXPIRED_CERTIFICATE;
      } else if (validity == secCertTimeNotValidYet) {
        collectedErrors |= nsICertOverrideService::ERROR_TIME;
        errorCodeTime =
            mozilla::pkix::MOZILLA_PKIX_ERROR_NOT_YET_VALID_CERTIFICATE;
      }
      break;
    }

    case SEC_ERROR_INVALID_TIME:
    case SEC_ERROR_EXPIRED_CERTIFICATE:
    case mozilla::pkix::MOZILLA_PKIX_ERROR_NOT_YET_VALID_CERTIFICATE:
      collectedErrors = nsICertOverrideService::ERROR_TIME;
      errorCodeTime = defaultErrorCodeToReport;
      break;

    case SSL_ERROR_BAD_CERT_DOMAIN:
      collectedErrors = nsICertOverrideService::ERROR_MISMATCH;
      errorCodeMismatch = SSL_ERROR_BAD_CERT_DOMAIN;
      break;

    case 0:
      NS_ERROR("No error code set during certificate validation failure.");
      PR_SetError(PR_INVALID_STATE_ERROR, 0);
      return SECFailure;

    default:
      PR_SetError(defaultErrorCodeToReport, 0);
      return SECFailure;
  }

  if (defaultErrorCodeToReport != SSL_ERROR_BAD_CERT_DOMAIN) {
    Input certInput;
    if (certInput.Init(cert->derCert.data, cert->derCert.len) != Success) {
      PR_SetError(SEC_ERROR_BAD_DER, 0);
      return SECFailure;
    }
    Input hostnameInput;
    Result result = hostnameInput.Init(
        BitwiseCast<const uint8_t*, const char*>(hostName.BeginReading()),
        hostName.Length());
    if (result != Success) {
      PR_SetError(SEC_ERROR_INVALID_ARGS, 0);
      return SECFailure;
    }
    // Use a lax policy so as to not generate potentially spurious name
    // mismatch "hints".
    BRNameMatchingPolicy nameMatchingPolicy(
        BRNameMatchingPolicy::Mode::DoNotEnforce);
    // CheckCertHostname expects that its input represents a certificate that
    // has already been successfully validated by BuildCertChain. This is
    // obviously not the case, however, because we're in the error path of
    // certificate verification. Thus, this is problematic. In the future, it
    // would be nice to remove this optimistic additional error checking and
    // simply punt to the front-end, which can more easily (and safely) perform
    // extra checks to give the user hints as to why verification failed.
    result = CheckCertHostname(certInput, hostnameInput, nameMatchingPolicy);
    // Treat malformed name information as a domain mismatch.
    if (result == Result::ERROR_BAD_DER ||
        result == Result::ERROR_BAD_CERT_DOMAIN) {
      collectedErrors |= nsICertOverrideService::ERROR_MISMATCH;
      errorCodeMismatch = SSL_ERROR_BAD_CERT_DOMAIN;
    } else if (IsFatalError(result)) {
      // Because its input has not been validated by BuildCertChain,
      // CheckCertHostname can return an error that is less important than the
      // original certificate verification error. Only return an error result
      // from this function if we've encountered a fatal error.
      PR_SetError(MapResultToPRErrorCode(result), 0);
      return SECFailure;
    }
  }

  return SECSuccess;
}

// Helper function to determine if overrides are allowed for this host.
// Overrides are not allowed for known HSTS hosts or hosts with pinning
// information. However, IP addresses can never be HSTS hosts and don't have
// pinning information.
static nsresult OverrideAllowedForHost(
    uint64_t aPtrForLog, const nsACString& aHostname,
    const OriginAttributes& aOriginAttributes, uint32_t aProviderFlags,
    /*out*/ bool& aOverrideAllowed) {
  aOverrideAllowed = false;

  // If this is an IP address, overrides are allowed, because an IP address is
  // never an HSTS host. nsISiteSecurityService takes this into account
  // already, but the real problem here is that calling NS_NewURI with an IPv6
  // address fails. We do this to avoid that. A more comprehensive fix would be
  // to have Necko provide an nsIURI to PSM and to use that here (and
  // everywhere). However, that would be a wide-spanning change.
  if (net_IsValidIPv6Addr(aHostname)) {
    aOverrideAllowed = true;
    return NS_OK;
  }

  // If this is an HTTP Strict Transport Security host or a pinned host and the
  // certificate is bad, don't allow overrides (RFC 6797 section 12.1).
  bool strictTransportSecurityEnabled = false;
  bool isStaticallyPinned = false;
  nsCOMPtr<nsISiteSecurityService> sss(do_GetService(NS_SSSERVICE_CONTRACTID));
  if (!sss) {
    MOZ_LOG(
        gPIPNSSLog, LogLevel::Debug,
        ("[0x%" PRIx64 "] Couldn't get nsISiteSecurityService to check HSTS",
         aPtrForLog));
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri),
                          NS_LITERAL_CSTRING("https://") + aHostname);
  if (NS_FAILED(rv)) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("[0x%" PRIx64 "] Creating new URI failed", aPtrForLog));
    return rv;
  }

  rv = sss->IsSecureURI(nsISiteSecurityService::HEADER_HSTS, uri,
                        aProviderFlags, aOriginAttributes, nullptr, nullptr,
                        &strictTransportSecurityEnabled);
  if (NS_FAILED(rv)) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("[0x%" PRIx64 "] checking for HSTS failed", aPtrForLog));
    return rv;
  }

  rv = sss->IsSecureURI(nsISiteSecurityService::STATIC_PINNING, uri,
                        aProviderFlags, aOriginAttributes, nullptr, nullptr,
                        &isStaticallyPinned);
  if (NS_FAILED(rv)) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("[0x%" PRIx64 "] checking for static pin failed", aPtrForLog));
    return rv;
  }

  aOverrideAllowed = !strictTransportSecurityEnabled && !isStaticallyPinned;
  return NS_OK;
}

class SSLServerCertVerificationJob : public Runnable {
 public:
  // Must be called only on the socket transport thread
  static SECStatus Dispatch(const RefPtr<SharedCertVerifier>& certVerifier,
                            const void* fdForLogging,
                            nsNSSSocketInfo* infoObject,
                            const UniqueCERTCertificate& serverCert,
                            UniqueCERTCertList& peerCertChain,
                            Maybe<nsTArray<uint8_t>>& stapledOCSPResponse,
                            Maybe<nsTArray<uint8_t>>& sctsFromTLSExtension,
                            Maybe<DelegatedCredentialInfo>& dcInfo,
                            uint32_t providerFlags, Time time, PRTime prtime);

 private:
  NS_DECL_NSIRUNNABLE

  // Must be called only on the socket transport thread
  SSLServerCertVerificationJob(const RefPtr<SharedCertVerifier>& certVerifier,
                               const void* fdForLogging,
                               nsNSSSocketInfo* infoObject,
                               const UniqueCERTCertificate& cert,
                               UniqueCERTCertList peerCertChain,
                               Maybe<nsTArray<uint8_t>>& stapledOCSPResponse,
                               Maybe<nsTArray<uint8_t>>& sctsFromTLSExtension,
                               Maybe<DelegatedCredentialInfo>& dcInfo,
                               uint32_t providerFlags, Time time,
                               PRTime prtime);
  const RefPtr<SharedCertVerifier> mCertVerifier;
  const void* const mFdForLogging;
  const RefPtr<nsNSSSocketInfo> mInfoObject;
  const UniqueCERTCertificate mCert;
  UniqueCERTCertList mPeerCertChain;
  const uint32_t mProviderFlags;
  const Time mTime;
  const PRTime mPRTime;
  Maybe<nsTArray<uint8_t>> mStapledOCSPResponse;
  Maybe<nsTArray<uint8_t>> mSCTsFromTLSExtension;
  Maybe<DelegatedCredentialInfo> mDCInfo;
};

SSLServerCertVerificationJob::SSLServerCertVerificationJob(
    const RefPtr<SharedCertVerifier>& certVerifier, const void* fdForLogging,
    nsNSSSocketInfo* infoObject, const UniqueCERTCertificate& cert,
    UniqueCERTCertList peerCertChain,
    Maybe<nsTArray<uint8_t>>& stapledOCSPResponse,
    Maybe<nsTArray<uint8_t>>& sctsFromTLSExtension,
    Maybe<DelegatedCredentialInfo>& dcInfo, uint32_t providerFlags, Time time,
    PRTime prtime)
    : Runnable("psm::SSLServerCertVerificationJob"),
      mCertVerifier(certVerifier),
      mFdForLogging(fdForLogging),
      mInfoObject(infoObject),
      mCert(CERT_DupCertificate(cert.get())),
      mPeerCertChain(std::move(peerCertChain)),
      mProviderFlags(providerFlags),
      mTime(time),
      mPRTime(prtime),
      mStapledOCSPResponse(std::move(stapledOCSPResponse)),
      mSCTsFromTLSExtension(std::move(sctsFromTLSExtension)),
      mDCInfo(std::move(dcInfo)) {}

// This function assumes that we will only use the SPDY connection coalescing
// feature on connections where we have negotiated SPDY using NPN. If we ever
// talk SPDY without having negotiated it with SPDY, this code will give wrong
// and perhaps unsafe results.
//
// Returns SECSuccess on the initial handshake of all connections, on
// renegotiations for any connections where we did not negotiate SPDY, or on any
// SPDY connection where the server's certificate did not change.
//
// Prohibit changing the server cert only if we negotiated SPDY,
// in order to support SPDY's cross-origin connection pooling.
static SECStatus BlockServerCertChangeForSpdy(
    nsNSSSocketInfo* infoObject, const UniqueCERTCertificate& serverCert) {
  // Get the existing cert. If there isn't one, then there is
  // no cert change to worry about.
  nsCOMPtr<nsIX509Cert> cert;

  if (!infoObject->IsHandshakeCompleted()) {
    // first handshake on this connection, not a
    // renegotiation.
    return SECSuccess;
  }

  infoObject->GetServerCert(getter_AddRefs(cert));
  if (!cert) {
    MOZ_ASSERT_UNREACHABLE(
        "TransportSecurityInfo must have a cert implementing nsIX509Cert");
    PR_SetError(SEC_ERROR_LIBRARY_FAILURE, 0);
    return SECFailure;
  }

  // Filter out sockets that did not neogtiate SPDY via NPN
  nsAutoCString negotiatedNPN;
  nsresult rv = infoObject->GetNegotiatedNPN(negotiatedNPN);
  MOZ_ASSERT(NS_SUCCEEDED(rv),
             "GetNegotiatedNPN() failed during renegotiation");

  if (NS_SUCCEEDED(rv) &&
      !StringBeginsWith(negotiatedNPN, NS_LITERAL_CSTRING("spdy/"))) {
    return SECSuccess;
  }
  // If GetNegotiatedNPN() failed we will assume spdy for safety's safe
  if (NS_FAILED(rv)) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("BlockServerCertChangeForSpdy failed GetNegotiatedNPN() call."
             " Assuming spdy.\n"));
  }

  // Check to see if the cert has actually changed
  UniqueCERTCertificate c(cert->GetCert());
  MOZ_ASSERT(c, "Somehow couldn't get underlying cert from nsIX509Cert");
  bool sameCert = CERT_CompareCerts(c.get(), serverCert.get());
  if (sameCert) {
    return SECSuccess;
  }

  // Report an error - changed cert is confirmed
  MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
          ("SPDY Refused to allow new cert during renegotiation\n"));
  PR_SetError(SSL_ERROR_RENEGOTIATION_NOT_ALLOWED, 0);
  return SECFailure;
}

static void AuthCertificateSetResults(
    nsNSSSocketInfo* aInfoObject, const UniqueCERTCertificate& aCert,
    UniqueCERTCertList& aBuiltCertChain, UniqueCERTCertList& aPeerCertChain,
    const CertificateTransparencyInfo& aCertificateTransparencyInfo,
    SECOidTag aEvOidPolicy, bool aSucceeded) {
  MOZ_ASSERT(aInfoObject);

  if (aSucceeded) {
    // Certificate verification succeeded. Delete any potential record of
    // certificate error bits.
    RememberCertErrorsTable::GetInstance().RememberCertHasError(aInfoObject,
                                                                SECSuccess);

    EVStatus evStatus;
    if (aEvOidPolicy == SEC_OID_UNKNOWN) {
      evStatus = EVStatus::NotEV;
    } else {
      evStatus = EVStatus::EV;
    }

    RefPtr<nsNSSCertificate> nsc = nsNSSCertificate::Create(aCert.get());
    aInfoObject->SetServerCert(nsc, evStatus);

    aInfoObject->SetSucceededCertChain(std::move(aBuiltCertChain));
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("AuthCertificate setting NEW cert %p", nsc.get()));

    aInfoObject->SetCertificateTransparencyInfo(aCertificateTransparencyInfo);
  } else {
    // Certificate validation failed; store the peer certificate chain on
    // infoObject so it can be used for error reporting.
    aInfoObject->SetFailedCertChain(std::move(aPeerCertChain));
  }
}

// Note: Takes ownership of |peerCertChain| if SECSuccess is not returned.
Result AuthCertificate(CertVerifier& certVerifier, nsNSSSocketInfo* infoObject,
                       const UniqueCERTCertificate& cert,
                       UniqueCERTCertList& peerCertChain,
                       const Maybe<nsTArray<uint8_t>>& stapledOCSPResponse,
                       const Maybe<nsTArray<uint8_t>>& sctsFromTLSExtension,
                       const Maybe<DelegatedCredentialInfo>& dcInfo,
                       uint32_t providerFlags, Time time) {
  MOZ_ASSERT(infoObject);
  MOZ_ASSERT(cert);

  // We want to avoid storing any intermediate cert information when browsing
  // in private, transient contexts.
  bool saveIntermediates =
      !(providerFlags & nsISocketProvider::NO_PERMANENT_STORAGE);

  SECOidTag evOidPolicy;
  UniqueCERTCertList builtCertChain;
  CertificateTransparencyInfo certificateTransparencyInfo;

  int flags = 0;
  if (!infoObject->SharedState().IsOCSPStaplingEnabled() ||
      !infoObject->SharedState().IsOCSPMustStapleEnabled()) {
    flags |= CertVerifier::FLAG_TLS_IGNORE_STATUS_REQUEST;
  }

  nsTArray<nsTArray<uint8_t>> peerCertsBytes;
  for (CERTCertListNode* n = CERT_LIST_HEAD(peerCertChain);
       !CERT_LIST_END(n, peerCertChain); n = CERT_LIST_NEXT(n)) {
    // Don't include the end-entity certificate.
    if (n == CERT_LIST_HEAD(peerCertChain)) {
      continue;
    }
    nsTArray<uint8_t> certBytes;
    certBytes.AppendElements(n->cert->derCert.data, n->cert->derCert.len);
    peerCertsBytes.AppendElement(std::move(certBytes));
  }

  Result rv = certVerifier.VerifySSLServerCert(
      cert, time, infoObject, infoObject->GetHostName(), builtCertChain,
      flags, Some(peerCertsBytes), stapledOCSPResponse,
      sctsFromTLSExtension, dcInfo, infoObject->GetOriginAttributes(),
      saveIntermediates, &evOidPolicy, &certificateTransparencyInfo);

  AuthCertificateSetResults(infoObject, cert, builtCertChain, peerCertChain,
                            certificateTransparencyInfo, evOidPolicy,
                            rv == Success);
  return rv;
}

/*static*/
SECStatus SSLServerCertVerificationJob::Dispatch(
    const RefPtr<SharedCertVerifier>& certVerifier, const void* fdForLogging,
    nsNSSSocketInfo* infoObject, const UniqueCERTCertificate& serverCert,
    UniqueCERTCertList& peerCertChain,
    Maybe<nsTArray<uint8_t>>& stapledOCSPResponse,
    Maybe<nsTArray<uint8_t>>& sctsFromTLSExtension,
    Maybe<DelegatedCredentialInfo>& dcInfo, uint32_t providerFlags, Time time,
    PRTime prtime) {
  // Runs on the socket transport thread
  if (!certVerifier || !infoObject || !serverCert) {
    NS_ERROR("Invalid parameters for SSL server cert validation");
    PR_SetError(PR_INVALID_ARGUMENT_ERROR, 0);
    return SECFailure;
  }

  if (!gCertVerificationThreadPool) {
    PR_SetError(PR_INVALID_STATE_ERROR, 0);
    return SECFailure;
  }

  UniqueCERTCertList peerCertChainCopy = std::move(peerCertChain);

  RefPtr<SSLServerCertVerificationJob> job(new SSLServerCertVerificationJob(
      certVerifier, fdForLogging, infoObject, serverCert,
      std::move(peerCertChainCopy), stapledOCSPResponse, sctsFromTLSExtension,
      dcInfo, providerFlags, time, prtime));

  nsresult nrv = gCertVerificationThreadPool->Dispatch(job, NS_DISPATCH_NORMAL);
  if (NS_FAILED(nrv)) {
    // We can't call SetCertVerificationResult here to change
    // mCertVerificationState because SetCertVerificationResult will call
    // libssl functions that acquire SSL locks that are already being held at
    // this point. However, we can set an error with PR_SetError and return
    // SECFailure, and the correct thing will happen (the error will be
    // propagated and this connection will be terminated).
    PRErrorCode error = nrv == NS_ERROR_OUT_OF_MEMORY ? PR_OUT_OF_MEMORY_ERROR
                                                      : PR_INVALID_STATE_ERROR;
    PR_SetError(error, 0);
    return SECFailure;
  }

  PR_SetError(PR_WOULD_BLOCK_ERROR, 0);
  return SECWouldBlock;
}

PRErrorCode AuthCertificateParseResults(
    uint64_t aPtrForLog, const nsACString& aHostName, int32_t aPort,
    const OriginAttributes& aOriginAttributes,
    const UniqueCERTCertificate& aCert, uint32_t aProviderFlags, PRTime aPRTime,
    PRErrorCode aDefaultErrorCodeToReport,
    /* out */ uint32_t& aCollectedErrors) {
  if (aDefaultErrorCodeToReport == 0) {
    MOZ_ASSERT_UNREACHABLE(
        "No error set during certificate validation failure");
    return SEC_ERROR_LIBRARY_FAILURE;
  }

  aCollectedErrors = 0;
  PRErrorCode errorCodeTrust = 0;
  PRErrorCode errorCodeMismatch = 0;
  PRErrorCode errorCodeTime = 0;
  if (DetermineCertOverrideErrors(aCert, aHostName, aPRTime,
                                  aDefaultErrorCodeToReport, aCollectedErrors,
                                  errorCodeTrust, errorCodeMismatch,
                                  errorCodeTime) != SECSuccess) {
    PRErrorCode errorCode = PR_GetError();
    MOZ_ASSERT(!ErrorIsOverridable(errorCode));
    if (errorCode == 0) {
      MOZ_ASSERT_UNREACHABLE(
          "No error set during DetermineCertOverrideErrors failure");
      return SEC_ERROR_LIBRARY_FAILURE;
    }
    return errorCode;
  }

  if (!aCollectedErrors) {
    MOZ_ASSERT_UNREACHABLE("aCollectedErrors should not be 0");
    return SEC_ERROR_LIBRARY_FAILURE;
  }

  bool overrideAllowed = false;
  if (NS_FAILED(OverrideAllowedForHost(aPtrForLog, aHostName, aOriginAttributes,
                                       aProviderFlags, overrideAllowed))) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("[0x%" PRIx64 "] AuthCertificateParseResults - "
             "OverrideAllowedForHost failed\n",
             aPtrForLog));
    return aDefaultErrorCodeToReport;
  }

  if (overrideAllowed) {
    nsCOMPtr<nsICertOverrideService> overrideService =
        do_GetService(NS_CERTOVERRIDE_CONTRACTID);

    uint32_t overrideBits = 0;
    uint32_t remainingDisplayErrors = aCollectedErrors;

    // it is fine to continue without the nsICertOverrideService
    if (overrideService) {
      bool haveOverride;
      bool isTemporaryOverride;  // we don't care
      RefPtr<nsIX509Cert> nssCert(nsNSSCertificate::Create(aCert.get()));
      if (!nssCert) {
        MOZ_ASSERT(false, "nsNSSCertificate::Create failed");
        return SEC_ERROR_NO_MEMORY;
      }
      nsresult rv = overrideService->HasMatchingOverride(
          aHostName, aPort, nssCert, &overrideBits, &isTemporaryOverride,
          &haveOverride);
      if (NS_SUCCEEDED(rv) && haveOverride) {
        // remove the errors that are already overriden
        remainingDisplayErrors &= ~overrideBits;
      }
    }

    if (!remainingDisplayErrors) {
      // all errors are covered by override rules, so let's accept the cert
      MOZ_LOG(
          gPIPNSSLog, LogLevel::Debug,
          ("[0x%" PRIx64 "] All errors covered by override rules", aPtrForLog));
      return 0;
    }
  } else {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("[0x%" PRIx64 "] HSTS or pinned host - no overrides allowed\n",
             aPtrForLog));
  }

  MOZ_LOG(
      gPIPNSSLog, LogLevel::Debug,
      ("[0x%" PRIx64 "] Certificate error was not overridden\n", aPtrForLog));

  // pick the error code to report by priority
  return errorCodeTrust
             ? errorCodeTrust
             : errorCodeMismatch
                   ? errorCodeMismatch
                   : errorCodeTime ? errorCodeTime : aDefaultErrorCodeToReport;
}

NS_IMETHODIMP
SSLServerCertVerificationJob::Run() {
  // Runs on a cert verification thread and only on parent process.
  MOZ_ASSERT(XRE_IsParentProcess());

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
          ("[%p] SSLServerCertVerificationJob::Run\n", mInfoObject.get()));

  Result rv =
      AuthCertificate(*mCertVerifier, mInfoObject, mCert, mPeerCertChain,
                      mStapledOCSPResponse, mSCTsFromTLSExtension, mDCInfo,
                      mProviderFlags, mTime);
  MOZ_ASSERT(
      (mPeerCertChain && rv == Success) || (!mPeerCertChain && rv != Success),
      "AuthCertificate() should take ownership of chain on failure");

  if (rv == Success) {
    RefPtr<SSLServerCertVerificationResult> runnable(
        new SSLServerCertVerificationResult(mInfoObject, 0));
    runnable->Dispatch();
    return NS_OK;
  }

  PRErrorCode error = MapResultToPRErrorCode(rv);
  uint64_t addr = reinterpret_cast<uintptr_t>(mFdForLogging);
  uint32_t collectedErrors = 0;
  PRErrorCode finalError = AuthCertificateParseResults(
      addr, mInfoObject->GetHostName(), mInfoObject->GetPort(),
      mInfoObject->GetOriginAttributes(), mCert, mProviderFlags, mPRTime, error,
      collectedErrors);

  if (collectedErrors != 0) {
    RefPtr<nsNSSCertificate> nssCert(nsNSSCertificate::Create(mCert.get()));
    mInfoObject->SetStatusErrorBits(nssCert, collectedErrors);
  }

  // NB: finalError may be 0 here, in which the connection will continue.
  RefPtr<SSLServerCertVerificationResult> resultRunnable(
      new SSLServerCertVerificationResult(mInfoObject, finalError));
  resultRunnable->Dispatch();
  return NS_OK;
}

}  // unnamed namespace

// Extracts whatever information we need out of fd (using SSL_*) and passes it
// to SSLServerCertVerificationJob::Dispatch. SSLServerCertVerificationJob
// should never do anything with fd except logging.
SECStatus AuthCertificateHook(void* arg, PRFileDesc* fd, PRBool checkSig,
                              PRBool isServer) {
  RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
  if (!certVerifier) {
    PR_SetError(SEC_ERROR_NOT_INITIALIZED, 0);
    return SECFailure;
  }

  // Runs on the socket transport thread

  MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
          ("[%p] starting AuthCertificateHook\n", fd));

  // Modern libssl always passes PR_TRUE for checkSig, and we have no means of
  // doing verification without checking signatures.
  MOZ_ASSERT(checkSig, "AuthCertificateHook: checkSig unexpectedly false");

  // PSM never causes libssl to call this function with PR_TRUE for isServer,
  // and many things in PSM assume that we are a client.
  MOZ_ASSERT(!isServer, "AuthCertificateHook: isServer unexpectedly true");

  nsNSSSocketInfo* socketInfo = static_cast<nsNSSSocketInfo*>(arg);

  UniqueCERTCertificate serverCert(SSL_PeerCertificate(fd));

  if (!checkSig || isServer || !socketInfo || !serverCert) {
    PR_SetError(PR_INVALID_STATE_ERROR, 0);
    return SECFailure;
  }

  UniqueCERTCertList peerCertChain(SSL_PeerCertificateChain(fd));
  if (!peerCertChain) {
    PR_SetError(PR_INVALID_STATE_ERROR, 0);
    return SECFailure;
  }

  bool onSTSThread;
  nsresult nrv;
  nsCOMPtr<nsIEventTarget> sts =
      do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &nrv);
  if (NS_SUCCEEDED(nrv)) {
    nrv = sts->IsOnCurrentThread(&onSTSThread);
  }

  if (NS_FAILED(nrv)) {
    NS_ERROR("Could not get STS service or IsOnCurrentThread failed");
    PR_SetError(PR_UNKNOWN_ERROR, 0);
    return SECFailure;
  }

  MOZ_ASSERT(onSTSThread);

  if (!onSTSThread) {
    PR_SetError(PR_INVALID_STATE_ERROR, 0);
    return SECFailure;
  }

  socketInfo->SetFullHandshake();

  if (BlockServerCertChangeForSpdy(socketInfo, serverCert) != SECSuccess) {
    return SECFailure;
  }

  // SSL_PeerStapledOCSPResponses will never return a non-empty response if
  // OCSP stapling wasn't enabled because libssl wouldn't have let the server
  // return a stapled OCSP response.
  // We don't own these pointers.
  const SECItemArray* csa = SSL_PeerStapledOCSPResponses(fd);
  Maybe<nsTArray<uint8_t>> stapledOCSPResponse;
  // we currently only support single stapled responses
  if (csa && csa->len == 1) {
    stapledOCSPResponse.emplace();
    stapledOCSPResponse->SetCapacity(csa->items[0].len);
    stapledOCSPResponse->AppendElements(csa->items[0].data, csa->items[0].len);
  }

  Maybe<nsTArray<uint8_t>> sctsFromTLSExtension;
  const SECItem* sctsFromTLSExtensionSECItem = SSL_PeerSignedCertTimestamps(fd);
  if (sctsFromTLSExtensionSECItem) {
    sctsFromTLSExtension.emplace();
    sctsFromTLSExtension->SetCapacity(sctsFromTLSExtensionSECItem->len);
    sctsFromTLSExtension->AppendElements(sctsFromTLSExtensionSECItem->data,
                                         sctsFromTLSExtensionSECItem->len);
  }

  uint32_t providerFlags = 0;
  socketInfo->GetProviderFlags(&providerFlags);

  // Get DC information
  Maybe<DelegatedCredentialInfo> dcInfo;
  SSLPreliminaryChannelInfo channelPreInfo;
  SECStatus rv = SSL_GetPreliminaryChannelInfo(fd, &channelPreInfo,
                                               sizeof(channelPreInfo));
  if (rv != SECSuccess) {
    PR_SetError(PR_INVALID_STATE_ERROR, 0);
    return SECFailure;
  }
  if (channelPreInfo.peerDelegCred) {
    dcInfo.emplace(DelegatedCredentialInfo(channelPreInfo.signatureScheme,
                                           channelPreInfo.authKeyBits));
  }

  // We *must* do certificate verification on a background thread because
  // we need the socket transport thread to be free for our OCSP requests,
  // and we *want* to do certificate verification on a background thread
  // because of the performance benefits of doing so.
  socketInfo->SetCertVerificationWaiting();
  rv = SSLServerCertVerificationJob::Dispatch(
      certVerifier, static_cast<const void*>(fd), socketInfo, serverCert,
      peerCertChain, stapledOCSPResponse, sctsFromTLSExtension, dcInfo,
      providerFlags, Now(), PR_Now());
  return rv;
}

SSLServerCertVerificationResult::SSLServerCertVerificationResult(
    nsNSSSocketInfo* infoObject, PRErrorCode errorCode)
    : Runnable("psm::SSLServerCertVerificationResult"),
      mInfoObject(infoObject),
      mErrorCode(errorCode) {}

void SSLServerCertVerificationResult::Dispatch() {
  nsresult rv;
  nsCOMPtr<nsIEventTarget> stsTarget =
      do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
  MOZ_ASSERT(stsTarget, "Failed to get socket transport service event target");
  rv = stsTarget->Dispatch(this, NS_DISPATCH_NORMAL);
  MOZ_ASSERT(NS_SUCCEEDED(rv),
             "Failed to dispatch SSLServerCertVerificationResult");
}

NS_IMETHODIMP
SSLServerCertVerificationResult::Run() {
  // TODO: Assert that we're on the socket transport thread
  mInfoObject->SetCertVerificationResult(mErrorCode);
  return NS_OK;
}

}  // namespace psm
}  // namespace mozilla
