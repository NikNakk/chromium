// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_IPC_SERVICE_SHARED_IMAGE_STUB_H_
#define GPU_IPC_SERVICE_SHARED_IMAGE_STUB_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "build/build_config.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/sequence_id.h"
#include "gpu/command_buffer/service/sync_point_manager.h"
#include "gpu/ipc/common/command_buffer_id.h"
#include "gpu/ipc/common/gpu_channel.mojom.h"
#include "gpu/ipc/service/gpu_ipc_service_export.h"
#include "ui/gfx/gpu_extra_info.h"

#if BUILDFLAG(IS_WIN)
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#endif

namespace gfx {
#if BUILDFLAG(IS_WIN)
class D3DSharedFence;
#endif

struct GpuFenceHandle;
}  // namespace gfx

namespace gpu {
class SharedContextState;
struct Mailbox;
class GpuChannel;
class GpuChannelSharedImageInterface;
class SharedImageFactory;

class GPU_IPC_SERVICE_EXPORT SharedImageStub {
 public:
  ~SharedImageStub();

  static std::unique_ptr<SharedImageStub> Create(GpuChannel* channel,
                                                 int32_t route_id);

  // Executes a DeferredRequest routed to this stub by a GpuChannel.
  void ExecuteDeferredRequest(mojom::DeferredSharedImageRequestPtr request);

  // Get memory size from MemoryTracker.
  uint64_t GetSize() const;

  SequenceId sequence() const { return sequence_; }
  SharedImageFactory* factory() const { return factory_.get(); }
  GpuChannel* channel() const { return channel_; }
  scoped_refptr<SharedContextState>& shared_context_state() {
    return context_state_;
  }
  const scoped_refptr<gpu::GpuChannelSharedImageInterface>&
  shared_image_interface();

#if BUILDFLAG(IS_WIN)
  void CopyToGpuMemoryBufferAsync(const Mailbox& mailbox,
                                  base::OnceCallback<void(bool)> callback);
#endif

#if BUILDFLAG(IS_FUCHSIA)
  void RegisterSysmemBufferCollection(zx::eventpair service_handle,
                                      zx::channel sysmem_token,
                                      const viz::SharedImageFormat& format,
                                      gfx::BufferUsage usage,
                                      bool register_with_image_pipe);
#endif  // BUILDFLAG(IS_FUCHSIA)

  void SetGpuExtraInfo(const gfx::GpuExtraInfo& gpu_extra_info);

  bool MakeContextCurrent(bool needs_gl = false);

  MemoryTracker* memory_tracker() { return memory_tracker_.get(); }

 private:
  SharedImageStub(GpuChannel* channel, int32_t route_id);

  void OnCreateSharedImage(mojom::CreateSharedImageParamsPtr params);
  void OnCreateSharedImageWithData(
      mojom::CreateSharedImageWithDataParamsPtr params);
  void OnCreateSharedImageWithBuffer(
      mojom::CreateSharedImageWithBufferParamsPtr params);
  base::WeakPtrFactory<SharedImageStub> weak_factory_{this};
};

}  // namespace gpu

#endif  // GPU_IPC_SERVICE_SHARED_IMAGE_STUB_H_
