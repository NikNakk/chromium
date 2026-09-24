// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/ipc/service/metal_shared_texture_resolver_mac.h"

#include <dlfcn.h>

#include <cstdlib>

#include "base/logging.h"
#include "ui/gl/scoped_egl_image.h"

#ifndef EGL_METAL_TEXTURE_ANGLE
#define EGL_METAL_TEXTURE_ANGLE 0x34A7
#endif

#ifndef EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE
#define EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE 0x34DD
#endif

namespace gpu {
namespace {

using TakeTextureFn = int (*)(uint64_t token, void** out_metal_texture);
using ReleaseTextureFn = void (*)(void* metal_texture);

struct MonadoMetalXpcApi {
  void* library = nullptr;
  TakeTextureFn take_texture = nullptr;
  ReleaseTextureFn release_texture = nullptr;

  bool valid() const {
    return library && take_texture && release_texture;
  }
};

const MonadoMetalXpcApi& GetMonadoMetalXpcApi() {
  static const MonadoMetalXpcApi api = [] {
    MonadoMetalXpcApi result;

    const char* override_path =
        std::getenv("MONADO_METAL_XPC_HELPER_LIBRARY");
    const char* library_path =
        (override_path && override_path[0])
            ? override_path
            : "/usr/local/lib/libmonado_metal_xpc_client.dylib";

    result.library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (!result.library) {
      DLOG(ERROR) << "Unable to load Monado Metal XPC helper '"
                  << library_path << "': " << dlerror();
      return result;
    }

    result.take_texture = reinterpret_cast<TakeTextureFn>(
        dlsym(result.library, "monado_metal_xpc_take_texture"));
    result.release_texture = reinterpret_cast<ReleaseTextureFn>(
        dlsym(result.library, "monado_metal_xpc_release_texture"));

    if (!result.take_texture || !result.release_texture) {
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

  void* metal_texture = nullptr;
  if (api.take_texture(texture_token, &metal_texture) != 0 || !metal_texture) {
    DLOG(ERROR) << __func__ << ": unable to resolve Metal texture token";
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
