// TrackIR profile curves (Profiles/*.xml) and the per-axis output transform of RunTrackingFrameLoop (00497120).
#pragma once

#include "vision/pose.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace tir {

enum Axis { AxisYaw = 0, AxisPitch, AxisRoll, AxisX, AxisY, AxisZ, AxisCount };

struct AxisCurve {
    bool enabled = true;
    bool inverted = false;
    std::vector<std::pair<double, double>> points;  // (input, slope), sorted by input
};

// sign(x) * integral from 0 to |x| of the piecewise-linear slope curve (IntegrateLinearSlopeCurve, 0045a490).
double integrateSlopeCurve(const std::vector<std::pair<double, double>>& points, double x);

struct Profile {
    std::string name = "Default";
    std::array<AxisCurve, AxisCount> axes;

    // Reads a TrackIR profile XML (ASCII or UTF-16). Throws std::runtime_error on failure.
    static Profile load(const std::string& path);
    // One-to-one slopes on every axis.
    static Profile linear();
    // Writes the same XML layout TrackIR uses (UTF-8), so load() and TrackIR can both read it back.
    bool save(const std::string& path) const;

    // Applies curves and inversion; values stay in degrees / centimetres.
    HeadPose apply(const HeadPose& in) const;
};

}  // namespace tir
