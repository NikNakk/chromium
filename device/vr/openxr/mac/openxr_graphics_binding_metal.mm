// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include "device/vr/openxr/mac/openxr_graphics_binding_metal.h"


#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/logging.h"
#include "base/memory/scoped_policy.h"
#include "base/trace_event/trace_event.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "device/vr/openxr/openxr_composition_layer.h"
#include "device/vr/openxr/openxr_platform.h"
#include "device/vr/openxr/spherical_video_mesh_parser.h"
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

// IOSurface keys shared with the ANGLE Metal bridge
// (apply_angle_metal_foveation.py). The metadata dictionary carries
// "version": kFoveationMetadataVersion; ANGLE writes the serial of the
// metadata it actually rendered with under the "-applied" key.
constexpr uint32_t kFoveationMetadataVersion = 2;
constexpr uint32_t kFoveationMetadataZoneCount = 16;

constexpr MTLPixelFormat kSupportedFormats[] = {
    MTLPixelFormatBGRA8Unorm_sRGB,
    MTLPixelFormatBGRA8Unorm,
};

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxMeshRenderVertices = 1000000;

// Temporary transport diagnostics for the generic Metal runtime path. Keep the
// readback deliberately sparse: it is only intended to distinguish a black or
// opaque Chromium transfer surface from a black/stale runtime swapchain.
constexpr NSUInteger kTransferDiagnosticGrid = 4;
constexpr NSUInteger kTransferDiagnosticStride = 256;
constexpr uint64_t kTransferDiagnosticInitialFrames = 16;
constexpr uint64_t kTransferDiagnosticPeriod = 60;

struct MetalTextureFingerprint {
  uint32_t samples = 0;
  uint32_t nonblack = 0;
  uint32_t alpha_zero = 0;
  uint32_t alpha_opaque = 0;
  uint64_t rgb_sum = 0;
  uint64_t alpha_sum = 0;
  uint64_t hash = 1469598103934665603ULL;
};

struct MeshRenderVertex {
  float position_x;
  float position_y;
  float texcoord_u;
  float texcoord_v;
};

struct MetalLayerData : public OpenXrCompositionLayer::GraphicsBindingData {
  MetalLayerData() { type = kMetal; }

  bool mesh_initialized = false;
  id<MTLBuffer> __strong mesh_vertex_buffer = nil;
  NSUInteger mesh_vertex_count = 0;
};

MetalLayerData& GetMetalLayerData(OpenXrCompositionLayer& layer) {
  auto* data = static_cast<MetalLayerData*>(layer.graphics_binding_data());
  CHECK(data);
  CHECK(data->type == OpenXrCompositionLayer::GraphicsBindingData::kMetal);
  return *data;
}

struct MeshRenderTransform {
  float destination_x_scale = 1.0f;
  float destination_x_offset = 0.0f;
  float destination_y_scale = 1.0f;
  float destination_y_offset = 0.0f;
  float source_u_scale = 1.0f;
  float source_u_offset = 0.0f;
  float source_v_scale = 1.0f;
  float source_v_offset = 0.0f;
};

bool AppendMeshTriangle(
    const SphericalVideoMesh& mesh,
    const std::array<uint32_t, 3>& indices,
    const MeshRenderTransform& transform,
    std::vector<MeshRenderVertex>* output) {
  std::array<double, 3> destination_u;
  std::array<double, 3> destination_y;
  for (size_t i = 0; i < indices.size(); ++i) {
    if (indices[i] >= mesh.vertices.size()) {
      return false;
    }
    const auto& vertex = mesh.vertices[indices[i]];
    const double length =
        std::sqrt(static_cast<double>(vertex.x) * vertex.x +
                  static_cast<double>(vertex.y) * vertex.y +
                  static_cast<double>(vertex.z) * vertex.z);
    if (!(length > 0.0)) {
      return false;
    }
    // Spherical Video V2 mesh coordinates use the same OpenGL-style
    // right-handed convention as the equirectangular direction used by the
    // existing media path: -Z forward, +X right, +Y up.
    const double longitude = std::atan2(vertex.x, -vertex.z);
    const double normalized_y =
        std::clamp(static_cast<double>(vertex.y) / length, -1.0, 1.0);
    const double latitude = std::asin(normalized_y);
    destination_u[i] = 0.5 + longitude / (2.0 * kPi);
    destination_y[i] = 2.0 * latitude / kPi;
  }

  const auto [min_u, max_u] =
      std::minmax_element(destination_u.begin(), destination_u.end());
  const bool crosses_seam = *max_u - *min_u > 0.5;
  if (crosses_seam) {
    for (double& u : destination_u) {
      if (u < 0.5) {
        u += 1.0;
      }
    }
  }

  auto append_copy = [&](double longitude_offset) {
    if (output->size() > kMaxMeshRenderVertices - 3) {
      return false;
    }
    for (size_t i = 0; i < indices.size(); ++i) {
      const auto& vertex = mesh.vertices[indices[i]];
      const double equirect_u = destination_u[i] + longitude_offset;
      const float destination_x =
          static_cast<float>(2.0 * equirect_u - 1.0) *
              transform.destination_x_scale +
          transform.destination_x_offset;
      const float destination_y_value =
          static_cast<float>(destination_y[i]) *
              transform.destination_y_scale +
          transform.destination_y_offset;

      const float source_u =
          vertex.u * transform.source_u_scale + transform.source_u_offset;
      const float source_v_gl =
          vertex.v * transform.source_v_scale + transform.source_v_offset;
      // Spherical Video V2 UVs use an OpenGL-style lower-left origin. The
      // IOSurface-backed Metal texture is sampled with a top-left origin.
      const float source_v_metal = 1.0f - source_v_gl;

      output->push_back(
          {destination_x, destination_y_value, source_u, source_v_metal});
    }
    return true;
  };

  if (!append_copy(0.0)) {
    return false;
  }
  // A triangle spanning the +/-pi longitude seam must be rasterized on both
  // clipped sides of the equirectangular target.
  if (crosses_seam && !append_copy(-1.0)) {
    return false;
  }
  return true;
}

