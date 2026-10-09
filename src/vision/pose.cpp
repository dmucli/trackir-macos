#include "pose.hpp"

#include <algorithm>
#include <complex>
#include <limits>

namespace tir {

MarkerModel MarkerModel::triangle(double leg, double base, ClipType type)
{
    double h = std::sqrt(leg * leg - base * base * 0.25);
    // Centroid of (0,-h), (-b/2,0), (b/2,0) is (0,-h/3).
    MarkerModel m;
    m.apex = {0, -h + h / 3.0, 0};
    m.left = {-base * 0.5, h / 3.0, 0};
    m.right = {base * 0.5, h / 3.0, 0};
    // Starting depths from 005cc4f0 (the original uses negative values along its -Y optical axis).
    if (type == ClipType::TrackClipPro) {
        m.initialDepth[0] = 780;
        m.initialDepth[1] = 830;
        m.initialDepth[2] = 780;
    }
    return m;
}

void LensUndistorter::undistort(double u, double v, double& xn, double& yn) const
{
    double xd = (u - lens_.cx) / lens_.focalPx;
    double yd = (v - lens_.cy) / lens_.focalPx;
    xn = xd;
    yn = yd;
    for (int i = 0; i < 20; i++) {
        double r2 = xn * xn + yn * yn;
        double factor = 1 + r2 * (lens_.k1 + r2 * (lens_.k2 + r2 * lens_.k3));
        xn = xd / factor;
        yn = yd / factor;
    }
}

void LensUndistorter::project(const Vec3& p, double& u, double& v) const
{
    double xn = p.x / p.z, yn = p.y / p.z;
    double r2 = xn * xn + yn * yn;
    double factor = 1 + r2 * (lens_.k1 + r2 * (lens_.k2 + r2 * lens_.k3));
    u = lens_.focalPx * xn * factor + lens_.cx;
    v = lens_.focalPx * yn * factor + lens_.cy;
}

namespace {

// Real roots of a4 x^4 + ... + a0 (Durand-Kerner, then Newton polishing).
std::vector<double> realQuarticRoots(double a4, double a3, double a2, double a1, double a0)
{
    using C = std::complex<double>;
    std::vector<double> roots;
    if (std::fabs(a4) < 1e-12)
        return roots;
    double c[4] = {a3 / a4, a2 / a4, a1 / a4, a0 / a4};
    auto eval = [&](C x) { return (((x + c[0]) * x + c[1]) * x + c[2]) * x + c[3]; };

    C z[4];
    C seed(0.4, 0.9);
    for (int i = 0; i < 4; i++)
        z[i] = std::pow(seed, double(i));
    for (int iter = 0; iter < 500; iter++) {
        double change = 0;
        for (int i = 0; i < 4; i++) {
            C denom(1, 0);
            for (int j = 0; j < 4; j++)
                if (j != i)
                    denom *= z[i] - z[j];
            if (std::abs(denom) < 1e-300)
                denom = 1e-300;
            C step = eval(z[i]) / denom;
            z[i] -= step;
            change = std::max(change, std::abs(step));
        }
        if (change < 1e-14)
            break;
    }

    for (const C& r : z) {
        if (std::fabs(r.imag()) > 1e-6 * std::max(1.0, std::abs(r)))
            continue;
        double x = r.real();
        for (int k = 0; k < 5; k++) {
            double f = (((x + c[0]) * x + c[1]) * x + c[2]) * x + c[3];
            double d = ((4 * x + 3 * c[0]) * x + 2 * c[1]) * x + c[2];
            if (std::fabs(d) < 1e-15)
                break;
            x -= f / d;
        }
        roots.push_back(x);
    }
    return roots;
}

// Gauss-Newton on the three law-of-cosines equations to remove numerical error from the closed form.
void refineDepths(double s[3], const double d2[3], const double cosines[3])
{
    // Equations: (s_i, s_j, d_ij^2, cos_ij) for pairs (1,2), (0,2), (0,1).
    const int pi[3] = {1, 0, 0}, pj[3] = {2, 2, 1};
    for (int iter = 0; iter < 10; iter++) {
        double r[3], J[3][3] = {};
        for (int e = 0; e < 3; e++) {
            int i = pi[e], j = pj[e];
            r[e] = s[i] * s[i] + s[j] * s[j] - 2 * s[i] * s[j] * cosines[e] - d2[e];
            J[e][i] = 2 * s[i] - 2 * s[j] * cosines[e];
            J[e][j] = 2 * s[j] - 2 * s[i] * cosines[e];
        }
        // Solve J * delta = r by Cramer's rule.
        auto det3 = [](double a[3][3]) {
            return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                   a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        };
        double det = det3(J);
        if (std::fabs(det) < 1e-12)
            return;
        double delta[3];
        for (int k = 0; k < 3; k++) {
            double A[3][3];
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++)
                    A[row][col] = col == k ? r[row] : J[row][col];
            delta[k] = det3(A) / det;
        }
        double change = 0;
        for (int k = 0; k < 3; k++) {
            s[k] -= delta[k];
            change = std::max(change, std::fabs(delta[k]));
        }
        if (change < 1e-10)
            return;
    }
}

// Rotation/translation mapping three model points onto three camera points with the same triangle shape.
Pose alignTriangles(const std::array<Vec3, 3>& model, const std::array<Vec3, 3>& cam)
{
    auto frame = [](const std::array<Vec3, 3>& p) {
        Vec3 e1 = (p[1] - p[0]).normalized();
        Vec3 e3 = e1.cross(p[2] - p[0]).normalized();
        Vec3 e2 = e3.cross(e1);
        return Mat3::fromColumns(e1, e2, e3);
    };
    Pose pose;
    pose.rotation = frame(cam) * frame(model).transposed();
    Vec3 cm = (model[0] + model[1] + model[2]) * (1.0 / 3);
    Vec3 cc = (cam[0] + cam[1] + cam[2]) * (1.0 / 3);
    pose.translation = cc - pose.rotation * cm;
    return pose;
}

}  // namespace

