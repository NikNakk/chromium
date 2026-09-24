# macOS OpenXR / WebXR port

This document describes the macOS OpenXR/WebXR port in this Chromium fork and
its relationship with Monado, PSVR2, and SwiftXR Shell.

The design keeps Chromium narrow: it behaves as a normal desktop browser until
a page explicitly requests an immersive WebXR session. SwiftXR Shell remains
the headset home/dashboard/desktop environment and yields presentation while an
external OpenXR application is active.

## Runtime architecture

```text
Blink / WebXR
      |
      v
Chromium isolated XR service
      |
      v
XR_KHR_metal_enable
      |
      v
Monado OpenXR client
      |
      +---- ordinary Monado IPC ----------> monado-service
      |
      +---- Metal-handle XPC side channel > monado-service
                                             |
                                             v
                                           PSVR2
```

Chromium does not implement Monado's XPC protocol. It calls a deliberately tiny
C helper exported by Monado; the helper owns all NSXPC and
`MTLSharedTextureHandle` handling.

The development branch is:

```text
macos-openxr-webxr
```

## Session binding

The macOS graphics binding uses `XR_KHR_metal_enable`.

It:

1. calls `xrGetMetalGraphicsRequirementsKHR`;
2. uses the exact `MTLDevice` returned by the runtime;
3. creates an `MTLCommandQueue` from that device;
4. passes it with `XrGraphicsBindingMetalKHR`;
5. negotiates BGRA8 Metal swapchain formats;
6. enumerates `XrSwapchainImageMetalKHR` textures.

The exact runtime device matters because the current Monado Metal binding
validates the command queue against the device returned in the graphics
requirements.

The first-pass formats are:

```text
MTLPixelFormatBGRA8Unorm_sRGB
MTLPixelFormatBGRA8Unorm
```

Chromium's existing projection-layer layout is retained: the two views occupy
one double-wide 2D swapchain image. The OpenXR swapchain therefore currently
uses `arraySize = 1`, and both projection views use `imageArrayIndex = 0`.

## Direct Metal SharedImage transport

The old proposed IOSurface/intermediate-texture/blit path is no longer the
preferred implementation. Monado's macOS service path already supports shared
Metal textures across process boundaries, so Chromium can render directly into
the same Metal allocation used by the OpenXR swapchain.

The implemented path is:

```text
XR service process                         GPU process
------------------                         -----------

XrSwapchainImageMetalKHR.texture
        |
        | Monado helper:
        | publish_claimable_texture()
        v
opaque uint64 token
        |
        +--------------- Mojo --------------------+
                                                    |
                                                    v
                                      Monado helper:
                                      take_texture_on_device()
                                                    |
                                  Monado XPC side channel
                                                    |
                                                    v
                                  shared MTLTexture on ANGLE's
                                  exact MTLDevice
                                                    |
                                                    v
                                  EGL_METAL_TEXTURE_ANGLE
                                                    |
                                                    v
                                      Chromium SharedImage
                                                    |
                                                    v
                                         Blink / WebGL
```

No `MTLSharedTextureHandle`, Objective-C XPC object, IOSurface, or Mach port is
sent through Chromium Mojo. Mojo carries only the opaque token plus the selected
array slice.

### Why the receiving Metal device is explicit

ANGLE's `EGL_ANGLE_metal_texture_client_buffer` requires an imported
`MTLTexture` to belong to the exact `MTLDevice` backing ANGLE's EGL display.

The GPU process therefore obtains that device from Chromium's
`GLDisplayEGL::GetMetalDevice()` and passes it to the Monado helper. Monado
recreates the shared texture with that receiving device before Chromium creates
the EGLImage. This avoids relying on `MTLSharedTextureHandle.device` happening
to return the same Objective-C device object.

### SharedImage backing

The GPU service has a macOS path that registers an externally supplied EGLImage
with Chromium's existing `EGLImageBacking`. ANGLE creates that EGLImage with:

