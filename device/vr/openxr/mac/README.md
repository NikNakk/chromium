# macOS OpenXR / WebXR port

This document describes the macOS OpenXR/WebXR port in this Chromium fork and
the intended relationship between Chromium, Monado, PSVR2, and SwiftXR Shell.

The goal is to keep Chromium's role narrow and standards-compatible: Chromium
should behave like a normal desktop browser until a page explicitly requests an
immersive WebXR session. The headset shell/dashboard is a separate concern and
belongs in SwiftXR Shell / Monado rather than in Chromium itself.

## Goals

- Enable Chromium WebXR immersive VR on macOS through OpenXR.
- Use the standard WebXR lifecycle:
  - normal web browsing outside immersive XR;
  - `navigator.xr.isSessionSupported("immersive-vr")`;
  - explicit user action;
  - `navigator.xr.requestSession("immersive-vr")`;
  - immersive presentation through OpenXR;
  - return to the normal browser when the XR session ends.
- Use the Khronos OpenXR loader and `XR_RUNTIME_JSON` during development.
- Use `XR_KHR_metal_enable` and native Metal on macOS.
- Keep the Chromium changes as close as practical to upstream Chromium's
  Windows/Linux OpenXR architecture.
- Let SwiftXR Shell provide headset home/dashboard/desktop functionality.

## Non-goals

For the initial port, Chromium does not need to become a complete spatial
browser shell.

In particular, Chromium does not initially need to provide:

- a floating browser window in an XR home environment;
- a VR launcher;
- desktop capture/presentation outside WebXR;
- app switching;
- a SteamVR-style dashboard;
- browser UI overlays over arbitrary non-browser XR applications.

Those functions belong naturally in SwiftXR Shell and/or Monado.

## Intended user experience

### Normal browsing

When no immersive application owns presentation, SwiftXR Shell is the headset
environment. Its existing desktop support can expose the macOS desktop and
therefore a normal Chromium window.

Conceptually:

```text
PSVR2
  |
  v
Monado
  |
  v
SwiftXR Shell
  |
  +-- desktop view
       |
       +-- Chromium window
            |
            +-- ordinary web page
```

Chromium remains an ordinary macOS desktop browser at this point.

### Entering WebXR

A WebXR-capable page detects support:

```js
await navigator.xr.isSessionSupported("immersive-vr")
```

The page offers an explicit "Enter VR" control. On user activation it requests:

```js
await navigator.xr.requestSession("immersive-vr")
```

Chromium then becomes the active immersive OpenXR client:

```text
Chromium page
    |
    v
WebXR
    |
    v
Chromium OpenXR backend
    |
    v
XR_KHR_metal_enable
    |
    v
Monado
    |
    v
PSVR2
```

SwiftXR Shell should yield headset presentation while Chromium's immersive
session is active.

### Leaving WebXR

When the page calls `XRSession.end()`, navigates away, closes, crashes, or
otherwise loses the immersive session, presentation returns to SwiftXR Shell.

The intended transition is:

```text
SwiftXR desktop/browser view
        |
        | user selects Enter VR
        v
Chromium immersive WebXR
        |
        | XR session ends
        v
SwiftXR desktop/browser view
```

The page/browser should not need to be relaunched or reloaded as part of this
transition.

## Relationship to Windows

The target is broadly analogous to current Chromium/OpenXR behaviour on
Windows.

Chrome/Edge run as conventional desktop browsers. A WebXR page explicitly
enters an immersive OpenXR session. A separate XR runtime/shell such as SteamVR
provides the headset dashboard, desktop view, launcher, and app switching.

For this project the analogous split is:

```text
Windows                          macOS port
-------                          ----------
Chrome / Edge                    Chromium
WebXR                            WebXR
OpenXR                           OpenXR
SteamVR/OpenXR runtime           Monado
SteamVR dashboard/desktop        SwiftXR Shell
PC VR headset                    PSVR2
```

This separation is preferable to building a custom spatial browser UI inside
Chromium.

## Presentation ownership

The initial implementation should use explicit foreground ownership.

### No external immersive client