bool AppendMeshWithTransform(
    const SphericalVideoMesh& mesh,
    const MeshRenderTransform& transform,
    std::vector<MeshRenderVertex>* output) {
  if (mesh.triangle_indices.size() % 3 != 0) {
    return false;
  }

  for (size_t i = 0; i < mesh.triangle_indices.size(); i += 3) {
    if (!AppendMeshTriangle(
            mesh,
            {mesh.triangle_indices[i], mesh.triangle_indices[i + 1],
             mesh.triangle_indices[i + 2]},
            transform, output)) {
      return false;
    }
  }
  return true;
}

std::optional<std::vector<MeshRenderVertex>> BuildMeshRenderVertices(
    const std::vector<SphericalVideoMesh>& meshes,
    device::mojom::XRLayerLayout layout) {
  std::vector<MeshRenderVertex> output;

  if (layout == device::mojom::XRLayerLayout::kMono) {
    if (meshes.size() != 1 ||
        !AppendMeshWithTransform(meshes.front(), {}, &output)) {
      return std::nullopt;
    }
    return output;
  }

  if (layout == device::mojom::XRLayerLayout::kStereoLeftRight) {
    MeshRenderTransform left;
    left.destination_x_scale = 0.5f;
    left.destination_x_offset = -0.5f;

    MeshRenderTransform right = left;
    right.destination_x_offset = 0.5f;

    if (meshes.size() == 1) {
      // RFC stereo left-right: the common mesh maps each eye after selecting
      // the corresponding half of the packed video texture.
      left.source_u_scale = 0.5f;
      right.source_u_scale = 0.5f;
      right.source_u_offset = 0.5f;
      if (!AppendMeshWithTransform(meshes.front(), left, &output) ||
          !AppendMeshWithTransform(meshes.front(), right, &output)) {
        return std::nullopt;
      }
      return output;
    }

    if (meshes.size() == 2 &&
        AppendMeshWithTransform(meshes[0], left, &output) &&
        AppendMeshWithTransform(meshes[1], right, &output)) {
      return output;
    }
    return std::nullopt;
  }

  if (layout == device::mojom::XRLayerLayout::kStereoTopBottom) {
    MeshRenderTransform left;
    left.destination_y_scale = 0.5f;
    left.destination_y_offset = 0.5f;

    MeshRenderTransform right = left;
    right.destination_y_offset = -0.5f;

    if (meshes.size() == 1) {
      // RFC stereo top-bottom uses OpenGL UVs: the left eye occupies the upper
      // half (v=.5..1), the right eye the lower half (v=0...5).
      left.source_v_scale = 0.5f;
      left.source_v_offset = 0.5f;
      right.source_v_scale = 0.5f;
      if (!AppendMeshWithTransform(meshes.front(), left, &output) ||
          !AppendMeshWithTransform(meshes.front(), right, &output)) {
        return std::nullopt;
      }
      return output;
    }

    if (meshes.size() == 2 &&
        AppendMeshWithTransform(meshes[0], left, &output) &&
        AppendMeshWithTransform(meshes[1], right, &output)) {
      return output;
    }
    return std::nullopt;
  }

  return std::nullopt;
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

id<MTLBuffer> EncodeTextureFingerprintReadback(
    id<MTLDevice> device,
    id<MTLCommandBuffer> command_buffer,
    id<MTLTexture> texture) {
  if (device == nil || command_buffer == nil || texture == nil ||
      texture.width == 0 || texture.height == 0 ||
      (texture.pixelFormat != MTLPixelFormatBGRA8Unorm &&
       texture.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB)) {
    return nil;
  }

  constexpr NSUInteger kSampleCount =
      kTransferDiagnosticGrid * kTransferDiagnosticGrid;
  id<MTLBuffer> buffer =
      [device newBufferWithLength:kSampleCount * kTransferDiagnosticStride
                          options:MTLResourceStorageModeShared];
  if (buffer == nil) {
    return nil;
  }

  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  if (blit == nil) {
    return nil;
  }

  NSUInteger sample = 0;
  for (NSUInteger gy = 0; gy < kTransferDiagnosticGrid; ++gy) {
    const NSUInteger y =
        ((texture.height - 1) * gy) / (kTransferDiagnosticGrid - 1);
    for (NSUInteger gx = 0; gx < kTransferDiagnosticGrid; ++gx) {
      const NSUInteger x =
          ((texture.width - 1) * gx) / (kTransferDiagnosticGrid - 1);
      [blit copyFromTexture:texture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(x, y, 0)
                 sourceSize:MTLSizeMake(1, 1, 1)
                   toBuffer:buffer
          destinationOffset:sample * kTransferDiagnosticStride
     destinationBytesPerRow:kTransferDiagnosticStride
   destinationBytesPerImage:kTransferDiagnosticStride];
      ++sample;
    }
  }
  [blit endEncoding];
  return buffer;
}

std::optional<MetalTextureFingerprint> ReadTextureFingerprint(
    id<MTLBuffer> buffer) {
  if (buffer == nil || buffer.contents == nullptr) {
    return std::nullopt;
  }

  MetalTextureFingerprint result;
  const auto* base = static_cast<const uint8_t*>(buffer.contents);
  constexpr NSUInteger kSampleCount =
      kTransferDiagnosticGrid * kTransferDiagnosticGrid;
  for (NSUInteger i = 0; i < kSampleCount; ++i) {
    const uint8_t* pixel = base + i * kTransferDiagnosticStride;
    const uint8_t blue = pixel[0];
    const uint8_t green = pixel[1];
    const uint8_t red = pixel[2];
    const uint8_t alpha = pixel[3];

    ++result.samples;
    result.nonblack += (red | green | blue) != 0;
    result.alpha_zero += alpha == 0;
    result.alpha_opaque += alpha == 255;
    result.rgb_sum += static_cast<uint64_t>(red) + green + blue;
    result.alpha_sum += alpha;

    for (uint8_t component : {blue, green, red, alpha}) {
      result.hash ^= component;
      result.hash *= 1099511628211ULL;
    }
  }
  return result;
}

void LogTextureFingerprint(const char* which,
                           LayerId layer_id,
                           uint64_t sequence,
                           id<MTLBuffer> buffer) {
  const auto fingerprint = ReadTextureFingerprint(buffer);
  if (!fingerprint) {
    LOG(INFO) << "XRTRANSFER " << which << " layer=" << layer_id
              << " seq=" << sequence << " readback=unavailable";
    return;
  }

  LOG(INFO) << "XRTRANSFER " << which << " layer=" << layer_id
            << " seq=" << sequence
            << " samples=" << fingerprint->samples
            << " nonblack=" << fingerprint->nonblack
            << " rgb_sum=" << fingerprint->rgb_sum
            << " alpha_zero=" << fingerprint->alpha_zero
            << " alpha_opaque=" << fingerprint->alpha_opaque
            << " alpha_sum=" << fingerprint->alpha_sum
            << " hash=" << fingerprint->hash;
}

}  // namespace

