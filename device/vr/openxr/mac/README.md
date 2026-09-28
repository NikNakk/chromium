# macOS OpenXR / WebXR port

This document describes the macOS OpenXR/WebXR implementation in this Chromium
fork and its relationship with Monado, PSVR2, and other macOS OpenXR runtimes.

The development branch is:

```text
macos-openxr-webxr
```

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
runtime-owned MTLTexture
      |
      +-- IOSurface-backed? -- yes --> Chromium standard IOSurface SharedImage
      |                                (zero-copy)
      |
      +-- no -----------------------> Chromium-owned IOSurface
                                       + Metal copy fallback
      |
      v
OpenXR runtime / compositor
```

For Monado, simple single-slice BGRA swapchains are now IOSurface-backed in the
runtime. Chromium therefore needs no Monado-specific transport in its GPU
process. Array/depth/other swapchains remain a Monado implementation detail and
may continue to use Monado's existing Metal shared-handle/XPC path.

The XR service still loads the OpenXR runtime normally and may therefore need
runtime-specific sandbox allowances. The GPU process does not load Monado's
Metal helper dylib and has no Monado Mach-service allowance.

## Session binding

The macOS graphics binding uses `XR_KHR_metal_enable`.

It:

1. calls `xrGetMetalGraphicsRequirementsKHR`;
2. uses the exact `MTLDevice` returned by the runtime;
3. creates an `MTLCommandQueue` on that device;
4. passes it in `XrGraphicsBindingMetalKHR`;
5. negotiates BGRA8 Metal swapchain formats;
6. enumerates `XrSwapchainImageMetalKHR` textures.

The current preferred formats are:

```text
MTLPixelFormatBGRA8Unorm_sRGB
MTLPixelFormatBGRA8Unorm
```

Chromium's base projection swapchain is currently a double-wide 2D image with
`arraySize = 1`; both projection views use `imageArrayIndex = 0`.

## IOSurface SharedImage transport

### Monado

For eligible Metal swapchains Monado's service path now uses its existing
native compositor allocation path. The Vulkan compositor creates native images
backed by IOSurface storage, ordinary Monado IPC transports retained IOSurface
handles back to the Metal client, and the client wraps each surface on the
application's `MTLDevice` with:

```objc
-[MTLDevice newTextureWithDescriptor:iosurface:plane:]
```

The resulting `XrSwapchainImageMetalKHR.texture` therefore has a non-null
`texture.iosurface`.

The IOSurface path is selected only for simple single-plane 2D colour
swapchains. Array/depth/unsupported swapchains keep Monado's existing
MTLSharedTextureHandle/XPC path, so clients such as Godot that use
`arraySize = 2` are unchanged.

### Chromium

`OpenXrGraphicsBindingMetal::CreateSharedImages()` checks the runtime texture
directly. When `texture.iosurface != nil`, and the IOSurface dimensions match
the OpenXR swapchain dimensions, Chromium creates a SharedImage from that
*existing* IOSurface using the normal `gfx::GpuMemoryBufferHandle` path.

This lands in Chromium's standard `IOSurfaceImageBacking` rather than a custom
OpenXR backing:

```text
XrSwapchainImageMetalKHR.texture
        |
        v
texture.iosurface
        |
        v
gfx::GpuMemoryBufferHandle
        |
        v
IOSurfaceImageBacking
        |
        v
Blink / ANGLE / WebGL
```

No replacement IOSurface is allocated for this path, so Blink renders directly
into the storage used by the OpenXR swapchain.

The SharedImage retains the usage flags used by the existing WebXR path and
matches the SharedImage colour space to the negotiated Metal format. In
particular, `MTLPixelFormatBGRA8Unorm_sRGB` is represented as sRGB rather than
linear storage.

### Runtime-neutral fallback

If the runtime texture is not IOSurface-backed, or the IOSurface dimensions do
not match the swapchain, Chromium retains the copy fallback:

```text
Blink / ANGLE
      |
      v
Chromium-owned IOSurface SharedImage
      |
      v
Metal blit / scale / EAC reprojection
      |
      v
