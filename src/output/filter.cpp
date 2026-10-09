#include "filter.hpp"

#include <algorithm>
#include <cmath>

namespace tir {

namespace {

double smoothingFactor(double cutoffHz, double dt)
{
    double tau = 1.0 / (2.0 * M_PI * cutoffHz);
    return 1.0 / (1.0 + tau / dt);
}

}  // namespace

double OneEuroFilter::step(double x, double dt)
{
    if (!primed_ || !(dt > 0)) {
        primed_ = true;
        x_ = x;
        dx_ = 0;
        return x_;
    }
    dt = std::min(dt, 0.25);  // after a dropout, do not let one huge step through unfiltered
    double dx = (x - x_) / dt;
    dx_ += smoothingFactor(dCutoff_, dt) * (dx - dx_);
    double cutoff = minCutoff_ + beta_ * std::fabs(dx_);
    x_ += smoothingFactor(cutoff, dt) * (x - x_);
    return x_;
}

double PoseFilter::restCutoffHz(double smoothing)
{
    double s = std::clamp(smoothing, 0.0, 0.95);
    return 0.2 * std::pow(50.0, 1.0 - s);
}

void PoseFilter::setSmoothing(double smoothing)
{
    double cutoff = restCutoffHz(smoothing);
    // Speeds: rotations in deg/s (a quick glance is ~200 deg/s), translations in cm/s (~30 cm/s).
    for (int i = 0; i < 3; i++)
        axes_[i].setParameters(cutoff, 0.05);
    for (int i = 3; i < 6; i++)
        axes_[i].setParameters(cutoff, 0.3);
}

void PoseFilter::reset()
{
    for (auto& a : axes_)
        a.reset();
}

HeadPose PoseFilter::step(const HeadPose& in, double dt)
{
    HeadPose out;
    out.yaw = axes_[0].step(in.yaw, dt);
    out.pitch = axes_[1].step(in.pitch, dt);
    out.roll = axes_[2].step(in.roll, dt);
    out.x = axes_[3].step(in.x, dt);
    out.y = axes_[4].step(in.y, dt);
    out.z = axes_[5].step(in.z, dt);
    return out;
}

}  // namespace tir