// static
void OpenXrGraphicsBinding::GetRequiredExtensions(
    std::vector<const char*>& extensions) {
  extensions.push_back(XR_KHR_METAL_ENABLE_EXTENSION_NAME);
}

class OpenXrGraphicsBindingMetal::Impl {
 public:
  // Rebuilds the runtime's rate map from its transported recipe and checks
  // that it reproduces the runtime's physical size on this device. The last
  // validated recipe is cached because it only changes with the map.
  bool ValidateResolvedFoveation(const OpenXrResolvedFoveationRateMap& state) {
    if (validated_foveation &&
        validated_foveation->logical_size == state.logical_size &&
        validated_foveation->physical_size == state.physical_size &&
        validated_foveation->horizontal_rates == state.horizontal_rates &&
        validated_foveation->vertical_rates == state.vertical_rates) {
      return true;
    }
    validated_foveation.reset();

    if (device == nil ||
        ![device supportsRasterizationRateMapWithLayerCount:1] ||
        state.horizontal_rates.size() != state.vertical_rates.size()) {
      return false;
    }
    const NSUInteger samples = state.horizontal_rates.size();
    MTLRasterizationRateLayerDescriptor* layer =
        [[MTLRasterizationRateLayerDescriptor alloc]
            initWithSampleCount:MTLSizeMake(samples, samples, 1)
                     horizontal:state.horizontal_rates.data()
                       vertical:state.vertical_rates.data()];
    MTLRasterizationRateMapDescriptor* descriptor =
        [[MTLRasterizationRateMapDescriptor alloc] init];
    descriptor.screenSize = MTLSizeMake(state.logical_size.width(),
                                        state.logical_size.height(), 1);
    [descriptor setLayer:layer atIndex:0];
    id<MTLRasterizationRateMap> map =
        [device newRasterizationRateMapWithDescriptor:descriptor];
    if (map == nil) {
      return false;
    }
    const MTLSize physical = [map physicalSizeForLayer:0];
    if (physical.width != static_cast<NSUInteger>(state.physical_size.width()) ||
        physical.height !=
            static_cast<NSUInteger>(state.physical_size.height())) {
      return false;
    }
    validated_foveation = state;
    return true;
  }

  std::optional<OpenXrResolvedFoveationRateMap> validated_foveation;
  uint64_t next_foveation_serial = 1;

  id<MTLDevice> __strong device = nil;
  id<MTLCommandQueue> __strong command_queue = nil;
  XrGraphicsBindingMetalKHR binding{XR_TYPE_GRAPHICS_BINDING_METAL_KHR};

