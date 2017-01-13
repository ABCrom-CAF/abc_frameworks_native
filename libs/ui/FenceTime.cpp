/*
* Copyright 2016 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <ui/FenceTime.h>

#include <cutils/compiler.h>  // For CC_[UN]LIKELY
#include <inttypes.h>
#include <stdlib.h>

#include <memory>

namespace android {

// ============================================================================
// FenceTime
// ============================================================================

const auto FenceTime::NO_FENCE = std::make_shared<FenceTime>(Fence::NO_FENCE);

void* FenceTime::operator new(size_t byteCount) noexcept {
    void *p = nullptr;
    if (posix_memalign(&p, alignof(FenceTime), byteCount)) {
        return nullptr;
    }
    return p;
}

void FenceTime::operator delete(void *p) {
    free(p);
}

FenceTime::FenceTime(const sp<Fence>& fence)
  : mState(((fence.get() != nullptr) && fence->isValid()) ?
            State::VALID : State::INVALID),
    mFence(fence),
    mSignalTime(mState == State::INVALID ?
            Fence::SIGNAL_TIME_INVALID : Fence::SIGNAL_TIME_PENDING) {
}

FenceTime::FenceTime(sp<Fence>&& fence)
  : mState(((fence.get() != nullptr) && fence->isValid()) ?
            State::VALID : State::INVALID),
    mFence(std::move(fence)),
    mSignalTime(mState == State::INVALID ?
            Fence::SIGNAL_TIME_INVALID : Fence::SIGNAL_TIME_PENDING) {
}

FenceTime::FenceTime(nsecs_t signalTime)
  : mState(Fence::isValidTimestamp(signalTime) ? State::VALID : State::INVALID),
    mFence(nullptr),
    mSignalTime(signalTime == Fence::SIGNAL_TIME_PENDING ?
            Fence::SIGNAL_TIME_INVALID : signalTime) {
}

void FenceTime::applyTrustedSnapshot(const Snapshot& src) {
    if (CC_UNLIKELY(src.state != Snapshot::State::SIGNAL_TIME)) {
        // Applying Snapshot::State::FENCE, could change the valid state of the
        // FenceTime, which is not allowed. Callers should create a new
        // FenceTime from the snapshot instead.
        ALOGE("FenceTime::applyTrustedSnapshot: Unexpected fence.");
        return;
    }

    if (src.state == Snapshot::State::EMPTY) {
        return;
    }

    nsecs_t signalTime = mSignalTime.load(std::memory_order_relaxed);
    if (signalTime != Fence::SIGNAL_TIME_PENDING) {
        // We should always get the same signalTime here that we did in
        // getSignalTime(). This check races with getSignalTime(), but it is
        // only a sanity check so that's okay.
        if (CC_UNLIKELY(signalTime != src.signalTime)) {
            ALOGE("FenceTime::applyTrustedSnapshot: signalTime mismatch. "
                    "(%" PRId64 " (old) != %" PRId64 " (new))",
                    signalTime, src.signalTime);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    mFence.clear();
    mSignalTime.store(src.signalTime, std::memory_order_relaxed);
}

bool FenceTime::isValid() const {
    // We store the valid state in the constructors and return it here.
    // This lets release code remember the valid state even after the
    // underlying fence is destroyed.
    return mState != State::INVALID;
}

nsecs_t FenceTime::getSignalTime() {
    // See if we already have a cached value we can return.
    nsecs_t signalTime = mSignalTime.load(std::memory_order_relaxed);
    if (signalTime != Fence::SIGNAL_TIME_PENDING) {
        return signalTime;
    }

    // Hold a reference to the fence on the stack in case the class'
    // reference is removed by another thread. This prevents the
    // fence from being destroyed until the end of this method, where
    // we conveniently do not have the lock held.
    sp<Fence> fence;
    {
        // With the lock acquired this time, see if we have the cached
        // value or if we need to poll the fence.
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mFence.get()) {
            // Another thread set the signal time just before we added the
            // reference to mFence.
            return mSignalTime.load(std::memory_order_relaxed);
        }
        fence = mFence;
    }

    // Make the system call without the lock held.
    signalTime = fence->getSignalTime();

    // Make the signal time visible to everyone if it is no longer pending
    // and remove the class' reference to the fence.
    if (signalTime != Fence::SIGNAL_TIME_PENDING) {
        std::lock_guard<std::mutex> lock(mMutex);
        mFence.clear();
        mSignalTime.store(signalTime, std::memory_order_relaxed);
    }

    return signalTime;
}

nsecs_t FenceTime::getCachedSignalTime() const {
    // memory_order_acquire since we don't have a lock fallback path
    // that will do an acquire.
    return mSignalTime.load(std::memory_order_acquire);
}

FenceTime::Snapshot FenceTime::getSnapshot() const {
    // Quick check without the lock.
    nsecs_t signalTime = mSignalTime.load(std::memory_order_relaxed);
    if (signalTime != Fence::SIGNAL_TIME_PENDING) {
        return Snapshot(signalTime);
    }

    // Do the full check with the lock.
    std::lock_guard<std::mutex> lock(mMutex);
    signalTime = mSignalTime.load(std::memory_order_relaxed);
    if (signalTime != Fence::SIGNAL_TIME_PENDING) {
        return Snapshot(signalTime);
    }
    return Snapshot(mFence);
}

// ============================================================================
// FenceTime::Snapshot
// ============================================================================

FenceTime::Snapshot::Snapshot(const sp<Fence>& srcFence)
    : state(State::FENCE), fence(srcFence) {
}

FenceTime::Snapshot::Snapshot(nsecs_t srcSignalTime)
    : state(State::SIGNAL_TIME), signalTime(srcSignalTime) {
}

size_t FenceTime::Snapshot::getFlattenedSize() const {
    constexpr size_t min = sizeof(state);
    switch (state) {
        case State::EMPTY:
            return min;
        case State::FENCE:
            return min + fence->getFlattenedSize();
        case State::SIGNAL_TIME:
            return min + sizeof(signalTime);
    }
    return 0;
}

size_t FenceTime::Snapshot::getFdCount() const {
    return state == State::FENCE ? fence->getFdCount() : 0u;
}

status_t FenceTime::Snapshot::flatten(
        void*& buffer, size_t& size, int*& fds, size_t& count) const {
    if (size < getFlattenedSize()) {
        return NO_MEMORY;
    }

    FlattenableUtils::write(buffer, size, state);
    switch (state) {
        case State::EMPTY:
            return NO_ERROR;
        case State::FENCE:
            return fence->flatten(buffer, size, fds, count);
        case State::SIGNAL_TIME:
            FlattenableUtils::write(buffer, size, signalTime);
            return NO_ERROR;
    }

    return NO_ERROR;
}

status_t FenceTime::Snapshot::unflatten(
        void const*& buffer, size_t& size, int const*& fds, size_t& count) {
    if (size < sizeof(state)) {
        return NO_MEMORY;
    }

    FlattenableUtils::read(buffer, size, state);
    switch (state) {
        case State::EMPTY:
            return NO_ERROR;
        case State::FENCE:
            fence = new Fence;
            return fence->unflatten(buffer, size, fds, count);
        case State::SIGNAL_TIME:
            if (size < sizeof(signalTime)) {
                return NO_MEMORY;
            }
            FlattenableUtils::read(buffer, size, signalTime);
            return NO_ERROR;
    }

    return NO_ERROR;
}

// ============================================================================
// FenceTimeline
// ============================================================================
void FenceTimeline::push(const std::shared_ptr<FenceTime>& fence) {
    std::lock_guard<std::mutex> lock(mMutex);
    while (mQueue.size() >= MAX_ENTRIES) {
        // This is a sanity check to make sure the queue doesn't grow unbounded.
        // MAX_ENTRIES should be big enough not to trigger this path.
        // In case this path is taken though, users of FenceTime must make sure
        // not to rely solely on FenceTimeline to get the final timestamp and
        // should eventually call Fence::getSignalTime on their own.
        std::shared_ptr<FenceTime> front = mQueue.front().lock();
        if (front) {
            // Make a last ditch effort to get the signalTime here since
            // we are removing it from the timeline.
            front->getSignalTime();
        }
        mQueue.pop();
    }
    mQueue.push(fence);
}

void FenceTimeline::updateSignalTimes() {
    while (!mQueue.empty()) {
        std::lock_guard<std::mutex> lock(mMutex);
        std::shared_ptr<FenceTime> fence = mQueue.front().lock();
        if (!fence) {
            // The shared_ptr no longer exists and no one cares about the
            // timestamp anymore.
            mQueue.pop();
            continue;
        } else if (fence->getSignalTime() != Fence::SIGNAL_TIME_PENDING) {
            // The fence has signaled and we've removed the sp<Fence> ref.
            mQueue.pop();
            continue;
        } else {
            // The fence didn't signal yet. Break since the later ones
            // shouldn't have signaled either.
            break;
        }
    }
}

} // namespace android