runtime-owned OpenXR MTLTexture
```

This is the expected path for runtimes such as Meta XR Simulator.

The same renderer-completion barrier described below runs before
`RenderLayer()`, so the fallback never reads the source IOSurface before
ANGLE has actually finished writing it. The fallback currently keeps its
conservative Metal-command-buffer completion wait after the copy before the
runtime texture is released; that is separate from renderer-to-IOSurface
synchronization.

## Gaze-driven foveation

Chromium now consumes the experimental graphics-independent
`XR_MNDX_foveation` policy exposed by the runtime. Policy selection, eye-gaze
projection, and logical-to-physical mapping live above the graphics backend;
Metal is only the first implementation.

The current path is:

```text
XR_EXT_eye_gaze_interaction
        |
        v
runtime XR_MNDX_foveation policy
        |
        v
per-eye gaze projected into each XrView FOV
        |
        v
packed-target normalized focal centres
        |
        +--> compositor-side 129-point logical -> physical mapping
        |
        +--> IOSurface metadata:
             org.chromium.openxr.metal-foveation-v1
                     |
                     v
                  ANGLE Metal
                     |
                     v
           MTLRasterizationRateMap
                     |
                     v
             WebGL scene rendering
```

The default browser policy is currently the validated **Aggressive** runtime
profile (1.0 centre / 0.5 middle / 0.25 periphery on the current Monado
implementation). If gaze is unavailable, Chromium removes both the renderer
metadata and compositor mapping rather than reusing an old gaze position.

The Metal backend quantizes the packed target to a 16x16 rate grid and caches
rate maps across frames. The matching dense 129-sample mapping is submitted
with the projection pixels that were actually rendered, including sparse-frame
reuse, so Monado's fused distortion/timewarp path samples the compact raster
correctly.

### ANGLE bridge

ANGLE is a separate DEPS checkout rather than part of Chromium's Git
repository. This fork therefore carries
`openxr/mac/apply_angle_metal_foveation.py`, which is run automatically by GN
for macOS OpenXR builds. It is pinned to the ANGLE revision in this Chromium
branch and modifies only ANGLE's Metal render-pass wrapper:

- read the XR foveation dictionary from an IOSurface colour/resolve target;
- validate the logical size and per-axis rate arrays;
- create/cache an `MTLRasterizationRateMap` on the target `MTLDevice`;
- set `MTLRenderPassDescriptor.rasterizationRateMap` before ANGLE creates the
  native render encoder;
- do nothing for every surface without the XR metadata.

This keeps the Chromium renderer/GLES command stream unchanged. A future ANGLE
roll intentionally fails GN generation until this tiny bridge is rebased,
rather than silently applying it to different source.

There is related work in **WebKit's ANGLE fork**:
`GL_ANGLE_variable_rasterization_rate_metal` and
`glBindMetalRasterizationRateMapANGLE()` expose a general Metal VRS binding
and include the corresponding Metal render-pass handling. That work is not
present in current upstream ANGLE/Chromium. Our IOSurface metadata bridge is
therefore not duplicating a Chromium upstream implementation, but it does use
the same underlying ANGLE/Metal capability as WebKit's private extension. If
that extension (or an equivalent) lands upstream, the metadata consumer should
be replaced with the upstream API rather than maintained in parallel.

For PSVR2 testing, Monado must also expose eye gaze, for example by ensuring
`PSVR2_GAZE_STREAMS=1` reaches `monado-service`.

## GPU completion and swapchain release

A Chromium `SyncToken` becoming signalled only guarantees that the GPU process
has processed/scheduled the renderer commands. It does not guarantee that
Metal has finished writing an IOSurface which another process or queue can
immediately read.

Chromium already has the required lower-level primitive:
`IOSurfaceImageBacking::IOSurfaceBackingEGLStateEndAccess()` calls
`GLDisplayEGL::CreateMetalSharedEvent()` for ANGLE/Metal writes and records
the resulting `(MTLSharedEvent, value)` in the backing.

The macOS WebXR submission path now reuses that event:

```text
Blink ends SharedImage GL write access
      |
      v
IOSurfaceImageBacking::EndAccess
      |
      +--> ANGLE enqueues MTLSharedEvent signal after its Metal writes
      |
      v
renderer SyncToken
      |
      v
GPU scheduler waits for SyncToken
      |
      v