  // Monado exposes IOSurface-backed swapchain textures with a synchronization
  // contract that we control, so it can stay zero-copy. Other runtimes use a
  // Chromium-owned IOSurface and an explicit render-pass transfer into the
  // runtime-owned OpenXR texture before release/submission.
  // Objective-C object pointers stored in C++ containers are strong under ARC;
  // libc++ invokes the ARC copy/destroy semantics as map entries move and die.
  std::map<void*, id<MTLTexture>> fallback_textures;
  std::map<LayerId, uint64_t> transfer_diagnostic_frames;
  bool runtime_is_monado = false;

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

struct MediaMeshVertex {
  float2 position;
  float2 texcoord;
};

vertex ScaleVertexOut xr_mesh_vertex(
    uint vertex_id [[vertex_id]],
    const device MediaMeshVertex* vertices [[buffer(0)]]) {
  ScaleVertexOut out;
  out.position = float4(vertices[vertex_id].position, 0.0, 1.0);
  out.texcoord = vertices[vertex_id].texcoord;
  return out;
}

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

float2 xr_project_eac(float3 w, float2 source_size) {
  constexpr float kPi = 3.14159265358979323846;
  float3 p = float3(w.x, -w.y, -w.z);
  float ax = abs(p.x);
  float ay = abs(p.y);
  float az = abs(p.z);
  float uf = 0.0;
  float vf = 0.0;
  int col = 1;
  int row = 0;
  int rotation = 0;

  if (ax >= ay && ax >= az) {
    if (p.x >= 0.0) {
      uf = -p.z / p.x;
      vf = p.y / p.x;
      col = 2;
      row = 0;
    } else {
      uf = -p.z / p.x;
      vf = -p.y / p.x;
      col = 0;
      row = 0;
    }
  } else if (ay >= ax && ay >= az) {
    if (p.y >= 0.0) {
      uf = p.x / p.y;
      vf = -p.z / p.y;
      col = 0;
      row = 1;
      rotation = 3;
    } else {
      uf = -p.x / p.y;
      vf = -p.z / p.y;
      col = 2;
      row = 1;
      rotation = 3;
    }
  } else {
    if (p.z >= 0.0) {
      uf = p.x / p.z;
      vf = p.y / p.z;
      col = 1;
      row = 0;
    } else {
      uf = p.x / p.z;
      vf = -p.y / p.z;
      col = 1;
      row = 1;
      rotation = 1;
    }
  }

  if (rotation == 1) {
    float t = uf;
    uf = -vf;
    vf = t;
  } else if (rotation == 3) {
    float t = -uf;
    uf = vf;
    vf = t;
  }

  uf = (2.0 / kPi) * atan(uf) + 0.5;
  vf = (2.0 / kPi) * atan(vf) + 0.5;
  float u_pad = 2.0 / max(source_size.x, 1.0);
  float v_pad = 2.0 / max(source_size.y, 1.0);
  return float2(
      (uf + float(col)) * (1.0 - 2.0 * u_pad) / 3.0 + u_pad,
      vf * (0.5 - 2.0 * v_pad) + v_pad + 0.5 * float(row));
}

fragment float4 xr_eac_fragment(
    ScaleVertexOut in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler source_sampler [[sampler(0)]]) {
  constexpr float kPi = 3.14159265358979323846;
  float2 uv = in.texcoord;
  float longitude = (uv.x - 0.5) * (2.0 * kPi);
  float latitude = (0.5 - uv.y) * kPi;
  float cos_latitude = cos(latitude);
  float3 direction = normalize(float3(
      sin(longitude) * cos_latitude,
      sin(latitude),
      -cos(longitude) * cos_latitude));
  float2 source_uv =
      xr_project_eac(direction,
                     float2(float(source.get_width()), float(source.get_height())));
  return source.sample(source_sampler, source_uv);
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
      mesh_vertex = [scale_library newFunctionWithName:@"xr_mesh_vertex"];
      scale_fragment = [scale_library newFunctionWithName:@"xr_scale_fragment"];
      eac_fragment = [scale_library newFunctionWithName:@"xr_eac_fragment"];
      if (scale_vertex == nil || mesh_vertex == nil || scale_fragment == nil ||
          eac_fragment == nil) {
        DLOG(ERROR) << "OpenXR Metal media shader functions are unavailable";
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

  id<MTLRenderPipelineState> EacPipeline(MTLPixelFormat pixel_format) {
    auto existing = eac_pipelines.find(static_cast<uint64_t>(pixel_format));
    if (existing != eac_pipelines.end()) {
      return existing->second;
    }

    // Ensure the shared shader library/functions and sampler are initialized.
    if (!ScalePipeline(pixel_format) || eac_fragment == nil) {
      return nil;
    }

    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = scale_vertex;
    descriptor.fragmentFunction = eac_fragment;
    descriptor.colorAttachments[0].pixelFormat = pixel_format;

    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) {
      DLOG(ERROR) << "Failed to create OpenXR Metal EAC pipeline: "
                  << (error ? error.localizedDescription.UTF8String
                            : "unknown error");
      return nil;
    }

    eac_pipelines.emplace(static_cast<uint64_t>(pixel_format), pipeline);
    return pipeline;
  }

  id<MTLRenderPipelineState> MeshPipeline(MTLPixelFormat pixel_format) {
    auto existing = mesh_pipelines.find(static_cast<uint64_t>(pixel_format));
    if (existing != mesh_pipelines.end()) {
      return existing->second;
    }

    if (!ScalePipeline(pixel_format) || mesh_vertex == nil) {
      return nil;
    }

    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = mesh_vertex;
    descriptor.fragmentFunction = scale_fragment;
    descriptor.colorAttachments[0].pixelFormat = pixel_format;

    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) {
      DLOG(ERROR) << "Failed to create OpenXR Metal mesh pipeline: "
                  << (error ? error.localizedDescription.UTF8String
                            : "unknown error");
      return nil;
    }

    mesh_pipelines.emplace(static_cast<uint64_t>(pixel_format), pipeline);
    return pipeline;
  }

