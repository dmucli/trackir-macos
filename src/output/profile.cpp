#include "profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace tir {

namespace {

double slopeAt(const std::vector<std::pair<double, double>>& pts, double t)
{
    if (t <= pts.front().first)
        return pts.front().second;
    if (t >= pts.back().first)
        return pts.back().second;
    for (size_t i = 1; i < pts.size(); i++)
        if (t <= pts[i].first) {
            double x0 = pts[i - 1].first, x1 = pts[i].first;
            double f = x1 > x0 ? (t - x0) / (x1 - x0) : 0;
            return pts[i - 1].second + f * (pts[i].second - pts[i - 1].second);
        }
    return pts.back().second;
}

// Exact trapezoid integral of the piecewise-linear slope between lo and hi (lo <= hi).
double integrateRange(const std::vector<std::pair<double, double>>& pts, double lo, double hi)
{
    std::vector<double> knots{lo, hi};
    for (const auto& p : pts)
        if (p.first > lo && p.first < hi)
            knots.push_back(p.first);
    std::sort(knots.begin(), knots.end());
    double sum = 0;
    for (size_t i = 1; i < knots.size(); i++)
        sum += (knots[i] - knots[i - 1]) * 0.5 * (slopeAt(pts, knots[i - 1]) + slopeAt(pts, knots[i]));
    return sum;
}

std::string readAsUtf8(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open profile " + path);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bool utf16 = raw.size() >= 2 && ((uint8_t(raw[0]) == 0xFF && uint8_t(raw[1]) == 0xFE) || raw[1] == '\0');
    if (!utf16)
        return raw;
    std::string out;
    for (size_t i = (uint8_t(raw[0]) == 0xFF ? 2 : 0); i + 1 < raw.size(); i += 2) {
        unsigned c = uint8_t(raw[i]) | uint8_t(raw[i + 1]) << 8;
        out.push_back(c < 0x80 ? char(c) : '?');
    }
    return out;
}

std::string tagValue(const std::string& xml, const std::string& tag)
{
    std::smatch m;
    if (std::regex_search(xml, m, std::regex("<" + tag + ">\\s*([^<]*?)\\s*</" + tag + ">")))
        return m[1];
    return {};
}

}  // namespace

double integrateSlopeCurve(const std::vector<std::pair<double, double>>& points, double x)
{
    if (points.empty())
        return x;
    double magnitude = x >= 0 ? integrateRange(points, 0, x) : integrateRange(points, x, 0);
    return x < 0 ? -magnitude : magnitude;
}

Profile Profile::linear()
{
    Profile p;
    p.name = "Linear";
    for (auto& a : p.axes)
        a.points = {{-180, 1}, {180, 1}};
    return p;
}

Profile Profile::load(const std::string& path)
{
    std::string xml = readAsUtf8(path);
    Profile p = linear();
    std::string name = tagValue(xml, "Name");
    for (const auto& [entity, c] : {std::pair<std::string, std::string>{"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}})
        for (size_t at; (at = name.find(entity)) != std::string::npos;)
            name.replace(at, entity.size(), c);
    if (!name.empty())
        p.name = name;

    std::regex curveRe("<Curve>([\\s\\S]*?)</Curve>");
    std::regex valRe("<Val>\\s*([-+0-9.eE]+)\\s*</Val>");
    int found = 0;
    for (std::sregex_iterator it(xml.begin(), xml.end(), curveRe), end; it != end; ++it) {
        std::string body = (*it)[1];
        std::string axisText = tagValue(body, "Axis");
        if (axisText.empty())
            continue;
        int axis = std::stoi(axisText);
        if (axis < 0 || axis >= AxisCount)
            continue;
        AxisCurve curve;
        curve.enabled = tagValue(body, "Enabled") != "0";
        curve.inverted = tagValue(body, "Inverted") == "1";
        std::vector<double> vals;
        for (std::sregex_iterator v(body.begin(), body.end(), valRe); v != end; ++v)
            vals.push_back(std::stod((*v)[1]));
        for (size_t i = 0; i + 1 < vals.size(); i += 2)
            curve.points.emplace_back(vals[i], vals[i + 1]);
        std::sort(curve.points.begin(), curve.points.end());
        if (curve.points.size() < 2)
            continue;
        p.axes[size_t(axis)] = curve;
        found++;
    }
    if (found == 0)
        throw std::runtime_error("no <Curve> entries in " + path);
    return p;
}

bool Profile::save(const std::string& path) const
{
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
            return false;
        auto escaped = [](const std::string& text) {
            std::string out;
            for (char c : text) {
                if (c == '<') out += "&lt;";
                else if (c == '>') out += "&gt;";
                else if (c == '&') out += "&amp;";
                else out += c;
            }
            return out;
        };
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<Profile>\r\n"
          << "  <Name>" << escaped(name) << "</Name>\r\n"
          << "  <Description>" << escaped(name) << "</Description>\r\n"
          << "  <ExclusiveLoad>0</ExclusiveLoad>\r\n  <TrueViewEnabled>1</TrueViewEnabled>\r\n";
        char buf[64];
        for (int a = 0; a < AxisCount; a++) {
            const AxisCurve& c = axes[size_t(a)];
            f << "  <Curve>\r\n    <Axis>" << a << "</Axis>\r\n    <Enabled>" << (c.enabled ? 1 : 0)
              << "</Enabled>\r\n    <Inverted>" << (c.inverted ? 1 : 0)
              << "</Inverted>\r\n    <Mirrored>1</Mirrored>\r\n    <Type>0</Type>\r\n    <Inputs>\r\n";
            for (const auto& p : c.points) {
                std::snprintf(buf, sizeof(buf), "      <Val>%.6g</Val>\r\n      <Val>%.6g</Val>\r\n", p.first, p.second);
                f << buf;
            }
            f << "    </Inputs>\r\n  </Curve>\r\n";
        }
        f << "</Profile>\r\n";
        if (!f)
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

HeadPose Profile::apply(const HeadPose& in) const
{
    const double raw[AxisCount] = {in.yaw, in.pitch, in.roll, in.x, in.y, in.z};
    double out[AxisCount];
    for (int a = 0; a < AxisCount; a++) {
        const AxisCurve& c = axes[size_t(a)];
        if (!c.enabled) {
            out[a] = 0;
            continue;
        }
        out[a] = integrateSlopeCurve(c.points, raw[a]);
        if (c.inverted)
            out[a] = -out[a];
    }
    HeadPose h;
    h.yaw = out[AxisYaw];
    h.pitch = out[AxisPitch];
    h.roll = out[AxisRoll];
    h.x = out[AxisX];
    h.y = out[AxisY];
    h.z = out[AxisZ];
    return h;
}

}  // namespace tir