snapshot IOSurface write-completion fences
      |
      v
MTLSharedEventListener on dedicated dispatch queue
      |
      v
event value reached
      |
      v
Mojo reply to XR service
      |
      v
RenderLayer() / fallback source read
      |
      v
xrReleaseSwapchainImage / xrEndFrame
```

The event wait is asynchronous; no `glFinish()` and no CPU thread waits on
Metal completion.

A 250 ms safety timeout prevents a lost context/GPU event from permanently
wedging the XR submission path. It is logged only at DVLOG level. In-flight
callbacks use weak pointers on both the GPU-channel and XR-render-loop sides so
session exit, context loss, and GPU-channel teardown do not produce late
submission callbacks.

Trace events are emitted for:

```text
OpenXRSyncTokenSignaled
OpenXRMetalSharedEventFired
OpenXRMetalSharedEventTimeout
OpenXRSwapchainReleased
```

Each carries the WebXR frame index, so renderer-submit-to-GPU-completion and
GPU-completion-to-OpenXR-release latency can be measured in a trace.

Passing the `MTLSharedEvent` itself into Monado's compositor is deliberately
out of scope for this implementation. Doing that would require exporting the
event across the XR-runtime/service process boundary (for Monado, most likely
through its existing XPC shared-event machinery) and making the compositor
perform a GPU-side wait. That could remove the CPU notification round trip if
trace measurements show it is material.

## WebGL orientation and layers

Metal's image origin differs from Chromium's ordinary OpenXR composition
orientation. The macOS binding applies the platform flip only when the Blink
layer itself has not already requested a Y inversion:

```text
ordinary WebXR layer: flip_y = false -> Metal/OpenXR flip applied
media layer:          flip_y = true  -> no extra platform flip
```

The macOS binding supports the existing Chromium 2D composition-layer
transport for projection, quad, cylinder, and equirect layers. Browser-native
spatial video uses the same layer plumbing.

For spatial video, equirectangular 180/360 metadata uses the direct equirect
layer path. Spherical Video V2 Mesh projection is also supported. Chromium
preserves WebM `ProjectionType = 3` / `ProjectionPrivate` and MP4 `mshp`
metadata in `VideoSpatialFormat`, carries it with the browser-owned XR media
layer, and parses the mesh inside the isolated XR service.

The mesh parser supports both `raw ` and raw-DEFLATE (`dfl8`) payloads,
triangle lists/strips/fans, the legacy MP4 `ytmp` alias, and one- or two-mesh
stereo metadata. The Metal binding converts the stream-authored 3D->UV mesh
into a cached equirectangular render mesh once per XR layer and uses it to
reproject each decoded frame before submission. Packed left-right/top-bottom
stereo follows the RFC UV transforms; two-mesh custom stereo is normalized to
a left-right equirectangular output layer.

This stream-authored mesh path takes precedence over the older analytic
YouTube EAC fallback. The site DOM heuristic and hard-coded EAC shader remain
temporarily as compatibility fallback for streams where no projection metadata
reaches Chromium; they can be removed once real-world YouTube testing confirms
that current adaptive streams consistently expose their mesh metadata.

## Feature rollout

The macOS OpenXR backend is compiled when `enable_openxr` is true, but follows
Chromium's existing `OpenXR` feature and remains disabled by default on macOS.
Enable ordinary WebXR/OpenXR with:

```sh
--enable-features=OpenXR
```

Browser-owned immersive video is a separate behaviour change and is guarded by
the disabled-by-default `ImmersiveVideoPlaybackViaOpenXr` feature. Enable both
when testing fullscreen spatial-video handoff:

```sh
--enable-features=OpenXR,ImmersiveVideoPlaybackViaOpenXr
```

For the current windowed-playback and browser-owned controls experiments:

```sh
--enable-features=OpenXR,ImmersiveVideoPlaybackViaOpenXr,ImmersiveVideoKeepBrowserWindowed,ImmersiveVideoControlsViaOpenXr
```

Keeping these separate makes the macOS OpenXR runtime/backend independently
upstreamable without automatically changing ordinary fullscreen `<video>`
behaviour.

For a persistent development setup, the same features are exposed in
`chrome://flags` on macOS:

