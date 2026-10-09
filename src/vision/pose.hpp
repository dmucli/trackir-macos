// Three-marker head pose: lens undistortion, marker labelling, P3P and recentring.
#pragma once

#include "geometry.hpp"
#include "protocol/camera.hpp"
#include "vision/blobs.hpp"

#include <array>
#include <optional>
#include <vector>

namespace tir {

enum class ClipType { TrackClip = 0, TrackClipPro = 1 };

// Marker positions in millimetres. Camera convention: x right, y down, z away from the camera.
// apex = P0 (116 mm from both others), left/right = P1/P2 (69.6 mm apart). TrackIR orders the image blobs top to
// bottom and treats the top one as P0 (SelectVectorMarkers 005cbd20, solver 005cc4f0).
struct MarkerModel {
    Vec3 apex, left, right;
    // Depths (mm) the Newton solver starts from; they pick the physically plausible P3P branch (005cc4f0).
    double initialDepth[3] = {700, 500, 500};

    // Isosceles triangle with legs `leg` (apex to each base marker) and base `base`, centred on its centroid.
    static MarkerModel triangle(double leg, double base, ClipType type = ClipType::TrackClip);
    // cModuleVector defaults: legs 116.052 mm, base 69.621 mm.
    static MarkerModel trackClipDefault(ClipType type = ClipType::TrackClip) { return triangle(116.052, 69.621, type); }
};

struct Pose {
    Mat3 rotation;     // model -> camera
    Vec3 translation;  // model origin in camera coordinates, mm
};

// Head pose relative to the recentred pose, in the units TrackIR uses before its curves.
struct HeadPose {
    double yaw = 0, pitch = 0, roll = 0;  // degrees
    double x = 0, y = 0, z = 0;           // centimetres
};

class LensUndistorter {
public:
    explicit LensUndistorter(const LensModel& lens) : lens_(lens) {}
    // Pixel -> undistorted normalised image coordinates (x/z, y/z).
    void undistort(double u, double v, double& xn, double& yn) const;
    // Camera point -> distorted pixel (used by tests and diagnostics).
    void project(const Vec3& p, double& u, double& v) const;

private:
    LensModel lens_;
};

// All real P3P solutions (Grunert) for bearing vectors of model points p[0..2].
std::vector<Pose> solveP3P(const std::array<Vec3, 3>& model, const std::array<Vec3, 3>& bearings);

// TrackIR's own solver (005cc4f0): Newton-Raphson on the three depths from a fixed starting guess.
// distances = {|P0P1|, |P0P2|, |P1P2|}. Returns false if it does not converge to positive depths.
bool solveDepthsNewton(const std::array<Vec3, 3>& bearings, const double distances[3], const double initial[3],
                       double depths[3]);

class PoseTracker {
public:
    PoseTracker(const LensModel& lens, const MarkerModel& model);

    // Uses the three heaviest blobs. Returns nullopt while the clip is not fully visible.
    std::optional<Pose> update(const std::vector<Blob>& blobs);
    void reset() { havePrevious_ = false; }
    // Reference used to pick between the two base-marker assignments when the clip is (re)acquired.
    void setReference(const Pose& pose) { reference_ = pose; haveReference_ = true; }

private:
    bool trackIdentities(const std::array<const Blob*, 3>& blobs, std::array<int, 3>& order) const;
    std::optional<Pose> solve(const std::array<const Blob*, 3>& blobs, const std::array<int, 3>& order) const;

    LensUndistorter lens_;
    std::array<Vec3, 3> model_;
    double initialDepth_[3];
    bool havePrevious_ = false;
    Pose previous_{};
    bool haveReference_ = false;
    Pose reference_{};
    std::array<double, 6> previousImage_{};  // P0, P1, P2 in pixels
};

class Recentering {
public:
    explicit Recentering(Vec3 pivotMm = {}) : pivot_(pivotMm) {}
    void recenter(const Pose& pose);
    bool centred() const { return centred_; }
    HeadPose relative(const Pose& pose) const;

private:
    Vec3 pivot_;
    bool centred_ = false;
    Mat3 centreRotation_{};
    Vec3 centrePivot_{};
};

}  // namespace tir
