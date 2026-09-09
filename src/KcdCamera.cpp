#include "KcdCamera.h"

#include "Config.h"
#include "Log.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace kcdvr {
namespace {

// Internal arithmetic uses the conventional constructor-style w, x, y, z
// order. Do not use this type to describe CryEngine memory.
struct Quat {
    float w;
    float x;
    float y;
    float z;
};

std::mutex g_poseMutex;
TrackingPose g_latestPose;
std::array<EyeCameraGeometry, 2> g_eyeGeometry{};
XrPosef g_originPose{{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.0F}};
bool g_hasOrigin = false;
bool g_loggedEyeGeometry = false;
std::atomic_bool g_dialogueCameraActive = false;

Quat Normalize(Quat value)
{
    const float length = std::sqrt(value.w * value.w + value.x * value.x +
                                   value.y * value.y + value.z * value.z);
    if (length <= 0.000001F) {
        return {1.0F, 0.0F, 0.0F, 0.0F};
    }
    return {value.w / length, value.x / length, value.y / length, value.z / length};
}

Quat Multiply(const Quat& a, const Quat& b)
{
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

Quat Conjugate(const Quat& value)
{
    return {value.w, -value.x, -value.y, -value.z};
}

Vec3 Rotate(const Quat& rotation, const Vec3& value)
{
    const Quat vector{0.0F, value.x, value.y, value.z};
    const Quat result = Multiply(Multiply(rotation, vector), Conjugate(rotation));
    return {result.x, result.y, result.z};
}

Quat ToQuat(const XrQuaternionf& value)
{
    return Normalize({value.w, value.x, value.y, value.z});
}

// OpenXR: +X right, +Y up, -Z forward.
// CryEngine: +X right, +Y forward, +Z up.
// The basis change is a +90-degree rotation around X.
Quat XrToCryRotation(const Quat& xrRotation)
{
    constexpr float rootHalf = 0.7071067811865475F;
    const Quat basis{rootHalf, rootHalf, 0.0F, 0.0F};
    return Normalize(Multiply(Multiply(basis, xrRotation), Conjugate(basis)));
}

Vec3 XrToCryPosition(const XrVector3f& position)
{
    return {position.x, -position.z, position.y};
}

TrackingPose RelativePoseLocked()
{
    if (!g_latestPose.valid) {
        return {};
    }
    if (!g_hasOrigin) {
        g_originPose = g_latestPose.pose;
        g_hasOrigin = true;
    }

    const Quat origin = ToQuat(g_originPose.orientation);
    const Quat current = ToQuat(g_latestPose.pose.orientation);
    const Quat inverseOrigin = Conjugate(origin);
    const Quat relativeRotation = Normalize(Multiply(inverseOrigin, current));

    const Vec3 positionDelta{
        g_latestPose.pose.position.x - g_originPose.position.x,
        g_latestPose.pose.position.y - g_originPose.position.y,
        g_latestPose.pose.position.z - g_originPose.position.z,
    };
    const Vec3 localDelta = Rotate(inverseOrigin, positionDelta);

    TrackingPose result;
    result.pose.orientation = {relativeRotation.x, relativeRotation.y,
                               relativeRotation.z, relativeRotation.w};
    result.pose.position = {localDelta.x, localDelta.y, localDelta.z};
    result.valid = true;
    return result;
}

}  // namespace

void SetLatestTrackingPose(const TrackingPose& pose)
{
    std::scoped_lock lock(g_poseMutex);
    g_latestPose = pose;
}

void SetLatestEyeViews(const std::array<XrView, 2>& views)
{
    XrQuaternionf rightOrientation = views[1].pose.orientation;
    const XrQuaternionf& leftOrientation = views[0].pose.orientation;
    const float orientationDot =
        leftOrientation.x * rightOrientation.x +
        leftOrientation.y * rightOrientation.y +
        leftOrientation.z * rightOrientation.z +
        leftOrientation.w * rightOrientation.w;
    if (orientationDot < 0.0F) {
        rightOrientation = {-rightOrientation.x, -rightOrientation.y,
                            -rightOrientation.z, -rightOrientation.w};
    }
    const XrQuaternionf centerOrientationXr{
        leftOrientation.x + rightOrientation.x,
        leftOrientation.y + rightOrientation.y,
        leftOrientation.z + rightOrientation.z,
        leftOrientation.w + rightOrientation.w,
    };
    const Quat inverseCenter = Conjugate(ToQuat(centerOrientationXr));
    const XrVector3f centerPosition{
        (views[0].pose.position.x + views[1].pose.position.x) * 0.5F,
        (views[0].pose.position.y + views[1].pose.position.y) * 0.5F,
        (views[0].pose.position.z + views[1].pose.position.z) * 0.5F,
    };

    std::array<EyeCameraGeometry, 2> geometry{};
    for (std::size_t eye = 0; eye < geometry.size(); ++eye) {
        const Vec3 delta{
            views[eye].pose.position.x - centerPosition.x,
            views[eye].pose.position.y - centerPosition.y,
            views[eye].pose.position.z - centerPosition.z,
        };
        const Vec3 localXr = Rotate(inverseCenter, delta);
        geometry[eye].offset = {localXr.x, -localXr.z, localXr.y};
        geometry[eye].fov = views[eye].fov;
        geometry[eye].valid =
            std::isfinite(views[eye].fov.angleLeft) &&
            std::isfinite(views[eye].fov.angleRight) &&
            std::isfinite(views[eye].fov.angleUp) &&
            std::isfinite(views[eye].fov.angleDown) &&
            views[eye].fov.angleLeft < views[eye].fov.angleRight &&
            views[eye].fov.angleDown < views[eye].fov.angleUp;
    }

    std::scoped_lock lock(g_poseMutex);
    g_eyeGeometry = geometry;
    if (!g_loggedEyeGeometry && geometry[0].valid && geometry[1].valid) {
        const Vec3 separation{
            geometry[1].offset.x - geometry[0].offset.x,
            geometry[1].offset.y - geometry[0].offset.y,
            geometry[1].offset.z - geometry[0].offset.z,
        };
        const float ipd = std::sqrt(separation.x * separation.x +
                                    separation.y * separation.y +
                                    separation.z * separation.z);
        Log("OpenXR eye geometry: IPD=%.2f mm, left FOV=(%.2f %.2f %.2f %.2f), right FOV=(%.2f %.2f %.2f %.2f)",
            ipd * 1000.0F,
            geometry[0].fov.angleLeft, geometry[0].fov.angleRight,
            geometry[0].fov.angleUp, geometry[0].fov.angleDown,
            geometry[1].fov.angleLeft, geometry[1].fov.angleRight,
            geometry[1].fov.angleUp, geometry[1].fov.angleDown);
        g_loggedEyeGeometry = true;
    }
}

EyeCameraGeometry GetLatestEyeGeometry(std::size_t eye)
{
    std::scoped_lock lock(g_poseMutex);
    return eye < g_eyeGeometry.size() ? g_eyeGeometry[eye] : EyeCameraGeometry{};
}

void SetDialogueCameraActive(bool active)
{
    g_dialogueCameraActive.store(active, std::memory_order_relaxed);
}

bool IsDialogueCameraActive()
{
    return g_dialogueCameraActive.load(std::memory_order_relaxed);
}

TrackingPose GetLatestTrackingPose()
{
    std::scoped_lock lock(g_poseMutex);
    return RelativePoseLocked();
}

void RecenterTracking()
{
    std::scoped_lock lock(g_poseMutex);
    if (g_latestPose.valid) {
        g_originPose = g_latestPose.pose;
        g_hasOrigin = true;
        Log("Tracking recentered");
    }
}

void LockGameCameraToHorizontal(SViewParamsPrefix& view)
{
    // CryEngine uses +Z as up. Keep only the game's yaw around that axis;
    // ApplyTrackingPose subsequently restores headset-controlled pitch/roll.
    const Quat base = Normalize(
        {view.rotation.w, view.rotation.x, view.rotation.y, view.rotation.z});
    const float yaw = std::atan2(
        2.0F * (base.w * base.z + base.x * base.y),
        1.0F - 2.0F * (base.y * base.y + base.z * base.z));
    const float halfYaw = yaw * 0.5F;
    const Quat horizontal{std::cos(halfYaw), 0.0F, 0.0F, std::sin(halfYaw)};
    view.rotation = {horizontal.x, horizontal.y, horizontal.z, horizontal.w};
}

void ApplyTrackingPose(SViewParamsPrefix& view)
{
    const TrackingPose tracking = GetLatestTrackingPose();
    if (!tracking.valid) {
        return;
    }

    const Quat headXr = ToQuat(tracking.pose.orientation);
    const Quat headCry = XrToCryRotation(headXr);
    const Quat base = Normalize(
        {view.rotation.w, view.rotation.x, view.rotation.y, view.rotation.z});
    const Quat result = Normalize(Multiply(base, headCry));
    view.rotation = {result.x, result.y, result.z, result.w};

    if (GetConfig().enablePositionTracking) {
        Vec3 localPosition = XrToCryPosition(tracking.pose.position);
        localPosition.x *= GetConfig().worldScale;
        localPosition.y *= GetConfig().worldScale;
        localPosition.z *= GetConfig().worldScale;
        const Vec3 worldOffset = Rotate(base, localPosition);
        view.position.x += worldOffset.x;
        view.position.y += worldOffset.y;
        view.position.z += worldOffset.z;
    }
}

}  // namespace kcdvr
