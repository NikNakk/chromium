// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Metal/Metal.h>

#include "gpu/ipc/service/metal_shared_event_waiter_mac.h"

#include <dispatch/dispatch.h>

#include <cstddef>
#include <utility>

#include "base/apple/scoped_nsobject.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/no_destructor.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"

namespace gpu {
namespace {

constexpr base::TimeDelta kMetalSharedEventTimeout = base::Milliseconds(250);

struct MetalSharedEventListenerStorage {
  MetalSharedEventListenerStorage() {
    queue = dispatch_queue_create(
        "org.chromium.webxr.metal-shared-event", DISPATCH_QUEUE_SERIAL);
    listener.reset(
        [[MTLSharedEventListener alloc] initWithDispatchQueue:queue]);
  }

  dispatch_queue_t queue = nullptr;
  base::apple::scoped_nsobject<MTLSharedEventListener> listener;
};

MTLSharedEventListener* GetMetalSharedEventListener() {
  static base::NoDestructor<MetalSharedEventListenerStorage> storage;
  return storage->listener.get();
}

class MetalSharedEventWaitState
    : public base::RefCountedThreadSafe<MetalSharedEventWaitState> {
 public:
  MetalSharedEventWaitState(size_t fence_count,
                            int32_t frame_index,
                            base::OnceClosure callback)
      : origin_task_runner_(base::SequencedTaskRunner::GetCurrentDefault()),
        remaining_(fence_count),
        frame_index_(frame_index),
        callback_(std::move(callback)) {}

  void ArmTimeout() {
    origin_task_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&MetalSharedEventWaitState::OnTimeout,
                       base::RetainedRef(this)),
        kMetalSharedEventTimeout);
  }

  void FenceFired() {
    bool completed = false;
    {
      base::AutoLock lock(lock_);
      if (done_) {
        return;
      }
      CHECK_GT(remaining_, 0u);
      remaining_--;
      if (remaining_ == 0) {
        done_ = true;
        completed = true;
      }
    }

    if (completed) {
      origin_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(&MetalSharedEventWaitState::RunCallback,
                         base::RetainedRef(this), true));
    }
  }

 private:
  friend class base::RefCountedThreadSafe<MetalSharedEventWaitState>;
  ~MetalSharedEventWaitState() = default;

  void OnTimeout() {
    bool timed_out = false;
    {
      base::AutoLock lock(lock_);
      if (!done_) {
        done_ = true;
        timed_out = true;
      }
    }
    if (!timed_out) {
      return;
    }

    DVLOG(1) << "WebXR Metal shared-event completion timed out for frame "
             << frame_index_;
    RunCallback(false);
  }

  void RunCallback(bool event_fired) {
    if (event_fired) {
      TRACE_EVENT_INSTANT("xr", "OpenXRMetalSharedEventFired",
                          "frame_index", frame_index_);
    } else {
      TRACE_EVENT_INSTANT("xr", "OpenXRMetalSharedEventTimeout",
                          "frame_index", frame_index_);
    }

    if (callback_) {
      std::move(callback_).Run();
    }
  }

  const scoped_refptr<base::SequencedTaskRunner> origin_task_runner_;
  base::Lock lock_;
  size_t remaining_ GUARDED_BY(lock_);
  bool done_ GUARDED_BY(lock_) = false;
  const int32_t frame_index_;
  base::OnceClosure callback_;
};

}  // namespace

void WaitForMetalSharedEventFences(
    std::vector<gfx::MTLSharedEventFence> fences,
    int32_t frame_index,
    base::OnceClosure callback) {
  fences = gfx::MTLSharedEventFence::Reduce(std::move(fences));
  if (fences.empty()) {
    DVLOG(2) << __func__ << ": no Metal shared-event fences for frame "
             << frame_index;
    std::move(callback).Run();
    return;
  }

  auto state = base::MakeRefCounted<MetalSharedEventWaitState>(
      fences.size(), frame_index, std::move(callback));
  state->ArmTimeout();

  MTLSharedEventListener* listener = GetMetalSharedEventListener();
  for (auto& fence : fences) {
    if (fence.HasSignaled()) {
      state->FenceFired();
      continue;
    }

    id<MTLSharedEvent> event = fence.GetSharedEvent();
    const uint64_t value = fence.fence_value();
    scoped_refptr<MetalSharedEventWaitState> retained_state = state;
    [event notifyListener:listener
                  atValue:value
                    block:^(id<MTLSharedEvent> shared_event,
                            uint64_t signaled_value) {
                      retained_state->FenceFired();
                    }];
  }
}

}  // namespace gpu