```text
target: EGL_METAL_TEXTURE_ANGLE
context: EGL_NO_CONTEXT
buffer: receiving-process MTLTexture
attribute: EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE
```

The current base projection swapchain is a normal 2D texture and uses slice 0.
The slice is still explicit in the transport so a future array-backed layer can
select a layer without changing the Mojo contract.

The backing then uses Chromium's ordinary mailbox/export/WebGL machinery. Blink
therefore renders directly into the OpenXR swapchain allocation rather than
rendering an intermediate texture which Chromium later copies.

### WebGL vertical orientation

The direct WebGL texture reaches OpenXR with WebGL's usual vertical orientation.
Chromium's existing OpenXR projection code corrects this by inverting the
projection FOV when the composition-layer image-layout extension is not being
used. For a direct SharedImage session, the runtime must therefore advertise
`fovMutable = XR_TRUE`. The macOS port applies the same capability check used
by Chromium's Windows direct-SharedImage path and fails session creation early
when a mandatory direct path cannot be used.

## Monado helper ABI

Chromium dynamically loads:

```text
/usr/local/lib/libmonado_metal_xpc_client.dylib
```

The helper intentionally exposes only a small C ABI:

```text
monado_metal_xpc_publish_claimable_texture(...)
monado_metal_xpc_take_texture_on_device(...)
monado_metal_xpc_release_texture(...)
```

The corresponding Monado branch builds and installs this helper. Chromium's
macOS GPU sandbox and dedicated XR-compositing sandbox permit executable mapping
of this single installed dylib and Mach lookup of only:

```text
org.freedesktop.monado.metal-ipc
```

There is deliberately no arbitrary helper-path environment override: such a
path would not be usable inside the sandbox without broadening its filesystem
policy. Other macOS utility services retain their normal sandbox and do not gain
access to the Monado helper or Mach service.

For development, install the Monado build with an install prefix that places
the helper at the path above (the current Chromium port expects
`/usr/local/lib`).

## Token ownership

Normal Monado Metal-XPC tokens remain PID scoped. Those ordinary tokens retain
their legacy 32-bit-compatible layout. Chromium's claimable handoff uses a
separate 64-bit token namespace with 56 random bits, since it travels through
Mojo rather than the legacy Monado image-metadata field.

Chromium needs one special handoff because the OpenXR runtime is used from the
isolated XR process while the SharedImage is constructed in Chromium's GPU
process. Monado therefore supports an explicitly **claimable texture token**:

1. the XR process publishes the texture and owns the token;
2. it explicitly marks that texture token claimable;
3. the first different PID that retrieves it becomes the new owner;
4. the claimable marker is removed immediately;
5. the token is PID scoped again to the receiving process;
6. texture retrieval consumes/discards the token.

This is narrower than making all Monado texture tokens globally retrievable.
The existing PID-scoped service/client path is unchanged for ordinary OpenXR
applications.

## Synchronization

Pixel transport is zero-copy, but synchronization is still required.

After Blink finishes exporting the SharedImage, Chromium receives the renderer's
GPU `SyncToken`. On macOS the OpenXR render loop now waits for those tokens
asynchronously with `SharedImageInterface::SignalSyncToken()`.

Only after the GPU write is complete does Chromium continue the normal OpenXR
submission path and release the acquired swapchain image.

```text
Blink/ANGLE write
      |
      v
Chromium GPU SyncToken
      |
      v
asynchronous SignalSyncToken completion
      |
      v
xrReleaseSwapchainImage / xrEndFrame
      |
      v
Monado's existing app <-> compositor synchronization
```

This avoids:

- `glFinish()`;
- CPU pixel copies;
- a Metal blit into another OpenXR texture;
- duplicating Monado's shared-event/XPC synchronization protocol in Chromium.

## WebGL and WebGPU

The first direct path is intentionally **WebGL only**.