// Grunert's solution as presented by Haralick et al. (1994). Indices: 0 = P1, 1 = P2, 2 = P3.
std::vector<Pose> solveP3P(const std::array<Vec3, 3>& model, const std::array<Vec3, 3>& bearingsIn)
{
    std::array<Vec3, 3> j = {bearingsIn[0].normalized(), bearingsIn[1].normalized(), bearingsIn[2].normalized()};
    double a2 = (model[1] - model[2]).dot(model[1] - model[2]);
    double b2 = (model[0] - model[2]).dot(model[0] - model[2]);
    double c2 = (model[0] - model[1]).dot(model[0] - model[1]);
    double ca = j[1].dot(j[2]);  // cos alpha, opposite P1
    double cb = j[0].dot(j[2]);  // cos beta, opposite P2
    double cg = j[0].dot(j[1]);  // cos gamma, opposite P3

    double amcb = (a2 - c2) / b2, apcb = (a2 + c2) / b2;
    double bmcb = (b2 - c2) / b2, bmab = (b2 - a2) / b2;
    double A4 = (amcb - 1) * (amcb - 1) - 4 * c2 / b2 * ca * ca;
    double A3 = 4 * (amcb * (1 - amcb) * cb - (1 - apcb) * ca * cg + 2 * c2 / b2 * ca * ca * cb);
    double A2 = 2 * (amcb * amcb - 1 + 2 * amcb * amcb * cb * cb + 2 * bmcb * ca * ca - 4 * apcb * ca * cb * cg +
                     2 * bmab * cg * cg);
    double A1 = 4 * (-amcb * (1 + amcb) * cb + 2 * a2 / b2 * cg * cg * cb - (1 - apcb) * ca * cg);
    double A0 = (1 + amcb) * (1 + amcb) - 4 * a2 / b2 * cg * cg;

    std::vector<Pose> poses;
    double d2[3] = {a2, b2, c2};
    double cosines[3] = {ca, cb, cg};
    for (double v : realQuarticRoots(A4, A3, A2, A1, A0)) {
        double denom = 2 * (cg - v * ca);
        if (std::fabs(denom) < 1e-12)
            continue;
        double u = ((-1 + amcb) * v * v - 2 * amcb * cb * v + 1 + amcb) / denom;
        double s1sq = b2 / (1 + v * v - 2 * v * cb);
        if (!(s1sq > 0))
            continue;
        double s[3] = {std::sqrt(s1sq), 0, 0};
        s[1] = u * s[0];
        s[2] = v * s[0];
        if (s[1] <= 0 || s[2] <= 0)
            continue;
        refineDepths(s, d2, cosines);
        if (s[0] <= 0 || s[1] <= 0 || s[2] <= 0)
            continue;
        std::array<Vec3, 3> cam = {j[0] * s[0], j[1] * s[1], j[2] * s[2]};
        Pose pose = alignTriangles(model, cam);
        bool duplicate = false;
        for (const Pose& p : poses)
            if (p.rotation.angleTo(pose.rotation) < 1e-6 && (p.translation - pose.translation).norm() < 1e-6)
                duplicate = true;
        if (!duplicate)
            poses.push_back(pose);
    }
    return poses;
}

