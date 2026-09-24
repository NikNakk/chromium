// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Metal/Metal.h>

#include "gpu/ipc/service/metal_shared_texture_resolver_mac.h"

#include <dlfcn.h>

#include "base/logging.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/scoped_egl_image.h"

#ifndef EGL_METAL_TEXTURE_ANGLE
#define EGL_METAL_TEXTURE_ANGLE 0x34A7
#endif

#ifndef EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE
#define EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE 0x34DD
#endif

namespace gpu {
namespace {

constexpr char kMonadoMetalXpcHelperPath[] =
    "/usr/local/lib/libmonado_metal_xpc_client.dylib";

using TakeTextureOnDeviceFn =
    int (*)(uint64_t token, void* metal_device, void** out_metal_texture);
using ReleaseTextureFn = void (*)(void* metal_texture);

struct MonadoMetalXpcApi {
  void* library = nullptr;
  TakeTextureOnDeviceFn take_texture_on_device = nullptr;
  ReleaseTextureFn release_texture = nullptr;

  bool valid() const {
    return library && take_texture_on_device && release_texture;
  }
};

const MonadoMetalXpcApi& GetMonadoMetalXpcApi() {
  static const MonadoMetalXpcApi api = [] {
    MonadoMetalXpcApi result;

    result.library =
        dlopen(kMonadoMetalXpcHelperPath, RTLD_NOW | RTLD_LOCAL);
    if (!result.library) {
      DLOG(ERROR) << "Unable to load Monado Metal XPC helper '"
                  << kMonadoMetalXpcHelperPath << "': " << dlerror();
      return result;
    }

    result.take_texture_on_device =
        reinterpret_cast<TakeTextureOnDeviceFn>(
            dlsym(result.library,
                  "monado_metal_xpc_take_texture_on_device"));
    result.release_texture = reinterpret_cast<ReleaseTextureFn>(
        dlsym(result.library, "monado_metal_xpc_release_texture"));

    if (!result.take_texture_on_device || !result.release_texture) {
      DLOG(ERROR) << "Monado Metal XPC helper is missing required exports";
    }
    return result;
  }();

  return api;
}

}  // namespace

gl::ScopedEGLImage ResolveMetalTextureTokenToEGLImage(uint64_t texture_token,
                                                       uint32_t array_slice) {
  const MonadoMetalXpcApi& api = GetMonadoMetalXpcApi();
  if (!api.valid()) {
    return {};
  }

  gl::GLDisplayEGL* display = gl::GLDisplayEGL::GetDisplayForCurrentContext();
  if (!display) {
    DLOG(ERROR) << __func__ << ": no EGL display for current GPU context";
    return {};
  }

  id<MTLDevice> metal_device = display->GetMetalDevice();
  if (metal_device == nil) {
    DLOG(ERROR) << __func__
                << ": current ANGLE display is not backed by a Metal device";
    return {};
  }

  void* metal_texture = nullptr;
  if (api.take_texture_on_device(texture_token,
                                 (__bridge void*)metal_device,
                                 &metal_texture) != 0 ||
      !metal_texture) {
    DLOG(ERROR) << __func__
                << ": unable to resolve Metal texture token on ANGLE device";
    return {};
  }

  const EGLint attrs[] = {
      EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE,
      static_cast<EGLint>(array_slice),
      EGL_NONE,
  };

  gl::ScopedEGLImage image = gl::MakeScopedEGLImage(
      EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
      reinterpret_cast<EGLClientBuffer>(metal_texture), attrs);

  api.release_texture(metal_texture);

  if (!image.get()) {
    DLOG(ERROR) << __func__
                << ": EGL_ANGLE_metal_texture_client_buffer import failed";
  }
  return image;
}

}  // namespace gpu
