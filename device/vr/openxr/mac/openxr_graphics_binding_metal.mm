// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Metal/Metal.h>

#include "device/vr/openxr/mac/openxr_graphics_binding_metal.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/logging.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "device/vr/openxr/openxr_composition_layer.h"
#include "device/vr/openxr/openxr_platform.h"
#include "device/vr/openxr/openxr_swapchain_info.h"
#include "device/vr/openxr/openxr_util.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/ipc/common/surface_handle.h"
#include "third_party/openxr/src/include/openxr/openxr.h"
#include "ui/gfx/buffer_types.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"

namespace device {

namespace {

constexpr MTLPixelFormat kSupportedFormats[] = {
    MTLPixelFormatBGRA8Unorm_sRGB,
    MTLPixelFormatBGRA8Unorm,
};

}  // namespace

// static
void OpenXrGraphicsBinding::GetRequiredExtensions(
    std::vector<const char*>& extensions) {
  extensions.push_back(XR_KHR_METAL_ENABLE_EXTENSION_NAME);
}

class OpenXrGraphicsBindingMetal::Impl {
 public:
  id<MTLDevice> __strong device = nil;
  id<MTLCommandQueue> __strong command_queue = nil;
  XrGraphicsBindingMetalKHR binding{XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
};

OpenXrGraphicsBindingMetal::OpenXrGraphicsBindingMetal(
    const OpenXrExtensionEnumeration* extension_enum)
    : OpenXrGraphicsBinding(extension_enum), impl_(std::make_unique<Impl>()) {}

OpenXrGraphicsBindingMetal::~OpenXrGraphicsBindingMetal() = default;

bool OpenXrGraphicsBindingMetal::Initialize(XrInstance instance,
                                            XrSystemId system) {
  if (impl_->command_queue != nil) {
    return true;
  }

  PFN_xrGetMetalGraphicsRequirementsKHR get_requirements = nullptr;
  XrResult result = xrGetInstanceProcAddr(
      instance, "xrGetMetalGraphicsRequirementsKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
  if (XR_FAILED(result) || !get_requirements) {
    DLOG(ERROR) << "xrGetMetalGraphicsRequirementsKHR is unavailable";
    return false;
  }

  XrGraphicsRequirementsMetalKHR requirements{
      XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
  result = get_requirements(instance, system, &requirements);
  if (XR_FAILED(result) || !requirements.metalDevice) {
    DLOG(ERROR) << "Failed to obtain OpenXR Metal graphics requirements: "
                << result;
    return false;
  }

  impl_->device = (__bridge id<MTLDevice>)requirements.metalDevice;
  impl_->command_queue = [impl_->device newCommandQueue];
  if (impl_->command_queue == nil) {
    DLOG(ERROR) << "Failed to create Metal command queue for OpenXR device";
    impl_->device = nil;
    return false;
  }

  impl_->binding.commandQueue = (__bridge void*)impl_->command_queue;
  DVLOG(1) << "Initialized OpenXR Metal graphics binding";
  return true;
}

const void* OpenXrGraphicsBindingMetal::GetSessionCreateInfo() const {
  return impl_->command_queue != nil ? &impl_->binding : nullptr;
}

int64_t OpenXrGraphicsBindingMetal::GetSwapchainFormat(XrSession session) const {
  uint32_t count = 0;
  if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &count, nullptr)) ||
      count == 0) {
    return 0;
  }

  std::vector<int64_t> formats(count);
  if (XR_FAILED(
          xrEnumerateSwapchainFormats(session, count, &count, formats.data()))) {
    return 0;
  }

  for (MTLPixelFormat candidate : kSupportedFormats) {
    const int64_t value = static_cast<int64_t>(candidate);
    if (std::ranges::find(formats, value) != formats.end()) {
      swapchain_format_ = value;
      DVLOG(1) << "OpenXR Metal swapchain format negotiated: " << value;
      return value;
    }
  }

  DLOG(ERROR) << "Runtime offers no supported BGRA8 Metal swapchain format";
  return 0;
}

XrResult OpenXrGraphicsBindingMetal::EnumerateSwapchainImages(
    OpenXrCompositionLayer& layer) {
  CHECK(layer.HasColorSwapchain());
  CHECK(layer.GetSwapchainImages().empty());

  uint32_t chain_length = 0;
  RETURN_IF_XR_FAILED(xrEnumerateSwapchainImages(
      layer.color_swapchain(), 0, &chain_length, nullptr));

  std::vector<XrSwapchainImageMetalKHR> xr_images(
      chain_length, {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR});
  RETURN_IF_XR_FAILED(xrEnumerateSwapchainImages(
      layer.color_swapchain(), xr_images.size(), &chain_length,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data())));

