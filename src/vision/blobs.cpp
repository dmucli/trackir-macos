#include "blobs.hpp"

#include <algorithm>
#include <numeric>

namespace tir {

namespace {

int findRoot(std::vector<int>& parent, int i)
{
    while (parent[size_t(i)] != i) {
        parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        i = parent[size_t(i)];
    }
    return i;
}

}  // namespace

std::vector<Blob> extractBlobs(const std::vector<Segment>& input, int minArea, int maxArea)
{
    std::vector<Segment> segs(input);
    std::sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b) {
        return a.y != b.y ? a.y < b.y : a.x0 < b.x0;
    });

    std::vector<int> parent(segs.size());
    std::iota(parent.begin(), parent.end(), 0);

    // Walk rows in order, comparing each row with the one directly above it.
    size_t prevBegin = 0, prevEnd = 0;
    size_t i = 0;
    while (i < segs.size()) {
        size_t rowBegin = i;
        int y = segs[i].y;
        while (i < segs.size() && segs[i].y == y)
            i++;
        size_t rowEnd = i;
        bool prevIsAdjacent = prevEnd > prevBegin && segs[prevBegin].y == y - 1;
        if (prevIsAdjacent) {
            for (size_t a = rowBegin; a < rowEnd; a++)
                for (size_t b = prevBegin; b < prevEnd; b++)
                    if (segs[a].x0 <= segs[b].x1 + 1 && segs[b].x0 <= segs[a].x1 + 1) {
                        int ra = findRoot(parent, int(a)), rb = findRoot(parent, int(b));
                        if (ra != rb)
                            parent[size_t(ra)] = rb;
                    }
        }
        prevBegin = rowBegin;
        prevEnd = rowEnd;
    }

    std::vector<int> blobOf(segs.size(), -1);
    std::vector<Blob> blobs;
    std::vector<double> sumX, sumY;
    for (size_t s = 0; s < segs.size(); s++) {
        int root = findRoot(parent, int(s));
        if (blobOf[size_t(root)] < 0) {
            blobOf[size_t(root)] = int(blobs.size());
            Blob b;
            b.minX = int(segs[s].x0);
            b.maxX = int(segs[s].x1);
            b.minY = b.maxY = segs[s].y;
            blobs.push_back(b);
            sumX.push_back(0);
            sumY.push_back(0);
        }
        size_t k = size_t(blobOf[size_t(root)]);
        const Segment& seg = segs[s];
        int length = int(seg.x1 - seg.x0) + 1;
        Blob& b = blobs[k];
        b.area += length;
        if (seg.moment0) {
            // Intensity-weighted, sub-pixel (FUN_00588600).
            double w = seg.moment0;
            b.weight += w;
            sumX[k] += double(seg.moment1) + seg.x0 * w;
            sumY[k] += seg.y * w;
        } else {
            double w = length;
            b.weight += w;
            sumX[k] += w * (seg.x0 + seg.x1) * 0.5;
            sumY[k] += w * seg.y;
        }
        b.minX = std::min(b.minX, int(seg.x0));
        b.maxX = std::max(b.maxX, int(seg.x1));
        b.minY = std::min(b.minY, seg.y);
        b.maxY = std::max(b.maxY, seg.y);
    }

    std::vector<Blob> out;
    for (size_t k = 0; k < blobs.size(); k++) {
        Blob b = blobs[k];
        if (b.area < minArea || b.area > maxArea || b.weight <= 0)
            continue;
        b.x = sumX[k] / b.weight;
        b.y = sumY[k] / b.weight;
        out.push_back(b);
    }
    std::sort(out.begin(), out.end(), [](const Blob& a, const Blob& b) { return a.weight > b.weight; });
    return out;
}

}  // namespace tir