// Mirrors 005cc4f0: residuals d_i^2 + d_j^2 - 2 cos_ij d_i d_j - D_ij^2 for pairs (0,1), (0,2), (1,2), solved by
// Newton-Raphson from a fixed starting point. The original stops after 19 iterations or at 1e-15 relative error.
bool solveDepthsNewton(const std::array<Vec3, 3>& bearingsIn, const double distances[3], const double initial[3],
                       double d[3])
{
    std::array<Vec3, 3> u = {bearingsIn[0].normalized(), bearingsIn[1].normalized(), bearingsIn[2].normalized()};
    const int pi[3] = {0, 0, 1}, pj[3] = {1, 2, 2};
    double c2[3], target[3];
    for (int e = 0; e < 3; e++) {
        c2[e] = 2 * u[size_t(pi[e])].dot(u[size_t(pj[e])]);
        target[e] = distances[e] * distances[e];
    }
    double scale = std::sqrt(target[0] * target[0] + target[1] * target[1] + target[2] * target[2]);
    for (int k = 0; k < 3; k++)
        d[k] = initial[k];

    for (int iter = 0; iter < 40; iter++) {
        double r[3], J[3][3] = {};
        double worst = 0;
        for (int e = 0; e < 3; e++) {
            int i = pi[e], j = pj[e];
            r[e] = d[i] * d[i] + d[j] * d[j] - c2[e] * d[i] * d[j] - target[e];
            J[e][i] = 2 * d[i] - c2[e] * d[j];
            J[e][j] = 2 * d[j] - c2[e] * d[i];
            worst = std::max(worst, std::fabs(r[e]));
        }
        if (worst <= scale * 1e-12)
            break;
        double det = J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) - J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
                     J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
        if (std::fabs(det) < 1e-12)
            return false;
        // delta = J^-1 r via the adjugate.
        double inv[3][3] = {
            {J[1][1] * J[2][2] - J[1][2] * J[2][1], J[0][2] * J[2][1] - J[0][1] * J[2][2], J[0][1] * J[1][2] - J[0][2] * J[1][1]},
            {J[1][2] * J[2][0] - J[1][0] * J[2][2], J[0][0] * J[2][2] - J[0][2] * J[2][0], J[0][2] * J[1][0] - J[0][0] * J[1][2]},
            {J[1][0] * J[2][1] - J[1][1] * J[2][0], J[0][1] * J[2][0] - J[0][0] * J[2][1], J[0][0] * J[1][1] - J[0][1] * J[1][0]},
        };
        for (int k = 0; k < 3; k++)
            d[k] -= (inv[k][0] * r[0] + inv[k][1] * r[1] + inv[k][2] * r[2]) / det;
    }
    for (int e = 0; e < 3; e++) {
        int i = pi[e], j = pj[e];
        double res = d[i] * d[i] + d[j] * d[j] - c2[e] * d[i] * d[j] - target[e];
        if (std::fabs(res) > scale * 1e-6)
            return false;
    }
    return d[0] > 0 && d[1] > 0 && d[2] > 0;
}

PoseTracker::PoseTracker(const LensModel& lens, const MarkerModel& model)
    : lens_(lens), model_{model.apex, model.left, model.right},
      initialDepth_{model.initialDepth[0], model.initialDepth[1], model.initialDepth[2]}
{
}

// Keeps marker identities from the previous frame, like the id bookkeeping in SelectVectorMarkers (005cbd20).
bool PoseTracker::trackIdentities(const std::array<const Blob*, 3>& b, std::array<int, 3>& order) const
{
    if (!havePrevious_)
        return false;
    double bestCost = std::numeric_limits<double>::max();
    std::array<int, 3> perm = {0, 1, 2};
    do {
        double cost = 0;
        for (int k = 0; k < 3; k++) {
            double dx = b[size_t(perm[size_t(k)])]->x - previousImage_[size_t(2 * k)];
            double dy = b[size_t(perm[size_t(k)])]->y - previousImage_[size_t(2 * k + 1)];
            cost += dx * dx + dy * dy;
        }
        if (cost < bestCost) {
            bestCost = cost;
            order = perm;
        }
    } while (std::next_permutation(perm.begin(), perm.end()));
    return bestCost < 3 * 40.0 * 40.0;
}

