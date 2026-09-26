// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/gpu/xr_raster_frame_transport_delegate.h"

#include "gpu/command_buffer/client/raster_interface.h"
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"

namespace blink {

void XRRasterFrameTransportDelegate::WaitOnFence(gfx::GpuFence* fence) {}

void XRRasterFrameTransportDelegate::VerifySyncToken(
    gpu::SyncToken& sync_token) {
  if (!sync_token.HasData() || sync_token.verified_flush()) {
    return;
  }

  auto wrapper = SharedGpuContext::ContextProviderWrapper();
  if (!wrapper) {
    return;
  }

  gpu::raster::RasterInterface* raster_interface =
      wrapper->ContextProvider().RasterInterface();
  if (!raster_interface) {
    return;
  }

  int8_t* sync_token_data = sync_token.GetData();
  raster_interface->VerifySyncTokensCHROMIUM(&sync_token_data, 1);
}

std::pair<gfx::GpuMemoryBufferHandle, gpu::SyncToken>
XRRasterFrameTransportDelegate::CopyImage(SharedImageHolder* image,
                                          bool last_transfer_succeeded) {
  return {gfx::GpuMemoryBufferHandle(), gpu::SyncToken()};
}

bool XRRasterFrameTransportDelegate::IsContextLost() {
  auto wrapper = SharedGpuContext::ContextProviderWrapper();
  if (!wrapper || wrapper->ContextProvider().IsContextLost()) {
    return true;
  }
  // We rely on the RasterInterface to create sync tokens.
  return !wrapper->ContextProvider().RasterInterface();
}
}  // namespace blink
