/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OSReauthenticator.h"

#include "OSKeyStore.h"
#include "nsNetCID.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/Logging.h"
#include "nsISupportsUtils.h"
#include "nsThreadUtils.h"

NS_IMPL_ISUPPORTS(OSReauthenticator, nsIOSReauthenticator)

extern mozilla::LazyLogModule gCredentialManagerSecretLog;

using namespace mozilla;
using dom::Promise;

#if defined(XP_WIN)
#  include <combaseapi.h>
#  include <ntsecapi.h>
#  include <wincred.h>
#  include <windows.h>
struct HandleCloser {
  typedef HANDLE pointer;
  void operator()(HANDLE h) {
    if (h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
  }
};
struct BufferFreer {
  typedef LPVOID pointer;
  void operator()(LPVOID b) { CoTaskMemFree(b); }
};
typedef std::unique_ptr<HANDLE, HandleCloser> ScopedHANDLE;
typedef std::unique_ptr<LPVOID, BufferFreer> ScopedBuffer;

// Get the token info holding the sid.
std::unique_ptr<char[]> GetTokenInfo(ScopedHANDLE& token) {
  DWORD length = 0;
  // https://docs.microsoft.com/en-us/windows/desktop/api/securitybaseapi/nf-securitybaseapi-gettokeninformation
  mozilla::Unused << GetTokenInformation(token.get(), TokenUser, nullptr, 0,
                                         &length);
  if (!length || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
            ("Unable to obtain current token info."));
    return nullptr;
  }
  std::unique_ptr<char[]> token_info(new char[length]);
  if (!GetTokenInformation(token.get(), TokenUser, token_info.get(), length,
                           &length)) {
    MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
            ("Unable to obtain current token info (second call, possible "
             "system error."));
    return nullptr;
  }
  return token_info;
}

std::unique_ptr<char[]> GetUserTokenInfo() {
  // Get current user sid to make sure the same user got logged in.
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    // Couldn't get a process token. This will fail any unlock attempts later.
    MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
            ("Unable to obtain process token."));
    return nullptr;
  }
  ScopedHANDLE scopedToken(token);
  return GetTokenInfo(scopedToken);
}
#endif  // XP_WIN

static nsresult ReauthenticateUser(const nsACString& prompt,
                                   /* out */ bool& reauthenticated) {
  reauthenticated = false;
#if defined(XP_WIN)
  //return ReauthenticateUserWindows(prompt, reauthenticated);
#elif defined(XP_MACOSX)
  return ReauthenticateUserMacOS(prompt, reauthenticated);
#endif  // Reauthentication is not implemented for this platform.
  return NS_OK;
}

static void BackgroundReauthenticateUser(RefPtr<Promise>& aPromise,
                                         const nsACString& aPrompt) {
  nsAutoCString recovery;
  bool reauthenticated;
  nsresult rv = ReauthenticateUser(aPrompt, reauthenticated);
  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "BackgroundReauthenticateUserResolve",
      [rv, reauthenticated, aPromise = std::move(aPromise)]() {
        if (NS_FAILED(rv)) {
          aPromise->MaybeReject(rv);
        } else {
          aPromise->MaybeResolve(reauthenticated);
        }
      }));
  NS_DispatchToMainThread(runnable.forget());
}

NS_IMETHODIMP
OSReauthenticator::AsyncReauthenticateUser(const nsACString& aPrompt,
                                           JSContext* aCx,
                                           Promise** promiseOut) {
  NS_ENSURE_ARG_POINTER(aCx);

  RefPtr<Promise> promiseHandle;
  nsresult rv = GetPromise(aCx, promiseHandle);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "BackgroundReauthenticateUser",
      [promiseHandle, aPrompt = nsAutoCString(aPrompt)]() mutable {
        BackgroundReauthenticateUser(promiseHandle, aPrompt);
      }));

  nsCOMPtr<nsIEventTarget> target(
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID));
  if (!target) {
    return NS_ERROR_FAILURE;
  }
  rv = target->Dispatch(runnable, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  promiseHandle.forget(promiseOut);
  return NS_OK;
}