- **Enable OpenXR WebXR Runtime** enables `OpenXR`;
- **Immersive video playback via OpenXR** enables
  `ImmersiveVideoPlaybackViaOpenXr`;
- **Keep browser windowed during immersive video** enables
  `ImmersiveVideoKeepBrowserWindowed`. Chromium still enters fullscreen long
  enough to validate and establish the UA-owned XR media session, then restores
  the desktop browser window while leaving the OpenXR session active. **Escape**
  ends the windowed immersive-media session before the key is dispatched to page
  JavaScript;
- **Immersive video controls via OpenXR** enables
  `ImmersiveVideoControlsViaOpenXr`. If the runtime supports at least two
  composition layers, Chromium adds a browser-owned transport quad over the
  spatial-video layer;
- the existing **Force WebXR Runtime** selector can optionally be set to
  **OpenXR** when more than one runtime is available.

Flags are stored in the Chromium profile and take effect after relaunch, so
launch switches are not required for routine testing once they are enabled.
Command-line feature switches remain useful for clean profiles, automated tests,
and reproducing an exact configuration.

## Browser-owned immersive media

The browser-native immersive-media path is implemented on top of normal
`HTMLVideoElement` / `WebMediaPlayer` decoding. When Chromium recognizes a
supported spatial projection, decoded frames feed an XR media drawing context
and composition layer rather than a site-specific downloader/player.

The privileged UA-owned immersive-media bypass is browser-validated: the
browser verifies that the requesting frame is in fullscreen and has an active
effectively-fullscreen video before skipping the normal transient activation /
origin VR permission checks. Blink's renderer-provided flag is not sufficient
by itself. When `ImmersiveVideoKeepBrowserWindowed` is enabled, this validation
still happens while fullscreen is active; only after the native XR media layer
has been created does Blink deliberately exit desktop fullscreen.

The resolved spatial format is cached for the lifetime of the UA-owned media
session. This matters for extension-provided `data-xr-projection` hints and for
adaptive streams: the page can return to its normal DOM state after fullscreen
ends, while a later decoded-size increase can still rebuild the XR media layer
with the same projection/stereo layout.

When `ImmersiveVideoControlsViaOpenXr` is enabled, the UA session can also
create a 1024x192 mono quad layer for transport controls. The panel is hidden
initially. A completed primary controller click places it about 1.2 m in front
of the current viewer pose and slightly below eye level. Head gaze highlights
one of five controls; another click activates **-10 s**, **play/pause**,
**+10 s**, **mute/unmute**, or **exit XR**. Clicking while gaze is outside a
button hides the panel. The panel is browser-owned and operates directly on the
source `HTMLVideoElement`, so the same controls work for native YouTube spatial
video and extension-adapted players. The current first version uses head gaze
rather than eye gaze or a tracked-controller ray.

The control quad is only enabled when the runtime reports at least two render
layers. It is preserved when an adaptive stream causes the equirect/mesh video
layer to be rebuilt at a larger decoded resolution.

The old SwiftXR Shell localhost handoff is no longer part of Chromium's generic
session-creation code. Presentation ownership/yielding belongs in the runtime
or shell layer.

For spatial video without usable container metadata, a page or extension may
annotate the underlying `HTMLVideoElement` before requesting fullscreen:

```html
<video data-xr-projection="360">
```

The attribute is deliberately generic and does not identify any player or site.
Supported values are `360`, `360_LR`, `360_TB`, `180_MONO`, `180`,
`180_LR`, `180_TB`, `EAC`, and `EAC_LR`. Real container projection metadata still
takes precedence. The hint only affects the existing browser-owned immersive
video path; it does not itself bypass fullscreen/session validation.

## WebGL and WebGPU

The working path is WebGL/ANGLE.

The IOSurface migration makes WebGPU more feasible because
`IOSurfaceImageBacking` already has a Dawn/Metal representation and already
tracks Dawn-produced `MTLSharedEvent` fences. WebGPU WebXR is not enabled by
this change.

A follow-on WebGPU implementation would need to:

1. allow the macOS OpenXR binding to advertise SharedImages for WebGPU;
2. add the appropriate `SHARED_IMAGE_USAGE_WEBGPU_READ/WRITE` usages;
3. ensure the XR WebGPU frame transport imports the same IOSurface backing into
   Dawn on the runtime's Metal device;