  bool PrepareMesh(OpenXrCompositionLayer& layer, MetalLayerData& data) {
    if (data.mesh_initialized) {
      return data.mesh_vertex_buffer != nil && data.mesh_vertex_count != 0;
    }
    data.mesh_initialized = true;

    const auto& projection_data = layer.read_only_data().media_projection_data;
    auto meshes =
        ParseSphericalVideoMesh(base::span<const uint8_t>(projection_data));
    if (!meshes) {
      DLOG(ERROR) << "Failed to parse spherical video mesh";
      return false;
    }

    auto render_vertices =
        BuildMeshRenderVertices(*meshes, layer.read_only_data().layout);
    if (!render_vertices || render_vertices->empty()) {
      DLOG(ERROR) << "Failed to build equirectangular render mesh";
      return false;
    }

    data.mesh_vertex_buffer = [device
        newBufferWithBytes:render_vertices->data()
                    length:render_vertices->size() * sizeof(MeshRenderVertex)
                   options:MTLResourceStorageModeShared];
    if (data.mesh_vertex_buffer == nil) {
      DLOG(ERROR) << "Failed to allocate OpenXR Metal mesh vertex buffer";
      return false;
    }
    data.mesh_vertex_count = render_vertices->size();
    DVLOG(1) << "Prepared spherical video mesh with "
             << data.mesh_vertex_count << " render vertices";
    return true;
  }

  id<MTLSamplerState> ScaleSampler() const { return scale_sampler; }

 private:
  id<MTLLibrary> __strong scale_library = nil;
  id<MTLFunction> __strong scale_vertex = nil;
  id<MTLFunction> __strong mesh_vertex = nil;
  id<MTLFunction> __strong scale_fragment = nil;
  id<MTLFunction> __strong eac_fragment = nil;
  id<MTLSamplerState> __strong scale_sampler = nil;
  std::map<uint64_t, id<MTLRenderPipelineState>> scale_pipelines;
  std::map<uint64_t, id<MTLRenderPipelineState>> eac_pipelines;
  std::map<uint64_t, id<MTLRenderPipelineState>> mesh_pipelines;
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

  XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &instance_properties))) {
    const std::string runtime_name(instance_properties.runtimeName);
    impl_->runtime_is_monado =
        runtime_name.find("Monado") != std::string::npos ||
        runtime_name.find("monado") != std::string::npos;
    LOG(INFO) << "OpenXR Metal runtime='" << runtime_name
              << "' monado_zero_copy=" << impl_->runtime_is_monado;
  } else {
    impl_->runtime_is_monado = false;
    LOG(WARNING) << "Could not query OpenXR runtime name; using generic "
                    "Metal copy transport";
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
  // WebGL can use runtime-owned IOSurface swapchains directly and otherwise
  // falls back to a Chromium-owned IOSurface plus a Metal copy. WebGPU still
  // needs the IOSurfaceImageBacking Dawn/Metal representation wired into XR.
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
    // Direct IOSurface path: Blink/ANGLE rendered into the runtime's storage,
    // so there is nothing to copy.
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

  if (!layer.read_only_data().media_projection_data.empty()) {
    TRACE_EVENT_INSTANT("xr", "OpenXrMetalMediaMeshReprojection");
    MetalLayerData& layer_data = GetMetalLayerData(layer);
    id<MTLRenderPipelineState> pipeline =
        impl_->MeshPipeline(runtime_texture.pixelFormat);
    id<MTLSamplerState> sampler = impl_->ScaleSampler();
    if (!pipeline || !sampler || !impl_->PrepareMesh(layer, layer_data) ||
        !(runtime_texture.usage & MTLTextureUsageRenderTarget)) {
      DLOG(ERROR) << __func__
                  << ": runtime texture cannot accept mesh reprojection";
      return false;
    }

    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = runtime_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      DLOG(ERROR) << __func__
                  << ": failed to create Metal mesh render encoder";
      return false;
    }

    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:layer_data.mesh_vertex_buffer offset:0 atIndex:0];
    [encoder setFragmentTexture:source_texture atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:layer_data.mesh_vertex_count];
    [encoder endEncoding];

    DVLOG(1) << __func__ << ": stream mesh reprojection "
             << source_texture.width << "x" << source_texture.height << " -> "
             << runtime_texture.width << "x" << runtime_texture.height
             << " vertices=" << layer_data.mesh_vertex_count;
  } else if (layer.read_only_data().needs_eac_reprojection) {
    TRACE_EVENT_INSTANT("xr", "OpenXrMetalEacReprojection");
    id<MTLRenderPipelineState> pipeline =
        impl_->EacPipeline(runtime_texture.pixelFormat);
    id<MTLSamplerState> sampler = impl_->ScaleSampler();
    if (!pipeline || !sampler ||
        !(runtime_texture.usage & MTLTextureUsageRenderTarget)) {
      DLOG(ERROR) << __func__
                  << ": runtime texture cannot accept EAC reprojection";
      return false;
    }

    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = runtime_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      DLOG(ERROR) << __func__
                  << ": failed to create Metal EAC render encoder";
      return false;
    }

    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source_texture atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
    [encoder endEncoding];

    DVLOG(1) << __func__ << ": EAC reprojection "
             << source_texture.width << "x" << source_texture.height << " -> "
             << runtime_texture.width << "x" << runtime_texture.height;
  } else {
    // Diagnostic A/B path for generic runtimes: always write the runtime
    // swapchain through a render pass, even when source and destination sizes
    // match. Meta XR Simulator implements Metal over a Vulkan compositor; this
    // avoids relying on its handling of a blit-written OpenXR swapchain image.
    TRACE_EVENT_INSTANT("xr", "OpenXrMetalRenderTransfer");
    id<MTLRenderPipelineState> pipeline =
        impl_->ScalePipeline(runtime_texture.pixelFormat);
    id<MTLSamplerState> sampler = impl_->ScaleSampler();
    if (!pipeline || !sampler ||
        !(runtime_texture.usage & MTLTextureUsageRenderTarget)) {
      DLOG(ERROR) << __func__
                  << ": runtime texture cannot accept render-pass Metal "
                     "fallback";
      return false;
    }

    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = runtime_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      DLOG(ERROR) << __func__
                  << ": failed to create Metal transfer render encoder";
      return false;
    }

    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source_texture atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
    [encoder endEncoding];

    DVLOG(1) << __func__ << ": render-pass WebXR transfer "
             << source_texture.width << "x" << source_texture.height
             << " -> OpenXR swapchain " << runtime_texture.width << "x"
             << runtime_texture.height;
  }

  uint64_t& diagnostic_sequence =
      impl_->transfer_diagnostic_frames[layer.GetLayerId()];
  ++diagnostic_sequence;
  const bool capture_fingerprint =
      diagnostic_sequence <= kTransferDiagnosticInitialFrames ||
      diagnostic_sequence % kTransferDiagnosticPeriod == 0;
  id<MTLBuffer> source_fingerprint = nil;
  id<MTLBuffer> runtime_fingerprint = nil;
  if (capture_fingerprint) {
    // Encode these after the transfer. They therefore cannot make an
    // incomplete source become complete before the transfer under test.
    source_fingerprint = EncodeTextureFingerprintReadback(
        impl_->device, command_buffer, source_texture);
    runtime_fingerprint = EncodeTextureFingerprintReadback(
        impl_->device, command_buffer, runtime_texture);
  }

  [command_buffer commit];

  // The fallback is deliberately conservative. The runtime may composite on a
  // different queue/API (Meta XR Simulator bridges Metal to Vulkan), so make
  // the copy complete before xrReleaseSwapchainImage hands ownership back.
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError) {
    DLOG(ERROR) << __func__ << ": Metal fallback transfer failed";
    return false;
  }

  if (capture_fingerprint) {
    LOG(INFO) << "XRTRANSFER metadata layer=" << layer.GetLayerId()
              << " seq=" << diagnostic_sequence
              << " type=" << static_cast<int>(layer.type())
              << " source=" << source_texture.width << "x"
              << source_texture.height
              << " runtime=" << runtime_texture.width << "x"
              << runtime_texture.height
              << " blend_source_alpha="
              << layer.mutable_data().blend_texture_source_alpha
              << " flip_y=" << layer.flip_y()
              << " needs_raster_access="
              << layer.read_only_data().needs_raster_access
              << " runtime_iosurface="
              << (runtime_texture.iosurface != nullptr);
    LogTextureFingerprint("source", layer.GetLayerId(), diagnostic_sequence,
                          source_fingerprint);
    LogTextureFingerprint("runtime", layer.GetLayerId(), diagnostic_sequence,
                          runtime_fingerprint);
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
  // Match the SharedImage's transfer function to the negotiated Metal
  // swapchain format. In particular, MTLPixelFormatBGRA8Unorm_sRGB performs
  // sRGB<->linear conversion, so describing the same storage to Chromium as
  // linear can crush midtones/shadows when the bytes are later consumed as
  // sRGB by the OpenXR runtime.
  const bool is_srgb =
      swapchain_format_ ==
      static_cast<int64_t>(MTLPixelFormatBGRA8Unorm_sRGB);
  const gfx::ColorSpace color_space =
      is_srgb
          ? gfx::ColorSpace::CreateSRGB()
          : gfx::ColorSpace(gfx::ColorSpace::PrimaryID::BT709,
                            gfx::ColorSpace::TransferID::LINEAR,
                            gfx::ColorSpace::MatrixID::RGB,
                            gfx::ColorSpace::RangeID::FULL);
  const bool foveation_capable_base_layer =
      IsBaseLayerFoveationAllowed() && layer.GetLayerId() == kInvalidLayerId;
  const gpu::SharedImageInfo direct_si_info{
      viz::SinglePlaneFormat::kBGRA_8888, runtime_size, color_space, usage,
      foveation_capable_base_layer ? "OpenXrMetalDirectFoveated"
                                   : "OpenXrMetalDirect"};
  const gpu::SharedImageInfo fallback_si_info{
      viz::SinglePlaneFormat::kBGRA_8888, transfer_size, color_space, usage,
      foveation_capable_base_layer ? "OpenXrMetalTransferFoveated"
                                   : "OpenXrMetalTransfer"};

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

    // Preserve Monado's zero-copy path, where the runtime and Chromium share
    // the IOSurface under a synchronization contract we control. For Meta XR
    // Simulator and other generic runtimes, deliberately use the copy fallback
    // below even if the runtime texture happens to expose an IOSurface. An
    // IOSurface alone does not describe the runtime's Metal/Vulkan interop
    // synchronization.
    IOSurfaceRef runtime_surface = texture.iosurface;
    if (impl_->runtime_is_monado && runtime_surface &&
        transfer_size == runtime_size &&
        !layer.read_only_data().needs_eac_reprojection &&
        layer.read_only_data().media_projection_data.empty()) {
      const size_t surface_width = IOSurfaceGetWidth(runtime_surface);
      const size_t surface_height = IOSurfaceGetHeight(runtime_surface);
      if (surface_width == static_cast<size_t>(runtime_size.width()) &&
          surface_height == static_cast<size_t>(runtime_size.height())) {
        swap_chain_info.shared_image = sii->CreateSharedImage(
            direct_si_info,
            gfx::GpuMemoryBufferHandle(gfx::ScopedIOSurface(
                runtime_surface, base::scoped_policy::RETAIN)));
        if (swap_chain_info.shared_image) {
          impl_->fallback_textures.erase(metal_texture);
          swap_chain_info.sync_token = sii->GenVerifiedSyncToken();
          LOG(INFO) << __func__
                    << ": transport=monado-iosurface-zero-copy layer="
                    << layer.GetLayerId()
                    << " size=" << runtime_size.ToString()
                    << " srgb=" << is_srgb;
          continue;
        }
        DLOG(WARNING) << __func__
                      << ": runtime IOSurface SharedImage import failed; "
                         "using copy fallback";
      } else {
        DLOG(WARNING) << __func__ << ": runtime IOSurface size "
                      << surface_width << "x" << surface_height
                      << " does not match swapchain "
                      << runtime_size.ToString() << "; using copy fallback";
      }
    }

    // Runtime-neutral fallback. Render into a Chromium-owned IOSurface that can
    // be shared with Blink/ANGLE, then render it into the runtime swapchain
    // texture in RenderLayer(). This costs one GPU pass per submitted layer but
    // avoids assuming that a generic runtime's IOSurface is safe to write
    // directly from Chromium.
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
              << ": transport=iosurface-render-copy layer="
              << layer.GetLayerId()
              << " transfer=" << transfer_size.ToString()
              << " runtime=" << runtime_size.ToString()
              << " runtime_iosurface=" << (texture.iosurface != nullptr)
              << " runtime_is_monado=" << impl_->runtime_is_monado
              << " srgb=" << is_srgb;
  }
}