SwiftXR Shell presents to the headset.

### External immersive client active

When Chromium, Godot, Unity, Unreal, Open Brush, or another OpenXR application
starts an immersive session, SwiftXR Shell yields presentation.

### External immersive client exits

SwiftXR Shell resumes presentation immediately, retaining its desktop/window
state.

The desired general model is therefore:

```text
no external immersive client
        -> SwiftXR Shell presents

external immersive client active
        -> SwiftXR Shell yields

external immersive client exits
        -> SwiftXR Shell resumes
```

This should be implemented generically rather than making SwiftXR Shell
Chromium-specific.

Monado is the natural place to arbitrate this because it already knows when
OpenXR clients create, begin, end, and destroy sessions.

A future runtime-facing abstraction could expose state equivalent to:

```text
foreground XR client:
    SwiftXR Shell
    Chromium
    Godot
    Unity
    Unreal
    ...
```

The exact API is still to be designed.

## Future dashboard/overlay mode

The first implementation should be a full hand-off: SwiftXR Shell stops
submitting headset frames while an external immersive client owns presentation.

A later enhancement could allow SwiftXR Shell to appear as a privileged
dashboard/overlay over another XR application, similar to SteamVR's dashboard.

Potential uses include:

- desktop access;
- launcher;
- app switching;
- notifications;
- settings;
- controller/battery status;
- exit/return-to-home controls.

That requires compositor/runtime support for multi-client composition or a
privileged overlay mechanism and is deliberately outside the first WebXR
milestone.

## Chromium macOS OpenXR architecture

The intended Chromium path is:

```text
Blink / WebXR
      |
      v
Chromium XR service
      |
      v
OpenXrPlatformHelperMac
      |
      v
Khronos OpenXR loader
      |
      v
Monado runtime
      |
      v
XR_KHR_metal_enable
      |
      v
Metal / PSVR2 compositor
```

Chromium's newer Linux OpenXR implementation is an important reference for the
desktop runtime lifecycle and SharedImage transport architecture.

## Current port status

Branch:

```text
macos-openxr-webxr
```

Initial commits:

```text
a20f76668cfe  Enable macOS OpenXR runtime discovery
54c895f9968f  Add initial macOS OpenXR Metal binding
```

### Runtime discovery

The first commit:

- enables OpenXR on macOS in Chromium build flags;
- treats macOS as a desktop OpenXR platform;
- configures Chromium's bundled Khronos loader for Apple/Metal;
- adds `OpenXrPlatformHelperMac`;
- wires macOS into the isolated XR runtime provider;
- supports runtime discovery through the normal OpenXR loader;
- allows `XR_RUNTIME_JSON` to select the development Monado runtime.

### Metal session binding

The second commit adds the initial native Metal graphics binding.

It:

1. calls `xrGetMetalGraphicsRequirementsKHR`;
2. uses the exact `MTLDevice` returned by the runtime;
3. creates an `MTLCommandQueue` from that device;
4. passes the queue using `XrGraphicsBindingMetalKHR`;
5. negotiates a supported BGRA8 Metal swapchain format;
6. enumerates `XrSwapchainImageMetalKHR` textures.

The exact runtime-provided device matters: the current Monado macOS Metal
implementation validates that the command queue belongs to the same
`MTLDevice` returned by `xrGetMetalGraphicsRequirementsKHR`.

Supported first-pass formats are deliberately narrow:

```text
MTLPixelFormatBGRA8Unorm_sRGB
MTLPixelFormatBGRA8Unorm
```

This matches the formats already supported by the current Monado Metal client.

## Pixel transport

The next major implementation step is Chromium-rendered pixel transport.

The preferred architecture is:

```text
Blink/WebXR renderer
        |
        v
Chromium SharedImage
        |
        v
IOSurface-backed buffer
        |
        v
MTLTexture
        |
        | GPU-only Metal copy/blit
        v
XrSwapchainImageMetalKHR
        |
        v
Monado
```

Chromium already has macOS SharedImage/IOSurface infrastructure and can move
IOSurface-backed GPU memory handles across its process boundaries. The port
should use that machinery rather than CPU readback/copying.

