// Groups the camera's per-row runs into marker blobs.
#pragma once

#include "protocol/frame.hpp"

#include <vector>

namespace tir {

struct Blob {
    double x = 0, y = 0;   // weighted centroid, sensor pixels
    double weight = 0;     // intensity sum when available, otherwise pixel count
    int area = 0;          // pixel count
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
};

// Runs on adjacent rows that overlap or touch diagonally belong to the same blob (8-connectivity).
std::vector<Blob> extractBlobs(const std::vector<Segment>& segments, int minArea = 1, int maxArea = 4000);

}  // namespace tir
