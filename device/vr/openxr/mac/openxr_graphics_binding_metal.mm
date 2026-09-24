// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Metal/Metal.h>

#include "device/vr/openxr/mac/openxr_graphics_binding_metal.h"

#include <dlfcn.h>

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
#include "third_party/openxr/src/include/openxr/openxr.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/gpu_fence.h"

namespace device {

namespace {

constexpr MTLPixelFormat kSupportedFormats[] = {
    MTLPixelFormatBGRA8Unorm_sRGB,
    MTLPixelFormatBGRA8Unorm,
};

using PublishClaimableTextureFn =
    int (*)(void* metal_texture, uint64_t* out_token);

constexpr char kMonadoMetalXpcHelperPath[] =
    "/usr/local/lib/libmonado_metal_xpc_client.dylib";

PublishClaimableTextureFn GetPublishClaimableTextureFn() {
  static PublishClaimableTextureFn fn = []() -> PublishClaimableTextureFn {
    void* library =
        dlopen(kMonadoMetalXpcHelperPath, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      DLOG(ERROR) << "Unable to load Monado Metal XPC helper '"
                  << kMonadoMetalXpcHelperPath << "': " << dlerror();
      return nullptr;
    }

    auto* publish = reinterpret_cast<PublishClaimableTextureFn>(
        dlsym(library, "monado_metal_xpc_publish_claimable_texture"));
    if (!publish) {
      DLOG(ERROR) << "Monado Metal XPC helper is missing publish export";
    }
    // Intentionally keep the helper loaded for the process lifetime: generated
    // SharedImages may still depend on its XPC connection machinery.
    return publish;
  }();
  return fn;
}

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
  // The external EGLImage backing used by this path provides WebGL/GL
  // representations. Chromium's Dawn/Metal SharedImage representation does not
  // yet import EGL_METAL_TEXTURE_ANGLE, so do not advertise WebGPU support.
  return !IsWebGPUSession() && impl_->device != nil &&
         GetPublishClaimableTextureFn() != nullptr;
}

bool OpenXrGraphicsBindingMetal::RequiresSharedImages() const {
  // Unlike Windows there is no texture-handle fallback. Blink renders directly
  // into the same shared MTLTexture storage returned by the OpenXR runtime.
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
    gpu::SharedImageInterface* sii) {}

bool OpenXrGraphicsBindingMetal::SupportsLayers() const {
  return false;
}

void OpenXrGraphicsBindingMetal::ResizeSharedBuffer(
    OpenXrCompositionLayer& layer,
    OpenXrSwapchainInfo& swap_chain_info,
    gpu::SharedImageInterface* sii) {}

bool OpenXrGraphicsBindingMetal::WaitOnFence(OpenXrCompositionLayer& layer,
                                             gfx::GpuFence& gpu_fence) {
  // The macOS render loop uses SharedImageInterface::SignalSyncToken instead
  // of gfx::GpuFence. gfx::GpuFence::Wait() has no macOS implementation.
  DLOG(ERROR) << __func__ << ": unexpected GpuFence on macOS";
  return false;
}

bool OpenXrGraphicsBindingMetal::RenderLayer(
    OpenXrCompositionLayer& layer,
    const scoped_refptr<viz::ContextProvider>& context_provider) {
  // No copy/composite is required for the base WebXR layer: Blink/ANGLE wrote
  // directly into the OpenXR runtime's shared MTLTexture storage.
  const OpenXrSwapchainInfo* swap_chain_info =
      layer.GetActiveSwapchainImage();
  return swap_chain_info && swap_chain_info->shared_image;
}

void OpenXrGraphicsBindingMetal::CreateSharedImages(
    OpenXrCompositionLayer& layer,
    gpu::SharedImageInterface* sii) {
  CHECK(sii);
  if (IsWebGPUSession()) {
    DLOG(ERROR) << __func__
                << ": direct Metal SharedImages do not yet support WebGPU";
    return;
  }

  PublishClaimableTextureFn publish_texture = GetPublishClaimableTextureFn();
  if (!publish_texture) {
    DLOG(ERROR) << __func__ << ": Monado Metal XPC helper unavailable";
    return;
  }

  const gfx::Size size = layer.GetSwapchainImageSize();
  if (size.IsEmpty()) {
    DLOG(ERROR) << __func__ << ": empty swapchain image size";
    return;
  }

  gpu::SharedImageUsageSet usage =
      gpu::SHARED_IMAGE_USAGE_DISPLAY_READ |
      gpu::SHARED_IMAGE_USAGE_GLES2_READ |
      gpu::SHARED_IMAGE_USAGE_GLES2_WRITE;
  if (layer.read_only_data().needs_raster_access) {
    usage |= gpu::SHARED_IMAGE_USAGE_RASTER_READ |
             gpu::SHARED_IMAGE_USAGE_RASTER_WRITE;
  }
  const gpu::SharedImageInfo si_info{
      viz::SinglePlaneFormat::kBGRA_8888, size,
      gfx::ColorSpace(gfx::ColorSpace::PrimaryID::BT709,
                      gfx::ColorSpace::TransferID::LINEAR),
      usage, "OpenXrMetalDirect"};

  for (auto& swap_chain_info : layer.GetSwapchainImages()) {
    if (swap_chain_info.shared_image) {
      continue;
    }
    void* metal_texture = swap_chain_info.metal_texture.get();
    if (!metal_texture) {
      DLOG(ERROR) << __func__ << ": OpenXR swapchain texture is null";
      return;
    }

    id<MTLTexture> texture = (__bridge id<MTLTexture>)metal_texture;
    if (texture.device != impl_->device) {
      DLOG(ERROR) << __func__
                  << ": runtime swapchain texture belongs to wrong MTLDevice";
      return;
    }
    if (texture.width != static_cast<NSUInteger>(size.width()) ||
        texture.height != static_cast<NSUInteger>(size.height())) {
      DLOG(ERROR) << __func__ << ": runtime texture size "
                  << texture.width << "x" << texture.height
                  << " does not match OpenXR swapchain size "
                  << size.ToString();
      return;
    }
    if (static_cast<int64_t>(texture.pixelFormat) != swapchain_format_) {
      DLOG(ERROR) << __func__ << ": runtime texture pixel format "
                  << static_cast<uint64_t>(texture.pixelFormat)
                  << " does not match negotiated format "
                  << swapchain_format_;
      return;
    }

    uint64_t texture_token = 0;
    if (publish_texture(metal_texture, &texture_token) != 0 ||
        texture_token == 0) {
      DLOG(ERROR) << __func__
                  << ": failed to publish claimable Monado texture token";
      return;
    }

    // Chromium's current projection swapchain is a double-wide 2D texture
    // (arraySize=1). array_slice remains explicit in the transport so future
    // array-backed layer types can select a Metal texture slice directly.
    swap_chain_info.shared_image =
        sii->CreateSharedImageFromMetalTextureToken(si_info, texture_token,
                                                    /*array_slice=*/0);
    if (!swap_chain_info.shared_image) {
      DLOG(ERROR) << __func__
                  << ": failed to create SharedImage from Metal token";
      return;
    }
    swap_chain_info.sync_token = sii->GenVerifiedSyncToken();
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
