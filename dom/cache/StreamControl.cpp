/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/StreamControl.h"

namespace mozilla {
namespace dom {
namespace cache {

void StreamControl::AddReadStream(ReadStream::Controllable* aReadStream) {
  AssertOwningThread();
  MOZ_DIAGNOSTIC_ASSERT(aReadStream);
  MOZ_ASSERT(!mReadStreamList.Contains(aReadStream));
  mReadStreamList.AppendElement(aReadStream);
}

void StreamControl::ForgetReadStream(ReadStream::Controllable* aReadStream) {
  AssertOwningThread();
  MOZ_ALWAYS_TRUE(mReadStreamList.RemoveElement(aReadStream));
}

void StreamControl::NoteClosed(ReadStream::Controllable* aReadStream,
                               const nsID& aId) {
  AssertOwningThread();
  ForgetReadStream(aReadStream);
  NoteClosedAfterForget(aId);
}

StreamControl::~StreamControl() {
  // owning thread only, but can't call virtual AssertOwningThread in destructor
  MOZ_DIAGNOSTIC_ASSERT(mReadStreamList.IsEmpty());
}

void StreamControl::CloseReadStreams(const nsID& aId) {
  AssertOwningThread();
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  uint32_t closedCount = 0;
#endif

  for (const auto& stream : mReadStreamList.ForwardRange()) {
    if (stream->MatchId(aId)) {
      const auto pinnedStream = stream;
      pinnedStream->CloseStream();
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
      closedCount += 1;
#endif
    }
  }

  MOZ_DIAGNOSTIC_ASSERT(closedCount > 0);
}

void StreamControl::CloseAllReadStreams() {
  AssertOwningThread();

  auto readStreamList = mReadStreamList;
  for (const auto& stream : readStreamList.ForwardRange()) {
    stream->CloseStream();
  }
}

void StreamControl::CloseAllReadStreamsWithoutReporting() {
  AssertOwningThread();

  for (const auto& stream : mReadStreamList.ForwardRange()) {
    const auto pinnedStream = stream;
    // Note, we cannot trigger IPC traffic here.  So use
    // CloseStreamWithoutReporting().
    pinnedStream->CloseStreamWithoutReporting();
  }
}

bool StreamControl::HasEverBeenRead() const {
  const auto range = mReadStreamList.NonObservingRange();
  return std::any_of(range.begin(), range.end(), [](const auto& stream) {
    return stream->HasEverBeenRead();
  });
}

}  // namespace cache
}  // namespace dom
}  // namespace mozilla
