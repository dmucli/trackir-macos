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

    // AirTracker found on real AirPods that looking up reads as negative pitch, and that tilting the head to the
    // right reads as positive roll. HeadPose wants pitch + up and roll + left, so both are negated.
    HeadPose h;
    h.yaw = e.yaw;
    h.pitch = -e.pitch;
    h.roll = -e.roll;
    return h;
}

}  // namespace tir
