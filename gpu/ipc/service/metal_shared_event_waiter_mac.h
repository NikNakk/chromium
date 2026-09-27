// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_IPC_SERVICE_METAL_SHARED_EVENT_WAITER_MAC_H_
#define GPU_IPC_SERVICE_METAL_SHARED_EVENT_WAITER_MAC_H_

#include <cstdint>
#include <vector>

#include "base/functional/callback.h"
#include "ui/gfx/mac/mtl_shared_event_fence.h"

namespace gpu {

// Asynchronously waits until every Metal shared-event fence has reached its
// value, then runs |callback| back on the calling sequence. A bounded timeout
// prevents a lost GPU/context from permanently wedging the XR submission path.
void WaitForMetalSharedEventFences(
    std::vector<gfx::MTLSharedEventFence> fences,
    int32_t frame_index,
    base::OnceClosure callback);

}  // namespace gpu

#endif  // GPU_IPC_SERVICE_METAL_SHARED_EVENT_WAITER_MAC_H_
