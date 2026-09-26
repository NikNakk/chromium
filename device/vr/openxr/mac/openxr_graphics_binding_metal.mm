// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Metal/Metal.h>

#include "device/vr/openxr/mac/openxr_graphics_binding_metal.h"

#include <dlfcn.h>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/logging.h"
#include "base/memory/scoped_policy.h"
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
#include "ui/gfx/gpu_memory_buffer_handle.h"
#include "ui/gfx/mac/io_surface.h"

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

id<MTLTexture> CreateIOSurfaceMetalTexture(id<MTLDevice> device,
                                           IOSurfaceRef io_surface,
                                           const gfx::Size& size,
                                           MTLPixelFormat pixel_format) {
  MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
  descriptor.textureType = MTLTextureType2D;
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                     MTLTextureUsageRenderTarget;
  descriptor.pixelFormat = pixel_format;
  descriptor.width = size.width();
  descriptor.height = size.height();
  descriptor.depth = 1;
  descriptor.mipmapLevelCount = 1;
  descriptor.arrayLength = 1;
  descriptor.sampleCount = 1;
  descriptor.storageMode = MTLStorageModeShared;

  return [device newTextureWithDescriptor:descriptor
                                iosurface:io_surface
                                    plane:0];
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

  // Most runtimes either expose an IOSurface-backed swapchain texture directly
  // or, for Monado, can publish that texture through Monado's Metal XPC helper.
  // If neither direct path is possible, Blink renders into one of these
  // Chromium-owned IOSurface textures and RenderLayer() blits it into the
  // runtime-owned OpenXR texture before release/submission.
  // Objective-C object pointers stored in C++ containers are strong under ARC;
  // libc++ invokes the ARC copy/destroy semantics as map entries move and die.
  std::map<void*, id<MTLTexture>> fallback_textures;

  id<MTLRenderPipelineState> ScalePipeline(MTLPixelFormat pixel_format) {
    auto existing = scale_pipelines.find(static_cast<uint64_t>(pixel_format));
    if (existing != scale_pipelines.end()) {
      return existing->second;
    }

    if (scale_library == nil) {
      static constexpr char kScaleShaderSource[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct ScaleVertexOut {
  float4 position [[position]];
  float2 texcoord;
};

vertex ScaleVertexOut xr_scale_vertex(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[3] = {
      float2(-1.0, -1.0),
      float2( 3.0, -1.0),
      float2(-1.0,  3.0),
  };
  constexpr float2 texcoords[3] = {
      float2(0.0, 1.0),
      float2(2.0, 1.0),
      float2(0.0, -1.0),
  };

  ScaleVertexOut out;
  out.position = float4(positions[vertex_id], 0.0, 1.0);
  out.texcoord = texcoords[vertex_id];
  return out;
}

fragment float4 xr_scale_fragment(
    ScaleVertexOut in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler source_sampler [[sampler(0)]]) {
  return source.sample(source_sampler, in.texcoord);
}
)metal";
      NSString* scale_shader =
          [NSString stringWithUTF8String:kScaleShaderSource];

      NSError* error = nil;
      scale_library = [device newLibraryWithSource:scale_shader
                                           options:nil
                                             error:&error];
      if (scale_library == nil) {
        DLOG(ERROR) << "Failed to compile OpenXR Metal scale shader: "
                    << (error ? error.localizedDescription.UTF8String
                              : "unknown error");
        return nil;
      }

      scale_vertex = [scale_library newFunctionWithName:@"xr_scale_vertex"];
      scale_fragment = [scale_library newFunctionWithName:@"xr_scale_fragment"];
      if (scale_vertex == nil || scale_fragment == nil) {
        DLOG(ERROR) << "OpenXR Metal scale shader functions are unavailable";
        return nil;
      }

      MTLSamplerDescriptor* sampler_descriptor =
          [[MTLSamplerDescriptor alloc] init];
      sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
      sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
      sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
      sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
      scale_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
      if (scale_sampler == nil) {
        DLOG(ERROR) << "Failed to create OpenXR Metal scale sampler";
        return nil;
      }
    }

    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = scale_vertex;
    descriptor.fragmentFunction = scale_fragment;
    descriptor.colorAttachments[0].pixelFormat = pixel_format;

    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) {
      DLOG(ERROR) << "Failed to create OpenXR Metal scale pipeline: "
                  << (error ? error.localizedDescription.UTF8String
                            : "unknown error");
      return nil;
    }

    scale_pipelines.emplace(static_cast<uint64_t>(pixel_format), pipeline);
    return pipeline;
  }

  id<MTLSamplerState> ScaleSampler() const { return scale_sampler; }

 private:
  id<MTLLibrary> __strong scale_library = nil;
  id<MTLFunction> __strong scale_vertex = nil;
  id<MTLFunction> __strong scale_fragment = nil;
  id<MTLSamplerState> __strong scale_sampler = nil;
  std::map<uint64_t, id<MTLRenderPipelineState>> scale_pipelines;
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
  // WebGL can always use the runtime-neutral IOSurface copy fallback. Monado
  // additionally gets the zero-copy XPC-token path, and runtimes whose Metal
  // swapchain textures are already IOSurface-backed get a generic zero-copy
  // path. WebGPU still needs a native Dawn/Metal import path.
  return !IsWebGPUSession() && impl_->device != nil;
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
  // The direct Metal SharedImage path can expose ordinary 2D OpenXR
  // composition-layer swapchains to Blink without an intermediate copy.
  // Projection, quad, cylinder and equirect layers all use 2D color
  // swapchains and can share the same transport as the base projection layer.
  return true;
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
  const OpenXrSwapchainInfo* swap_chain_info =
      layer.GetActiveSwapchainImage();
  if (!swap_chain_info || !swap_chain_info->shared_image) {
    return false;
  }

  auto fallback =
      impl_->fallback_textures.find(swap_chain_info->metal_texture.get());
  if (fallback == impl_->fallback_textures.end()) {
    // Direct IOSurface or Monado XPC-token path: Blink/ANGLE rendered into the
    // runtime's storage, so there is nothing to copy.
    return true;
  }

  id<MTLTexture> source_texture = fallback->second;
  id<MTLTexture> runtime_texture =
      (__bridge id<MTLTexture>)swap_chain_info->metal_texture.get();
  if (!source_texture || !runtime_texture ||
      source_texture.pixelFormat != runtime_texture.pixelFormat) {
    DLOG(ERROR) << __func__
                << ": fallback and runtime Metal textures are incompatible";
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
  if (!command_buffer) {
    DLOG(ERROR) << __func__ << ": failed to create Metal command buffer";
    return false;
  }

  if (source_texture.width == runtime_texture.width &&
      source_texture.height == runtime_texture.height) {
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (!blit) {
      DLOG(ERROR) << __func__ << ": failed to create Metal blit command";
      return false;
    }

    [blit copyFromTexture:source_texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(source_texture.width,
                                      source_texture.height, 1)
                toTexture:runtime_texture
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
  } else {
    id<MTLRenderPipelineState> pipeline =
        impl_->ScalePipeline(runtime_texture.pixelFormat);
    id<MTLSamplerState> sampler = impl_->ScaleSampler();
    if (!pipeline || !sampler ||
        !(runtime_texture.usage & MTLTextureUsageRenderTarget)) {
      DLOG(ERROR) << __func__
                  << ": runtime texture cannot accept scaled Metal fallback";
      return false;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = runtime_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      DLOG(ERROR) << __func__ << ": failed to create Metal scale encoder";
      return false;
    }

    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source_texture atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];

    DVLOG(1) << __func__ << ": scaled WebXR transfer "
             << source_texture.width << "x" << source_texture.height
             << " -> OpenXR swapchain " << runtime_texture.width << "x"
             << runtime_texture.height;
  }

  [command_buffer commit];

  // The fallback is deliberately conservative. The runtime may composite on a
  // different queue/API (Meta XR Simulator bridges Metal to Vulkan), so make
  // the copy complete before xrReleaseSwapchainImage hands ownership back.
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError) {
    DLOG(ERROR) << __func__ << ": Metal fallback blit failed";
    return false;
  }

  return true;
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

  const gfx::Size runtime_size = layer.GetSwapchainImageSize();
  gfx::Size transfer_size = layer.GetTransferSize();
  if (transfer_size.IsEmpty()) {
    transfer_size = runtime_size;
  }
  if (runtime_size.IsEmpty() || transfer_size.IsEmpty()) {
    DLOG(ERROR) << __func__ << ": empty swapchain/transfer image size";
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
  const gfx::ColorSpace color_space(
      gfx::ColorSpace::PrimaryID::BT709,
      gfx::ColorSpace::TransferID::LINEAR);
  const gpu::SharedImageInfo direct_si_info{
      viz::SinglePlaneFormat::kBGRA_8888, runtime_size, color_space, usage,
      "OpenXrMetalDirect"};
  const gpu::SharedImageInfo fallback_si_info{
      viz::SinglePlaneFormat::kBGRA_8888, transfer_size, color_space, usage,
      "OpenXrMetalTransfer"};

  PublishClaimableTextureFn publish_texture = GetPublishClaimableTextureFn();

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
    if (texture.textureType != MTLTextureType2D || texture.sampleCount != 1) {
      DLOG(ERROR) << __func__ << ": runtime swapchain texture must be a "
                     "single-sample MTLTextureType2D, got type="
                  << static_cast<uint64_t>(texture.textureType)
                  << " samples=" << texture.sampleCount;
      return;
    }
    if (texture.width != static_cast<NSUInteger>(runtime_size.width()) ||
        texture.height != static_cast<NSUInteger>(runtime_size.height())) {
      DLOG(ERROR) << __func__ << ": runtime texture size "
                  << texture.width << "x" << texture.height
                  << " does not match OpenXR swapchain size "
                  << runtime_size.ToString();
      return;
    }
    if (static_cast<int64_t>(texture.pixelFormat) != swapchain_format_) {
      DLOG(ERROR) << __func__ << ": runtime texture pixel format "
                  << static_cast<uint64_t>(texture.pixelFormat)
                  << " does not match negotiated format "
                  << swapchain_format_;
      return;
    }

    DVLOG(1) << __func__ << ": runtime Metal texture iosurface="
             << (texture.iosurface != nullptr)
             << " storageMode=" << static_cast<uint64_t>(texture.storageMode)
             << " usage=" << static_cast<uint64_t>(texture.usage);

    // Monado zero-copy path. The helper turns the runtime-owned MTLTexture into
    // a claimable XPC token which the GPU process resolves on ANGLE's device.
    //
    // Do this before considering any generic IOSurface sharing. Monado's helper
    // also supplies the runtime-specific cross-process synchronization needed
    // for a runtime-owned texture to be written safely by Chromium.
    if (transfer_size == runtime_size && publish_texture) {
      uint64_t texture_token = 0;
      if (publish_texture(metal_texture, &texture_token) == 0 &&
          texture_token != 0) {
        swap_chain_info.shared_image =
            sii->CreateSharedImageFromMetalTextureToken(
                direct_si_info, texture_token, /*array_slice=*/0);
        if (swap_chain_info.shared_image) {
          impl_->fallback_textures.erase(metal_texture);
          swap_chain_info.sync_token = sii->GenVerifiedSyncToken();
          LOG(INFO) << __func__
                    << ": transport=monado-xpc-zero-copy layer="
                    << layer.GetLayerId()
                    << " size=" << runtime_size.ToString();
          continue;
        }
        DLOG(WARNING) << __func__
                      << ": Monado token could not be imported as a "
                         "SharedImage; using IOSurface copy fallback";
      }
    }

    // Do not expose an arbitrary runtime-owned IOSurface directly to ANGLE.
    // That looked attractive as a generic zero-copy path, but ownership alone
    // is not enough: an OpenXR runtime may have graphics-API interop state and
    // synchronization associated with the MTLTexture object it returned.
    // Meta XR Simulator, for example, implements Metal on top of a Vulkan
    // compositor. Chromium writing the IOSurface from a separate process/queue
    // can therefore race the runtime's interop even though the pixels are in
    // shared storage. Use the explicit copy path below unless the runtime
    // provides a synchronization-aware transport such as Monado's XPC helper.

    // Runtime-neutral fallback. Render into a Chromium-owned IOSurface that can
    // be shared with Blink/ANGLE, then copy it into the runtime swapchain
    // texture in RenderLayer(). This costs one GPU blit per submitted layer but
    // lets arbitrary macOS OpenXR runtimes work without understanding Monado's
    // XPC transport.
    gfx::ScopedIOSurface fallback_surface = gfx::CreateIOSurface(
        transfer_size, viz::SinglePlaneFormat::kBGRA_8888,
        /*should_clear=*/true);
    if (!fallback_surface) {
      DLOG(ERROR) << __func__ << ": failed to allocate fallback IOSurface";
      return;
    }

    id<MTLTexture> fallback_texture = CreateIOSurfaceMetalTexture(
        impl_->device, fallback_surface.get(), transfer_size,
        texture.pixelFormat);
    if (!fallback_texture) {
      DLOG(ERROR) << __func__
                  << ": failed to create fallback Metal texture from IOSurface";
      return;
    }

    swap_chain_info.shared_image = sii->CreateSharedImage(
        fallback_si_info,
        gfx::GpuMemoryBufferHandle(gfx::ScopedIOSurface(
            fallback_surface.get(), base::scoped_policy::RETAIN)));
    if (!swap_chain_info.shared_image) {
      DLOG(ERROR) << __func__
                  << ": failed to create SharedImage for fallback IOSurface";
      return;
    }

    impl_->fallback_textures[metal_texture] = fallback_texture;
    swap_chain_info.sync_token = sii->GenVerifiedSyncToken();
    LOG(INFO) << __func__
              << ": transport=iosurface-copy layer=" << layer.GetLayerId()
              << " transfer=" << transfer_size.ToString()
              << " runtime=" << runtime_size.ToString()
              << " runtime_iosurface=" << (texture.iosurface != nullptr);
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
