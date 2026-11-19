#include "headphone_pose.hpp"

#include <algorithm>
#include <cmath>

namespace tir {

namespace {

constexpr double kDeg = 180.0 / M_PI;

}  // namespace

Quat Quat::normalized() const
{
    double n = std::sqrt(w * w + x * x + y * y + z * z);
    return n > 0 ? Quat{w / n, x / n, y / n, z / n} : Quat{};
}

Quat Quat::axisAngle(double ax, double ay, double az, double degrees)
{
    double n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n <= 0)
        return {};
    double h = degrees / kDeg / 2, s = std::sin(h) / n;
    return {std::cos(h), ax * s, ay * s, az * s};
}

HeadphoneEuler headphoneEuler(const Quat& q)
{
    const double w = q.w, x = q.x, y = q.y, z = q.z;
    HeadphoneEuler e;
    e.pitch = std::asin(std::clamp(2 * (y * z + w * x), -1.0, 1.0)) * kDeg;
    e.yaw = std::atan2(2 * (w * z - x * y), 1 - 2 * (x * x + z * z)) * kDeg;
    e.roll = std::atan2(2 * (w * y - x * z), 1 - 2 * (x * x + y * y)) * kDeg;
    return e;
}

// HeadPose axes: x right, y up, z towards the screen; yaw + left, pitch + up, roll + left. The rotation is yaw about
// the vertical, then pitch about the head's left-right axis, then roll about its forward axis.
HeadPose withNeckModel(HeadPose pose, double forwardCm, double upCm)
{
    const double psi = pose.yaw / kDeg, theta = pose.pitch / kDeg, phi = pose.roll / kDeg;
    // Head forward and up after pitch and roll, before yaw.
    const double fx = 0, fy = std::sin(theta), fz = std::cos(theta);
    const double ux = -std::sin(phi), uy = std::cos(theta) * std::cos(phi), uz = -std::sin(theta) * std::cos(phi);
    double px = forwardCm * fx + upCm * ux, py = forwardCm * fy + upCm * uy, pz = forwardCm * fz + upCm * uz;
    // Yaw left takes forward (0, 0, 1) to (-sin, 0, cos).
    double x = px * std::cos(psi) - pz * std::sin(psi), z = px * std::sin(psi) + pz * std::cos(psi);
    pose.x += x;
    pose.y += py - upCm;
    pose.z += z - forwardCm;
    return pose;
}

double stepYawDrift(double drift, double yaw, double rate, double dt)
{
    double maxStep = rate * dt;
    return drift + std::clamp(yaw - drift, -maxStep, maxStep);
}

void HeadphoneTracker::setDriftRate(double degPerSecond)
{
    driftRate_ = std::clamp(degPerSecond, 0.0, 10.0);
    if (driftRate_ == 0)
        drift_ = 0;
}

HeadPose HeadphoneTracker::update(const Quat& attitude, double seconds)
{
    Quat q = attitude.normalized();
    if (recenterPending_) {
        reference_ = q;
        drift_ = 0;
        recenterPending_ = false;
    }
    // Orientation relative to the centre, in the centre's body frame.
    HeadphoneEuler e = headphoneEuler((reference_.conjugate() * q).normalized());

    if (driftRate_ > 0) {
        double dt = haveTime_ ? std::clamp(seconds - lastTime_, 0.0, 0.2) : 0.0;
        drift_ = stepYawDrift(drift_, e.yaw, driftRate_, dt);
        e.yaw -= drift_;
    }
    lastTime_ = seconds;
    haveTime_ = true;

    // In the body frame, yaw + is a left turn and pitch + is nose up, as HeadPose wants (checked on AirPods 3;
    // AirTracker negates pitch for opentrack's convention, not because the sensor reads it backwards). Roll + is
    // right ear down, and HeadPose wants roll + left, so roll is negated.
    HeadPose h;
    h.yaw = e.yaw;
    h.pitch = e.pitch;
    h.roll = -e.roll;
    return h;
}

}  // namespace tir
