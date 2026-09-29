// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/openxr/openxr_input_helper.h"

#include "base/check.h"
#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "base/trace_event/trace_event.h"
#include "device/gamepad/public/cpp/gamepad.h"
#include "device/vr/openxr/openxr_api_wrapper.h"
#include "device/vr/openxr/openxr_extension_helper.h"
#include "device/vr/openxr/openxr_hand_utils.h"
#include "device/vr/openxr/openxr_util.h"
#include "device/vr/public/mojom/openxr_interaction_profile_type.mojom.h"
#include "device/vr/public/mojom/vr_service.mojom.h"
#include "third_party/openxr/src/include/openxr/openxr.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/geometry/transform_util.h"

namespace device {

XrResult OpenXRInputHelper::CreateOpenXRInputHelper(
    XrInstance instance,
    XrSystemId system,
    const std::string& system_name,
    const OpenXrExtensionHelper& extension_helper,
    XrSession session,
    XrSpace local_space,
    bool hand_input_enabled,
    bool eye_gaze_enabled,
    std::unique_ptr<OpenXRInputHelper>* helper) {
  std::unique_ptr<OpenXRInputHelper> new_helper =
      std::make_unique<OpenXRInputHelper>(session, local_space,
                                          hand_input_enabled);

  RETURN_IF_XR_FAILED(new_helper->Initialize(
      instance, system, system_name, extension_helper, eye_gaze_enabled));
  *helper = std::move(new_helper);
  return XR_SUCCESS;
}

OpenXRInputHelper::OpenXRInputHelper(XrSession session,
                                     XrSpace local_space,
                                     bool hand_input_enabled)
    : session_(session),
      local_space_(local_space),
      path_helper_(std::make_unique<OpenXRPathHelper>()),
      hand_input_enabled_(hand_input_enabled) {}

OpenXRInputHelper::~OpenXRInputHelper() {
  if (eye_gaze_space_ != XR_NULL_HANDLE) {
    xrDestroySpace(eye_gaze_space_);
  }
  if (eye_gaze_action_set_ != XR_NULL_HANDLE) {
    xrDestroyActionSet(eye_gaze_action_set_);
  }
}

bool OpenXRInputHelper::IsHandTrackingEnabled() const {
  // As long as we have at least one controller that can supply hand tracking
  // data, then hand tracking is enabled.
  return std::ranges::any_of(controller_states_,
                             [](const OpenXrControllerState& state) {
                               return state.controller.IsHandTrackingEnabled();
                             });
}

XrResult OpenXRInputHelper::Initialize(
    XrInstance instance,
    XrSystemId system,
    const std::string& system_name,
    const OpenXrExtensionHelper& extension_helper,
    bool eye_gaze_enabled) {
  RETURN_IF_XR_FAILED(path_helper_->Initialize(instance, system_name));

  // This map is used to store bindings for different kinds of interaction
  // profiles. This allows the runtime to choose a different input sources based
  // on availability.
  std::map<XrPath, std::vector<XrActionSuggestedBinding>> bindings;

  // Eye gaze participates in the same one-shot action-set attachment as the
  // controllers. OpenXR does not permit attaching another action set later in
  // the session, so this must be created before xrAttachSessionActionSets.
  if (eye_gaze_enabled &&
      extension_helper.ExtensionEnumeration()->ExtensionSupported(
          XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) {
    XrSystemEyeGazeInteractionPropertiesEXT gaze_properties = {
        XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties system_properties = {XR_TYPE_SYSTEM_PROPERTIES};
    system_properties.next = &gaze_properties;
    if (XR_SUCCEEDED(
            xrGetSystemProperties(instance, system, &system_properties)) &&
        gaze_properties.supportsEyeGazeInteraction == XR_TRUE) {
      XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
      base::span<char> action_set_name(set_info.actionSetName);
      size_t copied_size =
          base::strlcpy(action_set_name, "chromium_eye_gaze");
      CHECK_LT(copied_size, action_set_name.size());

      base::span<char> localized_action_set_name(
          set_info.localizedActionSetName);
      copied_size =
          base::strlcpy(localized_action_set_name, "Chromium eye gaze");
      CHECK_LT(copied_size, localized_action_set_name.size());

      RETURN_IF_XR_FAILED(
          xrCreateActionSet(instance, &set_info, &eye_gaze_action_set_));

      RETURN_IF_XR_FAILED(xrStringToPath(instance, "/user/eyes_ext",
                                         &eye_gaze_subaction_path_));

      XrActionCreateInfo action_info = {XR_TYPE_ACTION_CREATE_INFO};
      action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;

      base::span<char> action_name(action_info.actionName);
      copied_size = base::strlcpy(action_name, "chromium_gaze_pose");
      CHECK_LT(copied_size, action_name.size());

      base::span<char> localized_action_name(action_info.localizedActionName);
      copied_size = base::strlcpy(localized_action_name, "Eye gaze pose");
      CHECK_LT(copied_size, localized_action_name.size());
      action_info.countSubactionPaths = 1;
      action_info.subactionPaths = &eye_gaze_subaction_path_;
      RETURN_IF_XR_FAILED(xrCreateAction(eye_gaze_action_set_, &action_info,
                                         &eye_gaze_action_));

      XrPath interaction_profile = XR_NULL_PATH;
      XrPath binding_path = XR_NULL_PATH;
      RETURN_IF_XR_FAILED(xrStringToPath(
          instance, "/interaction_profiles/ext/eye_gaze_interaction",
          &interaction_profile));
      RETURN_IF_XR_FAILED(xrStringToPath(
          instance, "/user/eyes_ext/input/gaze_ext/pose", &binding_path));
      bindings[interaction_profile].push_back(
          {eye_gaze_action_, binding_path});
      eye_gaze_enabled_ = true;
    }
  }

  for (size_t i = 0; i < controller_states_.size(); i++) {
    RETURN_IF_XR_FAILED(controller_states_[i].controller.Initialize(
        static_cast<OpenXrHandednessType>(i), instance, session_,
        path_helper_.get(), extension_helper, hand_input_enabled_, &bindings));
    controller_states_[i].primary_button_pressed = false;
    controller_states_[i].squeeze_button_pressed = false;
  }

  for (auto it = bindings.begin(); it != bindings.end(); it++) {
    XrInteractionProfileSuggestedBinding profile_suggested_bindings = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    profile_suggested_bindings.interactionProfile = it->first;
    profile_suggested_bindings.suggestedBindings = it->second.data();
    profile_suggested_bindings.countSuggestedBindings = it->second.size();

    RETURN_IF_XR_FAILED(xrSuggestInteractionProfileBindings(
        instance, &profile_suggested_bindings));
  }

  std::vector<XrActionSet> action_sets;
  action_sets.reserve(controller_states_.size() + (eye_gaze_enabled_ ? 1 : 0));
  for (size_t i = 0; i < controller_states_.size(); i++) {
    action_sets.push_back(controller_states_[i].controller.action_set());
  }
  if (eye_gaze_enabled_) {
    action_sets.push_back(eye_gaze_action_set_);
  }

  XrSessionActionSetsAttachInfo attach_info = {
      XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach_info.countActionSets = action_sets.size();
  attach_info.actionSets = action_sets.data();
  RETURN_IF_XR_FAILED(xrAttachSessionActionSets(session_, &attach_info));

  if (eye_gaze_enabled_) {
    XrActionSpaceCreateInfo space_info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space_info.action = eye_gaze_action_;
    space_info.subactionPath = eye_gaze_subaction_path_;
    space_info.poseInActionSpace.orientation.w = 1.0f;
    RETURN_IF_XR_FAILED(
        xrCreateActionSpace(session_, &space_info, &eye_gaze_space_));
  }

  return XR_SUCCESS;
}

void OpenXRInputHelper::OnHideInputSources() {
  // Clear any "pressed" buttons for the time being. This prevents us from
  // sending up any clicks when we resume sending input state to the page.
  ResetControllerButtonState();
}

void OpenXRInputHelper::ResetControllerButtonState() {
  for (OpenXrControllerState& state : controller_states_) {
    state.primary_button_pressed = false;
    state.squeeze_button_pressed = false;
  }
}

std::vector<mojom::XRInputSourceStatePtr> OpenXRInputHelper::GetInputState(
    XrTime predicted_display_time) {
  TRACE_EVENT0("xr", "GetInputState");
  std::vector<mojom::XRInputSourceStatePtr> input_states;
  if (XR_FAILED(SyncActions(predicted_display_time))) {
    ResetControllerButtonState();
    return input_states;
  }

  for (uint32_t i = 0; i < controller_states_.size(); i++) {
    device::OpenXrController* controller = &controller_states_[i].controller;
    TRACE_EVENT1("xr", "ParseController", "Handedness",
                 controller->GetHandness());

    std::optional<GamepadButton> menu_button =
        controller->GetButton(OpenXrButtonType::kMenu);

    // Pressing a menu buttons is treated as a signal to exit the WebXR session.
    if (menu_button && menu_button.value().pressed) {
      OnExitGesture();
    }

    std::optional<GamepadButton> primary_button =
        controller->GetButton(OpenXrButtonType::kTrigger);
    std::optional<GamepadButton> squeeze_button =
        controller->GetButton(OpenXrButtonType::kSqueeze);

    // Having a trigger button is the minimum for an webxr input.
    // No trigger button indicates input is not connected.
    if (!primary_button) {
      continue;
    }

    device::mojom::XRInputSourceStatePtr state =
        device::mojom::XRInputSourceState::New();

    // ID 0 will cause a DCHECK in the hash table used on the blink side.
    // To ensure that we don't have any collisions with other ids, increment
    // all of the ids by one.
    state->source_id = i + 1;
    state->description = controller->GetDescription(predicted_display_time);
    if (!state->description) {
      continue;
    }

    state->mojo_from_input = controller->GetMojoFromGripTransform(
        predicted_display_time, local_space_, &state->emulated_position);
    state->primary_input_pressed = primary_button.value().pressed;
    state->primary_input_clicked =
        controller_states_[i].primary_button_pressed &&
        !state->primary_input_pressed;
    controller_states_[i].primary_button_pressed = state->primary_input_pressed;
    if (squeeze_button) {
      state->primary_squeeze_pressed = squeeze_button.value().pressed;
      state->primary_squeeze_clicked =
          controller_states_[i].squeeze_button_pressed &&
          !state->primary_squeeze_pressed;
      controller_states_[i].squeeze_button_pressed =
          state->primary_squeeze_pressed;
    }

    state->gamepad = controller->GetWebXRGamepad();

    // This will return null if hand tracking isn't possible/enabled.
    state->hand_tracking_data = controller->GetHandTrackingData();

    input_states.push_back(std::move(state));
  }

  return input_states;
}

std::optional<XrPosef> OpenXRInputHelper::GetEyeGazePose(
    XrSpace base_space,
    XrTime predicted_display_time) const {
  if (!eye_gaze_enabled_ || eye_gaze_action_ == XR_NULL_HANDLE ||
      eye_gaze_space_ == XR_NULL_HANDLE) {
    return std::nullopt;
  }

  XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
  get_info.action = eye_gaze_action_;
  get_info.subactionPath = eye_gaze_subaction_path_;
  XrActionStatePose pose_state = {XR_TYPE_ACTION_STATE_POSE};
  if (XR_FAILED(
          xrGetActionStatePose(session_, &get_info, &pose_state)) ||
      pose_state.isActive != XR_TRUE) {
    return std::nullopt;
  }

  XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(eye_gaze_space_, base_space,
                              predicted_display_time, &location)) ||
      !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
    return std::nullopt;
  }

  return location.pose;
}

XrResult OpenXRInputHelper::OnInteractionProfileChanged() {
  for (OpenXrControllerState& controller_state : controller_states_) {
    RETURN_IF_XR_FAILED(controller_state.controller.UpdateInteractionProfile());
  }
  return XR_SUCCESS;
}

XrResult OpenXRInputHelper::SyncActions(XrTime predicted_display_time) {
  std::vector<XrActiveActionSet> active_action_sets;
  active_action_sets.reserve(controller_states_.size() +
                             (eye_gaze_enabled_ ? 1 : 0));

  for (size_t i = 0; i < controller_states_.size(); i++) {
    active_action_sets.push_back(
        {controller_states_[i].controller.action_set(), XR_NULL_PATH});
  }
  if (eye_gaze_enabled_) {
    active_action_sets.push_back(
        {eye_gaze_action_set_, eye_gaze_subaction_path_});
  }

  XrActionsSyncInfo sync_info = {XR_TYPE_ACTIONS_SYNC_INFO};
  sync_info.countActiveActionSets = active_action_sets.size();
  sync_info.activeActionSets = active_action_sets.data();
  RETURN_IF_XR_FAILED(xrSyncActions(session_, &sync_info));

  for (auto& controller_state : controller_states_) {
    controller_state.controller.Update(local_space_, predicted_display_time);
  }

  return XR_SUCCESS;
}

std::optional<XrLocation> OpenXRInputHelper::GetXrLocationFromHandJoint(
    XrSpace mojo_space,
    const mojom::XRHandJointSpaceInfo& hand_joint_space_info,
    const gfx::Transform& joint_from_object) const {
  for (auto& controller_state : controller_states_) {
    if (controller_state.controller.IsHandTrackingEnabled() &&
        controller_state.controller.GetHandness() ==
            hand_joint_space_info.handedness) {
      auto mojo_from_joint = controller_state.controller.GetMojoFromJoint(
          MojomJointToOpenXRJoint(hand_joint_space_info.joint));
      if (mojo_from_joint) {
        return XrLocation{
            GfxTransformToXrPose(*mojo_from_joint * joint_from_object),
            mojo_space};
      }
    }
  }

  return std::nullopt;
}

std::optional<XrLocation> OpenXRInputHelper::GetXrLocationFromInputSource(
    const mojom::XRInputSourceSpaceInfo& space_info,
    const gfx::Transform& input_space_from_object) const {
  if (space_info.input_source_id >= controller_states_.size()) {
    return std::nullopt;
  }

  const auto& controller =
      controller_states_[space_info.input_source_id].controller;
  if (XrSpace input_space =
          controller.GetInputSpace(space_info.input_source_space_type);
      input_space != XR_NULL_HANDLE) {
    return XrLocation{GfxTransformToXrPose(input_space_from_object),
                      input_space};
  }

  // There is no corresponding controller or space.
  return std::nullopt;
}

}  // namespace device
