// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_MEDIA_CONTROLS_DRAWING_CONTEXT_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_MEDIA_CONTROLS_DRAWING_CONTEXT_H_

#include <cstdint>

#include "gpu/command_buffer/common/sync_token.h"
#include "third_party/blink/renderer/modules/xr/xr_layer_drawing_context.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"

namespace blink {

class XRCompositionLayer;
class XRSession;

// UA-owned raster drawing context for the compact transport panel used by
// browser-native immersive video. The panel is intentionally independent of
// page DOM/WebGL so it works for YouTube and extension-adapted players alike.
class XRMediaControlsDrawingContext final : public XRLayerDrawingContext {
 public:
  explicit XRMediaControlsDrawingContext(XRSession* session);
  ~XRMediaControlsDrawingContext() override;

  void SetState(bool paused, bool muted, int hovered_control);

  // Hit-tests normalized panel coordinates in [0, 1]. Returns one of the five
  // transport control indices, or -1 when the gaze falls between/outside them.
  static int HitTest(float normalized_x, float normalized_y);

  // XRLayerDrawingContext.
  void OnFrameStart() override;
  void OnFrameEnd() override;
  void SetCompositionLayer(XRCompositionLayer* layer) override;
  uint16_t TextureWidth() const override { return width_; }
  uint16_t TextureHeight() const override { return height_; }
  uint16_t TextureArrayLength() const override { return 1; }
  bool TextureWasQueried() const override { return content_changed_; }
  bool NeedsRasterAccess() const override { return true; }

  XRSession* session() const override { return session_.Get(); }
  std::unique_ptr<SharedImageHolder> TransferToSharedImageHolder() override;
  std::unique_ptr<SharedImageHolder> DoneWithSharedBuffer() override;
  XRFrameTransportDelegate* GetTransportDelegate() override {
    return frame_transport_delegate_.Get();
  }

  void Trace(Visitor* visitor) const override;

 private:
  Member<XRSession> session_;
  Member<XRCompositionLayer> layer_;
  Member<XRFrameTransportDelegate> frame_transport_delegate_;

  gpu::SyncToken sync_token_;
  uint16_t width_ = 1024;
  uint16_t height_ = 192;

  bool dirty_ = true;
  bool content_changed_ = false;
  bool paused_ = true;
  bool muted_ = false;
  int hovered_control_ = -1;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_MEDIA_CONTROLS_DRAWING_CONTEXT_H_