IOSurfaceRef OpenXrGraphicsBindingMetal::GetActiveBaseLayerRenderSurface() {
  if (!base_layer_) {
    return nullptr;
  }

  OpenXrSwapchainInfo* swapchain_info =
      base_layer_->GetActiveSwapchainImage();
  if (!swapchain_info || !swapchain_info->metal_texture) {
    return nullptr;
  }

  // Blink renders into the Chromium-owned fallback IOSurface when the runtime
  // texture is not directly shareable; that is the surface ANGLE reads.
  id<MTLTexture> render_texture = nil;
  auto fallback =
      impl_->fallback_textures.find(swapchain_info->metal_texture.get());
  if (fallback != impl_->fallback_textures.end()) {
    render_texture = fallback->second;
  } else {
    render_texture =
        (__bridge id<MTLTexture>)swapchain_info->metal_texture.get();
  }
  return render_texture ? render_texture.iosurface : nullptr;
}

bool OpenXrGraphicsBindingMetal::PublishFoveationMetadata(
    const gfx::Size& logical_size,
    const gfx::Size& physical_size,
    base::span<const float> horizontal_rates,
    base::span<const float> vertical_rates) {
  if (logical_size.IsEmpty() || physical_size.IsEmpty() ||
      physical_size.width() > logical_size.width() ||
      physical_size.height() > logical_size.height() ||
      horizontal_rates.size() != kFoveationMetadataZoneCount ||
      vertical_rates.size() != kFoveationMetadataZoneCount) {
    return false;
  }

  IOSurfaceRef surface = GetActiveBaseLayerRenderSurface();
  if (!surface) {
    DLOG(WARNING) << __func__
                  << ": active WebXR render texture is not IOSurface-backed";
    return false;
  }

  NSMutableArray<NSNumber*>* horizontal =
      [NSMutableArray arrayWithCapacity:kFoveationMetadataZoneCount];
  NSMutableArray<NSNumber*>* vertical =
      [NSMutableArray arrayWithCapacity:kFoveationMetadataZoneCount];
  for (uint32_t i = 0; i < kFoveationMetadataZoneCount; ++i) {
    [horizontal addObject:@(horizontal_rates[i])];
    [vertical addObject:@(vertical_rates[i])];
  }

  // Graphics state only: sizes, per-axis rates and a publish serial. The
  // physical size is what the OpenXR compositor will assume, so the renderer
  // can refuse a reconstruction that differs. No gaze is ever included.
  const uint64_t serial = impl_->next_foveation_serial++;
  NSDictionary* metadata = @{
    @"version" : @(kFoveationMetadataVersion),
    @"logical_width" : @(logical_size.width()),
    @"logical_height" : @(logical_size.height()),
    @"physical_width" : @(physical_size.width()),
    @"physical_height" : @(physical_size.height()),
    @"zone_count" : @(kFoveationMetadataZoneCount),
    @"horizontal" : horizontal,
    @"vertical" : vertical,
    @"serial" : @(serial),
  };
  // Drop any acknowledgement from an earlier render of this image first.
  IOSurfaceRemoveValue(surface, CFSTR("org.chromium.openxr.metal-foveation-applied"));
  IOSurfaceSetValue(surface, CFSTR("org.chromium.openxr.metal-foveation-v1"),
                    (__bridge CFTypeRef)metadata);

  DVLOG(3) << __func__ << ": published " << kFoveationMetadataZoneCount << "x"
           << kFoveationMetadataZoneCount << " foveation metadata logical="
           << logical_size.ToString() << " physical="
           << physical_size.ToString() << " serial=" << serial;
  return true;
}

