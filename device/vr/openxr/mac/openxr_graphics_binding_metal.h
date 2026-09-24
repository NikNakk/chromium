// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_OPENXR_MAC_OPENXR_GRAPHICS_BINDING_METAL_H_
#define DEVICE_VR_OPENXR_MAC_OPENXR_GRAPHICS_BINDING_METAL_H_

#include <cstdint>
#include <memory>

#include "device/vr/openxr/openxr_graphics_binding.h"
#include "device/vr/vr_export.h"

namespace device {

// Metal-backed OpenXR graphics binding for macOS. This first stage establishes
// the OpenXR Metal device/queue and enumerates runtime swapchain textures.
// Chromium SharedImage -> Metal texture transport is added separately.
class DEVICE_VR_EXPORT OpenXrGraphicsBindingMetal
    : public OpenXrGraphicsBinding {
 public:
  explicit OpenXrGraphicsBindingMetal(
      const OpenXrExtensionEnumeration* extension_enum);
  OpenXrGraphicsBindingMetal(const OpenXrGraphicsBindingMetal&) = delete;
  OpenXrGraphicsBindingMetal& operator=(const OpenXrGraphicsBindingMetal&) =
      delete;
  ~OpenXrGraphicsBindingMetal() override;

  bool Initialize(XrInstance instance, XrSystemId system) override;
  const void* GetSessionCreateInfo() const override;
  int64_t GetSwapchainFormat(XrSession session) const override;
  XrResult EnumerateSwapchainImages(OpenXrCompositionLayer& layer) override;
  bool CanUseSharedImages() const override;
  void CleanupWithoutSubmit() override;
  gfx::Size GetMaxTextureSize() override;
  bool SetOverlayTexture(gfx::GpuMemoryBufferHandle texture,
                         const gpu::SyncToken& sync_token,
                         const gfx::RectF& left,
                         const gfx::RectF& right) override;
  void OnSwapchainImageReady(OpenXrCompositionLayer& layer,
                             gpu::SharedImageInterface* sii) override;
  bool SupportsLayers() const override;
  void ResizeSharedBuffer(OpenXrCompositionLayer& layer,
                          OpenXrSwapchainInfo& swap_chain_info,
                          gpu::SharedImageInterface* sii) override;

 protected:
  bool WaitOnFence(OpenXrCompositionLayer& layer,
                   gfx::GpuFence& gpu_fence) override;
  bool RenderLayer(
      OpenXrCompositionLayer& layer,
      const scoped_refptr<viz::ContextProvider>& context_provider) override;
  void CreateSharedImages(OpenXrCompositionLayer& layer,
                          gpu::SharedImageInterface* sii) override;
  bool ShouldFlipSubmittedImage(OpenXrCompositionLayer& layer) const override;
  std::unique_ptr<OpenXrCompositionLayer::GraphicsBindingData>
  CreateLayerGraphicsBindingData() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  mutable int64_t swapchain_format_ = 0;
};

}  // namespace device

#endif  // DEVICE_VR_OPENXR_MAC_OPENXR_GRAPHICS_BINDING_METAL_H_