4. propagate Dawn `EndAccess` shared-event fences through the same XR
   completion wait;
5. verify texture-format/view-format and colour-space handling for the OpenXR
   swapchain;
6. exercise projection and WebXR Layers paths under Dawn before removing the
   current WebGPU guard.

No Monado-specific GPU-process transport should be needed for that work.

## Current scope

Implemented on this branch:

- immersive OpenXR sessions on macOS;
- `XR_KHR_metal_enable`;
- BGRA8/sRGB Metal swapchains;
- standard IOSurface SharedImage zero-copy for IOSurface-backed runtime
  swapchains;
- runtime-neutral IOSurface + Metal-copy fallback;
- asynchronous renderer GPU completion using ANGLE Metal shared events;
- projection and WebXR 2D composition layers;
- browser-native equirectangular spatial-video playback;
- Spherical Video V2 stream-mesh reprojection for WebM and MP4, including
  `raw `/`dfl8`, packed stereo, two-mesh custom stereo, and legacy `ytmp`;
- YouTube EAC analytic reprojection as a metadata-missing compatibility fallback;
- clean normal session exit/re-entry;
- graphics-API-independent gaze-foveation policy and projection-map transport;
- Metal/ANGLE variable-rasterization-rate rendering for IOSurface-backed WebGL
  XR targets.

Not yet implemented or intentionally out of scope:

- WebGPU WebXR;
- GPU-side cross-process shared-event waiting in Monado;
- a generic browser XR home/dashboard;
- privileged multi-client dashboard overlays.

## Development runtime selection

Chromium uses the normal OpenXR loader and `XR_RUNTIME_JSON`, for example:

```sh
XR_RUNTIME_JSON=/path/to/openxr_monado.json \
out/mac-webxr/Chromium.app/Contents/MacOS/Chromium \
  --user-data-dir=/tmp/chromium-webxr \
  --force-webxr-runtime=openxr \
  --enable-features=OpenXR \
  --no-first-run
```

For Monado zero-copy, use a runtime build containing the IOSurface-backed
single-slice Metal swapchain changes. No Monado helper dylib is required by the
Chromium GPU process.

## Bring-up checklist

1. build/install the matching Monado branch;
2. start `monado-service`;
3. build Chromium `macos-openxr-webxr`; GN generation automatically applies
   the pinned ANGLE Metal foveation bridge;
4. confirm `navigator.xr.isSessionSupported("immersive-vr")`;
5. request a simple WebGL immersive session;
6. confirm the runtime returns BGRA8 single-slice swapchains;
7. confirm `XrSwapchainImageMetalKHR.texture.iosurface` is non-null under
   Monado;
8. confirm Chromium logs `transport=iosurface-zero-copy` at DVLOG level and
   does not enter `RenderLayer()`'s copy branch;
9. confirm the GPU process does not load
   `libmonado_metal_xpc_client.dylib` and has no Monado sandbox violation;
10. with eye gaze enabled, confirm ANGLE logs
    `Chromium XR foveation logical=... physical=... zones=16` when a rate map
    is first built or changes, then compare GPU duration with foveation disabled;
11. stress a heavy WebXR scene and inspect the three completion/release trace
    events above for each frame;
12. exit while a frame is in flight, then repeatedly enter/exit immersive mode;
13. test Meta XR Simulator and confirm it still takes the fallback copy path;
14. test an array-size-2 client such as Godot against Monado and confirm Monado
    retains its shared-handle path.

## Design principles

1. Keep Chromium's OpenXR transport runtime-neutral.
2. Prefer standard IOSurface/SharedImage mechanisms over runtime-specific GPU
   process code.
3. Keep Monado-specific XPC implementation details inside Monado.
4. Reuse Chromium's existing SharedImage access tracking and Metal shared-event
   synchronization.
5. Do not release an OpenXR IOSurface to a different queue/process merely
   because its renderer SyncToken has been scheduled.
6. Preserve the fallback for macOS runtimes whose swapchain textures are not
   IOSurface-backed.
7. Keep presentation ownership/dashboard policy outside generic Chromium
   WebXR session creation.