bool OpenXrGraphicsBindingMetal::PublishBaseLayerResolvedFoveation(
    const OpenXrResolvedFoveationRateMap& state) {
  if (!base_layer_ || state.logical_size.IsEmpty() ||
      state.horizontal_rates.size() != kFoveationMetadataZoneCount ||
      state.vertical_rates.size() != kFoveationMetadataZoneCount) {
    return false;
  }

  // The runtime's compositor will sample with its own map. Rebuild the map
  // from the transported recipe on this device and refuse to publish if the
  // physical extent differs, rather than render with a different transform.
  if (!impl_->ValidateResolvedFoveation(state)) {
    LOG(ERROR) << "Runtime Metal foveation recipe does not reproduce its "
                  "physical size "
               << state.physical_size.ToString() << "; rendering unfoveated";
    return false;
  }

  return PublishFoveationMetadata(state.logical_size, state.physical_size,
                                  state.horizontal_rates,
                                  state.vertical_rates);
}

void OpenXrGraphicsBindingMetal::ClearPublishedBaseLayerFoveation() {
  IOSurfaceRef surface = GetActiveBaseLayerRenderSurface();
  if (!surface) {
    return;
  }

  IOSurfaceRemoveValue(surface, CFSTR("org.chromium.openxr.metal-foveation-v1"));
  IOSurfaceRemoveValue(surface, CFSTR("org.chromium.openxr.metal-foveation-applied"));
}

OpenXrGraphicsBinding::FoveationRenderStatus
OpenXrGraphicsBindingMetal::GetBaseLayerFoveationRenderStatus() {
  IOSurfaceRef surface = GetActiveBaseLayerRenderSurface();
  if (!surface) {
    return FoveationRenderStatus::kNone;
  }

  // ANGLE removes the metadata when it refuses it, and records the serial of
  // the metadata it actually rendered with.
  NSDictionary* metadata = CFBridgingRelease(
      IOSurfaceCopyValue(surface, CFSTR("org.chromium.openxr.metal-foveation-v1")));
  if (![metadata isKindOfClass:[NSDictionary class]]) {
    return FoveationRenderStatus::kNone;
  }
  NSNumber* serial = metadata[@"serial"];
  NSNumber* applied = CFBridgingRelease(IOSurfaceCopyValue(
      surface, CFSTR("org.chromium.openxr.metal-foveation-applied")));
  if ([serial isKindOfClass:[NSNumber class]] &&
      [applied isKindOfClass:[NSNumber class]] &&
      [applied unsignedLongLongValue] == [serial unsignedLongLongValue]) {
    return FoveationRenderStatus::kApplied;
  }
  return FoveationRenderStatus::kNotApplied;
}

bool OpenXrGraphicsBindingMetal::ShouldFlipSubmittedImage(
    OpenXrCompositionLayer& layer) const {
  // Metal's image origin is inverted relative to the orientation expected by
  // Chromium's existing OpenXR composition path, so ordinary WebXR layers
  // need a runtime Y flip. Some layer producers (notably XR media layers)
  // already mark their content as Y-flipped; in that case the two inversions
  // cancel and no additional runtime flip should be requested.
  return !layer.flip_y();
}

std::unique_ptr<OpenXrCompositionLayer::GraphicsBindingData>
OpenXrGraphicsBindingMetal::CreateLayerGraphicsBindingData() const {
  return std::make_unique<MetalLayerData>();
}

}  // namespace device
