#include "settings.hpp"

#include "app/resources.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

namespace tir {

namespace {

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<double> numbers(const std::string& s)
{
    std::vector<double> out;
    std::stringstream in(s);
    std::string item;
    while (std::getline(in, item, ','))
        out.push_back(std::stod(item));
    return out;
}

std::string joinNumbers(const double* v, int n)
{
    std::string out;
    char buf[32];
    for (int i = 0; i < n; i++) {
        std::snprintf(buf, sizeof(buf), "%s%.6g", i ? "," : "", v[i]);
        out += buf;
    }
    return out;
}

}  // namespace

std::string Settings::defaultPath()
{
    return resourceDirectory() + "/settings.ini";
}

std::string profileDirectory()
{
    return resourceDirectory() + "/Profiles";
}

Settings Settings::load(const std::string& path)
{
    Settings s;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        try {
            if (key == "profile") s.profile = value;
            else if (key == "smoothing") s.smoothing = std::clamp(std::stod(value), 0.0, 0.95);
            else if (key == "clip_type") s.clipType = value == "pro" ? ClipType::TrackClipPro : ClipType::TrackClip;
            else if (key == "clip") {
                auto v = numbers(value);
                if (v.size() == 2) { s.clipLeg = v[0]; s.clipBase = v[1]; }
            } else if (key == "pivot") {
                auto v = numbers(value);
                if (v.size() == 3) s.pivot = {v[0], v[1], v[2]};
            }
            else if (key == "threshold") s.camera.threshold = std::stoi(value);
            else if (key == "exposure") s.camera.exposure = std::stoi(value);
            else if (key == "ir") s.camera.irIntensity = std::stoi(value);
            else if (key == "udp") s.udp = value == "1" || value == "on" || value == "true";
            else if (key == "udp_host") s.udpHost = value;
            else if (key == "udp_port") s.udpPort = std::stoi(value);
            else if (key == "game_keys") s.gameKeys = value;
            else if (key == "axis_sign") {
                auto v = numbers(value);
                for (size_t i = 0; i < v.size() && i < 6; i++)
                    s.axisSign[i] = v[i] < 0 ? -1 : 1;
            }
            else if (key == "focal_scale") s.focalScale = std::clamp(std::stod(value), 0.5, 2.0);
            else if (key == "hotkey_recenter") s.hotkeyRecenter = value;
            else if (key == "hotkey_pause") s.hotkeyPause = value;
        } catch (...) {
            // keep the default for a malformed value
        }
    }
    return s;
}

bool Settings::save(const std::string& path) const
{
    size_t slash = path.rfind('/');
    if (slash != std::string::npos)
        mkdir(path.substr(0, slash).c_str(), 0755);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
            return false;
        double clip[2] = {clipLeg, clipBase}, piv[3] = {pivot.x, pivot.y, pivot.z};
        double signs[6];
        for (int i = 0; i < 6; i++)
            signs[i] = axisSign[size_t(i)];
        f << "# trackir-mac settings (edited by the menu-bar app and `trackir-mac check`)\n"
          << "profile = " << profile << "\n"
          << "smoothing = " << smoothing << "\n"
          << "clip_type = " << (clipType == ClipType::TrackClipPro ? "pro" : "clip") << "\n"
          << "clip = " << joinNumbers(clip, 2) << "\n"
          << "pivot = " << joinNumbers(piv, 3) << "\n"
          << "threshold = " << camera.threshold << "\n"
          << "exposure = " << camera.exposure << "\n"
          << "ir = " << camera.irIntensity << "\n"
          << "udp = " << (udp ? 1 : 0) << "\n"
          << "udp_host = " << udpHost << "\n"
          << "udp_port = " << udpPort << "\n"
          << "game_keys = " << gameKeys << "\n"
          << "axis_sign = " << joinNumbers(signs, 6) << "\n"
          << "focal_scale = " << focalScale << "\n"
          << "hotkey_recenter = " << hotkeyRecenter << "\n"
          << "hotkey_pause = " << hotkeyPause << "\n";
        if (!f)
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace tir
