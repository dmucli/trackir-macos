// Head pose from AirPods (or other headphones with spatial-audio head tracking): CoreMotion attitude quaternion ->
// recentred yaw/pitch/roll in the HeadPose convention, with optional yaw drift correction.
//
// The quaternion maths and the drift correction are ported from AirTracker (https://github.com/crippler95/airtracker,
// MIT licence, Copyright (c) 2026 AirTracker contributors): QuaternionMath.opentrackEuler and
// OrientationPipeline.stepDrift.
#pragma once

#include "vision/pose.hpp"

namespace tir {

struct Quat {
    double w = 1, x = 0, y = 0, z = 0;

    Quat operator*(const Quat& o) const
    {
        return {w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }
    Quat conjugate() const { return {w, -x, -y, -z}; }
    Quat normalized() const;
    // Rotation of `degrees` about the axis (ax, ay, az).
    static Quat axisAngle(double ax, double ay, double az, double degrees);
};

// Yaw/pitch/roll (degrees) of a rotation in the headphones' body frame (x right, y forward, z up), decomposed in
// intrinsic Z-X'-Y'' order (R = Rz(yaw) Rx(pitch) Ry(roll)): yaw + = turn left, pitch + = nose up about the
// x axis, roll + = right ear down.
struct HeadphoneEuler {
    double yaw = 0, pitch = 0, roll = 0;
};
HeadphoneEuler headphoneEuler(const Quat& q);

// One drift step: moves the accumulator towards `yaw` by at most rate * dt. Subtracting the result from the raw yaw
// pulls the output back to the centre at no more than `rate` degrees per second.
double stepYawDrift(double drift, double yaw, double rate, double dt);

class HeadphoneTracker {
public:
    // The next sample becomes the centre (also true on construction).
    void recenter() { recenterPending_ = true; }
    // Degrees per second; 0 disables the correction.
    void setDriftRate(double degPerSecond);

    // `attitude`: CMDeviceMotion.attitude.quaternion. `seconds`: monotonic sample time.
    // Returns the pose relative to the centre in HeadPose axes (yaw + left, pitch + up, roll + left; x/y/z stay 0,
    // headphones only measure rotation).
    HeadPose update(const Quat& attitude, double seconds);

private:
    bool recenterPending_ = true;
    Quat reference_;
    double driftRate_ = 0, drift_ = 0, lastTime_ = 0;
    bool haveTime_ = false;
};

}  // namespace tir
