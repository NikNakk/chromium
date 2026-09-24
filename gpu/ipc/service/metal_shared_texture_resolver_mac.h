// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_IPC_SERVICE_METAL_SHARED_TEXTURE_RESOLVER_MAC_H_
#define GPU_IPC_SERVICE_METAL_SHARED_TEXTURE_RESOLVER_MAC_H_

#include <cstdint>

#include "ui/gl/scoped_egl_image.h"

namespace gpu {

// Resolves an opaque Monado Metal-XPC token in the GPU process and wraps the
// reconstructed MTLTexture (or one array slice) as an EGLImage using
// EGL_ANGLE_metal_texture_client_buffer.
//
// Chromium deliberately knows nothing about MTLSharedTextureHandle or NSXPC;
// those details are owned by the dynamically loaded Monado client helper.
gl::ScopedEGLImage ResolveMetalTextureTokenToEGLImage(uint64_t texture_token,
                                                       uint32_t array_slice);

}  // namespace gpu

#endif  // GPU_IPC_SERVICE_METAL_SHARED_TEXTURE_RESOLVER_MAC_H_
