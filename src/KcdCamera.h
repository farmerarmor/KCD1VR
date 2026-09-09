#pragma once

#include <openxr/openxr.h>

#include <array>
#include <cstddef>

namespace kcdvr {

struct Vec3 {
    float x;
    float y;
    float z;
};

// CryEngine's Quat_tpl stores its vector first, then the scalar: x, y, z, w.
// This is the in-memory layout used inside SViewParams.
struct CryQuat {
    float x;
    float y;
    float z;
    float w;
};

// Prefix of CryAction::SViewParams. Only verified fields touched by this mod are
// represented; the game owns the remainder of the structure.
struct SViewParamsPrefix {
    Vec3 position;
    CryQuat rotation;
    CryQuat localRotationLast;
    float nearPlane;
    float fov;
};

static_assert(offsetof(SViewParamsPrefix, rotation) == 0x0C);
static_assert(offsetof(SViewParamsPrefix, fov) == 0x30);

struct TrackingPose {
    XrPosef pose{{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.0F}};
    bool valid = false;
};

struct EyeCameraGeometry {
    Vec3 offset{};
    XrFovf fov{};
    bool valid = false;
};

void SetLatestTrackingPose(const TrackingPose& pose);
TrackingPose GetLatestTrackingPose();
void SetLatestEyeViews(const std::array<XrView, 2>& views);
EyeCameraGeometry GetLatestEyeGeometry(std::size_t eye);
void SetDialogueCameraActive(bool active);
bool IsDialogueCameraActive();
void RecenterTracking();
void LockGameCameraToHorizontal(SViewParamsPrefix& view);
void ApplyTrackingPose(SViewParamsPrefix& view);

}  // namespace kcdvr