  std::vector<OpenXrSwapchainInfo> images;
  images.reserve(xr_images.size());
  for (const auto& image : xr_images) {
    images.emplace_back(image.texture);
  }
  layer.SetSwapchainImages(std::move(images));
  return XR_SUCCESS;
}

bool OpenXrGraphicsBindingMetal::CanUseSharedImages() const {
  return impl_->device != nil && impl_->command_queue != nil;
}

bool OpenXrGraphicsBindingMetal::RequiresSharedImages() const {
  // macOS has no texture-handle fallback equivalent to the Windows path.
  // WebXR renders into an IOSurface-backed SharedImage which is copied into
  // the runtime-owned Metal swapchain texture before xrEndFrame.
  return true;
}

void OpenXrGraphicsBindingMetal::CleanupWithoutSubmit() {}

gfx::Size OpenXrGraphicsBindingMetal::GetMaxTextureSize() {
  return gfx::Size(16384, 16384);
}

bool OpenXrGraphicsBindingMetal::SetOverlayTexture(
    gfx::GpuMemoryBufferHandle texture,
    const gpu::SyncToken& sync_token,
    const gfx::RectF& left,
    const gfx::RectF& right) {
  return true;
}

void OpenXrGraphicsBindingMetal::OnSwapchainImageReady(
    OpenXrCompositionLayer& layer,
    gpu::SharedImageInterface* sii) {
  OpenXrSwapchainInfo* swap_chain_info = layer.GetActiveSwapchainImage();
  CHECK(swap_chain_info);
  ResizeSharedBuffer(layer, *swap_chain_info, sii);
}

bool OpenXrGraphicsBindingMetal::SupportsLayers() const {
  return false;
}

void OpenXrGraphicsBindingMetal::ResizeSharedBuffer(
    OpenXrCompositionLayer& layer,
    OpenXrSwapchainInfo& swap_chain_info,
    gpu::SharedImageInterface* sii) {
  CHECK(sii);

  // For the first Metal implementation keep the renderer-visible IOSurface at
  // the OpenXR swapchain size. This avoids an extra scaling render pass; WebXR
  // framebufferScaleFactor support can be added once the zero-copy path is
  // established.
  const gfx::Size buffer_size = layer.GetSwapchainImageSize();
  if (buffer_size.IsEmpty()) {
    DLOG(ERROR) << __func__ << ": empty swapchain image size";
    return;
  }

  if (swap_chain_info.shared_image &&
      swap_chain_info.shared_buffer_size == buffer_size) {
    return;
  }

  if (swap_chain_info.shared_image) {
    swap_chain_info.shared_image->UpdateDestructionSyncToken(
        swap_chain_info.sync_token);
    swap_chain_info.shared_image.reset();
    swap_chain_info.sync_token.Clear();
  }
  swap_chain_info.shared_buffer_size = {0, 0};

  gpu::SharedImageUsageSet shared_image_usage =
      gpu::SHARED_IMAGE_USAGE_SCANOUT | gpu::SHARED_IMAGE_USAGE_DISPLAY_READ |
      gpu::SHARED_IMAGE_USAGE_GLES2_READ | gpu::SHARED_IMAGE_USAGE_GLES2_WRITE;

  if (layer.read_only_data().needs_raster_access) {
    shared_image_usage |= gpu::SHARED_IMAGE_USAGE_RASTER_READ |
                          gpu::SHARED_IMAGE_USAGE_RASTER_WRITE;
  }
  if (IsWebGPUSession()) {
    shared_image_usage |= gpu::SHARED_IMAGE_USAGE_WEBGPU_READ |
                          gpu::SHARED_IMAGE_USAGE_WEBGPU_WRITE;
  }

  const gpu::SharedImageInfo si_info{
      viz::SinglePlaneFormat::kBGRA_8888, buffer_size,
      gfx::ColorSpace(gfx::ColorSpace::PrimaryID::BT709,
                      gfx::ColorSpace::TransferID::LINEAR),
      shared_image_usage, "OpenXrMetalBinding"};

  scoped_refptr<gpu::ClientSharedImage> shared_image =
      sii->CreateSharedImage(si_info, gpu::kNullSurfaceHandle,
                             gfx::BufferUsage::SCANOUT);
  if (!shared_image) {
    DLOG(ERROR) << __func__ << ": failed to allocate IOSurface SharedImage";
    return;
  }

  // This overload of CreateSharedImage must produce a clonable GMB handle.
  // Validate that the Mac backing really is an IOSurface before exposing the
  // mailbox to Blink; RenderLayer relies on importing this same IOSurface into
  // Metal.
  gfx::GpuMemoryBufferHandle handle =
      shared_image->CloneGpuMemoryBufferHandle();
  if (handle.type != gfx::IO_SURFACE_BUFFER || !handle.io_surface().get()) {
    DLOG(ERROR) << __func__ << ": SharedImage is not IOSurface-backed";
    return;
  }

  swap_chain_info.shared_image = std::move(shared_image);
  swap_chain_info.sync_token = sii->GenVerifiedSyncToken();
  swap_chain_info.shared_buffer_size = buffer_size;
}

