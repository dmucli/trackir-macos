// Minimal 3-D vector/matrix helpers for the pose solver.
#pragma once

#include <cmath>

namespace tir {

struct Vec3 {
    double x = 0, y = 0, z = 0;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const { double n = norm(); return n > 0 ? *this * (1.0 / n) : *this; }
};

struct Mat3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    static Mat3 fromColumns(const Vec3& a, const Vec3& b, const Vec3& c)
    {
        Mat3 r;
        const Vec3* cols[3] = {&a, &b, &c};
        for (int j = 0; j < 3; j++) {
            r.m[0][j] = cols[j]->x;
            r.m[1][j] = cols[j]->y;
            r.m[2][j] = cols[j]->z;
        }
        return r;
    }

    Mat3 operator*(const Mat3& o) const
    {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }

    Vec3 operator*(const Vec3& v) const
    {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }

    Mat3 transposed() const
    {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                r.m[i][j] = m[j][i];
        return r;
    }

    // Rotation angle of this * other^T, in radians.
    double angleTo(const Mat3& other) const
    {
        Mat3 d = *this * other.transposed();
        double c = (d.m[0][0] + d.m[1][1] + d.m[2][2] - 1.0) * 0.5;
        return std::acos(std::fmax(-1.0, std::fmin(1.0, c)));
    }
};

inline Mat3 rotationX(double a)
{
    Mat3 r;
    double c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c; r.m[1][2] = -s;
    r.m[2][1] = s; r.m[2][2] = c;
    return r;
}

inline Mat3 rotationY(double a)
{
    Mat3 r;
    double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[0][2] = s;
    r.m[2][0] = -s; r.m[2][2] = c;
    return r;
}

inline Mat3 rotationZ(double a)
{
    Mat3 r;
    double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[0][1] = -s;
    r.m[1][0] = s; r.m[1][1] = c;
    return r;
}

constexpr double kDegPerRad = 57.29577951308232;

}  // namespace tir