Chromium's `EGLImageBacking` can expose the external Metal texture through GL
representations, which is sufficient for the current WebGL WebXR path.
Chromium's Dawn/Metal SharedImage representation does not currently import this
external `EGL_METAL_TEXTURE_ANGLE` backing.

The macOS OpenXR binding therefore does not advertise SharedImage support for a
WebGPU XR session. WebGPU can be added later with a native Dawn/Metal external
texture representation rather than by pretending the current GL representation
works.

## Current scope

The first usable milestone supports:

- immersive OpenXR session creation on macOS;
- native Metal OpenXR binding;
- one double-wide projection layer;
- BGRA8 swapchains;
- direct shared-Metal rendering for WebGL;
- asynchronous GPU completion before OpenXR release;
- normal OpenXR session exit.

Not yet implemented in this transport milestone:

- WebGPU WebXR;
- Chromium overlay composition over the direct OpenXR texture;
- general WebXR Layers support;
- a browser-native XR home/dashboard;
- privileged multi-client dashboard overlays.

Those are independent follow-on features and should not be folded into the
basic pixel-transport path.

## Development runtime selection

During development Chromium can use the normal OpenXR loader and
`XR_RUNTIME_JSON`, for example:

```sh
XR_RUNTIME_JSON=/path/to/openxr_monado.json \
out/mac-webxr/Chromium.app/Contents/MacOS/Chromium \
  --user-data-dir=/tmp/chromium-webxr \
  --force-webxr-runtime=openxr \
  --no-first-run
```

Before starting Chromium, ensure that:

- the matching Monado runtime/client build is selected;
- `libmonado_metal_xpc_client.dylib` is installed at
  `/usr/local/lib/libmonado_metal_xpc_client.dylib`;
- the launchd/direct Monado Metal XPC service is registered as described in the
  Monado macOS service documentation.

## Bring-up checklist

A useful order for validation is:

1. build/install the Monado direct-XPC branch and helper dylib;
2. bootstrap the launchd-managed Monado service;
3. build Chromium `macos-openxr-webxr`;
4. confirm `navigator.xr.isSessionSupported("immersive-vr")`;
5. request a simple WebGL `immersive-vr` session;
6. confirm Chromium creates/enumerates the Metal OpenXR swapchain;
7. confirm the XR process publishes one claimable token per swapchain image;
8. confirm the GPU process claims each token and reconstructs the texture on
   ANGLE's Metal device;
9. confirm `EGL_METAL_TEXTURE_ANGLE` image creation succeeds;
10. confirm frame submission waits for the renderer SyncToken rather than
    falling back to `glFinish`;
11. confirm the minimal WebXR sample is visible and head tracked in PSVR2;
12. end the session and confirm presentation returns cleanly to SwiftXR Shell.

Useful failure signatures are deliberately logged at each boundary: helper
loading, token publication, token claim, Metal-device reconstruction, EGLImage
creation, SharedImage creation, and renderer synchronization.

## User-experience model

Outside immersive WebXR, Chromium remains an ordinary browser visible through
SwiftXR Shell's existing desktop support. A page enters immersive VR only after
the normal WebXR user gesture and `requestSession("immersive-vr")` flow.

While Chromium owns the immersive OpenXR session, SwiftXR Shell should yield
foreground presentation. When that session ends, the shell resumes without the
browser needing to relaunch.

Presentation ownership should remain a generic Monado/SwiftXR concern so the
same behaviour works for Chromium, Unity, Unreal, Godot, Open Brush, and other
OpenXR clients.

## Design principles

1. Keep Chromium's role standards-compatible and narrow.
2. Keep XPC/Metal-handle transport inside Monado.
3. Pass only opaque tokens through Chromium IPC.
4. Render directly into shared OpenXR Metal storage where possible.
5. Use Chromium SyncTokens for renderer-to-XR completion.
6. Reuse Monado's existing compositor synchronization after OpenXR release.
7. Avoid CPU copies and avoid an unnecessary IOSurface/blit stage.
8. Keep shell/dashboard ownership in SwiftXR Shell/Monado.