bool OpenXrGraphicsBindingMetal::WaitOnFence(OpenXrCompositionLayer& layer,
                                             gfx::GpuFence& gpu_fence) {
  // gfx::GpuFence::Wait() has no macOS implementation. The render loop forces
  // the GL-finish synchronization path on macOS before calling RenderLayer(),
  // so a GPU fence should never reach this binding.
  DLOG(ERROR) << __func__ << ": unexpected GPU fence on macOS";
  return false;
}

bool OpenXrGraphicsBindingMetal::RenderLayer(
    OpenXrCompositionLayer& layer,
    const scoped_refptr<viz::ContextProvider>& context_provider) {
  OpenXrSwapchainInfo* swap_chain_info = layer.GetActiveSwapchainImage();
  if (!swap_chain_info || !swap_chain_info->shared_image ||
      swap_chain_info->metal_texture.get() == nullptr) {
    return false;
  }

  gfx::GpuMemoryBufferHandle handle =
      swap_chain_info->shared_image->CloneGpuMemoryBufferHandle();
  if (handle.type != gfx::IO_SURFACE_BUFFER || !handle.io_surface().get()) {
    DLOG(ERROR) << __func__ << ": active SharedImage has no IOSurface";
    return false;
  }

  const gfx::Size size = swap_chain_info->shared_buffer_size;
  if (size.IsEmpty()) {
    return false;
  }

  // Import the renderer-visible IOSurface on the same Metal device required by
  // the OpenXR runtime. The descriptor mirrors Chromium's IOSurface SharedImage
  // Metal representation.
  MTLTextureDescriptor* texture_desc =
      [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:
              static_cast<MTLPixelFormat>(swapchain_format_)
                                   width:size.width()
                                  height:size.height()
                               mipmapped:NO];
  texture_desc.usage =
      MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  texture_desc.storageMode = MTLStorageModePrivate;

  id<MTLTexture> __strong source_texture =
      [impl_->device newTextureWithDescriptor:texture_desc
                                    iosurface:handle.io_surface().get()
                                        plane:0];
  if (source_texture == nil) {
    DLOG(ERROR) << __func__ << ": failed to import IOSurface as MTLTexture";
    return false;
  }

  id<MTLTexture> destination_texture =
      (__bridge id<MTLTexture>)swap_chain_info->metal_texture.get();
  if (destination_texture == nil ||
      destination_texture.width != static_cast<NSUInteger>(size.width()) ||
      destination_texture.height != static_cast<NSUInteger>(size.height())) {
    DLOG(ERROR) << __func__ << ": OpenXR Metal texture size mismatch";
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (command_buffer == nil) {
    DLOG(ERROR) << __func__ << ": failed to create MTLCommandBuffer";
    return false;
  }

  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  if (blit == nil) {
    DLOG(ERROR) << __func__ << ": failed to create MTLBlitCommandEncoder";
    return false;
  }

  const MTLOrigin origin = MTLOriginMake(0, 0, 0);
  const MTLSize copy_size =
      MTLSizeMake(static_cast<NSUInteger>(size.width()),
                  static_cast<NSUInteger>(size.height()), 1);
  [blit copyFromTexture:source_texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:origin
             sourceSize:copy_size
              toTexture:destination_texture
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:origin];
  [blit endEncoding];
  [command_buffer commit];

  // Correctness-first synchronization. OpenXR may consume the swapchain image
  // as soon as it is released, so do not release it until the Metal copy is
  // complete. A MTLSharedEvent/semaphore hand-off can replace this blocking
  // wait once the path is functionally verified.
  [command_buffer waitUntilCompleted];
  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
    DLOG(ERROR) << __func__ << ": Metal copy failed, status="
                << static_cast<int>(command_buffer.status);
    return false;
  }

  return true;
}

void OpenXrGraphicsBindingMetal::CreateSharedImages(
    OpenXrCompositionLayer& layer,
    gpu::SharedImageInterface* sii) {
  CHECK(sii);
  for (auto& swap_chain_info : layer.GetSwapchainImages()) {
    ResizeSharedBuffer(layer, swap_chain_info, sii);
  }
}

bool OpenXrGraphicsBindingMetal::ShouldFlipSubmittedImage(
    OpenXrCompositionLayer& layer) const {
  return true;
}

std::unique_ptr<OpenXrCompositionLayer::GraphicsBindingData>
OpenXrGraphicsBindingMetal::CreateLayerGraphicsBindingData() const {
  return nullptr;
}

}  // namespace device