std::optional<Pose> PoseTracker::solve(const std::array<const Blob*, 3>& b, const std::array<int, 3>& order) const
{
    std::array<Vec3, 3> bearings;
    for (int k = 0; k < 3; k++) {
        double xn, yn;
        const Blob* blob = b[size_t(order[size_t(k)])];
        lens_.undistort(blob->x, blob->y, xn, yn);
        bearings[size_t(k)] = Vec3{xn, yn, 1.0}.normalized();
    }
    const double distances[3] = {(model_[0] - model_[1]).norm(), (model_[0] - model_[2]).norm(),
                                 (model_[1] - model_[2]).norm()};
    double depth[3];
    if (solveDepthsNewton(bearings, distances, initialDepth_, depth)) {
        std::array<Vec3, 3> cam = {bearings[0] * depth[0], bearings[1] * depth[1], bearings[2] * depth[2]};
        return alignTriangles(model_, cam);
    }
    // Newton left its basin (unusual geometry): fall back to the closed form, nearest to the previous pose.
    auto candidates = solveP3P(model_, bearings);
    if (candidates.empty())
        return std::nullopt;
    const Pose* best = &candidates[0];
    if (havePrevious_)
        for (const Pose& p : candidates)
            if (p.rotation.angleTo(previous_.rotation) < best->rotation.angleTo(previous_.rotation))
                best = &p;
    return *best;
}

std::optional<Pose> PoseTracker::update(const std::vector<Blob>& blobs)
{
    if (blobs.size() < 3) {
        havePrevious_ = false;
        return std::nullopt;
    }
    std::array<const Blob*, 3> picked = {&blobs[0], &blobs[1], &blobs[2]};
    std::array<int, 3> order{};
    std::optional<Pose> pose;

    if (trackIdentities(picked, order)) {
        pose = solve(picked, order);
    } else {
        // (Re)acquisition: P0 is the top blob (smallest y) as in SelectVectorMarkers. P1/P2 are interchangeable as
        // far as the distances go, so take whichever assignment is closer to the reference pose.
        order = {0, 1, 2};
        std::sort(order.begin(), order.end(), [&](int a, int c) { return picked[size_t(a)]->y < picked[size_t(c)]->y; });
        pose = solve(picked, order);
        std::array<int, 3> swapped = {order[0], order[2], order[1]};
        auto alt = solve(picked, swapped);
        const Pose* ref = havePrevious_ ? &previous_ : haveReference_ ? &reference_ : nullptr;
        if (alt && (!pose || (ref && alt->rotation.angleTo(ref->rotation) < pose->rotation.angleTo(ref->rotation)))) {
            pose = alt;
            order = swapped;
        }
    }
    if (!pose) {
        havePrevious_ = false;
        return std::nullopt;
    }

    previous_ = *pose;
    havePrevious_ = true;
    for (int k = 0; k < 3; k++) {
        previousImage_[size_t(2 * k)] = picked[size_t(order[size_t(k)])]->x;
        previousImage_[size_t(2 * k + 1)] = picked[size_t(order[size_t(k)])]->y;
    }
    return pose;
}

void Recentering::recenter(const Pose& pose)
{
    centreRotation_ = pose.rotation;
    centrePivot_ = pose.rotation * pivot_ + pose.translation;
    centred_ = true;
}

// Rotation relative to the centre pose, expressed in camera axes and decomposed as Ry(yaw) Rx(pitch) Rz(roll).
// Output axes are user-centric: x = user's right, y = up, z = towards the camera; yaw = turning left is positive.
HeadPose Recentering::relative(const Pose& pose) const
{
    HeadPose h;
    if (!centred_)
        return h;
    Mat3 rel = pose.rotation * centreRotation_.transposed();
    double camYaw = std::atan2(rel.m[0][2], rel.m[2][2]);
    double camPitch = std::asin(std::fmax(-1.0, std::fmin(1.0, -rel.m[1][2])));
    double camRoll = std::atan2(rel.m[1][0], rel.m[1][1]);
    Vec3 p = pose.rotation * pivot_ + pose.translation;
    Vec3 d = p - centrePivot_;
    h.yaw = -camYaw * kDegPerRad;
    h.pitch = -camPitch * kDegPerRad;
    h.roll = -camRoll * kDegPerRad;
    h.x = -d.x / 10.0;
    h.y = -d.y / 10.0;
    h.z = -d.z / 10.0;
    return h;
}

}  // namespace tir
