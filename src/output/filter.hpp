// Adaptive smoothing of the head pose before the profile curves: the 1€ filter (Casiez, Roussel & Vogel, CHI 2012).
// A low-pass whose cutoff rises with speed: heavy smoothing while the head is still (where the profile would
// magnify sensor noise into a shaky view), little lag during fast movements.
#pragma once

#include "vision/pose.hpp"

namespace tir {

class OneEuroFilter {
public:
    // minCutoff (Hz): cutoff at rest. beta: added cutoff per unit/s of speed. dCutoff (Hz): speed estimate filter.
    OneEuroFilter(double minCutoff = 1.0, double beta = 0.05, double dCutoff = 1.0)
        : minCutoff_(minCutoff), beta_(beta), dCutoff_(dCutoff)
    {
    }
    void setParameters(double minCutoff, double beta) { minCutoff_ = minCutoff; beta_ = beta; }
    void reset() { primed_ = false; }
    // dt: seconds since the previous sample.
    double step(double x, double dt);

private:
    double minCutoff_, beta_, dCutoff_;
    bool primed_ = false;
    double x_ = 0, dx_ = 0;
};

// One filter per axis. `smoothing` is the user setting 0..0.95 (settings.ini), mapped to the cutoff at rest:
// 0 -> 10 Hz (almost none), 0.3 -> 3 Hz, 0.5 -> 1.4 Hz, 0.95 -> 0.24 Hz.
class PoseFilter {
public:
    explicit PoseFilter(double smoothing = 0.3) { setSmoothing(smoothing); }
    void setSmoothing(double smoothing);
    void reset();
    HeadPose step(const HeadPose& in, double dt);

    static double restCutoffHz(double smoothing);

private:
    OneEuroFilter axes_[6];
};

}  // namespace tir