The initial rendering milestone should remain deliberately constrained:

- one projection layer;
- BGRA8;
- IOSurface-backed intermediate images;
- Metal blit/copy into the acquired OpenXR swapchain image;
- no browser overlay;
- no WebXR Layers support beyond what is needed for the base projection layer;
- no unnecessary MoltenVK path.

Once basic presentation works, synchronization can be refined to avoid CPU
waits where possible.

## Development runtime selection

During development, Chromium should be launched with the desired Monado runtime
manifest, for example:

```sh
XR_RUNTIME_JSON=/path/to/openxr_monado.json \
out/mac-webxr/Chromium.app/Contents/MacOS/Chromium \
  --user-data-dir=/tmp/chromium-webxr \
  --force-webxr-runtime=openxr \
  --no-first-run
```

Chromium's OpenXR feature is disabled by default on non-Windows desktop
platforms at present, but `--force-webxr-runtime=openxr` selects and enables
the OpenXR runtime for development.

## Browser-side milestones

Useful milestones, in order:

1. Chromium builds normally on macOS with OpenXR enabled.
2. The Khronos loader loads the selected Monado runtime.
3. Chromium detects an OpenXR system.
4. From JavaScript:
   ```js
   'xr' in navigator
   ```
   is true.
5. From a secure WebXR context:
   ```js
   await navigator.xr.isSessionSupported("immersive-vr")
   ```
   reports support.
6. `requestSession("immersive-vr")` reaches native Metal OpenXR session
   creation.
7. Chromium creates and enumerates the Metal OpenXR swapchain.
8. Chromium SharedImages are exported as IOSurfaces.
9. Metal copies/blits those images into acquired
   `XrSwapchainImageMetalKHR` textures.
10. A minimal WebXR sample is visible and head-tracked in PSVR2.
11. Session exit cleanly returns presentation to SwiftXR Shell.

The first content test should be a minimal WebXR sample rather than YouTube.

## YouTube and non-WebXR content

WebXR support in Chromium does not automatically turn ordinary video sites into
immersive XR applications.

For normal/non-WebXR web content, SwiftXR Shell's desktop support remains the
appropriate mechanism.

Specialized 180/360 video support may still be better handled by the existing
SwiftXR / yt-dlp / custom video-player path unless a site explicitly supplies a
WebXR experience.

## Design principles

1. **Keep Chromium narrow.**
   Port upstream-style OpenXR/WebXR support; avoid turning the fork into a
   complete XR desktop environment.

2. **Keep shell concerns in SwiftXR.**
   Home, launcher, desktop, app switching and future dashboard behaviour belong
   there.

3. **Keep ownership arbitration in Monado.**
   Presentation ownership should work for all XR clients, not just Chromium.

4. **Use native Metal.**
   The port already has a working Metal-capable Monado runtime. Do not add
   MoltenVK solely to make Chromium WebXR work unless a concrete requirement
   emerges.

5. **Avoid CPU copies.**
   Prefer Chromium SharedImage -> IOSurface -> Metal texture -> OpenXR
   swapchain GPU paths.

6. **Preserve normal WebXR semantics.**
   A site may advertise immersive support, but entering immersive VR remains an
   explicit user action rather than something triggered automatically on page
   navigation.

7. **Make the first path simple.**
   Projection layer first; dashboard overlays, browser overlays, controller
   polish and advanced WebXR layers can follow once presentation is reliable.

## Longer-term architecture

The intended mature system is:

```text
                         PSVR2
                           |
                           v
                        Monado
                           |
             +-------------+-------------+
             |                           |
             v                           v
       SwiftXR Shell                 XR applications
       ------------                  ---------------
       headset home                  Chromium WebXR
       desktop                       Godot
       launcher                      Unity
       app switching                 Unreal
       future dashboard              Open Brush
       future overlays               other OpenXR apps
```

This makes SwiftXR Shell the macOS/PSVR2 equivalent of the system XR shell while
allowing Chromium to remain a conventional browser with standards-compliant
immersive WebXR support.
